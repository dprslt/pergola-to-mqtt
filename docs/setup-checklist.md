# Setup checklist

Work through this in order. Each stage has a test that either passes or fails
unambiguously, and later stages are meaningless until the earlier ones pass.

The reason to be strict about the order: **a swapped MISO wire, a remote on the
wrong frequency, and an AGC threshold set too high all look identical** — nothing
appears in the serial monitor. Guessing between them wastes an evening. Each stage
below eliminates exactly one cause.

---

## Stage 0 — before applying power

Every mistake from here on is recoverable except one: swapping VCC and GND kills
the module. It costs 20 seconds to rule out.

- [ ] ESP32 **unplugged** from USB.
- [ ] Run the continuity test in
      [hardware.md](hardware.md#1-continuity--before-applying-power) — probing each
      candidate pin against the SMA connector shell identifies GND positively,
      which confirms the whole header numbering at once.
- [ ] Confirm the supply wire lands on the ESP32's `3V3` pin — not `5V`, not `VIN`.
      The CC1101 is a 1.8–3.6 V part with no regulator on the module.
- [ ] **Check the loom colours against
      [the pinout](hardware.md#pinout), not against convention.** On this build
      brown is VCC and red is GDO0. Connecting red to `3V3` ties GDO0 hard high and
      shorts it against the CC1101's own output.
- [ ] Confirm the module is the **433 MHz** variant. The 315 / 433 / 868 MHz boards
      are visually identical and differ only in their matching network; the
      silkscreen says which.
- [ ] Antenna fitted — SMA whip, or 17.3 cm of straight wire. Never transmit
      without one.
- [ ] Remember the header body is on the **back** of the board, so the numbering
      mirrors left-to-right when you flip it to plug wires in. See the warning in
      [hardware.md](hardware.md#pinout).

Full pinout, loom mapping and board identification:
[hardware.md](hardware.md#pinout).

---

## Stage 1 — is the chip alive?

**Test:** flash the sniffer and read the banner.

```bash
cd firmware/sniffer
pio run -t upload
pio device monitor -b 115200
```

- [ ] `VERSION=0x14` (older parts `0x04`).

Nothing below this line will work until that passes.

| Reading | Means |
|---|---|
| `0x14` / `0x04` | SPI is fine. Continue. |
| `0x00` | MISO is not getting back, or the chip has no power |
| `0xFF` | CSn stuck, MOSI/SCLK swapped, or the module is unpowered |
| anything else | intermittent wiring — shorten the dupont leads |

Check in this order: 3V3 and GND first, then MISO, then CSn, then MOSI/SCLK — MISO
first because a **swapped MOSI/MISO pair is the usual outcome of the mirrored-header
trap** in Stage 0. Details:
[hardware.md](hardware.md#2-version-register--after-wiring).

Long unshielded leads on SCK are a real cause of intermittent reads; keep them
under 10 cm.

Also confirm the status byte's `CHIP_RDYn` bit is clear — `status` reports it
indirectly, and if it never clears the crystal is not running, which means power
or a dead module.

---

## Stage 2 — is the receiver receiving?

**Test:** RSSI must move when the remote transmits.

```
> rssi 10000
```

Press a remote button a metre away, in the same room.

- [ ] Idle reading is a plausible noise floor, roughly −90 to −110 dBm.
- [ ] Pressing the button raises it by **20 dB or more**.

| Symptom | Cause |
|---|---|
| Reading pinned near 0 dBm | no antenna, or the front end is saturated |
| No idle noise at all | not actually in RX — check `status` for `MARCSTATE=0x0D` |
| Idle floor fine, no jump on press | remote is not on 433.92 MHz → Stage 3 |

If the jump is there, skip Stage 3 and go to Stage 4.

---

## Stage 3 — where is the remote actually transmitting?

Only needed if Stage 2 showed no RSSI jump.

**Test:** sweep the band while holding the button down.

```
> scan
```

or, with the bar chart:

```bash
cd tools && python3 pergola_capture.py --scan
```

- [ ] Run it once with the remote **silent** to establish a noise floor.
- [ ] Run it again holding a button down for the whole sweep.
- [ ] A peak at least 10 dB above the floor.

| Peak near | Likely |
|---|---|
| 433.92 MHz | a generic OOK remote — the good case |
| 433.42 MHz | **Somfy RTS family — expect a rolling code**, see [remote-protocol.md](remote-protocol.md) |
| 434.00–434.79 | still fine, just retune with `freq <MHz>` |
| no peak | the remote is not in this band at all; check for 868 MHz markings on it |

Set the frequency with `freq 433.42` (or whatever the peak was) and go back to
Stage 2.

---

## Stage 4 — are frames being captured?

**Test:** press a button and get `F,...` lines.

- [ ] At least one `F,...` line per press.
- [ ] The summary line says `repeats=` something greater than 1.
- [ ] The summary line says `identical=yes`.

| Symptom | Fix |
|---|---|
| Continuous garbage frames with no button press | raise the OOK decision boundary: `w 1D 92`, or `w 1D 93` |
| Still garbage | cap the gain: `w 1B 43` |
| Nothing on press, but RSSI jumps | lower the decision boundary: `w 1D 90` |
| `identical=NO` | noisy capture — move closer, then raise the boundary |
| Many width classes (5+) | same as above; a clean remote has 2–3 plus one sync gap |
| `TRUNCATED` on every frame | you are holding the button too long; tap it |
| 1–2 µs pulses in the output | raise `glitch 80` |
| One press arrives as many frames | raise `gap 30000` |
| Several presses merge into one frame | lower `gap 10000` |

Full symptom table:
[cc1101/05-recipes.md](cc1101/05-recipes.md#tuning-cheat-sheet).

---

## Stage 5 — capture each button properly

**Test:** three clean presses of each of the three buttons.

```bash
cd tools
python3 -m venv .venv && . .venv/bin/activate && pip install -r requirements.txt
python3 pergola_capture.py --button open  --count 3
python3 pergola_capture.py --button stop  --count 3
python3 pergola_capture.py --button close --count 3
python3 pergola_analyze.py captures/*.jsonl
```

- [ ] Every button reaches a verdict.
- [ ] Codes differ between buttons (if all three are identical, the labels got
      mixed up — recapture).

Three presses is the minimum. One press cannot distinguish a fixed code from a
rolling one, and two cannot distinguish a rolling code from a bad capture.

Run the analyser on the shipped examples first if you want to see what a good
result looks like:

```bash
python3 pergola_analyze.py captures/example-fixed-code.jsonl
```

---

## Stage 6 — the go/no-go test

**Test:** replay a captured frame and watch the roof.

```
> keep 0
> tx 0 4
```

- [ ] The pergola responds.

If it does: the code is replayable, and everything downstream is software. Write
the codes into [remote-protocol.md](remote-protocol.md) and move on to timing the
travel.

If it does not, in order:

1. Was the capture clean? `identical=yes`, 2–3 width classes.
2. Is the transmitter working at all? `status` should show `PATABLE[1]=0xC0`, and
   the module needs its antenna.
3. Does the ESP32 brown out on transmit? Try `power 60`, and add the decoupling
   capacitors from [hardware.md](hardware.md#power).
4. Try more repeats: `tx 0 8`.
5. Did the analyser say `ROLLING`? Then a replay cannot work by design — see
   [remote-protocol.md](remote-protocol.md#if-it-is-a-rolling-code).

---

## Stage 7 — measure the travel

Needed before the MQTT daemon can offer a position, and for the timed macros that
work around the light quirk.

- [ ] Time a full open, from fully closed, with a stopwatch. Three runs.
- [ ] Time a full close, from fully open. Three runs.
- [ ] Note whether `close` also switches the light on, and what turns it off
      again.

Record all of it in [behaviour.md](behaviour.md), which has a table waiting for
the numbers.
