# CC1101 — pins, SPI and control interface

Source: TI SWRS061I §5, §6, §10, §11, §19.1.

## Pinout

The bare chip is QFN-20 [p.21]. The pins that reach the header on a breakout
module are marked ★.

| # | Name | Type | Notes |
|---|---|---|---|
| 1 | SCLK ★ | in | SPI clock |
| 2 | SO (GDO1) ★ | out | SPI MISO. Doubles as GDO1 when CSn is high; default 3-state |
| 3 | GDO2 ★ | out | General digital output: test signals, FIFO status, CCA, divided XOSC clock, **serial RX data** |
| 4 | DVDD | pwr | 1.8–3.6 V digital supply |
| 5 | DCOUPL | pwr | 1.6–2.0 V regulator output, decoupling only — **never** feed other devices from it |
| 6 | GDO0 (ATEST) ★ | **I/O** | Same outputs as GDO2, **plus serial TX data input**, plus analog temperature sensor |
| 7 | CSn ★ | in | SPI chip select, active low |
| 8 | XOSC_Q1 | ana | Crystal, or external clock in |
| 9, 11, 14, 15 | AVDD | pwr | 1.8–3.6 V analog supply |
| 10 | XOSC_Q2 | ana | Crystal |
| 12 | RF_P | RF | LNA input (RX) / PA output (TX), positive |
| 13 | RF_N | RF | LNA input (RX) / PA output (TX), negative |
| 16 | GND | gnd | Analog ground |
| 17 | RBIAS | ana | External bias resistor sets the reference current |
| 18 | DGUARD | pwr | Digital noise isolation supply |
| 19 | GND ★ | gnd | Digital ground |
| 20 | SI ★ | in | SPI MOSI |

**GDO0 is the pin that matters for this project.** It is the only GDO that is
bidirectional: in asynchronous and synchronous serial mode it is the serial TX
data *input* while transmitting [p.34], and it can be configured as the serial RX
data *output* while receiving. One wire, both directions.

## The 4-wire SPI

`SI`, `SO`, `SCLK`, `CSn` [p.34]. Every transfer starts with a **header byte**:

```
bit 7   6    5 4 3 2 1 0
    R/W  BURST  ADDRESS[5:0]
```

- `R/W` = 1 to read, 0 to write.
- `BURST` = 1 auto-increments the address after each byte; terminate by raising
  CSn [p.31].
- For addresses `0x30–0x3D` the burst bit changes meaning entirely: burst=1
  selects the **status register**, burst=0 selects the **command strobe** at that
  address. This is why status registers cannot be burst-read and must be read one
  at a time [p.32].

Timing limits [p.30]: SCLK ≤ 10 MHz with a 100 ns gap inserted between address
and data, ≤ 9 MHz for single access with no gap, ≤ 6.5 MHz for burst access.
**Use 4–5 MHz** on an ESP32 and stop thinking about it.

### Chip status byte

Every header byte, data byte and command strobe clocks a status byte back out on
SO [p.31]:

```
bit 7    CHIP_RDYn                 0 = power + crystal stable. Must be 0.
bits 6:4 STATE[2:0]                000 IDLE  001 RX  010 TX  011 FSTXON
                                   100 CALIBRATE  101 SETTLING
                                   110 RXFIFO_OVERFLOW  111 TXFIFO_UNDERFLOW
bits 3:0 FIFO_BYTES_AVAILABLE[3:0]  bytes readable in RX FIFO (on reads) or
                                    writable in TX FIFO (on writes); 15 = "15 or more"
```

This byte is the cheapest sanity check you have. If bit 7 never goes low, the
crystal is not running — suspect power or a dead module. If STATE never leaves
`000` after an `SRX` strobe, the PLL is not settling.

> **Read hazard** [p.32]: registers updated by hardware (`MARCSTATE`, `TXBYTES`,
> `RXBYTES`, `PKTSTATUS`, and the status byte itself) can return a corrupt value
> on any single read — roughly 80 ppm at max data rate. Read twice and compare if
> a value is load-bearing. Prefer GDO interrupts over SPI polling; high-rate
> polling also degrades RX sensitivity [p.42].

## Command strobes

Single header byte, no data [p.67]. Address range `0x30–0x3D` with burst=0.

| Addr | Strobe | Effect |
|---|---|---|
| 0x30 | `SRES` | Reset chip |
| 0x31 | `SFSTXON` | Enable + calibrate synthesiser; wait state for fast RX↔TX turnaround |
| 0x32 | `SXOFF` | Turn off crystal oscillator |
| 0x33 | `SCAL` | Calibrate synthesiser and turn it off (works from IDLE even with `FS_AUTOCAL=0`) |
| 0x34 | `SRX` | Enter RX (calibrates first if coming from IDLE and `FS_AUTOCAL=1`) |
| 0x35 | `STX` | Enter TX from IDLE. From RX with CCA enabled, only if the channel is clear |
| 0x36 | `SIDLE` | Leave RX/TX, turn off synthesiser, exit WOR |
| 0x38 | `SWOR` | Start wake-on-radio polling |
| 0x39 | `SPWD` | Power down when CSn goes high |
| 0x3A | `SFRX` | Flush RX FIFO — only legal in IDLE or RXFIFO_OVERFLOW |
| 0x3B | `SFTX` | Flush TX FIFO — only legal in IDLE or TXFIFO_UNDERFLOW |
| 0x3C | `SWORRST` | Reset WOR real-time clock to Event1 |
| 0x3D | `SNOP` | No-op — the polite way to fetch the status byte |

Strobes execute immediately, **except** `SPWD`, `SWOR` and `SXOFF`, which execute
when CSn goes high [p.32].

Two behaviours that cause real bugs:

- After `SRES`, wait for SO to go low again before issuing the next header
  byte [p.32].
- `SIDLE` **discards every pending strobe until IDLE is actually reached** [p.32].
  Issuing `SIDLE` then immediately `SRX` can silently lose the `SRX`. Poll
  `MARCSTATE` (0x35, status) until it reads `0x01` (IDLE) before strobing again.

## FIFO and PATABLE access

Both FIFOs are 64 bytes and share SPI address `0x3F` [p.32–33]:

| Header | Access |
|---|---|
| `0x3F` | single-byte write → TX FIFO |
| `0x7F` | burst write → TX FIFO |
| `0xBF` | single-byte read → RX FIFO |
| `0xFF` | burst read → RX FIFO |

**This project never touches the FIFOs.** Asynchronous serial mode bypasses the
packet handler and the FIFOs entirely [p.63].

`PATABLE` is at `0x3E`: an 8-entry power table with an internal index counter that
increments on each byte and resets when CSn goes high [p.33]. To write anything
beyond entry 0 you must use burst mode. Everything except entry 0 is lost when the
chip enters SLEEP [p.59].

## Reset

Power-on reset is automatic; the internal sequence is complete when `CHIP_RDYn`
(SO, once CSn is low) goes low [p.51].

Manual power-up sequence, needed **only just after the supply is first turned
on** [p.51]:

1. `SCLK = 1`, `SI = 0` — avoids accidentally triggering pin-control mode.
2. Strobe CSn low, then high.
3. Hold CSn high for at least 40 µs.
4. Pull CSn low, wait for SO to go low (`CHIP_RDYn`).
5. Send `SRES` on SI.
6. When SO goes low again, reset is done and the chip is IDLE.

After that, a bare `SRES` strobe is enough.

## Optional 3-pin radio control

`MCSM0.PIN_CTRL_EN` reuses CSn/SCLK/SI to command SLEEP/IDLE/RX/TX directly with
pin levels [p.34]. We do not use it — but note step 1 of the reset sequence exists
precisely to avoid tripping it by accident.
