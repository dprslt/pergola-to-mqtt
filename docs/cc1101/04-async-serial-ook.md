# CC1101 — asynchronous serial mode

Source: TI SWRS061I §27.1, §11.2, §15. This is the mode the whole project rests
on, so it gets its own page.

## What it is

Normally the CC1101 is a packet radio: it finds a preamble, matches a sync word,
optionally checks an address and a CRC, de-whitens, and hands you bytes through a
64-byte FIFO. A remote control has none of those things, so all of that machinery
would sit there and never trigger.

Asynchronous serial mode switches it off. Set
`PKTCTRL0.PKT_FORMAT = 3` [p.74] and:

- **In RX**, the chip puts the raw demodulated baseband on a GDO pin — configure
  that pin with `GDOx_CFG = 0x0D` ("Serial Data Output, used for asynchronous
  serial mode") [p.62]. No data decision is made on-chip; the raw signal comes
  out and *you* decide what is a `1` [p.63].
- **In TX**, GDO0 becomes a data *input*. Whatever you wiggle on it is keyed
  straight onto the carrier [p.63, p.34].

So the ESP32 sees the remote's on-air waveform as a GPIO that goes high and low,
and it can transmit by driving that same GPIO. Timing the edges with
`micros()` is all the "protocol decoding" the radio needs to do.

## What you give up

Enabling async serial disables [p.63]:

- packet handling hardware and FIFO buffering
- the data whitener
- the interleaver and FEC
- **Manchester encoding/decoding** — do it in software if needed
- MSK modulation is not supported at all in this mode

None of which we want.

## The three gotchas

### 1. Glitches

> *"In asynchronous serial mode there will be glitches of 37 – 38.5 ns duration
> (1/XOSC) occurring infrequently and with random periods."* [p.63]

TI's suggested fix is an RC low-pass on the data line between CC1101 and MCU —
their worked example for 2.4 kBaud is 1 kΩ + 2.7 nF, giving a 59 kHz cut-off:
high enough to pass the data, low enough to kill the glitch.

In practice an ESP32 GPIO interrupt may or may not even see a 38 ns pulse, and if
it does you get a spurious edge pair. Our firmware handles this in software
instead: any pulse shorter than a configurable threshold (default 50 µs) is
treated as a spike, and the pulse before and after it are merged. That is cheaper
than soldering and easier to tune. The RC filter remains the belt-and-braces
option if captures stay noisy.

### 2. Jitter

The modulator samples the async input **8× faster than the programmed data
rate** [p.63]. Two consequences:

- **Transmitting**: the error in your bit period must be under ⅛ of a bit period.
  At 4.8 kBaud that is 208 µs / 8 = 26 µs of slack — trivially met by
  `delayMicroseconds()` on an ESP32, but *not* if you let WiFi or a long ISR
  stall the loop. Disable WiFi or pin transmission to a core while sending.
- **Receiving**: the output is time-discretised into 8 samples per bit, so tolerate
  ±⅛ of a bit period of jitter on every edge [p.63]. At 4.8 kBaud that is ±26 µs.
  Do not expect a captured 400 µs pulse to read as exactly 400 µs — it will
  wander, which is why the analyser clusters pulse widths instead of matching them
  exactly.

### 3. RX with no squelch

Because no data decision is made on-chip and there is no sync word to gate on, the
data line is **always** carrying something. With no transmitter nearby, the AGC
winds gain up and you get a stream of noise edges.

Three defences, in order of usefulness:

1. **Frame segmentation by silence.** A remote sends a burst, then goes quiet.
   Treat "no edge for N µs" (default 5 ms) as end-of-frame, and drop any frame
   with fewer than N edges (default 20). Noise rarely produces 40 clean edges
   followed by a clean gap.
2. **`AGCCTRL0.FILTER_LENGTH`** — the OOK decision boundary [p.87]. Raise it
   (`0x92` = 12 dB, `0x93` = 16 dB) and the demodulator demands a bigger
   amplitude gap before calling something a `1`. This is the single most effective
   knob against noise.
3. **`AGCCTRL2.MAX_LNA_GAIN` / `MAGN_TARGET`** — cap the gain so the front end
   stops amplifying nothing into something.

Our firmware tunes 1 over serial and lets you poke 2 and 3 with the `w` register
write command, so you can dial it in live.

## Alternative: RX_HARD_DATA + RX_SYMBOL_TICK

There is a second raw-output option: configure GDO pins for `RX_HARD_DATA[1]`
(`0x16`) and `RX_SYMBOL_TICK` (`0x1D`) [p.62, p.64]. `RX_SYMBOL_TICK` goes high
for half a symbol period each time a new symbol appears, and `RX_HARD_DATA[1]`
carries the hard decision for 2-ary formats. It works in both synchronous and
asynchronous interfaces.

We do not use it: it makes the chip's symbol clock authoritative, which is exactly
the assumption we cannot make about an unknown remote. Raw edge timing is more
honest. Worth remembering if edge timing ever proves too noisy.

## Wiring implication

One pin does both directions [p.34]:

```
ESP32 GPIO  ──────────────  CC1101 GDO0
             RX: CC1101 drives, ESP32 reads  (IOCFG0 = 0x0D)
             TX: ESP32 drives, CC1101 reads  (IOCFG0 = 0x2E, high-Z)
```

**Set `IOCFG0` to high-Z (`0x2E`) before driving the pin from the ESP32.** If both
sides drive it you are shorting two outputs together. Our driver's
`beginTransmit()` / `beginReceive()` handle the handover, including flipping the
ESP32 pin between `INPUT` and `OUTPUT`.

## Sequence to receive

```
SRES                          reset
write config registers        (IOCFG0=0x0D, PKTCTRL0=0x32, MDMCFG2=0x30, …)
write PATABLE
SIDLE, wait MARCSTATE==0x01
SCAL (or rely on FS_AUTOCAL)
SRX
→ CC1101 now drives GDO0 with demodulated baseband, forever
   (MCSM1.RXOFF_MODE=3 keeps it in RX)
```

## Sequence to transmit

```
SIDLE, wait MARCSTATE==0x01
IOCFG0 = 0x2E                 release the pin
pinMode(GDO0, OUTPUT); digitalWrite(GDO0, LOW)
STX                           carrier on, keyed by the pin
  … drive the pulse train with delayMicroseconds() …
digitalWrite(GDO0, LOW)
SIDLE
IOCFG0 = 0x0D                 give the pin back
pinMode(GDO0, INPUT)
SRX                           back to listening
```

Keep bursts short: EU EN 300 220 allows 433.05–434.79 MHz licence-free but caps
duty cycle at 10% [p.64 notes the regulations, the 10% figure is from EN 300 220
itself]. A remote's own burst is a few tens of milliseconds repeated 3–10 times;
matching that is both legal and more likely to be accepted by the receiver.
