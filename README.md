# pergola-to-mqtt

Clone a 433 MHz pergola remote with an ESP32 + CC1101, then expose the pergola to
Home Assistant as an MQTT device.

The pergola (bioclimatic louvered roof with an integrated light bar) ships with a
3-button remote — **open**, **stop**, **close** — and nothing else. No app, no
cloud, no local API. This repo reverse-engineers the remote's RF frames and
replays them from an ESP32 so Home Assistant can drive the roof.

## Status

| Phase | What | State |
|---|---|---|
| 0 | Repo, datasheet notes, hardware wiring | ✅ done |
| 1 | Sniffer firmware — capture raw OOK pulse trains | ✅ done, untested on hardware |
| 2 | Host tools — decode captures, identify protocol | ✅ done |
| 3 | **Bring-up — prove the wiring before anything else** | ⏳ needs hardware |
| 4 | Confirm replay actually moves the roof | ⏳ needs hardware |
| 5 | Measure full open/close travel time | ⏳ needs hardware |
| 6 | MQTT daemon + Home Assistant discovery | 📋 designed, not built |

**Phase 3 gates everything downstream.** Until the CC1101's `VERSION` register
reads back `0x14` over SPI, every later symptom is meaningless — a silent radio,
an empty capture and a wrong-frequency scan all look identical to a swapped MISO
wire. Work through **[docs/setup-checklist.md](docs/setup-checklist.md)** in
order and do not skip ahead "just to try a capture".

Phase 4 is the go/no-go gate for the project as a whole: if the remote uses a
**rolling code** (Somfy RTS and friends), a plain replay will not work and the
plan changes. See [docs/remote-protocol.md](docs/remote-protocol.md) for how to
tell.

## Hardware

- ESP32 dev board (ESP32-WROOM-32 / DevKitC)
- CC1101 433 MHz transceiver module (E07-M1101D or a generic 8-pin board)
- 433 MHz antenna — a 17.3 cm straight wire works
- Dupont wires

Pinout, ESP32 GPIO mapping, power notes and module variants:
**[docs/hardware.md](docs/hardware.md)**.

Before wiring anything, run the continuity check in Stage 0 of
[docs/setup-checklist.md](docs/setup-checklist.md) — swapping VCC and GND is the
one mistake that destroys the module, and it takes 20 seconds to rule out.

## Quick start

```bash
# 1. flash the sniffer
cd firmware/sniffer
pio run -t upload

# 2. watch raw frames appear as you press remote buttons
pio device monitor -b 115200

# 3. or capture them to disk, three presses per button, one file each
cd ../../tools
python3 -m venv .venv && . .venv/bin/activate && pip install -r requirements.txt
python3 pergola_capture.py --button open  --count 3
python3 pergola_capture.py --button stop  --count 3
python3 pergola_capture.py --button close --count 3

# 4. decode, and get a fixed-vs-rolling verdict per button
python3 pergola_analyze.py captures/*.jsonl
```

Three presses per button is the minimum: one press cannot distinguish a fixed code
from a rolling one, and two cannot distinguish a rolling code from a bad capture.

The sniffer has a serial CLI (`?` for help) so you can retune frequency,
bandwidth, gap threshold and any CC1101 register without reflashing — including
`scan` to find the remote's real carrier frequency, and `tx` to replay a captured
frame.

### Try the host tools before the hardware arrives

Two synthetic captures ship with the repo — a fixed-code remote and a rolling-code
one — so you can see what a good result looks like before having to judge a real
one:

```bash
cd tools
python3 pergola_analyze.py captures/example-fixed-code.jsonl    # -> FIXED
python3 pergola_analyze.py captures/example-rolling-code.jsonl  # -> ROLLING
python3 test_analyze.py && python3 test_capture.py              # 25 tests, no hardware
```

## Repo layout

```
docs/
  hardware.md            ESP32 ↔ CC1101 wiring, antenna, power
  setup-checklist.md     bring-up in order, with a gate at each stage
  remote-protocol.md     findings about THIS remote (fill in after capture)
  behaviour.md           how the pergola reacts to each button; the light quirk
  home-assistant.md      MQTT topic + discovery design for the daemon
  cc1101/                distilled CC1101 datasheet reference (see below)
firmware/
  sniffer/               PlatformIO project: OOK pulse capture + replay CLI
tools/
  pergola_capture.py     serial → labelled JSONL captures, plus band scan
  pergola_analyze.py     JSONL → bits, protocol guess, fixed-vs-rolling verdict
  test_analyze.py        synthesised EV1527 + Somfy frames; also the protocol spec
  test_capture.py        serial line-grammar tests
  captures/              your captures (gitignored) + two synthetic examples
```

## CC1101 reference

The CC1101 datasheet (TI **SWRS061I**, 98 pages) is the source of truth for every
register this project touches. Rather than re-read it each time, the parts that
matter are distilled in [docs/cc1101/](docs/cc1101/README.md), with page citations
back to the original:

- [Pins, SPI and command strobes](docs/cc1101/01-pins-and-interface.md)
- [Register map and the fields we use](docs/cc1101/02-registers.md)
- [Frequency, data rate and OOK/ASK](docs/cc1101/03-frequency-and-modulation.md)
- [Asynchronous serial mode — how raw sniffing works](docs/cc1101/04-async-serial-ook.md)
- [Ready-made register recipes](docs/cc1101/05-recipes.md)

The PDF itself is not committed (TI's, not ours). Fetch it with
`docs/datasheets/fetch.sh`.

## Legal / safety

- 433.05–434.79 MHz is licence-free in the EU under EN 300 220, but with a **10%
  duty cycle** limit. Transmit in short bursts; do not hold a carrier.
- This clones a remote for a pergola its owner owns. Don't point it at anything
  that isn't yours.
- The roof has pinch points. Keep the stop command reachable and don't automate
  travel you can't see.

## Licence

MIT — see [LICENSE](LICENSE).
