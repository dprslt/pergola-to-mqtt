# CC1101 — register map

Source: TI SWRS061I §29, Tables 43–45.

## SPI address space

Add these offsets to the base address to form the header byte [p.70]:

| | Write | Read |
|---|---|---|
| single byte | `+0x00` | `+0x80` |
| burst | `+0x40` | `+0xC0` |

`0x00–0x2E` are configuration registers (read/write, burst allowed).
`0x30–0x3D` are command strobes (burst bit 0) **or** status registers (burst bit 1).
`0x3E` is PATABLE, `0x3F` is the TX/RX FIFO.

## Configuration registers 0x00–0x2E

Reset values from [p.71–92]. "SLEEP" = value preserved across the SLEEP state.

| Addr | Name | Reset | SLEEP | Purpose |
|---|---|---|---|---|
| 0x00 | IOCFG2 | 0x29 | yes | GDO2 pin function |
| 0x01 | IOCFG1 | 0x2E | yes | GDO1/SO pin function |
| 0x02 | IOCFG0 | 0x3F | yes | GDO0 pin function |
| 0x03 | FIFOTHR | 0x07 | yes | FIFO thresholds, RX attenuation, ADC retention |
| 0x04 | SYNC1 | 0xD3 | yes | Sync word high byte |
| 0x05 | SYNC0 | 0x91 | yes | Sync word low byte |
| 0x06 | PKTLEN | 0xFF | yes | Packet length / max length |
| 0x07 | PKTCTRL1 | 0x04 | yes | PQT, autoflush, append status, address check |
| 0x08 | PKTCTRL0 | 0x45 | yes | **Whitening, packet format, CRC, length mode** |
| 0x09 | ADDR | 0x00 | yes | Device address for filtering |
| 0x0A | CHANNR | 0x00 | yes | Channel number |
| 0x0B | FSCTRL1 | 0x0F | yes | **IF frequency** |
| 0x0C | FSCTRL0 | 0x00 | yes | Frequency offset (2's complement) |
| 0x0D | FREQ2 | 0x1E | yes | **Carrier word [23:16]** |
| 0x0E | FREQ1 | 0xC4 | yes | **Carrier word [15:8]** |
| 0x0F | FREQ0 | 0xEC | yes | **Carrier word [7:0]** |
| 0x10 | MDMCFG4 | 0x8C | yes | **Channel bandwidth exp/mant, data rate exponent** |
| 0x11 | MDMCFG3 | 0x22 | yes | **Data rate mantissa** |
| 0x12 | MDMCFG2 | 0x02 | yes | **DC filter, modulation format, Manchester, sync mode** |
| 0x13 | MDMCFG1 | 0x22 | yes | FEC, preamble count, channel spacing exponent |
| 0x14 | MDMCFG0 | 0xF8 | yes | Channel spacing mantissa |
| 0x15 | DEVIATN | 0x47 | yes | FSK deviation — **no effect in OOK/ASK** |
| 0x16 | MCSM2 | 0x07 | yes | RX timeout config |
| 0x17 | MCSM1 | 0x30 | yes | **CCA mode, RXOFF_MODE, TXOFF_MODE** |
| 0x18 | MCSM0 | 0x04 | yes | **FS_AUTOCAL, PO_TIMEOUT, pin control, XOSC force** |
| 0x19 | FOCCFG | 0x36 | yes | Frequency offset compensation — **not supported in OOK/ASK** |
| 0x1A | BSCFG | 0x6C | yes | Bit synchronisation |
| 0x1B | AGCCTRL2 | 0x03 | yes | **Max DVGA/LNA gain, magnitude target** |
| 0x1C | AGCCTRL1 | 0x40 | yes | **Carrier-sense thresholds** |
| 0x1D | AGCCTRL0 | 0x91 | yes | **Hysteresis, wait time, freeze, OOK decision boundary** |
| 0x1E | WOREVT1 | 0x87 | yes | Wake-on-radio Event0 high byte |
| 0x1F | WOREVT0 | 0x6B | yes | Wake-on-radio Event0 low byte |
| 0x20 | WORCTRL | 0xF8 | yes | Wake-on-radio control |
| 0x21 | FREND1 | 0xB6 | yes | RX front-end currents |
| 0x22 | FREND0 | 0x10 | yes | **TX LO buffer current, PA_POWER index** |
| 0x23 | FSCAL3 | 0xA9 | yes | Synthesiser calibration / charge pump |
| 0x24 | FSCAL2 | 0x0A | yes | Synthesiser calibration / VCO current |
| 0x25 | FSCAL1 | 0x20 | yes | Synthesiser calibration / VCO capacitor array |
| 0x26 | FSCAL0 | 0x0D | yes | Synthesiser calibration control |
| 0x27 | RCCTRL1 | 0x41 | yes | RC oscillator config |
| 0x28 | RCCTRL0 | 0x00 | yes | RC oscillator config |
| 0x29 | FSTEST | 0x59 | no | Test only — do not write |
| 0x2A | PTEST | 0x7F | no | 0xBF enables the temperature sensor in IDLE |
| 0x2B | AGCTEST | 0x3F | no | Test only — do not write |
| 0x2C | TEST2 | 0x88 | no | SmartRF-supplied value |
| 0x2D | TEST1 | 0x31 | no | SmartRF-supplied value |
| 0x2E | TEST0 | 0x0B | no | SmartRF-supplied value; bit 1 = `VCO_SEL_CAL_EN` |

## Status registers 0x30–0x3D

Read with burst bit set — i.e. header `0xF0 + offset` [p.69]. Read-only.

| Addr | Read hdr | Name | Contents |
|---|---|---|---|
| 0x30 | 0xF0 | PARTNUM | Part number — reads `0x00` |
| 0x31 | 0xF1 | VERSION | Chip version — typically `0x14`; `0x00`/`0xFF` means **SPI is not working** |
| 0x32 | 0xF2 | FREQEST | Frequency offset estimate. **Always 0 in ASK/OOK** [p.92] |
| 0x33 | 0xF3 | LQI | bit 7 = CRC OK, bits 6:0 = link quality |
| 0x34 | 0xF4 | RSSI | Signed RSSI, ½ dB steps — see [03](03-frequency-and-modulation.md#rssi) |
| 0x35 | 0xF5 | MARCSTATE | Main state machine state (table below) |
| 0x36 | 0xF6 | WORTIME1 | WOR timer high byte |
| 0x37 | 0xF7 | WORTIME0 | WOR timer low byte |
| 0x38 | 0xF8 | PKTSTATUS | Live GDO levels + packet status (table below) |
| 0x39 | 0xF9 | VCO_VC_DAC | PLL calibration, test only |
| 0x3A | 0xFA | TXBYTES | bit 7 underflow, bits 6:0 count |
| 0x3B | 0xFB | RXBYTES | bit 7 overflow, bits 6:0 count |
| 0x3C | 0xFC | RCCTRL1_STATUS | Last RC calibration result |
| 0x3D | 0xFD | RCCTRL0_STATUS | Last RC calibration result |

**`VERSION` (0xF1) is the go-to "is my wiring right?" probe.** It should read
`0x14` (older parts `0x04`). All-zeros or all-ones means MISO is not getting
back, CSn is wrong, or the module has no power.

### MARCSTATE values [p.93]

| Val | Name | | Val | Name |
|---|---|---|---|---|
| 0x00 | SLEEP | | 0x0D | **RX** |
| 0x01 | **IDLE** | | 0x0E | RX_END |
| 0x02 | XOFF | | 0x0F | RX_RST |
| 0x03 | VCOON_MC | | 0x10 | TXRX_SWITCH |
| 0x04 | REGON_MC | | 0x11 | **RXFIFO_OVERFLOW** |
| 0x05 | MANCAL | | 0x12 | FSTXON |
| 0x06 | VCOON | | 0x13 | **TX** |
| 0x07 | REGON | | 0x14 | TX_END |
| 0x08 | STARTCAL | | 0x15 | RXTX_SWITCH |
| 0x09 | BWBOOST | | 0x16 | **TXFIFO_UNDERFLOW** |
| 0x0A | FS_LOCK | | | |
| 0x0B | IFADCON | | | |
| 0x0C | ENDCAL | | | |

SLEEP and XOFF can never be read back, because pulling CSn low to do the read
already moves the chip to IDLE [p.93].

### PKTSTATUS bits [p.94]

| Bit | Name | Meaning |
|---|---|---|
| 7 | CRC_OK | Last CRC matched |
| 6 | CS | Carrier sense (cleared on entering IDLE) |
| 5 | PQT_REACHED | Preamble quality reached |
| 4 | CCA | Channel clear |
| 3 | SFD | Sync word received, until end of packet |
| 2 | GDO2 | Live GDO2 level (non-inverted, regardless of `GDO2_INV`) |
| 0 | GDO0 | Live GDO0 level (non-inverted, regardless of `GDO0_INV`) |

---

## Bit-level detail for the registers this project sets

### IOCFG0 / IOCFG1 / IOCFG2 [p.71]

```
IOCFG2:  bit 6 GDO2_INV        bits 5:0 GDO2_CFG    reset 0x29 = CHIP_RDYn
IOCFG1:  bit 7 GDO_DS (drive strength, all GDO pins)
         bit 6 GDO1_INV        bits 5:0 GDO1_CFG    reset 0x2E = 3-state
IOCFG0:  bit 7 TEMP_SENSOR_ENABLE
         bit 6 GDO0_INV        bits 5:0 GDO0_CFG    reset 0x3F = CLK_XOSC/192
```

GDO0 ships as a 135–141 kHz clock output so it can clock a single-crystal MCU
[p.61]. **Turn it off in init** — TI explicitly recommends this for RF
performance [p.71].

Selected `GDOx_CFG` values [p.62]:

| Value | Signal |
|---|---|
| `0x00` | RX FIFO at or above threshold |
| `0x01` | RX FIFO at or above threshold, or end of packet |
| `0x06` | Sync word sent/received → de-asserts at end of packet |
| `0x07` | Packet received with CRC OK |
| `0x08` | Preamble quality reached |
| `0x09` | Clear channel assessment |
| `0x0A` | PLL lock detector |
| `0x0B` | Serial clock (synchronous serial mode) |
| `0x0C` | Serial synchronous data output |
| **`0x0D`** | **Serial data output — asynchronous serial mode. This is the one we use.** |
| `0x0E` | Carrier sense |
| `0x0F` | CRC_OK |
| `0x16` / `0x17` | RX_HARD_DATA[1] / [0] |
| `0x1D` | RX_SYMBOL_TICK |
| `0x29` | CHIP_RDYn |
| `0x2B` | XOSC_STABLE |
| `0x2E` | High impedance (3-state) |
| `0x2F` | Hardwired 0 (or 1 with `GDOx_INV=1`) — for external LNA/PA switching |
| `0x30`–`0x3F` | `CLK_XOSC` divided by 1 … 192 |

Two constraints [p.62]:

- Only **one** GDO pin may output `CLK_XOSC/n` at a time; the other two must then
  be configured below `0x30`.
- With `GDOx_CFG < 0x20`, GDO0/GDO2 are hardwired low (high if inverted) in SLEEP
  until `CHIP_RDYn` falls. With `GDOx_CFG ≥ 0x20` the pins keep working in SLEEP.

### PKTCTRL0 (0x08) [p.74]

```
bit 6   WHITE_DATA          1 = whitening on (reset)
bits 5:4 PKT_FORMAT         0 normal/FIFO   1 synchronous serial
                            2 random TX (PN9 test)   3 ASYNCHRONOUS SERIAL
bit 2   CRC_EN              1 = CRC on (reset)
bits 1:0 LENGTH_CONFIG      0 fixed (PKTLEN)   1 variable (first byte)
                            2 infinite   3 reserved
```

For raw OOK sniffing we want `PKT_FORMAT=3`, `WHITE_DATA=0`, `CRC_EN=0`. Length
mode is irrelevant in async mode; SmartRF Studio emits `0x32` (infinite length),
which is what the [recipes](05-recipes.md) use.

### PKTCTRL1 (0x07) [p.73]

```
bits 7:5 PQT[2:0]           preamble quality threshold; 0 = always accept sync
bit 3   CRC_AUTOFLUSH
bit 2   APPEND_STATUS       1 = append RSSI + LQI bytes after payload (reset)
bits 1:0 ADR_CHK            0 none  1 address  2 address+0x00  3 address+0x00+0xFF
```

### FSCTRL1 (0x0B) [p.75]

`bits 4:0 FREQ_IF` — IF frequency, `f_IF = (f_xosc / 2^10) · FREQ_IF`.
Reset `0x0F` → 381 kHz at 26 MHz.

### MDMCFG4 (0x10) / MDMCFG3 (0x11) [p.76]

```
MDMCFG4: bits 7:6 CHANBW_E   bits 5:4 CHANBW_M   bits 3:0 DRATE_E
MDMCFG3: bits 7:0 DRATE_M
```

Formulas in [03-frequency-and-modulation.md](03-frequency-and-modulation.md).

### MDMCFG2 (0x12) [p.77]

```
bit 7   DEM_DCFILT_OFF      0 = DC blocking on (better sensitivity)
bits 6:4 MOD_FORMAT         0 2-FSK   1 GFSK   3 ASK/OOK   4 4-FSK   7 MSK
bit 3   MANCHESTER_EN
bits 2:0 SYNC_MODE          0 no preamble/sync        1 15/16 bits
                            2 16/16   3 30/32
                            4 no preamble/sync + carrier sense
                            5 15/16 + CS   6 16/16 + CS   7 30/32 + CS
```

`MDMCFG2 = 0x30` is OOK with no sync word — exactly what a dumb remote needs.

### MDMCFG1 (0x13) / MDMCFG0 (0x14) [p.78]

```
MDMCFG1: bit 7 FEC_EN   bits 6:4 NUM_PREAMBLE (2,3,4,6,8,12,16,24 bytes)
         bits 1:0 CHANSPC_E
MDMCFG0: bits 7:0 CHANSPC_M
```

Irrelevant here — we use a single channel, `CHANNR = 0`.

### MCSM1 (0x17) [p.81]

```
bits 5:4 CCA_MODE      0 always  1 RSSI below threshold
                       2 unless receiving  3 RSSI below threshold unless receiving
bits 3:2 RXOFF_MODE    next state after RX: 0 IDLE  1 FSTXON  2 TX  3 STAY IN RX
bits 1:0 TXOFF_MODE    next state after TX: 0 IDLE  1 FSTXON  2 stay TX  3 RX
```

A sniffer wants `RXOFF_MODE = 3` (stay in RX) so it never silently drops out.
`MCSM1 = 0x3C`.

### MCSM0 (0x18) [p.82]

```
bits 5:4 FS_AUTOCAL   0 never (manual SCAL)   1 IDLE→RX/TX   2 RX/TX→IDLE   3 every 4th
bits 3:2 PO_TIMEOUT   0:1 count (~2.3 µs)  1:16 (~37 µs)  2:64 (~150 µs)  3:256 (~600 µs)
bit 1   PIN_CTRL_EN
bit 0   XOSC_FORCE_ON
```

`MCSM0 = 0x18` → autocal on IDLE→RX/TX, `PO_TIMEOUT = 2`. TI recommends
`PO_TIMEOUT` 2 or 3 when the crystal is off during power-down [p.82].

### AGCCTRL2 (0x1B) [p.85]

```
bits 7:6 MAX_DVGA_GAIN   0 all gains … 3 top 3 gain settings unusable
bits 5:3 MAX_LNA_GAIN    0 max gain … 7 ~17.1 dB below max
bits 2:0 MAGN_TARGET     0:24 dB 1:27 2:30 3:33 4:36 5:38 6:40 7:42 dB
```

### AGCCTRL1 (0x1C) [p.86]

```
bit 6   AGC_LNA_PRIORITY
bits 5:4 CARRIER_SENSE_REL_THR   0 disabled  1 +6 dB  2 +10 dB  3 +14 dB
bits 3:0 CARRIER_SENSE_ABS_THR   signed, dB relative to MAGN_TARGET; -8 (0b1000) = disabled
```

### AGCCTRL0 (0x1D) [p.87]

```
bits 7:6 HYST_LEVEL      0 none … 3 large
bits 5:4 WAIT_TIME       0:8  1:16  2:24  3:32 channel filter samples
bits 3:2 AGC_FREEZE      0 normal  1 freeze on sync  2 freeze analog  3 freeze both
bits 1:0 FILTER_LENGTH   FSK/MSK: averaging length 8/16/32/64 samples
                         ASK/OOK: DECISION BOUNDARY 4 / 8 / 12 / 16 dB
```

**Remember this one.** In OOK, `FILTER_LENGTH` is the amplitude gap the demodulator
needs between a `1` and a `0`. Too small and noise decodes as data; too large and
a weak remote is ignored. `0x91` (8 dB) is a good starting point; try `0x92`
(12 dB) if you get spurious frames and `0x90` (4 dB) if the remote is too weak.

### FREND0 (0x22) [p.89]

```
bits 5:4 LODIV_BUF_CURRENT_TX
bits 2:0 PA_POWER         index into PATABLE
```

In OOK/ASK: **PATABLE[0] is the power used for a `0`, and
PATABLE[PA_POWER] the power for a `1`** [p.89]. So OOK TX needs `PA_POWER ≥ 1`,
`PATABLE[0] = 0x00` (carrier off) and `PATABLE[1]` = the wanted output power.
`FREND0 = 0x11`.

### FSCAL3 (0x23) [p.89]

```
bits 7:6 FSCAL3[7:6]         SmartRF-supplied
bits 5:4 CHP_CURR_CAL_EN     0 disables the charge pump calibration stage
bits 3:0 FSCAL3[3:0]         calibration result: I_OUT = I0 · 2^(FSCAL3[3:0]/4)
```

`FSCAL3`, `FSCAL2` and `FSCAL1` hold calibration results. You can calibrate once
per frequency at start-up, cache these three, and restore them on a frequency
change instead of recalibrating — 75 µs instead of ~712 µs [p.64, p.89].

### FIFOTHR (0x03) [p.72]

```
bit 6   ADC_RETENTION   set to 1 before SLEEP if you need RX filter BW < 325 kHz on wake
bits 5:4 CLOSE_IN_RX    RX attenuation: 0 dB / 6 / 12 / 18 dB
bits 3:0 FIFO_THR       threshold, 0 → 4 RX bytes … 15 → 64 RX bytes
```

`CLOSE_IN_RX` is worth knowing about: if you sniff a remote from 20 cm away the
front end saturates, and 6–18 dB of attenuation fixes captures that otherwise look
like mush. See TI DN010.
