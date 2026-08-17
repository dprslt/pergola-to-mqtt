#!/usr/bin/env python3
"""Tests for pergola_analyze, driven by synthesised pulse trains.

No hardware needed. Frames are generated for the two protocol families the
pergola remote is most likely to belong to -- a fixed-code EV1527 relative and a
rolling-code Somfy RTS relative -- with jitter added, because a real capture
never has exact timings.

    python3 -m pytest test_analyze.py -q
    python3 test_analyze.py            (runs without pytest too)
"""

from __future__ import annotations

import random

from pergola_analyze import (
    Frame,
    analyse_frame,
    cluster_widths,
    compare_presses,
    decode_manchester,
    decode_pwm,
    split_repeats,
)

# Async serial mode discretises the data line into 8 samples per bit, so every
# edge lands within +/-1/8 of a bit period. At 4.8 kBaud that is +/-26 us.
JITTER_US = 20


def jitter(value: int, rng: random.Random, amount: int = JITTER_US) -> int:
    return max(1, value + rng.randint(-amount, amount))


# --------------------------------------------------------------------------- #
# Synthesisers
# --------------------------------------------------------------------------- #


def ev1527(code: int, nbits: int = 24, repeats: int = 6, seed: int = 1) -> Frame:
    """Fixed-code remote: one short + one long pulse per bit, long sync gap.

    short-high then long-low  -> 0
    long-high  then short-low -> 1
    Sync is a short high pulse followed by a 31x low gap.
    """
    rng = random.Random(seed)
    short, long = 320, 960
    durations: list[int] = []
    for _ in range(repeats):
        durations.append(jitter(short, rng))
        durations.append(jitter(short * 31, rng))
        for i in range(nbits - 1, -1, -1):
            if (code >> i) & 1:
                durations.append(jitter(long, rng))
                durations.append(jitter(short, rng))
            else:
                durations.append(jitter(short, rng))
                durations.append(jitter(long, rng))
    return Frame(seq=1, durations=durations, first_level=1)


def somfy_rts(code: int, nbits: int = 56, repeats: int = 3, seed: int = 2) -> Frame:
    """Rolling-code remote: hardware sync bursts, then a Manchester body.

    Shape follows Somfy RTS: 2560 us hardware sync pairs, a 4550 us software
    sync, then `nbits` Manchester bits at a 640 us half-period.
    """
    rng = random.Random(seed)
    half = 640

    # Emit (level, duration) and merge with the previous pulse when the level
    # matches: the air cannot represent two adjacent same-level pulses
    # separately, and a synthesiser that pretends otherwise produces a frame no
    # receiver could ever see.
    pulses: list[list[int]] = []

    def emit(level: int, us: int) -> None:
        if pulses and pulses[-1][0] == level:
            pulses[-1][1] += us
        else:
            pulses.append([level, us])

    for r in range(repeats):
        for _ in range(2 if r == 0 else 7):
            emit(1, 2560)
            emit(0, 2560)
        emit(1, 4550)  # software sync
        emit(0, half)

        # Manchester, G.E. Thomas: 1 -> high then low, 0 -> low then high.
        for i in range(nbits - 1, -1, -1):
            if (code >> i) & 1:
                emit(1, half)
                emit(0, half)
            else:
                emit(0, half)
                emit(1, half)

        if r + 1 < repeats:
            emit(0, 30415)  # inter-frame gap

    return Frame(
        seq=1,
        durations=[jitter(us, rng) for _, us in pulses],
        first_level=pulses[0][0],
    )


# --------------------------------------------------------------------------- #
# Clustering
# --------------------------------------------------------------------------- #


def test_clustering_separates_short_and_long():
    rng = random.Random(7)
    widths = [jitter(320, rng) for _ in range(40)] + [
        jitter(960, rng) for _ in range(40)
    ]
    classes = cluster_widths(widths)
    assert len(classes) == 2, [c.centre for c in classes]
    assert abs(classes[0].centre - 320) < 30
    assert abs(classes[1].centre - 960) < 30


def test_clustering_does_not_chain_a_ramp():
    # A gradual ramp must not merge into one giant class; the span cap stops it.
    classes = cluster_widths([100, 120, 145, 175, 210, 255, 305, 370, 450])
    assert len(classes) > 1


# --------------------------------------------------------------------------- #
# Fixed-code path
# --------------------------------------------------------------------------- #


def test_ev1527_decodes_to_the_original_code():
    code = 0xA53C69
    frame = ev1527(code)
    report = analyse_frame(frame)

    assert report.repeats_identical, "repeats of one press must match"
    assert len(report.repeats) == 6, len(report.repeats)

    pwm = [d for d in report.decodes if d.scheme == "pwm"]
    assert pwm, f"no PWM decode; got {[d.scheme for d in report.decodes]}"
    assert pwm[0].nbits == 24, pwm[0].nbits
    assert int(pwm[0].bits, 2) == code, pwm[0].hex


def test_ev1527_is_fingerprinted_as_fixed():
    report = analyse_frame(ev1527(0x123456))
    joined = " ".join(report.hints)
    assert "EV1527" in joined, report.hints
    assert "FIXED" in joined


def test_repeated_presses_of_a_fixed_code_read_as_FIXED():
    reports = []
    for seed in range(3):
        frame = ev1527(0xA53C69, seed=seed)
        frame.button = "open"
        reports.append(analyse_frame(frame))

    verdict = compare_presses(reports)["open"]
    assert verdict["verdict"] == "FIXED", verdict
    assert len(verdict["unique_codes"]) == 1


def test_different_buttons_give_different_codes():
    reports = []
    for button, code in (("open", 0xA53C61), ("stop", 0xA53C62), ("close", 0xA53C64)):
        for seed in range(3):
            frame = ev1527(code, seed=seed)
            frame.button = button
            reports.append(analyse_frame(frame))

    verdicts = compare_presses(reports)
    assert all(v["verdict"] == "FIXED" for v in verdicts.values()), verdicts
    codes = {b: v["unique_codes"][0] for b, v in verdicts.items()}
    assert len(set(codes.values())) == 3, codes


# --------------------------------------------------------------------------- #
# Rolling-code path
# --------------------------------------------------------------------------- #


def test_somfy_body_decodes_as_manchester():
    frame = somfy_rts(0x0123456789ABCD)
    classes = cluster_widths(frame.durations)
    repeats, _ = split_repeats(frame, classes)
    assert repeats

    primary = max(repeats, key=lambda r: len(r.durations))
    decoded = decode_manchester(primary, classes)
    assert decoded is not None, "Manchester decode failed on a Manchester body"
    assert decoded.nbits >= 50, decoded.nbits


def test_somfy_is_not_mistaken_for_pwm():
    frame = somfy_rts(0x0123456789ABCD)
    classes = cluster_widths(frame.durations)
    repeats, _ = split_repeats(frame, classes)
    primary = max(repeats, key=lambda r: len(r.durations))
    assert decode_pwm(primary, classes) is None


def test_somfy_shape_is_fingerprinted_as_rolling():
    report = analyse_frame(somfy_rts(0x0123456789ABCD))
    joined = " ".join(report.hints)
    assert "Somfy" in joined, report.hints
    assert "ROLLING" in joined or "rolling" in joined


def test_changing_codes_read_as_ROLLING():
    reports = []
    for i, code in enumerate((0x0123456789AB01, 0x0123456789AB02, 0x0123456789AB03)):
        frame = somfy_rts(code, seed=10 + i)
        frame.button = "open"
        reports.append(analyse_frame(frame))

    verdict = compare_presses(reports)["open"]
    assert verdict["verdict"].startswith("ROLLING"), verdict
    assert len(verdict["unique_codes"]) == 3


# --------------------------------------------------------------------------- #
# Robustness
# --------------------------------------------------------------------------- #


def test_single_press_is_inconclusive_not_wrong():
    frame = ev1527(0xABCDEF)
    frame.button = "open"
    verdict = compare_presses([analyse_frame(frame)])["open"]
    assert verdict["verdict"] == "need more presses", verdict


def test_pure_noise_does_not_produce_a_confident_code():
    rng = random.Random(99)
    frame = Frame(
        seq=1, durations=[rng.randint(60, 3000) for _ in range(200)], first_level=1
    )
    report = analyse_frame(frame)
    pwm = [d for d in report.decodes if d.scheme == "pwm"]
    # Random widths must not pair up into a clean two-symbol code.
    assert not pwm, pwm[0].bits if pwm else ""


def test_empty_frame_is_handled():
    report = analyse_frame(Frame(seq=1, durations=[], first_level=1))
    assert report.repeats == []
    assert report.decodes == []
    assert report.code == ""


def test_capture_starting_on_a_low_pulse_still_decodes():
    # A capture can begin mid-air, on either level.
    frame = ev1527(0xA53C69)
    frame.durations = frame.durations[1:]
    frame.first_level = 0
    report = analyse_frame(frame)
    pwm = [d for d in report.decodes if d.scheme == "pwm"]
    assert pwm, "should still decode when the frame starts low"
    assert pwm[0].nbits == 24, pwm[0].nbits


def test_levels_alternate():
    frame = Frame(seq=1, durations=[1, 2, 3, 4], first_level=1)
    assert frame.levels() == [1, 0, 1, 0]
    frame.first_level = 0
    assert frame.levels() == [0, 1, 0, 1]


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_") or not callable(fn):
            continue
        try:
            fn()
            print(f"PASS {name}")
        except AssertionError as exc:
            failures += 1
            print(f"FAIL {name}: {exc}")
        except Exception as exc:  # noqa: BLE001
            failures += 1
            print(f"ERROR {name}: {type(exc).__name__}: {exc}")
    print(f"\n{failures} failure(s)")
    raise SystemExit(1 if failures else 0)
