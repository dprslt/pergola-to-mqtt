#!/usr/bin/env python3
"""Regenerate the synthetic example captures in captures/.

They exist so pergola_analyze.py can be tried, and its output understood, before
any hardware is wired up -- and so there is a known-good input to compare against
when a real capture looks wrong.

    python3 make_examples.py
"""

from __future__ import annotations

import json
from pathlib import Path

from test_analyze import ev1527, somfy_rts

HERE = Path(__file__).parent
OUT = HERE / "captures"


def dump(name: str, frames: list[tuple[str, object]]) -> Path:
    path = OUT / name
    with path.open("w") as fh:
        fh.write("# synthesised example capture -- no hardware involved.\n")
        fh.write("# regenerate with tools/make_examples.py\n")
        for i, (button, frame) in enumerate(frames, 1):
            fh.write(
                json.dumps(
                    {
                        "seq": i,
                        "t_ms": i * 4000,
                        "rssi": -57.5,
                        "first_level": frame.first_level,
                        "truncated": False,
                        "count": len(frame.durations),
                        "durations": frame.durations,
                        "button": button,
                        "captured_at": "2026-08-17T22:00:00Z",
                        "source": name,
                        "firmware": "synthetic",
                    }
                )
                + "\n"
            )
    return path


def main() -> None:
    OUT.mkdir(exist_ok=True)

    # A fixed-code remote: three buttons, three presses each, codes differing
    # only in the low nibble -- which is what a real EV1527 keyfob looks like.
    fixed: list[tuple[str, object]] = []
    for button, code in (("open", 0xA53C61), ("stop", 0xA53C62), ("close", 0xA53C64)):
        for seed in range(3):
            fixed.append((button, ev1527(code, seed=seed)))
    print("wrote", dump("example-fixed-code.jsonl", fixed))

    # A rolling-code remote: same button, a counter that steps every press.
    rolling: list[tuple[str, object]] = []
    for i, code in enumerate((0x0123456789AB01, 0x0123456789AB02, 0x0123456789AB03)):
        rolling.append(("open", somfy_rts(code, seed=10 + i)))
    print("wrote", dump("example-rolling-code.jsonl", rolling))


if __name__ == "__main__":
    main()
