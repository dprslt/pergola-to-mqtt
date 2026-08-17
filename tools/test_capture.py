#!/usr/bin/env python3
"""Tests for pergola_capture's line parsers.

The serial link is the one interface between firmware and host, so a mismatch
here silently loses captures. These check the grammar documented at the top of
firmware/sniffer/src/main.cpp against the parser that consumes it.

    python3 test_capture.py
"""

from __future__ import annotations

from pergola_capture import parse_frame_line, parse_scan_line, quick_summary


def test_parses_a_well_formed_frame():
    line = "F,7,41523,-58.5,1,0,6,428,1284,431,1290,425,1288"
    frame = parse_frame_line(line)
    assert frame is not None
    assert frame["seq"] == 7
    assert frame["t_ms"] == 41523
    assert frame["rssi"] == -58.5
    assert frame["first_level"] == 1
    assert frame["truncated"] is False
    assert frame["count"] == 6
    assert frame["durations"] == [428, 1284, 431, 1290, 425, 1288]


def test_truncated_flag_is_read():
    frame = parse_frame_line("F,1,100,-60.0,1,1,2,300,900")
    assert frame is not None and frame["truncated"] is True


def test_frame_starting_low_is_preserved():
    frame = parse_frame_line("F,1,100,-60.0,0,0,2,300,900")
    assert frame is not None and frame["first_level"] == 0


def test_count_mismatch_keeps_what_arrived():
    # A dropped serial byte should cost one frame's precision, not the capture.
    frame = parse_frame_line("F,1,100,-60.0,1,0,9,300,900")
    assert frame is not None
    assert frame["count"] == 2
    assert frame["durations"] == [300, 900]


def test_comments_and_scan_rows_are_not_frames():
    assert parse_frame_line("# ready") is None
    assert parse_frame_line("S,433.920,-58.0") is None
    assert parse_frame_line("") is None
    assert parse_frame_line("garbage") is None


def test_truncated_line_is_rejected_not_half_parsed():
    assert parse_frame_line("F,1,100") is None


def test_non_numeric_payload_is_rejected():
    assert parse_frame_line("F,1,100,-60.0,1,0,2,300,nine hundred") is None


def test_parses_scan_rows():
    assert parse_scan_line("S,433.920,-58.0") == (433.920, -58.0)
    assert parse_scan_line("S,433.920") is None
    assert parse_scan_line("F,1,2,3,4,5,6,7") is None


def test_quick_summary_is_readable():
    frame = parse_frame_line("F,1,100,-60.0,1,0,4,300,900,300,900")
    assert frame is not None
    summary = quick_summary(frame)
    assert "4 pulses" in summary
    assert "2.4 ms" in summary
    assert "300-900 us" in summary


def test_quick_summary_survives_an_empty_frame():
    assert quick_summary({"durations": [], "rssi": -60.0, "truncated": False}) == "empty"


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
