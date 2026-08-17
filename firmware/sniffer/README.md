# Sniffer firmware

Captures 433 MHz OOK bursts from the pergola remote and can replay them. Built for
PlatformIO on an ESP32.

```bash
pio run -t upload
pio device monitor          # 115200 baud
```

Wiring is in [../../docs/hardware.md](../../docs/hardware.md); work through
[../../docs/setup-checklist.md](../../docs/setup-checklist.md) before trusting
anything this prints.

## What it does

The CC1101 runs in asynchronous serial mode, which switches off the whole packet
engine — sync word, address filter, CRC, FIFOs — and hands the raw demodulated
baseband to GPIO4 as a level that goes high and low. A GPIO interrupt timestamps
every edge; the main loop groups edges into bursts, removes the spikes the chip is
documented to emit, and prints each burst as a list of pulse durations.

No radio library. The driver is in this project so every register it writes is the
one documented in [../../docs/cc1101/](../../docs/cc1101/README.md), and a library
update can never silently change the configuration underneath a capture.

## Output grammar

Consumed by `tools/pergola_capture.py`.

```
#...                                         comment; ignore when parsing
F,seq,t_ms,rssi,lvl,trunc,n,d0,d1,...,dn-1   one captured burst
S,mhz,rssi                                   one row of a frequency scan
```

Durations are microseconds and strictly alternate in level, starting at `lvl`
(1 = carrier on). `trunc` means the burst hit the 1024-pulse ceiling and continues
in the next frame.

Every frame is followed by a readable summary, so the serial monitor is useful on
its own:

```
F,3,18422,-52.5,1,0,300,321,9930,318,957,955,320, ... 
# frame 3: n=300 245.7ms rssi=-52.5dBm lvl=1 widths=321x150 958x144 9923x6 repeats=6 identical=yes
```

`repeats=6 identical=yes` is the line to look at: a remote sends its burst several
times per press, and identical repeats mean the capture is clean. Whether the code
is *fixed* needs a comparison across separate presses — that is the host tools'
job.

## Serial CLI

Type `?` for the list. Everything is adjustable at runtime, so chasing a stubborn
remote never needs a reflash.

| Command | Does |
|---|---|
| `status` | chip + capture state, and whether `VERSION` looks sane |
| `reg` | dump all 47 configuration registers by name |
| `rssi [ms]` | watch RSSI while you press a button |
| `scan [lo hi kHz ms]` | sweep RSSI across the band; default `433.0 434.8 50 40` |
| `freq <MHz>` | retune, e.g. `freq 433.42` |
| `rate <baud>` | data rate, e.g. `rate 2400` |
| `bw <kHz>` | channel filter bandwidth, e.g. `bw 102` |
| `gap <us>` | silence that ends a frame (default 20000) |
| `glitch <us>` | pulses shorter than this are spikes (default 50) |
| `minp <n>` | minimum pulses for a frame to count (default 16) |
| `power <hex>` | `PATABLE[1]`, e.g. `power C0` (+10 dBm) or `power 60` (0 dBm) |
| `w <addr> <val>` | write any register, hex |
| `x <addr>` / `xs <addr>` | read a config / status register, hex |
| `out on\|off` | frame printing |
| `keep <slot>` | store the last frame in slot 0–3 |
| `slots` | list stored frames |
| `tx <slot> <rep> [ms]` | replay a slot |
| `defaults` | re-apply the default configuration |
| `zero` | reset the counters |

### Replaying a capture

```
> keep 0
# slot 0 <- frame 3 (300 pulses)
> tx 0 4
```

This is the project's go/no-go test. If the roof moves, the code is replayable and
the rest is software. If it does not — and the capture was clean — suspect a
rolling code.

`tx` detaches the capture interrupt first, hands GDO0 from the CC1101 to the
ESP32, keys the carrier, then gives the pin back. Getting that handover wrong
would have both chips driving the same net.

**Duty cycle**: 433.05–434.79 MHz is licence-free in the EU under EN 300 220 but
capped at 10% duty cycle. A real remote sends a few tens of milliseconds and
repeats a handful of times; `tx 0 4` matches that. Do not loop it.

## Tuning

The defaults target a generic 433.92 MHz OOK remote. When captures look wrong,
change one thing at a time — the table in
[../../docs/cc1101/05-recipes.md](../../docs/cc1101/05-recipes.md#tuning-cheat-sheet)
maps every symptom to the knob that fixes it.

The one worth knowing by heart: **`AGCCTRL0` is the OOK decision boundary**, not a
filter length. `w 1D 92` demands a bigger amplitude gap before calling something a
`1` — the fix for constant garbage frames. `w 1D 90` is the opposite, for a remote
that is too weak to register.

## Layout

| File | Contains |
|---|---|
| `include/pins.h` | the wiring, in one place |
| `include/cc1101_regs.h` | register addresses, strobes, header encoding |
| `include/cc1101_config.h` | the default register table, one justified byte at a time |
| `include/cc1101.h`, `src/cc1101.cpp` | the driver: SPI, reset, state machine, tunables |
| `include/pulse_sniffer.h`, `src/pulse_sniffer.cpp` | edge ISR, ring buffer, framing, deglitch |
| `src/main.cpp` | serial CLI, output format, replay |

Roughly 19% of RAM and 24% of flash, leaving ample room for WiFi and MQTT when the
daemon lands.

## Arduino IDE instead of PlatformIO

It works, with two changes: copy the contents of `include/` and `src/` into one
sketch folder, and rename `main.cpp` to match the folder. PlatformIO is
recommended — the pin map, build flags and monitor speed are all committed here,
so there is nothing to remember or re-enter.
