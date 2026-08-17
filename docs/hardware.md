# Hardware — ESP32 ↔ CC1101 wiring

Everything physical: which module, which pin goes where, power, antenna, and how
to prove the wiring is right before blaming the firmware.

For the step-by-step bring-up procedure, see
[setup-checklist.md](setup-checklist.md). This page is the reference; that one is
the order to do it in.

## Bill of materials

| Part | Notes |
|---|---|
| ESP32 dev board | ESP32-WROOM-32 / DevKitC. Any variant with the default VSPI pins broken out |
| CC1101 433 MHz module | **E07-M1101D** (Ebyte, 10 dBm) or any generic 8-pin CC1101 board |
| 433 MHz antenna | SMA whip, or a 17.3 cm straight wire soldered to the RF pad |
| Dupont wires | 6 minimum (7 if you also wire GDO2) |

Nothing else. No level shifters, no external regulator, no RC filter — the glitch
filtering that TI suggests in hardware is done in software instead
([04-async-serial-ook.md](cc1101/04-async-serial-ook.md#1-glitches)).

## The module

The board this project was built against is an **E07-M1101D V2.0**, identifiable
without a datasheet:

- Back silkscreen reads `E07-M1101D` — at an angle the `D` reads convincingly as
  a `0`, which is why it is easy to mis-search for.
- Front silkscreen reads `433M` on one edge and `V2.0` on the other.
- Front carries the CC1101 QFN-20 and a 26 MHz crystal, which is the value every
  frequency calculation in [03-frequency-and-modulation.md](cc1101/03-frequency-and-modulation.md)
  assumes.
- 2×4 header (8 pins) plus an SMA connector.

**Any CC1101 breakout works identically.** The chip exposes only 7 useful signals
plus power, so a "10-pin" board is an 8-pin board that duplicates GND/VCC or
breaks out an unconnected pin. Nothing in this repo depends on the carrier board.

The one pin the project genuinely cannot live without is **GDO0**, because
asynchronous serial mode bypasses the FIFOs entirely and hands the raw baseband
out on that single wire — and takes TX data back in on it. Every CC1101 breakout
exposes GDO0. GDO2 is optional here and can be left floating.

## Pinout

The E07-M1101D numbers its header on the **front** silkscreen: `2` top-left, `8`
top-right, `1` bottom-left, `7` bottom-right. That is column-pair numbering — odd
pins along the board edge, even pins on the chip side.

**Front view — CC1101 chip visible, antenna pointing up:**

```
              ╔═══ SMA ═══╗
        ┌─────╨───────────╨─────┐
        │       [ CC1101 ]      │
        │  ┌─────────────────┐  │
        │  │  2   4   6   8  │  │  ← chip-side row
        │  │  1   3   5   7  │  │  ← board-edge row
        │  └─────────────────┘  │
        └───────────────────────┘
```

| Pin | Signal | ESP32 | Loom | Why this GPIO |
|---|---|---|---|---|
| 1 | GND | GND | black | |
| 2 | VCC | 3V3 | **brown** | **3.3 V only** — see [Power](#power) |
| 3 | **GDO0** | GPIO4 | **red** | Not strapping, not flash, interrupt-capable, bidirectional |
| 4 | CSN | GPIO5 | orange | Default VSPI CS |
| 5 | SCK | GPIO18 | yellow | Default VSPI SCK |
| 6 | MOSI | GPIO23 | green | Default VSPI MOSI |
| 7 | MISO (GDO1) | GPIO19 | blue | Default VSPI MISO |
| 8 | GDO2 | — | — | Unused; leave floating |

> ⚠️ **The header body is on the back of the board.** When you flip it over to
> plug in Dupont wires, the numbering mirrors left-to-right: pin 1 ends up
> **bottom-right**, pin 7 bottom-left. This is the single most common way to lose
> an hour on this board.

> 🟤 **The loom colours are deliberately non-standard: brown is VCC, red is
> GDO0.** The cable is a bonded rainbow ribbon, so the wires cannot be reordered
> without splitting it, and keeping the strict resistor-code sequence
> (black-brown-red-orange-yellow-green-blue → pins 1–7) means the loom physically
> cannot cross itself. That error-proofing was judged worth more than the
> red-means-power convention.
>
> The hazard this creates: connecting **red** to `3V3` on the assumption that red
> is power ties GDO0 hard to 3.3 V, and since the CC1101 drives GDO0 as an output
> in RX, that shorts two outputs together. **Brown is the supply.** Tape the loom.

The run zigzags rather than following one row, because consecutive pins alternate
between the edge row (odd) and the chip-side row (even) within each column pair:

```
BACK VIEW — antenna up, "E07-M1101D" text on the right

              ╔═══ SMA ═══╗
        ┌─────╨───────────╨─────┐
        │                       │   E
        │  ┌─────────────────┐  │   0
   chip │  │  ·   grn  org  BRN│  │  7
   side │  │  8    6    4    2 │  │  ─
        │  │                   │  │  M
   edge │  │  7    5    3    1 │  │  1
        │  │ blu  yel  red  BLK│  │  1
        │  └─────────────────┘  │   0
        └───────────────────────┘   1D
                            ▲
                    black + brown here
```

### Why these ESP32 pins

SPI lands on GPIO18/19/23/5 because those are the ESP32's **default VSPI pins**,
so `SPI.begin()` needs no argument and the IO-MUX fast path is used instead of
the GPIO matrix.

GDO0 on **GPIO4** is the deliberate choice, and the constraints are tighter than
they look:

- **GPIO6–11** are wired to the onboard SPI flash. Using one bricks the boot.
- **GPIO34–39** are input-only. GDO0 must be driven by the ESP32 during transmit
  ([04-async-serial-ook.md](cc1101/04-async-serial-ook.md#wiring-implication)),
  so these are disqualified outright.
- **GPIO0, 2, 12, 15** are strapping pins sampled at reset. A module driving one
  at power-up changes boot mode or flash voltage.
- **GPIO4** is free of all three problems and supports `attachInterrupt`, which
  the sniffer depends on for edge timing.

GPIO5 is technically a strapping pin too (sampled high at boot), but it idles
high as a chip select, so the conventional VSPI assignment is safe.

## Power

- **3.3 V only.** The CC1101 accepts 1.8–3.6 V. The module has **no
  regulator and no level shifting** — feeding it from `VIN` or a `5V` pin
  destroys it.
- The ESP32's own 3.3 V I/O drives the CC1101 directly, no translation needed.
- TX at 10 dBm draws roughly 30 mA, RX around 16 mA. Both are comfortably within
  what an ESP32 devkit's onboard regulator supplies, so powering the module from
  the board's `3V3` pin is fine.
- **Never** power anything from `DCOUPL` (chip pin 5). It is a decoupling output
  for the internal regulator, not a supply rail
  ([01-pins-and-interface.md](cc1101/01-pins-and-interface.md#pinout)).

If captures are noisy or the chip resets during transmit, add a 10 µF electrolytic
across VCC/GND at the module before suspecting anything else — a brownout on the
TX current step looks exactly like a firmware bug.

## Antenna

- Screw the SMA whip on **before transmitting**. At 10 dBm the risk of damaging
  the PA into an open load is small, but it costs nothing to avoid, and an
  unterminated output radiates badly enough to waste a debugging session.
- Without SMA, a **17.3 cm straight wire** is a quarter-wave at 433.92 MHz and
  works well enough to sniff a remote from across a room.
- Keep the antenna away from the ESP32's own 2.4 GHz antenna and from USB cables.
  Receive sensitivity on this chip is around −110 dBm; a remote a metre away is
  enormously stronger than that, so if you can only receive with the remote
  touching the module, something is wrong with the wiring or the frequency, not
  the antenna.

## Verifying the wiring

Two checks, in this order. The first prevents damage; the second proves the whole
SPI chain end to end.

### 1. Continuity — before applying power

The one mistake that kills the module is swapping VCC and GND. Multimeter in
continuity mode, board unpowered and unplugged:

| Probe | Expected | Meaning |
|---|---|---|
| Pin 1 ↔ SMA connector shell | beeps, ~0 Ω | Pin 1 is GND ✅ |
| Pin 2 ↔ SMA connector shell | no beep; hundreds of kΩ, or open | Pin 2 is VCC ✅ |

If those two hold, the numbering above is confirmed and every remaining possible
mistake is a non-destructive SPI swap.

### 2. `VERSION` register — after wiring

Read status register `VERSION` at address `0xF1`:

| Reads | Meaning |
|---|---|
| `0x14` | Correct. Wiring, power and SPI all good |
| `0x04` | Also fine — older silicon revision |
| `0x00` or `0xFF` | MISO not getting back, CSN on the wrong pin, or no power |

This is the cheapest whole-chain probe there is
([02-registers.md](cc1101/02-registers.md)). Run SPI at **4–5 MHz** — the chip
tolerates more, but the margins get fiddly and there is nothing to gain
([01-pins-and-interface.md](cc1101/01-pins-and-interface.md#the-4-wire-spi)).

If `VERSION` reads `0x00`/`0xFF`, work through the causes in that order — MISO
first. A swapped MOSI/MISO pair is by far the most common outcome of the
mirrored-header trap above.

## Other module variants

| Board | Difference | Works? |
|---|---|---|
| E07-M1101D-SMA | Same module, SMA fitted from the factory | Yes |
| Generic 8-pin CC1101 (ELECHOUSE-style) | Same signal set, **possibly different pin order** | Yes — re-read the silkscreen |
| 10-pin (2×5) CC1101 boards | Duplicate GND/VCC pins | Yes |
| E07 modules for 868/915 MHz | Wrong band — matching network is tuned elsewhere | No |

Trust the silkscreen on the board in your hand, not a pinout diagram found
online. Pin order genuinely differs between vendors even when the signal set is
identical, and swapping SI/SO is the classic hour-long debug.
