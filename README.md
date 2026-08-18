# pergola-to-mqtt

Our pergola shipped with a three-button remote and nothing else. No app, no cloud,
no API, no way to tell Home Assistant to shut the roof when it starts raining. So
this listens to what the remote says over the air, works out the protocol, and says
the same thing from an ESP32.

It's a Green Outside "Actual", 3×4 m, motorised louvres with a light bar in the
frame. The remote does open, stop, close. That's the whole feature set.

## Where it's got to

It works. The remote turned out to be a plain fixed-code OOK transmitter, so there's
no rolling code and no crypto to get around: 24 bits at 315 MHz, a 20-bit address
shared by all three buttons plus one bit to say which button. The ESP32 sends those
codes, the roof moves. A small MQTT daemon publishes a cover and a light into Home
Assistant by auto-discovery.

Two assumptions I started with were wrong:.

The remote isn't on 433 MHz. Nearly everything in this space is, and I swept
433.0–434.8 MHz over and over with a receiver I'd already proved was healthy, getting
nothing but noise. The SAW resonator inside the remote says 315.

Replaying a recorded frame doesn't work, but synthesising the same frame from the
decoded bits does. Two separate reasons, both irritating, both written up in
[docs/remote-protocol.md](docs/remote-protocol.md).

## The thing that will bite you

**A full open has to be followed by a stop.** Let the roof travel all the way open
without sending one and it latches: close does nothing until a stop clears it. It's
not a tidiness rule, it's a lockout, and it applies to anything that sends an open
including a throwaway script or a serial session. The daemon schedules the stop when
the move starts rather than when travel finishes, so nothing can skip it.

The louvres also have pinch points and they move as soon as the command lands. Keep a
real remote in reach and don't automate travel you can't see.

## Hardware

An ESP32 dev board, a CC1101 module, an antenna and seven jumper wires. Mine is a
WROOM-32 DevKitC with an Ebyte E07-M1101D. The module is a 433 MHz board being used
at 315, which the CC1101 chip handles natively, though sensitivity suffers and it
wants the remote within a metre or so. A 315 MHz board would be better if you're
buying one.

Pin map, GPIO choices and power notes: [docs/hardware.md](docs/hardware.md).

Do the continuity check in stage 0 of
[docs/setup-checklist.md](docs/setup-checklist.md) before applying power. Swapping VCC
and GND is the one mistake that kills the module and it takes twenty seconds to rule
out.

## Getting started

PlatformIO lives in a venv at the repo root, so there's no `pio` on `PATH`.
[CLAUDE.md](CLAUDE.md) explains why and how to rebuild it.

```bash
.venv/bin/pio run -d firmware/sniffer -t upload
.venv/bin/pio device monitor -d firmware/sniffer
```

That gives you a serial CLI (`?` lists it). `open`, `stop` and `close` send this
pergola's own codes. `scan` sweeps for a carrier, `forge` builds a word from hex and
`tx` transmits it, and any CC1101 register can be poked live without reflashing,
which is how the 315 MHz discovery happened in the first place.

Decoding runs on the device, so pressing a button prints this:

```
# frame 44: n=49 33.5ms rssi=-23.0dBm lvl=1 widths=345x25 1028x24 code=0xF3A758 (this pergola: OPEN)
```

For a different remote, capture to disk and analyse on the host:

```bash
cd tools
python3 -m venv .venv && . .venv/bin/activate && pip install -r requirements.txt
python3 pergola_capture.py --button open --count 3
python3 pergola_analyze.py captures/*.jsonl
```

Three presses minimum per button. One press can't tell a fixed code from a rolling
one, and two can't tell a rolling code from a bad capture.

The analyser needs no hardware and no dependencies, so you can try it on the two
synthetic captures in the repo before committing to any of this:

```bash
python3 pergola_analyze.py captures/example-fixed-code.jsonl    # FIXED
python3 pergola_analyze.py captures/example-rolling-code.jsonl  # ROLLING
python3 test_analyze.py && python3 test_capture.py              # 32 tests
```

## Home Assistant

`firmware/daemon` joins WiFi, publishes discovery and exposes three entities: the
roof as a cover with a position slider, the light bar as a light, and a diagnostic
sensor showing the last code transmitted.

The position is dead reckoned from stopwatch measurements (6.30 s to open, about 6 to
close) because nothing in this system reports back. The remote's encoder is transmit
only, and there's a wired wall button that moves the roof without putting anything on
the air, so even a permanently listening receiver would miss it. Treat the percentage
as a guess. Topics, setup and the credential handling are in
[docs/home-assistant.md](docs/home-assistant.md).

## Repo layout

```
docs/
  hardware.md            wiring, antenna, power
  setup-checklist.md     bring-up in order, one gate per stage
  remote-protocol.md     315 MHz, the codes, why replay fails
  behaviour.md           travel times, the open lockout, the light
  home-assistant.md      MQTT topics, entities, secrets
  cc1101/                the parts of the datasheet this project uses
firmware/
  common/                driver, codec, codes and state machine, shared
  sniffer/               serial CLI for capture and transmit
  daemon/                WiFi, MQTT, discovery
tools/
  pergola_capture.py     serial to labelled JSONL, plus a band scan
  pergola_analyze.py     JSONL to codes and a fixed-vs-rolling verdict
  ev1527.py              the codec, mirroring the C++ one
```

## CC1101 notes

The datasheet (TI SWRS061I) is 98 pages and this project touches maybe fifteen of
them. Those parts are distilled in [docs/cc1101/](docs/cc1101/README.md) with page
citations, covering the SPI interface and command strobes, the register map,
frequency and OOK setup, asynchronous serial mode, and a few ready-made register
sets. Asynchronous serial mode is the one worth reading if you only read one: it's
what makes raw sniffing possible.

The PDF isn't committed since it's TI's. `docs/datasheets/fetch.sh` will get it.

## Licence

MIT, see [LICENSE](LICENSE).
