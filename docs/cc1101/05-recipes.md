# CC1101 — register recipes

Copy-pasteable configurations, with every non-default byte justified. All assume
**f_xosc = 26 MHz**.

These are the exact values `firmware/sniffer` writes — see
`firmware/sniffer/include/cc1101_config.h`. If you change one there, change it
here.

## Recipe 1 — 433.92 MHz OOK receive, raw pulse output

The sniffer's default. Async serial mode, no sync word, no packet handler; the
demodulated baseband appears on GDO0.

| Addr | Reg | Value | Why |
|---|---|---|---|
| 0x00 | IOCFG2 | `0x2E` | high-Z; GDO2 unused |
| 0x01 | IOCFG1 | `0x2E` | 3-state (shared with SO) |
| 0x02 | IOCFG0 | `0x0D` | **serial data output, async serial mode** |
| 0x03 | FIFOTHR | `0x47` | `ADC_RETENTION=1` (RX BW < 325 kHz), `FIFO_THR=7`, no RX attenuation |
| 0x06 | PKTLEN | `0xFF` | unused in async mode |
| 0x07 | PKTCTRL1 | `0x04` | `PQT=0`, `APPEND_STATUS=1`; both inert here |
| 0x08 | PKTCTRL0 | `0x32` | **`PKT_FORMAT=3` async serial**, whitening off, CRC off, infinite length |
| 0x0A | CHANNR | `0x00` | single channel |
| 0x0B | FSCTRL1 | `0x06` | IF = 152 kHz, suits a 203 kHz channel filter |
| 0x0C | FSCTRL0 | `0x00` | no frequency offset |
| 0x0D | FREQ2 | `0x10` | 433.920 MHz |
| 0x0E | FREQ1 | `0xB0` | " |
| 0x0F | FREQ0 | `0x71` | " |
| 0x10 | MDMCFG4 | `0x87` | channel BW 203 kHz (`E=10,M=00`), `DRATE_E=7` |
| 0x11 | MDMCFG3 | `0x83` | `DRATE_M=131` → 4.798 kBaud |
| 0x12 | MDMCFG2 | `0x30` | **ASK/OOK**, no Manchester, **`SYNC_MODE=0`** |
| 0x13 | MDMCFG1 | `0x22` | reset; channel spacing unused |
| 0x14 | MDMCFG0 | `0xF8` | reset |
| 0x15 | DEVIATN | `0x00` | no effect in OOK |
| 0x16 | MCSM2 | `0x07` | reset; no RX timeout |
| 0x17 | MCSM1 | `0x3C` | **`RXOFF_MODE=3` — stay in RX forever** |
| 0x18 | MCSM0 | `0x18` | `FS_AUTOCAL=1` (IDLE→RX/TX), `PO_TIMEOUT=2` |
| 0x19 | FOCCFG | `0x16` | inert in OOK |
| 0x1A | BSCFG | `0x6C` | reset; no bit sync in async mode |
| 0x1B | AGCCTRL2 | `0x07` | max DVGA + max LNA gain, `MAGN_TARGET=42 dB` |
| 0x1C | AGCCTRL1 | `0x00` | carrier-sense thresholds disabled |
| 0x1D | AGCCTRL0 | `0x91` | **OOK decision boundary 8 dB**, medium hysteresis |
| 0x21 | FREND1 | `0x56` | RX front-end currents for low data rate |
| 0x22 | FREND0 | `0x11` | **`PA_POWER=1`** → OOK `1` = PATABLE[1], `0` = PATABLE[0] |
| 0x23 | FSCAL3 | `0xE9` | synthesiser calibration |
| 0x24 | FSCAL2 | `0x2A` | " |
| 0x25 | FSCAL1 | `0x00` | " |
| 0x26 | FSCAL0 | `0x1F` | " |
| 0x2C | TEST2 | `0x81` | matches `ADC_RETENTION=1` |
| 0x2D | TEST1 | `0x35` | matches `ADC_RETENTION=1` |
| 0x2E | TEST0 | `0x09` | `VCO_SEL_CAL_EN=0`, fixed frequency |

`PATABLE` (burst write to `0x7E`): `{0x00, 0xC0, 0, 0, 0, 0, 0, 0}` — index 0 is
carrier off, index 1 is +10 dBm at 433 MHz. Drop index 1 to `0x60` (0 dBm) if your
board browns out on transmit.

WOR registers (`0x1E`–`0x20`) and `SYNC1`/`SYNC0`/`ADDR` are left at reset — nothing
here uses them.

### Bring-up sequence

```
manual reset (see 01-pins-and-interface.md)
read VERSION (0xF1)  →  expect 0x14. If 0x00 or 0xFF, stop and fix the wiring.
burst-write 0x00..0x2E from the table
burst-write PATABLE
SIDLE ; poll MARCSTATE until 0x01
SRX
```

## Recipe 2 — the same, for OOK transmit

No new register values: the RX config already sets `MOD_FORMAT=3`,
`PKT_FORMAT=3` and `FREND0.PA_POWER=1`. Only the GDO0 direction changes.

```
SIDLE ; poll MARCSTATE until 0x01
IOCFG0 = 0x2E                 CC1101 releases GDO0 (high-Z)
ESP32: pinMode(GDO0, OUTPUT), digitalWrite(GDO0, LOW)
STX                           carrier now keyed by the GDO0 level
  drive the pulse train
digitalWrite(GDO0, LOW)
SIDLE ; poll MARCSTATE until 0x01
IOCFG0 = 0x0D                 hand GDO0 back
ESP32: pinMode(GDO0, INPUT)
SRX
```

Getting the handover wrong means CC1101 and ESP32 both drive the same net. It will
usually survive it, but the transmission will be garbage.

## Recipe 3 — frequency scan

To find a remote's real carrier when you don't know it. Reuses recipe 1 and only
walks `FREQ`:

```
for f in 433.00 .. 434.80 step 0.05 MHz:
    SIDLE ; poll MARCSTATE until 0x01
    write FREQ2/1/0 for f
    SRX
    delay ~5 ms          (let AGC and RSSI settle)
    read RSSI (0xF4), convert with offset 74 dB
    record max over a ~1 s dwell while the button is held
```

Hold the remote button down through the whole sweep and the peak is your answer.
Run it once with the remote silent to get a noise floor to compare against.

Remember: frequency registers must only change in IDLE [p.57], and RSSI needs a
moment to become valid after entering RX [p.44].

## Tuning cheat-sheet

When captures are wrong, change one of these — in this order.

| Symptom | Knob | Try |
|---|---|---|
| No frames at all | wiring, then `VERSION` | must read `0x14` |
| No frames, `VERSION` fine | frequency | run `scan`; remote may be at 433.42 |
| No frames, frequency right | `AGCCTRL0` decision boundary | `0x90` (4 dB) — more sensitive |
| Constant garbage frames | `AGCCTRL0` decision boundary | `0x92`/`0x93` (12/16 dB) |
| Constant garbage frames | `AGCCTRL2` gain cap | `0x03`, `0x43`, `0x83` |
| Frames truncated mid-burst | inter-frame gap | raise `gap` (default 5000 µs) |
| Several bursts merged into one | inter-frame gap | lower `gap` |
| Pulse widths wander a lot | data rate | try 2.4 or 9.6 kBaud |
| Pulse widths wander a lot | channel BW | narrow to 102 or 58 kHz |
| 1–2 µs pulses in the capture | deglitch threshold | raise `min` (default 50 µs), or fit TI's RC filter |
| Only works with remote very close | RX attenuation | `FIFOTHR.CLOSE_IN_RX` — or move away |

Every one of these is reachable from the sniffer's serial CLI without reflashing.
`w <addr> <val>` writes any register; `x <addr>` reads one back.
