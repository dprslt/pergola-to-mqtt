#!/usr/bin/env python3
"""Decode captured OOK pulse trains and work out what the remote is doing.

Reads the JSONL files written by pergola_capture.py and answers, in order of
importance:

  1. Is a capture clean enough to trust?
  2. What are the symbol widths, and how many times does the burst repeat?
  3. What bits does one repeat carry?
  4. Do repeated presses of the SAME button produce the SAME code?

Question 4 is the project's go/no-go gate. A fixed code (EV1527, PT2262 and
relatives) repeats byte-for-byte forever and can simply be replayed. A rolling
code (Somfy RTS and relatives) changes on every press, and replaying a captured
frame will do nothing.

Usage:
    python3 pergola_analyze.py captures/*.jsonl
    python3 pergola_analyze.py captures/open-*.jsonl --json report.json
    python3 pergola_analyze.py captures/open-*.jsonl --frame 3 --verbose
"""

from __future__ import annotations

import argparse
import glob
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence

# --------------------------------------------------------------------------- #
# Width clustering
# --------------------------------------------------------------------------- #

DEFAULT_TOLERANCE = 0.25

# A pulse this many times wider than the dominant symbol is a gap between
# repeats, not a symbol. Remotes leave a big margin here -- EV1527's sync gap is
# 31x its short pulse -- so the exact value barely matters.
SYNC_MULTIPLE = 4.0


@dataclass
class WidthClass:
    centre: float
    count: int
    lo: float
    hi: float

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"<{self.centre:.0f}us x{self.count}>"


def cluster_widths(
    durations: Sequence[int], tolerance: float = DEFAULT_TOLERANCE
) -> list[WidthClass]:
    """Group pulse widths into classes.

    Agglomerative, walking widths in ascending order: a width joins a class when
    it is within `tolerance` of that class's running mean. Async serial mode is
    documented to jitter every edge by up to +/-1/8 of a bit period, so exact
    matching is useless and some tolerance is mandatory.

    A class is additionally capped at 1 + 2*tolerance times its smallest member,
    which stops a long ramp of gradually increasing widths from chaining a 400 us
    symbol and an 800 us symbol into one class.
    """
    if not durations:
        return []

    classes: list[dict] = []
    for d in sorted(float(x) for x in durations):
        placed = False
        for c in classes:
            centre = c["sum"] / c["n"]
            within_tolerance = abs(d - centre) <= tolerance * centre
            within_span = d <= c["lo"] * (1.0 + 2.0 * tolerance)
            if within_tolerance and within_span:
                c["sum"] += d
                c["n"] += 1
                c["hi"] = max(c["hi"], d)
                placed = True
                break
        if not placed:
            classes.append({"sum": d, "n": 1, "lo": d, "hi": d})

    return [
        WidthClass(centre=c["sum"] / c["n"], count=c["n"], lo=c["lo"], hi=c["hi"])
        for c in classes
    ]


def dominant_class(classes: Sequence[WidthClass]) -> WidthClass | None:
    """The most populous width class -- in practice, the symbol width."""
    return max(classes, key=lambda c: c.count) if classes else None


def classify(classes: Sequence[WidthClass], duration: float) -> int:
    """Index of the nearest width class."""
    return min(range(len(classes)), key=lambda i: abs(classes[i].centre - duration))


# --------------------------------------------------------------------------- #
# Frames
# --------------------------------------------------------------------------- #


@dataclass
class Frame:
    """One captured burst."""

    seq: int
    durations: list[int]
    first_level: int = 1
    rssi: float | None = None
    t_ms: int | None = None
    truncated: bool = False
    button: str = "unlabelled"
    source: str = ""

    def levels(self) -> list[int]:
        """Level of each duration; they strictly alternate."""
        return [
            self.first_level if i % 2 == 0 else 1 - self.first_level
            for i in range(len(self.durations))
        ]

    @property
    def total_us(self) -> int:
        return sum(self.durations)

    @classmethod
    def from_json(cls, obj: dict) -> "Frame":
        return cls(
            seq=int(obj.get("seq", 0)),
            durations=[int(x) for x in obj["durations"]],
            first_level=int(obj.get("first_level", 1)),
            rssi=obj.get("rssi"),
            t_ms=obj.get("t_ms"),
            truncated=bool(obj.get("truncated", False)),
            button=obj.get("button", "unlabelled"),
            source=obj.get("source", ""),
        )


@dataclass
class Repeat:
    """One transmission of the burst, between two inter-repeat gaps."""

    start: int
    end: int  # exclusive
    durations: list[int]
    first_level: int
    gap_after: int | None = None

    @property
    def length(self) -> int:
        return len(self.durations)


def split_repeats(
    frame: Frame,
    classes: Sequence[WidthClass],
    sync_multiple: float = SYNC_MULTIPLE,
    min_pulses: int = 8,
) -> tuple[list[Repeat], float]:
    """Cut a frame at the long gaps that separate repeated transmissions.

    Returns the repeats and the threshold used, so a caller can report it.
    """
    symbol = dominant_class(classes)
    if symbol is None:
        return [], 0.0
    threshold = symbol.centre * sync_multiple

    levels = frame.levels()
    repeats: list[Repeat] = []
    start = 0
    for i in range(len(frame.durations) + 1):
        at_end = i == len(frame.durations)
        is_gap = (not at_end) and frame.durations[i] >= threshold
        if not (at_end or is_gap):
            continue
        if i - start >= min_pulses:
            repeats.append(
                Repeat(
                    start=start,
                    end=i,
                    durations=frame.durations[start:i],
                    first_level=levels[start],
                    gap_after=None if at_end else frame.durations[i],
                )
            )
        start = i + 1

    return repeats, threshold


# --------------------------------------------------------------------------- #
# Decoders
# --------------------------------------------------------------------------- #


@dataclass
class Decode:
    scheme: str
    bits: str
    confidence: str = "ok"
    note: str = ""

    @property
    def hex(self) -> str:
        if not self.bits:
            return ""
        value = int(self.bits, 2)
        width = (len(self.bits) + 3) // 4
        return f"0x{value:0{width}X}"

    @property
    def nbits(self) -> int:
        return len(self.bits)


def decode_pwm(repeat: Repeat, classes: Sequence[WidthClass]) -> Decode | None:
    """Pulse-width encoding: each bit is one high pulse plus one low pulse.

    short-high + long-low  -> 0
    long-high  + short-low -> 1

    This is the EV1527 / PT2262 / HT6P20 family, i.e. most cheap remotes. The 0/1
    assignment is a convention, not a fact -- the complement is equally valid, so
    the report shows both.
    """
    # Pair from the first high pulse: a capture can begin mid-air on either level.
    offset = 0 if repeat.first_level == 1 else 1
    body = repeat.durations[offset:]
    if len(body) < 4:
        return None

    # Only the two widths that actually carry data matter here; a leading sync
    # pulse would be a third, much wider class.
    local = cluster_widths(body)
    data_classes = sorted(
        (c for c in local if c.count >= 2), key=lambda c: c.count, reverse=True
    )[:2]
    if len(data_classes) < 2:
        return None
    short, long = sorted(data_classes, key=lambda c: c.centre)
    if long.centre < short.centre * 1.4:
        return None  # widths too close together to be a two-symbol code
    threshold = (short.centre + long.centre) / 2.0

    bits = []
    pairs = len(body) // 2
    for p in range(pairs):
        high = body[2 * p]
        low = body[2 * p + 1]
        high_long = high >= threshold
        low_long = low >= threshold
        if high_long == low_long:
            # Both short or both long: not a two-symbol PWM code.
            return None
        bits.append("1" if high_long else "0")

    if not bits:
        return None
    note = ""
    if len(body) % 2:
        note = "trailing half-bit ignored"
    return Decode(scheme="pwm", bits="".join(bits), note=note)


def _to_chips(repeat: Repeat, half_period: float) -> list[int] | None:
    """Expand pulses into fixed-width half-symbol samples ("chips")."""
    chips: list[int] = []
    level = repeat.first_level
    for d in repeat.durations:
        n = int(round(d / half_period))
        if n < 1:
            return None
        if n > 8:
            return None  # a gap this long is not part of a Manchester body
        chips.extend([level] * n)
        level = 1 - level
    return chips


MANCHESTER_MIN_BITS = 8
MANCHESTER_MAX_SKIP = 3


def decode_manchester(repeat: Repeat, classes: Sequence[WidthClass]) -> Decode | None:
    """Manchester: every bit is a transition in the middle of a fixed period.

    Used by Somfy RTS and several other rolling-code remotes. Decoded by
    resampling the pulse train into half-symbol chips and reading them in pairs.

    Two things make this fiddly on a real capture, and both are handled here:

    * **Alignment.** A segment does not begin on a bit boundary -- the tail of a
      sync pulse merges into the first chip of the body, since the air cannot
      represent two adjacent same-level pulses separately. So try a few leading
      offsets and keep whichever yields the most bits.
    * **Where the body ends.** Decoding stops at the first chip pair with no
      transition rather than rejecting the whole repeat, so trailing sync or
      padding does not throw away a good body.

    Both polarity conventions exist in the wild; this returns the G.E. Thomas
    mapping (10 -> 1) and notes that the complement is IEEE 802.3.
    """
    symbol = dominant_class(classes)
    if symbol is None:
        return None
    half = symbol.centre

    chips = _to_chips(repeat, half)
    if chips is None or len(chips) < 2 * MANCHESTER_MIN_BITS:
        return None

    best_bits: list[str] = []
    best_skip = 0
    for skip in range(MANCHESTER_MAX_SKIP + 1):
        bits: list[str] = []
        i = skip
        while i + 1 < len(chips):
            a, b = chips[i], chips[i + 1]
            if a == b:
                break  # no mid-bit transition: the body ended here
            bits.append("1" if (a == 1 and b == 0) else "0")
            i += 2
        if len(bits) > len(best_bits):
            best_bits = bits
            best_skip = skip

    if len(best_bits) < MANCHESTER_MIN_BITS:
        return None

    note = "G.E. Thomas polarity; complement it for IEEE 802.3"
    if best_skip:
        note += f"; {best_skip} leading half-chip(s) skipped"
    consumed = best_skip + 2 * len(best_bits)
    if consumed < len(chips):
        note += f"; {len(chips) - consumed} trailing half-chip(s) ignored"
    return Decode(scheme="manchester", bits="".join(best_bits), note=note)


def symbol_string(repeat: Repeat, classes: Sequence[WidthClass]) -> str:
    """Always-available fallback: the repeat as width-class letters."""
    letters = "abcdefghijklmnop"
    return "".join(letters[classify(classes, d)] for d in repeat.durations)


def decode_repeat(repeat: Repeat, classes: Sequence[WidthClass]) -> list[Decode]:
    """Every decoding that fits, best guess first."""
    out = []
    for decoder in (decode_pwm, decode_manchester):
        d = decoder(repeat, classes)
        if d is not None:
            out.append(d)
    return out


# --------------------------------------------------------------------------- #
# Protocol fingerprinting
# --------------------------------------------------------------------------- #


def fingerprint(
    frame: Frame, classes: Sequence[WidthClass], repeats: Sequence[Repeat]
) -> list[str]:
    """Timing-based guesses at the protocol family. Guesses, not conclusions."""
    hints: list[str] = []
    symbol = dominant_class(classes)
    if symbol is None:
        return hints

    widths = sorted(c.centre for c in classes)
    long_gaps = [c.centre for c in classes if c.centre > symbol.centre * 8]

    primary = max(repeats, key=lambda r: len(r.durations)) if repeats else None
    pwm = decode_pwm(primary, classes) if primary else None

    # EV1527 / PT2262: two data widths ~1:3, a sync gap around 31x the short
    # pulse, and 24 data bits (20 address + 4 data).
    if pwm and 22 <= pwm.nbits <= 26 and long_gaps:
        ratio = long_gaps[0] / symbol.centre
        if 20 <= ratio <= 45:
            hints.append(
                f"EV1527 / PT2262 family: {pwm.nbits} bits, sync gap "
                f"{ratio:.0f}x the short pulse. FIXED code -- replay should work."
            )

    # Somfy RTS: 433.42 MHz, 2-7 hardware sync pulses near 2560 us, a 4550 us
    # software sync, then 56 Manchester bits at a 640 us half-period.
    near_640 = any(560 <= w <= 720 for w in widths)
    near_1280 = any(1150 <= w <= 1450 for w in widths)
    near_2560 = any(2300 <= w <= 2800 for w in widths)
    if near_640 and near_1280 and near_2560:
        hints.append(
            "Somfy RTS shape: 2560 us hardware sync plus a 640/1280 us Manchester "
            "body. ROLLING code -- replay will NOT work, and it lives on "
            "433.42 MHz, not 433.92."
        )
    elif near_2560 and near_640:
        hints.append(
            "Possible Somfy RTS relative: check the carrier with `scan`, and "
            "expect a rolling code."
        )

    if len(classes) > 5:
        hints.append(
            f"{len(classes)} distinct width classes is a lot -- likely a noisy "
            "capture. Move closer, or raise the OOK decision boundary "
            "(AGCCTRL0 -> 0x92)."
        )

    return hints


# --------------------------------------------------------------------------- #
# Analysis
# --------------------------------------------------------------------------- #


@dataclass
class FrameReport:
    frame: Frame
    classes: list[WidthClass]
    repeats: list[Repeat]
    sync_threshold: float
    decodes: list[Decode]
    repeats_identical: bool
    lengths_vary: bool
    primary: Repeat | None
    hints: list[str] = field(default_factory=list)

    @property
    def code(self) -> str:
        """Canonical identity of this press, for cross-press comparison."""
        if self.decodes:
            return f"{self.decodes[0].scheme}:{self.decodes[0].bits}"
        if self.primary:
            return "sym:" + symbol_string(self.primary, self.classes)
        return ""


def analyse_frame(frame: Frame, tolerance: float = DEFAULT_TOLERANCE) -> FrameReport:
    classes = cluster_widths(frame.durations, tolerance)
    repeats, threshold = split_repeats(frame, classes)

    # Compare repeats over their common prefix. Segment boundaries land wherever
    # the long gap is, so the first and last repeat in a capture routinely carry
    # one pulse more or less than the others -- that is a framing artefact, not a
    # difference in the code, and flagging it as one would cry wolf on every
    # perfectly good capture.
    identical = True
    lengths_vary = False
    if len(repeats) > 1:
        seqs = [[classify(classes, d) for d in r.durations] for r in repeats]
        n = min(len(s) for s in seqs)
        lengths_vary = len({len(s) for s in seqs}) > 1
        identical = all(s[:n] == seqs[0][:n] for s in seqs)

    # Decode the longest repeat: a capture can start mid-burst, so the first
    # segment is often a fragment.
    primary = max(repeats, key=lambda r: len(r.durations)) if repeats else None
    decodes = decode_repeat(primary, classes) if primary else []

    return FrameReport(
        frame=frame,
        classes=classes,
        repeats=repeats,
        sync_threshold=threshold,
        decodes=decodes,
        repeats_identical=identical,
        lengths_vary=lengths_vary,
        primary=primary,
        hints=fingerprint(frame, classes, repeats),
    )


def hamming(a: str, b: str) -> int | None:
    if len(a) != len(b):
        return None
    return sum(1 for x, y in zip(a, b) if x != y)


def compare_presses(reports: Sequence[FrameReport]) -> dict:
    """Group frames by button and decide fixed vs rolling."""
    by_button: dict[str, list[FrameReport]] = {}
    for r in reports:
        by_button.setdefault(r.frame.button, []).append(r)

    verdicts = {}
    for button, group in sorted(by_button.items()):
        codes = [r.code for r in group if r.code]
        unique = sorted(set(codes))
        verdict = "inconclusive"
        detail = ""

        if not codes:
            detail = "nothing decoded"
        elif len(group) < 2:
            verdict = "need more presses"
            detail = "capture the same button at least three times"
        elif len(unique) == 1:
            verdict = "FIXED"
            detail = f"all {len(codes)} presses produced the same code"
        else:
            verdict = "ROLLING or noisy"
            bodies = [c.split(":", 1)[1] for c in unique]
            distances = [
                d
                for i in range(len(bodies))
                for j in range(i + 1, len(bodies))
                if (d := hamming(bodies[i], bodies[j])) is not None
            ]
            detail = f"{len(unique)} distinct codes across {len(codes)} presses"
            if distances:
                detail += f"; Hamming distance {min(distances)}-{max(distances)} bits"

        verdicts[button] = {
            "verdict": verdict,
            "detail": detail,
            "presses": len(group),
            "unique_codes": unique,
        }
    return verdicts


# --------------------------------------------------------------------------- #
# Loading
# --------------------------------------------------------------------------- #


def load_frames(paths: Iterable[str]) -> list[Frame]:
    frames: list[Frame] = []
    for pattern in paths:
        matches = sorted(glob.glob(pattern)) or [pattern]
        for path in matches:
            p = Path(path)
            if not p.exists():
                print(f"warning: {path} not found", file=sys.stderr)
                continue
            for lineno, line in enumerate(p.read_text().splitlines(), 1):
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError as exc:
                    print(f"warning: {p}:{lineno}: {exc}", file=sys.stderr)
                    continue
                if "durations" not in obj:
                    continue
                frame = Frame.from_json(obj)
                if not frame.source:
                    frame.source = p.name
                frames.append(frame)
    return frames


# --------------------------------------------------------------------------- #
# Reporting
# --------------------------------------------------------------------------- #


def print_frame_report(r: FrameReport, verbose: bool = False) -> None:
    f = r.frame
    rssi = f"{f.rssi:.1f} dBm" if f.rssi is not None else "n/a"
    print(
        f"  frame {f.seq} [{f.button}] {len(f.durations)} pulses, "
        f"{f.total_us / 1000:.1f} ms, rssi {rssi}"
        f"{'  TRUNCATED' if f.truncated else ''}"
    )

    widths = "  ".join(f"{c.centre:.0f}us x{c.count}" for c in r.classes)
    print(f"    widths:  {widths}")

    if r.repeats:
        sizes = sorted({len(x.durations) for x in r.repeats})
        print(
            f"    repeats: {len(r.repeats)} "
            f"({'identical' if r.repeats_identical else 'NOT identical'}), "
            f"{sizes[0] if len(sizes) == 1 else f'{sizes[0]}-{sizes[-1]}'} pulses"
            f"{' (lengths vary at the segment seams)' if r.lengths_vary else ''}, "
            f"split at >= {r.sync_threshold:.0f}us"
        )
    else:
        print("    repeats: none found -- frame too short or all one width")

    for d in r.decodes:
        line = f"    {d.scheme:11s} {d.nbits:3d} bits  {d.hex}  {d.bits}"
        print(line)
        if d.note:
            print(f"                 ({d.note})")

    if not r.decodes and r.primary:
        print(f"    symbols:  {symbol_string(r.primary, r.classes)}")

    for hint in r.hints:
        print(f"    hint: {hint}")

    if verbose and r.repeats:
        for i, rep in enumerate(r.repeats):
            print(f"    repeat {i}: {rep.durations}")


def build_report(
    frames: Sequence[Frame], tolerance: float
) -> tuple[dict, list[FrameReport]]:
    reports = [analyse_frame(f, tolerance) for f in frames]
    return {
        "frames": [
            {
                "seq": r.frame.seq,
                "button": r.frame.button,
                "source": r.frame.source,
                "pulses": len(r.frame.durations),
                "total_us": r.frame.total_us,
                "rssi": r.frame.rssi,
                "truncated": r.frame.truncated,
                "widths": [
                    {"centre_us": round(c.centre, 1), "count": c.count}
                    for c in r.classes
                ],
                "repeats": len(r.repeats),
                "repeats_identical": r.repeats_identical,
                "decodes": [
                    {
                        "scheme": d.scheme,
                        "bits": d.bits,
                        "nbits": d.nbits,
                        "hex": d.hex,
                        "note": d.note,
                    }
                    for d in r.decodes
                ],
                "code": r.code,
                "hints": r.hints,
            }
            for r in reports
        ],
        "buttons": compare_presses(reports),
    }, reports


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Decode pergola remote captures and tell fixed codes from "
        "rolling ones.",
    )
    ap.add_argument("paths", nargs="+", help="JSONL capture files (globs allowed)")
    ap.add_argument(
        "--tolerance",
        type=float,
        default=DEFAULT_TOLERANCE,
        help=f"width clustering tolerance (default {DEFAULT_TOLERANCE})",
    )
    ap.add_argument("--frame", type=int, help="only analyse this frame seq")
    ap.add_argument("--button", help="only analyse frames labelled with this button")
    ap.add_argument("--verbose", action="store_true", help="dump raw repeat timings")
    ap.add_argument("--json", metavar="FILE", help="also write the report as JSON")
    args = ap.parse_args(argv)

    frames = load_frames(args.paths)
    if args.frame is not None:
        frames = [f for f in frames if f.seq == args.frame]
    if args.button:
        frames = [f for f in frames if f.button == args.button]

    if not frames:
        print("no frames loaded", file=sys.stderr)
        return 1

    report, reports = build_report(frames, args.tolerance)

    print(f"{len(frames)} frame(s) from {len({f.source for f in frames})} file(s)\n")

    by_button: dict[str, list[FrameReport]] = {}
    for r in reports:
        by_button.setdefault(r.frame.button, []).append(r)

    for button, group in sorted(by_button.items()):
        print(f"button: {button}  ({len(group)} press(es))")
        for r in group:
            print_frame_report(r, args.verbose)
        print()

    print("verdict")
    print("-------")
    for button, v in report["buttons"].items():
        print(f"  {button:10s} {v['verdict']:18s} {v['detail']}")
        if len(v["unique_codes"]) > 1:
            for c in v["unique_codes"]:
                print(f"             {c}")

    any_rolling = any(
        v["verdict"].startswith("ROLLING") for v in report["buttons"].values()
    )
    all_fixed = report["buttons"] and all(
        v["verdict"] == "FIXED" for v in report["buttons"].values()
    )

    print()
    if all_fixed:
        print(
            "All buttons produced stable codes. Store one frame per button with\n"
            "`keep <slot>` and replay it with `tx <slot> 4` to confirm the roof\n"
            "actually moves, then write the codes into docs/remote-protocol.md."
        )
    elif any_rolling:
        print(
            "At least one button changed code between presses. Before concluding\n"
            "it is a rolling code, rule out a noisy capture: check RSSI, capture\n"
            "again closer to the remote, and confirm the repeats within a single\n"
            "press are identical. If they are identical within a press but differ\n"
            "between presses, it is genuinely rolling -- see\n"
            "docs/remote-protocol.md for what that means for the project."
        )
    else:
        print("Capture the same button at least three times to reach a verdict.")

    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2))
        print(f"\nwrote {args.json}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
