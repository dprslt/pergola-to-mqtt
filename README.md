# pergola-to-mqtt

Our pergola came with a three button remote and nothing else. No app, no cloud,
nothing to talk to, no way to tell Home Assistant to close the roof when it starts
raining.

So I listened to the remote, worked out what it says, and made an ESP32 say the same
thing.

It's a Green Outside "Actual", 3×4 m, motorised louvres with a light bar in the
frame. Open, stop, close. That's the whole feature set.

## Status

Works.

The remote turned out to be a plain fixed code transmitter, so there was no rolling
code and no crypto to get around. 24 bits at 315 MHz: a 20 bit address shared by all
three buttons, plus one bit saying which button. The ESP32 sends those codes and the
roof moves.

A small daemon publishes a cover and a light into Home Assistant over MQTT. That part
is tested against the real broker.

Two things I assumed at the start were wrong, and each one cost me an evening.

1. **It's not on 433 MHz.** Almost everything in this space is. I swept 433.0 to
   434.8 MHz over and over with a receiver I'd already proved was healthy, and got
   nothing but noise. The resonator inside the remote says 315.
2. **Replaying a recording doesn't work.** Rebuilding the frame from the decoded bits
   does. Two separate reasons, both annoying, both written up in
   [docs/remote-protocol.md](docs/remote-protocol.md).

## Read this before you transmit anything

**A full open has to be followed by a stop.** Let the roof travel all the way open
with nothing sending a stop and it latches: close does nothing until a stop clears
it.

That goes for anything that sends an open, including a throwaway script or a serial
session. The daemon schedules the stop when the move starts, not when travel
finishes, so nothing can skip it.

The louvres also have pinch points, and they move the moment the command lands. Keep
a real remote in reach and don't automate travel you can't see.

## Hardware

An ESP32 dev board, a CC1101 module, an antenna, seven jumper wires.

Mine is a WROOM-32 DevKitC with an Ebyte E07-M1101D. That module is sold as a 433 MHz
board and I run it at 315, which the chip does natively, but sensitivity suffers and
it wants the remote within a metre or so. Buy a 315 MHz board if you're buying one.

Pin map, pin choices and power notes: [docs/hardware.md](docs/hardware.md).

Do the continuity check in stage 0 of
[docs/setup-checklist.md](docs/setup-checklist.md) before you apply power. Swapping
VCC and GND is the one mistake that kills the module, and it takes twenty seconds to
rule out.

## Getting started

PlatformIO lives in a venv at the repo root, so there's no `pio` on your PATH.
[CLAUDE.md](CLAUDE.md) says why, and how to rebuild it.

```bash
.venv/bin/pio run -d firmware/sniffer -t upload
.venv/bin/pio device monitor -d firmware/sniffer
```

That gives you a serial prompt, and `?` lists everything it takes. `open`, `stop` and
`close` send this pergola's own codes. `scan` sweeps for a carrier, `forge` builds a
word from hex and `tx` transmits it. Any CC1101 register can be poked live without
reflashing, which is how I found 315 MHz in the first place.

Decoding runs on the device, so pressing a button prints this:

```
# frame 44: n=49 33.5ms rssi=-23.0dBm lvl=1 widths=345x25 1028x24 code=0xF3A758 (this pergola: OPEN)
```

For a different remote, capture to disk and analyse it on your laptop:

```bash
cd tools
python3 -m venv .venv && . .venv/bin/activate && pip install -r requirements.txt
python3 pergola_capture.py --button open --count 3
python3 pergola_analyze.py captures/*.jsonl
```

Three presses per button, minimum. One press can't tell a fixed code from a rolling
one, and two can't tell a rolling code from a bad capture.

The analyser needs no hardware and no dependencies, so you can try it on the two
example captures in the repo before committing to any of this:

```bash
python3 pergola_analyze.py captures/example-fixed-code.jsonl    # FIXED
python3 pergola_analyze.py captures/example-rolling-code.jsonl  # ROLLING
python3 test_analyze.py && python3 test_capture.py              # 32 tests
```

## Home Assistant

`firmware/daemon` joins the WiFi, publishes discovery and exposes three things: the
roof as a cover with a position slider, the light bar as a light, and a sensor showing
the last code transmitted.

The position is dead reckoned from stopwatch measurements (6.30 s to open, about 6 to
close), because nothing in this system reports back. The remote's encoder only
transmits, and there's a wired wall button that moves the roof without putting
anything on the air, so even a receiver listening permanently would miss it. Treat
the percentage as a guess.

Topics, setup and how the credentials are handled:
[docs/home-assistant.md](docs/home-assistant.md).

## Layout

```
docs/          hardware, bring-up checklist, protocol, behaviour, Home Assistant
docs/cc1101/   the parts of the datasheet this project actually uses
firmware/
  common/      driver, codec, codes and state machine, shared by both firmwares
  sniffer/     serial prompt for capture and transmit
  daemon/      WiFi, MQTT, discovery
tools/         capture to JSONL, analyse it, and the codec mirrored in Python
```

## CC1101 notes

The datasheet (TI SWRS061I) is 98 pages and this project touches maybe fifteen of
them. Those are distilled in [docs/cc1101/](docs/cc1101/README.md) with page
citations. If you read one page, read the one on asynchronous serial mode: it's what
makes raw sniffing possible.

The PDF isn't committed since it's TI's. `docs/datasheets/fetch.sh` will get it.

## Licence

MIT, see [LICENSE](LICENSE).
