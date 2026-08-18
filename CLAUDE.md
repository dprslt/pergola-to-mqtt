# Notes for agents working in this repo

An ESP32 + CC1101 clones a pergola remote and exposes the roof to Home Assistant
over MQTT. The reverse engineering is finished and the codes are known; see
`docs/remote-protocol.md`.

## Read this before touching the radio or the roof

**A full open must be followed by a stop.** If the roof reaches fully open and no
stop is sent, it cannot be closed again — it latches, and only a stop clears it.
Anything that sends an open owns the stop that ends it, including a one-off script
or a manual serial session. `CoverState_t::startMove()` schedules that stop when the
move *starts*, not when travel ends, so a stall cannot skip it. Do not add a way to
disable this. Details in `docs/behaviour.md`.

The roof has pinch points and moves on command. Do not transmit a movement command
without the user's knowledge.

## Building

There is **no `pio` on `PATH`**. It lives in a venv at the repo root, because the
system `python3` is 3.14 and PlatformIO does not support it:

```bash
.venv/bin/pio run -d firmware/sniffer            # build
.venv/bin/pio run -d firmware/sniffer -t upload  # flash
.venv/bin/pio device monitor -d firmware/sniffer # watch
```

Recreate the venv if it is missing — do not install PlatformIO globally or with
Homebrew:

```bash
~/.pyenv/versions/3.11.12/bin/python3 -m venv .venv
.venv/bin/python -m pip install platformio
```

Host tools use a **separate** venv under `tools/`, which is plain Python and has no
3.11 constraint. Do not merge them.

```bash
cd tools && python3 test_analyze.py && python3 test_capture.py   # no hardware needed
```

## Layout

```
firmware/common/     shared by both firmwares via lib_extra_dirs = ../common
  cc1101/            radio driver, register config, board pins
  pergola/           this pergola's codes, EV1527 codec, cover state machine
  sniffer/           pulse capture from a GPIO
firmware/sniffer/    serial CLI: capture, decode, scan, forge, transmit
firmware/daemon/     WiFi + MQTT + Home Assistant discovery
tools/               capture to JSONL, analyse, and the codec mirrored in Python
```

The CC1101 driver is in `common/` on purpose. Both firmwares drive the same radio
with the same registers, and duplicating the driver is how the two silently drift
apart. `tools/ev1527.py` mirrors `common/pergola/ev1527.cpp`; change them together.

## Things that cost real time here

**The remote is on 315 MHz, not 433.** A sweep of 433.0–434.8 MHz with a working
receiver finds nothing at all. The firmware defaults to 315 MHz; the CC1101's lower
band is 300–348 MHz and `setFrequencyMHz()` snaps out of the 348–387 MHz gap where
the synthesiser cannot lock.

**`gap` must be shorter than the sync gap.** The remote's sync low is 31α ≈ 10.9 ms.
A capture threshold above that merges consecutive repeats into one frame, and the
repeat comparison then straddles partial words and reports `identical=NO` on a code
that is perfectly fixed. This made a fixed code look rolling for an evening. The
default is now 5000 µs.

**Forge, do not replay.** Replaying a captured frame does not work: the sniffer
segments on the sync low, so a capture holds `[24 data bits][sync high]` and the
sync low is gone, and captured pulses carry glitch-merged widths that are neither α
nor 3α. `forge` synthesises exact timings with the sync in the right place. The
daemon has no capture to replay anyway.

**Never gate behaviour on the believed position.** There is no feedback of any kind:
the remote's encoder is transmit-only, and the pergola has a **wired wall button
that moves the roof emitting no RF**. Position is dead-reckoned and a reboot resets
it to "closed". Three guards were written and then removed for this reason — a
light-on precondition, an "already at target" short-circuit that silently dropped
commands, and a "position trusted" entity whose green state could be false. If you
find yourself writing `if (position_ == ...)` to decide whether to act, don't.

**Opening the serial port resets the ESP32**, which wipes the daemon's position
estimate. Any `pio device monitor` desynchronises it. Re-anchor by driving to an end.

**The Dupont loom colours are non-standard: brown is VCC, red is GDO0.** The ribbon
is bonded, so it keeps strict resistor-code order instead. Connecting red to a power
rail ties GDO0 to 3.3 V against the CC1101's output driver. `docs/hardware.md` has
the pin map.

## Credentials

`firmware/daemon` has no `secrets.h`. `scripts/inject_secrets.py` resolves six
fields from `firmware/daemon/.env` (gitignored) into a generated header before each
build. Do not pass them as `-D` flags: SCons expands `$NAME` inside construction
variables, so a password containing `$o3V` reaches the compiler four characters
shorter, the build stays green, and only the broker complains. `docs/home-assistant.md`
has the full reasoning.

## Documentation

| File | Contents |
|---|---|
| `docs/hardware.md` | pin map, ESP32 GPIO choices, antenna, power |
| `docs/setup-checklist.md` | bring-up in order, one gate per stage |
| `docs/remote-protocol.md` | 315 MHz, the three codes, why replay fails |
| `docs/behaviour.md` | travel times, the open lockout, the light |
| `docs/home-assistant.md` | MQTT topics, entities, credential handling |
| `docs/cc1101/` | distilled datasheet, with page citations |

These were written from measurements on the real unit. If you contradict one, either
you are wrong or the hardware changed — check before editing.
