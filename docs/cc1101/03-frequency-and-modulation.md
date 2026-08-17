# CC1101 — frequency, data rate and OOK/ASK

Source: TI SWRS061I §12, §13, §16, §17.3, §21, §24, §25.

All numbers below assume **f_xosc = 26 MHz**, which is what every common 433 MHz
CC1101 breakout uses. Check your module: a 27 MHz crystal shifts every result.

Operating ranges: 300–348 MHz, 387–464 MHz, 779–928 MHz [p.64]. 433.92 MHz sits
comfortably in the middle band.

## Carrier frequency

```
f_carrier = (f_xosc / 2^16) · FREQ[23:0]
```

[p.75]. `FREQ[23:0]` is split across `FREQ2` (0x0D), `FREQ1` (0x0E), `FREQ0`
(0x0F). `FREQ[23:22]` always reads 0 with a 26–27 MHz crystal.

Step size is `26 MHz / 65536` = **396.7 Hz**, so you can land on any 433 MHz
channel essentially exactly.

Inverting it, to program a frequency:

```
FREQ = round(f_carrier · 2^16 / f_xosc)
```

| Target | FREQ | FREQ2 | FREQ1 | FREQ0 | Why you'd want it |
|---|---|---|---|---|---|
| 433.050 MHz | 1091552 | `0x10` | `0xA7` | `0xE0` | bottom of the EU 433 ISM band |
| 433.420 MHz | 1092485 | `0x10` | `0xAB` | `0x85` | **Somfy RTS** and other rolling-code awning/blind remotes |
| 433.920 MHz | 1093745 | `0x10` | `0xB0` | `0x71` | **the usual suspect** — most cheap remotes |
| 434.000 MHz | 1093947 | `0x10` | `0xB1` | `0x3B` | occasionally used |
| 434.790 MHz | 1095938 | `0x10` | `0xB9` | `0x02` | top of the EU 433 ISM band |

> **Change FREQ only in IDLE.** Altering any frequency programming register while
> the synthesiser is running gives an undefined response [p.57]. Strobe `SIDLE`,
> wait for `MARCSTATE == 0x01`, write FREQ, strobe `SRX`.

If you do not know which frequency your remote uses, don't guess — sweep. The
sniffer firmware's `scan` command walks the band reading RSSI while you hold a
button down, and the peak tells you the answer.

### IF frequency

```
f_IF = (f_xosc / 2^10) · FSCTRL1.FREQ_IF[4:0]
```

[p.75]. One LSB = 25.39 kHz. Reset `FREQ_IF = 15` → 381 kHz.

Rule of thumb: keep the IF somewhere between half and one times the channel
filter bandwidth. For a 203 kHz filter, `FREQ_IF = 6` (152 kHz) or `8` (203 kHz)
both work.

### Channel spacing and channel number

```
f_channel_offset = (f_xosc / 2^18) · (256 + CHANSPC_M) · 2^CHANSPC_E
f_carrier        = (f_xosc / 2^16) · (FREQ + CHAN · (256 + CHANSPC_M) · 2^(CHANSPC_E-2))
```

[p.57, p.78]. Reset values give 199.951 kHz spacing. Max spacing with a 26 MHz
crystal is 405 kHz.

We use a single channel, so `CHANNR = 0` and the spacing registers never matter.

## Data rate

```
R_DATA = ((256 + DRATE_M) · 2^DRATE_E / 2^28) · f_xosc
```

[p.35, p.76]. `DRATE_E` lives in `MDMCFG4[3:0]`, `DRATE_M` in `MDMCFG3[7:0]`.
Range 0.6–500 kBaud.

To solve for a target rate:

```
DRATE_E = floor( log2( R · 2^20 / f_xosc ) )
DRATE_M = round( R · 2^28 / (f_xosc · 2^DRATE_E) ) - 256
```

If `DRATE_M` rounds to 256, set it to 0 and increment `DRATE_E` [p.35].

Handy values — note `DRATE_M = 131` (`0x83`) gives a clean binary ladder:

| Rate | DRATE_E | DRATE_M | MDMCFG4[3:0] | MDMCFG3 | Actual |
|---|---|---|---|---|---|
| 1.2 kBaud | 5 | 131 | `0x5` | `0x83` | 1199.6 |
| 2.4 kBaud | 6 | 131 | `0x6` | `0x83` | 2399.1 |
| **4.8 kBaud** | **7** | **131** | **`0x7`** | **`0x83`** | **4798.2** |
| 9.6 kBaud | 8 | 131 | `0x8` | `0x83` | 9596.5 |
| 115.2 kBaud | 12 | 34 | `0xC` | `0x22` | 115051 (reset) |

**In asynchronous serial mode the data rate does not gate anything** — no bit
synchroniser runs, we time the edges ourselves. What it still does is set the
demodulator's data filter bandwidth, so it should be within a factor of a few of
the remote's real symbol rate. Typical remotes use 300–1500 µs symbols
(0.7–3 kBaud); **4.8 kBaud is a good default** that passes them all without
letting through too much noise.

## Receiver channel filter bandwidth

```
BW_channel = f_xosc / (8 · (4 + CHANBW_M) · 2^CHANBW_E)
```

[p.35, p.76]. Both fields are in `MDMCFG4[7:4]`.

| CHANBW_M ↓ / CHANBW_E → | 00 | 01 | 10 | 11 |
|---|---|---|---|---|
| 00 | 812 kHz | 406 | **203** | 102 |
| 01 | 650 | 325 | 162 | 81 |
| 10 | 541 | 270 | 135 | 68 |
| 11 | 464 | 232 | 116 | 58 |

Narrow is more sensitive; wide is more forgiving. Cheap remotes use unstabilised
SAW resonators that drift over temperature and can sit 100 kHz or more off their
nominal frequency, and the CC1101 **cannot** compensate for that in OOK (see
below). So for sniffing an unknown remote, start wide: **203 kHz**
(`CHANBW_E=10, CHANBW_M=00` → `MDMCFG4[7:4] = 0x8`). Narrow it later if you want
range.

## Modulation

`MDMCFG2.MOD_FORMAT` [p.77]: `0` 2-FSK, `1` GFSK, `3` **ASK/OOK**, `4` 4-FSK,
`7` MSK.

### OOK vs ASK [p.43]

- **OOK** simply turns the PA on for a `1` and off for a `0`.
- **ASK** lets you program the modulation depth and shape the pulse amplitude, for
  a narrower spectrum.

Both are `MOD_FORMAT = 3`; which one you get depends on how you fill PATABLE.
Remotes are OOK, so: `PATABLE[0] = 0x00` (fully off) and `PATABLE[1]` = wanted
power.

Three consequences of choosing OOK that bite people:

1. **`DEVIATN` has no effect** in OOK/ASK, in TX or RX [p.43]. Don't tune it.
2. **Frequency offset compensation is not supported** for ASK or OOK [p.36].
   `FREQEST` always reads 0 [p.92], and `FOCCFG` is inert. This is why you widen
   the channel filter instead.
3. **SmartRF Studio's preferred FSK/MSK AGC settings are wrong for OOK** — TI says
   so explicitly and points at DN022 [p.43]. In particular
   `AGCCTRL0.FILTER_LENGTH` stops being a filter length and becomes the OOK
   decision boundary [p.87]:

   | FILTER_LENGTH | OOK/ASK decision boundary |
   |---|---|
   | 00 | 4 dB |
   | 01 | 8 dB |
   | 10 | 12 dB |
   | 11 | 16 dB |

### Manchester coding

`MDMCFG2.MANCHESTER_EN` can encode/decode Manchester in hardware [p.42], but it
is **unavailable in asynchronous serial mode** [p.63], and incompatible with
FEC/interleaving, MSK and 4-FSK. If the remote turns out to be Manchester-coded,
decode it in software from the raw pulse train — which is what
`tools/pergola_analyze.py` does.

### Sync word

Not used here. `SYNC_MODE = 0` disables preamble/sync transmission and detection
entirely [p.77]. A remote has no CC1101-shaped sync word to find, and with sync
detection on, the chip would never start handing us anything.

## Output power

Two-stage: `PATABLE` holds up to 8 settings, `FREND0.PA_POWER` picks one [p.59].

Recommended `PATABLE` bytes for **433 MHz** with multi-layer inductors
(Table 39, [p.60]):

| Output | Setting | Typical current |
|---|---|---|
| −30 dBm | `0x12` | 11.9 mA |
| −20 dBm | `0x0E` | 12.4 mA |
| −15 dBm | `0x1D` | 13.1 mA |
| −10 dBm | `0x34` | 14.4 mA |
| 0 dBm | `0x60` | 15.9 mA |
| +5 dBm | `0x84` | 19.4 mA |
| +7 dBm | `0xC8` | 24.2 mA |
| +10 dBm | `0xC0` | 29.1 mA |

Reset `PATABLE[0] = 0xC6` gives +7.8 dBm at 433 MHz, drawing 25.2 mA [p.60].

Two rules:

- **`0x61`–`0x6F` are not allowed** as PA settings [p.59].
- Writing any entry other than `PATABLE[0]` requires **burst** access [p.59],
  and everything but entry 0 is lost on SLEEP.

For OOK, indexes 0 and 1 are the `0` and `1` levels [p.59]. For ASK *shaping*,
the modulator runs a counter at 8× the symbol rate, counting up while
transmitting `1`s and down while transmitting `0`s, saturating at `PA_POWER` and
0, and uses that counter as the PATABLE index — so to use the whole table for
shaping, set `PA_POWER = 7` [p.60].

## RSSI

`RSSI` is status register `0x34` (read header `0xF4`), a 2's-complement value with
½ dB resolution [p.44]:

```
RSSI_dec = raw byte read as unsigned
if RSSI_dec >= 128:  RSSI_dBm = (RSSI_dec - 256) / 2 - RSSI_offset
else:                RSSI_dBm =  RSSI_dec       / 2 - RSSI_offset
```

`RSSI_offset` = **74 dB** at 433 MHz, for every data rate TI measured
(1.2 / 38.4 / 250 / 500 kBaud) [p.45].

Notes:

- The value is only valid a little while after entering RX; see TI DN505 [p.44].
- With sync word detection enabled, RSSI **freezes** on sync detect until the chip
  re-enters RX. We disable sync detection, so RSSI stays live — which is exactly
  what makes the `scan` command possible [p.44].
- Update rate scales with the channel filter bandwidth and inversely with
  `AGCCTRL0.FILTER_LENGTH` [p.44]. A wide filter gives faster RSSI.
- If `PKTCTRL1.APPEND_STATUS` is set, packet RSSI is appended to the payload —
  irrelevant in async serial mode, which has no payload.

Sensitivity for reference: **−116 dBm at 0.6 kBaud, 433 MHz** [p.2]. A remote
pressed in the same room will land somewhere around −40 to −70 dBm.

### Close-in reception

If you sniff with the remote 20 cm from the antenna, the front end saturates and
captures look like noise. `FIFOTHR.CLOSE_IN_RX` adds 0/6/12/18 dB of RX
attenuation [p.72] — see TI DN010. Alternatively, just stand further away.
