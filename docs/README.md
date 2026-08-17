# Documentation

## Start here

If the hardware is not built yet, or nothing is working:
**[setup-checklist.md](setup-checklist.md)**. It is ordered so that each stage
eliminates exactly one cause of "nothing appears in the serial monitor" — a
symptom that a swapped wire, a wrong frequency and a mis-set AGC threshold all
produce identically.

## Pages

| Page | What it covers | State |
|---|---|---|
| [setup-checklist.md](setup-checklist.md) | Ordered bring-up, from continuity check to the go/no-go replay test | ready to use |
| [hardware.md](hardware.md) | Bill of materials, ESP32 ↔ CC1101 pin map, power, antenna, ESP32 pin footguns | ready to use |
| [remote-protocol.md](remote-protocol.md) | What this remote actually transmits, and how to tell a fixed code from a rolling one | **template — fill in after capture** |
| [behaviour.md](behaviour.md) | How the pergola responds to each button, the light quirk, travel time, the macro model | **template — fill in after measuring** |
| [home-assistant.md](home-assistant.md) | MQTT topics, discovery payloads, and the open-loop problem | design for phase 6 |
| [cc1101/](cc1101/README.md) | Distilled CC1101 datasheet reference, with page citations | reference |
| [datasheets/](datasheets/fetch.sh) | Script to fetch the PDFs, which are not committed | — |

## CC1101 reference

The datasheet is 98 pages and this project needs about eight of them. Those are
distilled in [cc1101/](cc1101/README.md), each claim cited back to its page in TI
SWRS061I:

| Page | Covers |
|---|---|
| [01-pins-and-interface.md](cc1101/01-pins-and-interface.md) | Pinout, 4-wire SPI, status byte, command strobes, reset |
| [02-registers.md](cc1101/02-registers.md) | Full register map, plus bit detail for every field we set |
| [03-frequency-and-modulation.md](cc1101/03-frequency-and-modulation.md) | Carrier word, IF, data rate, channel bandwidth, OOK/ASK, RSSI, output power |
| [04-async-serial-ook.md](cc1101/04-async-serial-ook.md) | Asynchronous serial mode — how raw sniffing and replay work, and its three gotchas |
| [05-recipes.md](cc1101/05-recipes.md) | Copy-pasteable register sets, and the symptom → knob tuning table |

The PDF is TI's, not ours, so it is gitignored. Fetch it with
`datasheets/fetch.sh`.

## Two facts worth knowing before reading any code

**Asynchronous serial mode is the trick.** The CC1101 is built for packet radio —
preamble, sync word, address filter, CRC, FIFOs. A cheap remote uses none of it.
Setting `PKTCTRL0.PKT_FORMAT = 3` switches all of it off and puts the raw
demodulated baseband on a GPIO, which the ESP32 then times with an interrupt. That
one register turns a packet radio into a dumb OOK modem, and it is why this project
needs so little of the datasheet.

**The project has one gate.** Everything downstream depends on whether the remote
sends the same code every press. A fixed code can simply be replayed; a rolling
code cannot, by design. The capture tooling is built around answering that question
honestly rather than around decoding anything in particular — hence three presses
per button, and a verdict that distinguishes "the code changed" from "the capture
was bad".

## Elsewhere in the repo

- [../firmware/sniffer/README.md](../firmware/sniffer/README.md) — the sniffer's
  serial CLI, output grammar, and tuning notes
- [../tools/README.md](../tools/README.md) — capture and analysis scripts, the
  capture file format, and how to read a verdict
