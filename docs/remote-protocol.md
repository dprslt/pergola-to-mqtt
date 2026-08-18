# The remote's protocol

Findings for **this** pergola's remote, measured on 2026-08-18 with the
`firmware/sniffer` 0.1.0 and an E07-M1101D V2.0. Wiring: [hardware.md](hardware.md).

## Summary

| Property | Value |
|---|---|
| Carrier | **315.000 MHz** — *not* 433 MHz |
| Modulation | OOK / ASK |
| Encoder chip | silkscreened **`2262`** (U1) |
| Encoding | 24 bits, one bit per pulse pair, 1:3 short:long |
| α (short pulse) | **351 µs** (measured 344–362) |
| Sync gap | 31α ≈ **10 880 µs** |
| Word length | 49 pulses — 48 data + 1 sync |
| Repeats per press | ~12 while the button is held |
| **Rolling code?** | **No. Fixed.** |

## Codes

20-bit address shared by every button, 4-bit button field, one bit per button:

| Button | Full word | Hex | Address | Nibble | Verified |
|---|---|---|---|---|---|
| open | `111100111010011101011000` | **`0xF3A758`** | `0xF3A75` | `1000` | ✅ moves the roof |
| stop | `111100111010011101010100` | **`0xF3A754`** | `0xF3A75` | `0100` | ✅ halts travel |
| close | `111100111010011101010010` | **`0xF3A752`** | `0xF3A75` | `0010` | ✅ closes the roof |

All three were confirmed on hardware on 2026-08-18 by **forging** the word from the
hex above and transmitting it — not by replaying a capture. Open, stop mid-travel,
close and stop again all behaved correctly.

`0xF3A751` (nibble `0001`) is the obvious fourth code — the PCB carries
unpopulated switch footprints and the pergola has a light bar — but it has **not**
been observed or tested. Do not transmit it casually; an unknown command on a
motor controller is not a free experiment.

## Waveform

Each press sends a sync pulse then 24 bits, and repeats the whole word ~12 times.

```
sync:   α high, 31α low                  (~351 µs high, ~10 880 µs low)
bit 0:  α high, 3α low                   (~351 µs high, ~1 053 µs low)
bit 1:  3α high, α low                   (~1 053 µs high, ~351 µs low)
```

The 1:3 ratio and the 31α sync are what identify the family. Measured long pulses
clustered at ~1 024 µs against a predicted 3α = 1 053 µs, and the observed sync
gap of ~10 700 µs against a predicted 10 880 µs.

### The chip marking does not match the encoding

U1 is silkscreened `2262`, which implies PT2262 tri-state encoding: 12 tri-state
symbols, each built from two half-bits pairing as `00`→0, `11`→1, `01`→F.

**The captured data is not tri-state.** Pairing the half-bits produces `10`
combinations, which is not a legal PT2262 symbol, and inverting the polarity does
not fix it. The data decodes cleanly as **24 independent bits** — the EV1527
scheme — so that is what is documented above.

Either it is a 2262-marked clone that encodes differently, or an EV1527-class die
in a 2262-marked package. It does not matter for this project: replay uses the
captured edge timings directly.

> **Bit polarity: confirmed.** Each pulse pair starts with the carrier-**on**
> period (`firstLevel = 1`). This began as an inference from the decode being
> self-consistent, but it is now verified the hard way: a forged non-inverted
> frame moved the roof on 2026-08-18. An inverted-polarity variant of `forge`
> existed while this was still in doubt; it was removed once the question was
> settled.

## How this was established

Worth recording, because the first several hours looked like a hardware fault:

1. `VERSION=0x14`, `MARCSTATE=0x0D` — the receiver was provably healthy.
2. Pressing the remote at 433.92 MHz produced nothing. A sweep of 433.0–434.8 MHz
   with the button held was indistinguishable from the noise floor, ruling out
   433.92 generic OOK **and** 433.42 Somfy RTS.
3. Opening the remote settled it: `Y1` is marked `R315`, and U1 is an OOK encoder.
   The receiver was never at fault — it was tuned 118 MHz away.
4. `firmware/sniffer` originally clamped tuning to 387–464 MHz. It now accepts the
   CC1101's lower band, 300–348 MHz, and snaps out of the 348–387 gap where the
   synthesiser cannot lock.
5. At 315.000 MHz the remote reads **−22.5 dBm**, roughly 70 dB above the floor.

**Lesson worth keeping: `gap` must be shorter than the sync gap.** The default
20 000 µs exceeded the 10 880 µs sync, so consecutive repeats merged into one
frame and the built-in repeat comparison straddled partial words, reporting
`identical=NO` on a code that is in fact perfectly fixed. `gap 5000` fixed it.

## Settings that work

Not the firmware defaults — apply these after any reset, since none are persisted:

```
freq 315.0
gap 5000
```

`minp 16` and `glitch 50` (both defaults) are fine. The AGC defaults
(`AGCCTRL0=0x91`, `AGCCTRL2=0x07`) are correct too: the earlier squelching of
`0x93` plus a gain cap was only ever needed to quieten the noise floor while
hunting on the wrong frequency, and at −22.5 dBm the real signal needs no help.

Sensitivity at 315 MHz is well below what the module manages at 433 — the matching
network and the antenna are both cut for 433 — but the remote only has to be
within a metre or so.

## Transmitting

**Forging works. Replaying a capture does not.** That is the opposite of what this
project originally assumed, and it cost an evening, so the detail matters.

Working recipe — this moved the roof:

```
freq 315.0
forge 0 F3A758 24 351      # open
forge 1 F3A754 24 351      # stop
tx 0 12 1                  # 12 repeats, 1 ms between words
```

`forge <slot> <hex> [bits] [alpha_us]` synthesises the word into a slot: sync
(α high, 31α low) then 24 bits — 50 pulses, 44.9 ms. `tx <slot> <rep> [ms]` sends
it. Use a **1 ms** inter-word gap, because the 31α sync low is already inside the
forged frame.

### Why replay fails

Two independent defects, either of which is fatal on its own:

1. **The sync ends up in the wrong place.** The sniffer segments frames *on* the
   sync low, so a captured frame holds `[24 data bits][sync high]` — the 31α sync
   low is gone, because it was the separator. Replay therefore emits
   data-then-sync and substitutes `tx`'s inter-word gap for the sync low. The
   default gap is 20 ms against a real 10.9 ms, so the receiver measures a 57α
   sync where it expects 31α, and rejects every word.
2. **Captured pulses are corrupt.** Real captures contain widths of 675, 695,
   1377 and 1393 µs — neither α (~351) nor 3α (~1053). Those are glitch-merged
   edges plus the CC1101's ±⅛-bit sampling jitter. A decoder that width-checks
   every pulse discards the whole word.

Forging sidesteps both: exact timings, zero jitter, sync in the right place. It is
also what the MQTT daemon needs regardless, since it cannot depend on a captured
frame sitting in a RAM slot.

### Transmit power is still unmeasured

There is **no independent confirmation of radiated power** — with a single CC1101
you cannot receive while transmitting. Every indicator is internal: the strobes
succeed, `MARCSTATE` reaches TX, and `FREND0.PA_POWER=1` with
`PATABLE={0x00,0xC0,…}` is correct for OOK. It plainly radiates enough, since the
roof responds — but the PA matching network and the antenna are both cut for
433 MHz, so output at 315 MHz is well under the module's rating. If range proves
marginal, a 23.8 cm quarter-wave wire for 315 MHz is the first thing to try.

## Regulatory note

⚠️ **315 MHz is not an EU licence-free SRD band.** The 10% duty-cycle allowance
under EN 300 220 that the README cites applies to **433.05–434.79 MHz**, and does
not cover 315 MHz. The remote itself already transmits there, so replaying it
changes nothing about what is on air — but do not assume EN 300 220 gives you
cover. Keep bursts to the ~12 repeats the remote itself sends, and do not hold a
carrier.
