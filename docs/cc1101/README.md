# CC1101 reference notes

Distilled from **TI SWRS061I — CC1101 Low-Power Sub-1 GHz RF Transceiver**
(98 pages). Every non-obvious claim below cites the datasheet page it came from,
as `[p.NN]`, so you can go back to the original when these notes are not enough.

The PDF is not committed to this repo (it is TI's document, not ours). Fetch it:

```bash
docs/datasheets/fetch.sh
```

## The pages

| File | Covers | Datasheet sections |
|---|---|---|
| [01-pins-and-interface.md](01-pins-and-interface.md) | Pinout, 4-wire SPI, chip status byte, command strobes, FIFO and PATABLE access, reset sequence | §5, §6, §10, §11, §19.1 |
| [02-registers.md](02-registers.md) | Full register map, and bit-level detail for the fields this project sets | §29, Tables 43–45 |
| [03-frequency-and-modulation.md](03-frequency-and-modulation.md) | Carrier frequency word, IF, channel spacing, data rate, channel filter bandwidth, OOK/ASK, RSSI | §12, §13, §16, §17.3, §21, §24 |
| [04-async-serial-ook.md](04-async-serial-ook.md) | Asynchronous serial mode — the mode that makes raw sniffing and replay possible, and its gotchas | §27.1, §11.2, §15 |
| [05-recipes.md](05-recipes.md) | Copy-pasteable register sets: 433.92 MHz OOK receive, OOK transmit, frequency scan | — |

## Why this project cares about so little of a 98-page datasheet

The CC1101 is built for framed, packetised links: preamble, sync word, address
filter, CRC, FIFOs, FEC, whitening, wake-on-radio. A cheap remote control uses
none of that. It keys the carrier on and off in a pattern of a few hundred
microseconds per symbol and repeats the whole burst a handful of times.

So we deliberately switch nearly all of the chip's cleverness **off** and use it
as a dumb OOK modem: asynchronous serial mode, no sync word, no packet handler,
no CRC. The chip then hands us the demodulated baseband on a GPIO and we time the
edges ourselves [p.63]. That reduces the datasheet surface to: how to set the
carrier, how to set the receive bandwidth and data filter, how to route baseband
to a pin, and how to read RSSI.

## The five facts worth memorising

1. **Carrier**: `f_carrier = (f_xosc / 2^16) · FREQ[23:0]` [p.75]. With a 26 MHz
   crystal, 433.92 MHz → `FREQ = 0x10B071`.
2. **Async serial mode** is `PKTCTRL0.PKT_FORMAT = 3`; RX baseband comes out on a
   GDO pin configured as `0x0D`, and TX data goes *in* on GDO0 [p.63, p.62].
3. **OOK/ASK** is `MDMCFG2.MOD_FORMAT = 3`. In OOK, PATABLE[0] is the power for a
   `0` and PATABLE[FREND0.PA_POWER] the power for a `1` [p.59, p.89].
4. **Frequency programming must only change in IDLE** [p.57]. Strobe `SIDLE`,
   rewrite FREQ, strobe `SRX`.
5. **`AGCCTRL0.FILTER_LENGTH` means something different in OOK**: it is the
   OOK/ASK decision boundary (4/8/12/16 dB), not a filter length [p.87]. This is
   the single most useful knob when a remote is received unreliably.
