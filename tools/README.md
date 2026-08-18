# Host tools

`pergola_capture.py` talks to the sniffer and writes labelled capture files;
`pergola_analyze.py` reads those files and tells you what the remote is doing.
`hwtest_daemon.py` is different from both: it tests the *daemon* against a real
board, and is documented at the bottom of this file.

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
```

The analyser needs no dependencies at all — pure standard library — so captures
can be analysed on any machine, including one that has never seen the hardware.

## Try it without hardware

Two synthetic captures ship with the repo: a fixed-code remote and a
rolling-code one. Run the analyser on them first, so you know what a good result
looks like before you have to judge a real one:

```bash
python3 pergola_analyze.py captures/example-fixed-code.jsonl
python3 pergola_analyze.py captures/example-rolling-code.jsonl
```

The first ends in `FIXED` for all three buttons; the second in
`ROLLING or noisy`. Regenerate them with `python3 make_examples.py`.

## Capturing

The label is the point. The project's central question — does a button produce the
same code every time? — can only be answered by comparing presses of the *same*
button, so every capture records which button made it.

```bash
# three presses of each button, one file each
python3 pergola_capture.py --button open  --count 3
python3 pergola_capture.py --button stop  --count 3
python3 pergola_capture.py --button close --count 3
```

Three presses is the minimum. One press can never distinguish a fixed code from a
rolling one, and two cannot tell a rolling code from a bad capture.

Other flags:

| Flag | Does |
|---|---|
| `--port /dev/tty.usbserial-0001` | pick the port; otherwise auto-detected |
| `--list-ports` | show what is connected and exit |
| `--baud 115200` | must match `monitor_speed` in `platformio.ini` |
| `--seconds 30` | stop after a time limit instead of a frame count |
| `--send "freq 433.42"` | run a firmware CLI command first |
| `--scan` | sweep the band to find the carrier |
| `--verbose` | show lines the parser did not recognise |

### When nothing is received

```bash
python3 pergola_capture.py --send "status" --seconds 2   # is the chip alive?
python3 pergola_capture.py --scan                        # where is the remote?
```

`--scan` sweeps 433.0–434.8 MHz, prints an RSSI bar chart, and names the
strongest frequency. **Hold the remote button down for the whole sweep.** Run it
once with the remote silent first, so you have a noise floor to compare against —
a peak less than 10 dB above the floor means nothing was transmitting.

If the peak lands near 433.42 MHz rather than 433.92, you are most likely looking
at a Somfy RTS-family rolling code. See
[../docs/remote-protocol.md](../docs/remote-protocol.md).

## Analysing

```bash
python3 pergola_analyze.py captures/*.jsonl
python3 pergola_analyze.py captures/open-*.jsonl --verbose   # raw repeat timings
python3 pergola_analyze.py captures/*.jsonl --json report.json
```

For each frame it reports:

- **widths** — clustered pulse widths with populations. A clean capture of a
  simple remote shows two or three tight clusters plus one long sync gap. Six or
  more classes means noise.
- **repeats** — how many times the burst repeated in one press, and whether those
  repeats are identical. Repeats that disagree *within a single press* mean the
  capture is bad, not that the code is rolling.
- **decodes** — bits and hex under each encoding that fits: `pwm` (EV1527 /
  PT2262 family) and `manchester` (Somfy RTS family). Bit polarity is a
  convention; the complement is equally valid.
- **hints** — protocol-family guesses from the timings. Guesses, not conclusions.

Then a per-button **verdict**: `FIXED`, `ROLLING or noisy`, or
`need more presses`.

### Reading the verdict

| Verdict | Meaning | Next |
|---|---|---|
| `FIXED` | Same code every press | Replay it: `keep 0` then `tx 0 4` on the sniffer |
| `ROLLING or noisy` | Codes differ between presses | Rule out noise first, then read [../docs/remote-protocol.md](../docs/remote-protocol.md) |
| `need more presses` | Fewer than two presses | Capture more |
| `inconclusive` | Nothing decoded | Bad capture; check RSSI and the width classes |

To tell a genuine rolling code from a noisy capture: a rolling code gives
**identical repeats within each press** but **different codes between presses**.
Noise gives repeats that disagree with each other inside one press.

## Capture file format

JSON Lines. One object per burst; `#` lines are comments.

```json
{"seq": 7, "t_ms": 41523, "rssi": -58.5, "first_level": 1, "truncated": false,
 "count": 132, "durations": [428, 1284, 431, 1290],
 "button": "open", "captured_at": "2026-08-17T22:11:03+00:00",
 "source": "open-20260817-221103.jsonl", "port": "/dev/tty.usbserial-0001"}
```

`durations` are microseconds and strictly alternate in level, starting at
`first_level` (1 = carrier on). `rssi` is the peak within roughly 250 ms of the
burst, not an instantaneous reading — see the note in
`firmware/sniffer/src/main.cpp`. `truncated` means the burst hit the firmware's
1024-pulse ceiling and continues in the next frame, so its boundaries are not
trustworthy.

Captures are gitignored: they contain the codes that open your pergola.

## Tests

No hardware required. The analyser is tested against synthesised frames — one
fixed-code family, one rolling-code family, both with realistic edge jitter — and
the capture parsers against the firmware's documented output grammar.

```bash
python3 test_analyze.py
python3 test_capture.py
```

Both run standalone with no test framework. They are also plain pytest functions,
so `python3 -m pytest -q` works if you happen to have pytest installed.

`test_analyze.py` is also where the synthesisers live, so it doubles as an
executable specification of what each protocol family looks like on the wire.

## Daemon hardware tests

`hwtest_daemon.py` is the odd one out here. Everything else in `tools/` either
needs no hardware or only needs the sniffer; this one needs a flashed daemon, a
live broker, and the board on USB.

It exists for one reason. The daemon's central safety rule is that an `open` is
always terminated by a `stop`, and the hard part is not the happy path but losing
the CPU part way through. That cannot be unit tested: it needs a real reset at a
real moment.

```bash
python3 hwtest_daemon.py --case owed-stop-reset        # no roof movement
python3 hwtest_daemon.py --case owed-stop-open --yes   # MOVES THE ROOF
python3 hwtest_daemon.py --case watchdog               # selftest build only
python3 hwtest_daemon.py --case watchdog-recovery      # selftest build only
```

Resets are driven through the serial adapter's RTS line rather than by pulling
power. The board reports `reset reason 1` for those, the same `ESP_RST_POWERON`
class a brownout produces, so RAM is cleared exactly as a supply failure would
clear it. What it cannot reproduce is a sagging rail catching a flash write half
done; NVS's own atomicity covers that, not our code.

`owed-stop-open` is the only case that moves anything, so it refuses to run
without `--yes`. It leaves the roof closed and the position estimate re-anchored.

The two watchdog cases need a build that can be told to wedge `loop()`:

```bash
.venv/bin/pio run -d firmware/daemon -e esp32dev-selftest -t upload
cd tools && python3 hwtest_daemon.py --case watchdog
cd tools && python3 hwtest_daemon.py --case watchdog-recovery
.venv/bin/pio run -d firmware/daemon -e esp32dev-ota -t upload   # put it back
```

That build announces itself at every boot and its hang code is behind
`-DPERGOLA_WDT_SELFTEST`, so it cannot exist in a normal image. Check with
`strings .pio/build/esp32dev/firmware.elf | grep -c selftest`, which must be `0`.
