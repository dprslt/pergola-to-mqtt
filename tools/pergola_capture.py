#!/usr/bin/env python3
"""Record labelled RF captures from the sniffer firmware.

Reads the sniffer's serial output, turns every `F,...` line into one JSON object
per line, and writes it to captures/. The label matters: the whole point is to
compare presses of the same button against each other and against other buttons,
which is only possible if each capture says which button made it.

Typical session -- three presses of each button, one file per button:

    python3 pergola_capture.py --button open   --count 3
    python3 pergola_capture.py --button stop   --count 3
    python3 pergola_capture.py --button close  --count 3
    python3 pergola_analyze.py captures/*.jsonl

Finding the carrier when nothing is received at all:

    python3 pergola_capture.py --scan

Talking to the firmware directly (it has a serial CLI -- `?` for help):

    python3 pergola_capture.py --send "freq 433.42" --button open --count 3
    python3 pergola_capture.py --send "?" --seconds 2
"""

from __future__ import annotations

import argparse
import datetime
import json
import sys
import time
from pathlib import Path

DEFAULT_BAUD = 115200
CAPTURE_DIR = Path(__file__).parent / "captures"

# Substrings that identify a USB serial adapter, most specific first. ESP32 boards
# use CP210x, CH340 or a native USB CDC depending on the vendor.
PORT_HINTS = ("usbserial", "usbmodem", "wchusbserial", "ttyUSB", "ttyACM", "SLAB")


def _serial():
    """Import pyserial on demand.

    Deferred so the line parsers below stay importable -- and testable -- on a
    machine that has never seen an ESP32.
    """
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:  # pragma: no cover
        print("pyserial is required:  pip install -r requirements.txt", file=sys.stderr)
        raise SystemExit(2)
    return serial, list_ports


def find_port() -> str | None:
    _, list_ports = _serial()
    ports = list(list_ports.comports())
    for hint in PORT_HINTS:
        for p in ports:
            if hint.lower() in p.device.lower():
                return p.device
    return ports[0].device if ports else None


def list_available_ports() -> None:
    _, list_ports = _serial()
    ports = list(list_ports.comports())
    if not ports:
        print("no serial ports found", file=sys.stderr)
        return
    print("available serial ports:", file=sys.stderr)
    for p in ports:
        print(f"  {p.device}  {p.description}", file=sys.stderr)


# --------------------------------------------------------------------------- #
# Line parsing -- mirrors the grammar documented at the top of
# firmware/sniffer/src/main.cpp
# --------------------------------------------------------------------------- #


def parse_frame_line(line: str) -> dict | None:
    """F,seq,t_ms,rssi,first_level,truncated,count,d0,d1,..."""
    if not line.startswith("F,"):
        return None
    parts = line.split(",")
    if len(parts) < 8:
        return None
    try:
        seq = int(parts[1])
        t_ms = int(parts[2])
        rssi = float(parts[3])
        first_level = int(parts[4])
        truncated = bool(int(parts[5]))
        count = int(parts[6])
        durations = [int(x) for x in parts[7:]]
    except ValueError:
        return None

    if count != len(durations):
        print(
            f"warning: frame {seq} says {count} pulses but carries "
            f"{len(durations)}; keeping what arrived",
            file=sys.stderr,
        )
    return {
        "seq": seq,
        "t_ms": t_ms,
        "rssi": rssi,
        "first_level": first_level,
        "truncated": truncated,
        "count": len(durations),
        "durations": durations,
    }


def parse_scan_line(line: str) -> tuple[float, float] | None:
    """S,mhz,rssi"""
    if not line.startswith("S,"):
        return None
    parts = line.split(",")
    if len(parts) != 3:
        return None
    try:
        return float(parts[1]), float(parts[2])
    except ValueError:
        return None


# --------------------------------------------------------------------------- #
# Modes
# --------------------------------------------------------------------------- #


def quick_summary(frame: dict) -> str:
    """One line, without pulling in the full analyser."""
    d = frame["durations"]
    if not d:
        return "empty"
    total = sum(d) / 1000.0
    return (
        f"{len(d)} pulses, {total:.1f} ms, "
        f"{min(d)}-{max(d)} us, rssi {frame['rssi']:.1f} dBm"
        f"{'  TRUNCATED' if frame['truncated'] else ''}"
    )


def do_capture(args: argparse.Namespace, ser: serial.Serial) -> int:
    CAPTURE_DIR.mkdir(exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_path = CAPTURE_DIR / f"{args.button}-{stamp}.jsonl"

    target = f"{args.count} frame(s)" if args.count else "frames"
    limit = f", stopping after {args.seconds}s" if args.seconds else ""
    print(f"recording {target} for button '{args.button}'{limit}")
    print(f"  -> {out_path}")
    print("  press the remote button now; Ctrl-C to stop\n")

    started = time.time()
    written = 0
    fh = out_path.open("w")
    fh.write(f"# button={args.button} port={ser.port} baud={ser.baudrate}\n")
    fh.write(f"# started={datetime.datetime.now().isoformat(timespec='seconds')}\n")

    try:
        while True:
            if args.count and written >= args.count:
                break
            if args.seconds and time.time() - started > args.seconds:
                print("\ntime limit reached")
                break

            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if not line:
                continue

            if line.startswith("#"):
                if not args.quiet:
                    print(f"  {line}")
                continue

            frame = parse_frame_line(line)
            if frame is None:
                if args.verbose:
                    print(f"  ? {line[:100]}")
                continue

            frame.update(
                {
                    "button": args.button,
                    "captured_at": datetime.datetime.now(
                        datetime.timezone.utc
                    ).isoformat(timespec="seconds"),
                    "source": out_path.name,
                    "port": ser.port,
                }
            )
            fh.write(json.dumps(frame) + "\n")
            fh.flush()
            written += 1
            print(f"  [{written}] frame {frame['seq']}: {quick_summary(frame)}")

    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        fh.close()

    if written == 0:
        out_path.unlink(missing_ok=True)
        print(
            "\nNo frames captured, and nothing written.\n"
            "  * Is the remote on 433.92 MHz? Run: pergola_capture.py --scan\n"
            "  * Does the chip answer at all? Run: pergola_capture.py "
            '--send "status" --seconds 2\n'
            "  * See docs/setup-checklist.md.",
            file=sys.stderr,
        )
        return 1

    print(f"\nwrote {written} frame(s) to {out_path}")
    print(f"next: python3 pergola_analyze.py {out_path}")
    return 0


def do_scan(args: argparse.Namespace, ser: serial.Serial) -> int:
    lo, hi = args.range
    cmd = f"scan {lo} {hi} {args.step} {args.dwell}"
    print(f"sending: {cmd}")
    print("hold the remote button down for the whole sweep\n")
    ser.write((cmd + "\n").encode())

    rows: list[tuple[float, float]] = []
    deadline = time.time() + args.timeout
    try:
        while time.time() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            if line.startswith("#"):
                print(f"  {line}")
                if "scan done" in line or "scan aborted" in line:
                    break
                continue
            row = parse_scan_line(line)
            if row:
                rows.append(row)
    except KeyboardInterrupt:
        print("\nstopped")

    if not rows:
        print("no scan rows received", file=sys.stderr)
        return 1

    floor = min(r[1] for r in rows)
    peak = max(r[1] for r in rows)
    span = max(peak - floor, 1.0)

    print(f"\n{'MHz':>9}  {'dBm':>7}  (floor {floor:.1f}, peak {peak:.1f})")
    for mhz, rssi in rows:
        bar = "#" * int(40 * (rssi - floor) / span)
        marker = "  <-- peak" if rssi == peak else ""
        print(f"{mhz:9.3f}  {rssi:7.1f}  {bar}{marker}")

    best = [r for r in rows if r[1] == peak]
    print(
        f"\nStrongest at {best[0][0]:.3f} MHz ({peak:.1f} dBm), "
        f"{peak - floor:.1f} dB above the floor."
    )
    if peak - floor < 10:
        print(
            "That is not a convincing peak. Either the remote was not "
            "transmitting during the sweep, or it is outside the swept range."
        )
    else:
        print(f"Set it with:  pergola_capture.py --send 'freq {best[0][0]:.2f}'")

    CAPTURE_DIR.mkdir(exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    csv_path = CAPTURE_DIR / f"scan-{stamp}.csv"
    csv_path.write_text(
        "mhz,rssi_dbm\n" + "".join(f"{m:.3f},{r:.1f}\n" for m, r in rows)
    )
    print(f"wrote {csv_path}")
    return 0


def do_send_only(args: argparse.Namespace, ser: serial.Serial) -> int:
    print(f"sending: {args.send}")
    ser.write((args.send + "\n").encode())
    deadline = time.time() + (args.seconds or 3)
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        print(raw.decode("utf-8", errors="replace").rstrip())
    return 0


# --------------------------------------------------------------------------- #


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Record labelled RF captures from the pergola sniffer.",
        epilog="Capture each button at least three times: one press cannot "
        "distinguish a fixed code from a rolling one.",
    )
    ap.add_argument("--port", help="serial port (auto-detected if omitted)")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--list-ports", action="store_true", help="list ports and exit")

    ap.add_argument(
        "--button",
        default="unlabelled",
        help="label for this capture: open, stop, close, ...",
    )
    ap.add_argument("--count", type=int, default=0, help="stop after N frames")
    ap.add_argument("--seconds", type=float, default=0, help="stop after N seconds")

    ap.add_argument("--send", metavar="CMD", help="send a firmware CLI command first")
    ap.add_argument(
        "--scan", action="store_true", help="sweep the band for the carrier"
    )
    ap.add_argument(
        "--range",
        nargs=2,
        type=float,
        default=[433.0, 434.8],
        metavar=("LO", "HI"),
        help="scan range in MHz (default 433.0 434.8)",
    )
    ap.add_argument("--step", type=float, default=50, help="scan step in kHz")
    ap.add_argument("--dwell", type=int, default=40, help="scan dwell in ms")
    ap.add_argument(
        "--timeout", type=float, default=180, help="scan timeout in seconds"
    )

    ap.add_argument("--quiet", action="store_true", help="hide firmware comments")
    ap.add_argument("--verbose", action="store_true", help="show unparsed lines")
    args = ap.parse_args(argv)

    if args.list_ports:
        list_available_ports()
        return 0

    serial, _ = _serial()

    port = args.port or find_port()
    if not port:
        print("no serial port found", file=sys.stderr)
        list_available_ports()
        return 2

    try:
        ser = serial.Serial(port, args.baud, timeout=0.5)
    except serial.SerialException as exc:
        print(f"cannot open {port}: {exc}", file=sys.stderr)
        list_available_ports()
        return 2

    with ser:
        # The ESP32 resets when the port opens; give it time to print its banner.
        time.sleep(0.4)
        ser.reset_input_buffer()

        if args.scan:
            return do_scan(args, ser)

        if args.send and args.button == "unlabelled" and not args.count:
            return do_send_only(args, ser)

        if args.send:
            print(f"sending: {args.send}")
            ser.write((args.send + "\n").encode())
            time.sleep(0.5)

        return do_capture(args, ser)


if __name__ == "__main__":
    sys.exit(main())
