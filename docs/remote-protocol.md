# The remote's protocol

**Status: not yet measured.** This page is the template to fill in once the
captures are done, plus the decision procedure for the one question that decides
whether the project works at all.

Fill it in as soon as you have a verdict from `pergola_analyze.py` — the exact
timings and codes are the thing you will keep coming back to, and they only exist
in your captures until they are written down here.

## Measured facts

| | Value | How measured |
|---|---|---|
| Carrier frequency | _TBD_ MHz | `scan`, or `pergola_capture.py --scan` |
| Modulation | OOK (assumed) | — |
| Short symbol | _TBD_ µs | analyser "widths" |
| Long symbol | _TBD_ µs | analyser "widths" |
| Sync / inter-repeat gap | _TBD_ µs | analyser "split at >=" |
| Repeats per press | _TBD_ | analyser "repeats" |
| Bits per frame | _TBD_ | analyser decode |
| Encoding | _TBD_ (pwm / manchester) | analyser decode |
| Protocol family | _TBD_ | analyser "hint" |
| Fixed or rolling | **_TBD_** | see below |

### Codes

| Button | Bits | Hex | Notes |
|---|---|---|---|
| open | _TBD_ | _TBD_ | |
| stop | _TBD_ | _TBD_ | |
| close | _TBD_ | _TBD_ | also switches the light on — see [behaviour.md](behaviour.md) |

If the codes differ only in a few low bits, the shared high bits are the remote's
address and the low bits are the command — which is how EV1527 and PT2262 work
(20 address bits + 4 data bits). Note the split here once you can see it.

## Fixed or rolling: how to tell

This is the project's go/no-go gate, and it is the reason `pergola_capture.py`
insists on a button label and at least three presses.

Capture the **same button** three or more times, then:

```bash
python3 pergola_analyze.py captures/open-*.jsonl
```

| Within one press | Between presses | Verdict |
|---|---|---|
| repeats identical | codes identical | **Fixed code.** Replay works. |
| repeats identical | codes differ | **Rolling code.** Replay cannot work. |
| repeats differ | — | **Bad capture.** Fix the capture before concluding anything. |

The middle row and the bottom row look the same in a careless reading, which is
why the analyser reports repeat consistency separately. A rolling code is
internally consistent within a press — the remote sends the same new code several
times — and only changes between presses. A noisy capture is inconsistent
*inside* a single press.

If repeats differ within a press, go back to
[setup-checklist.md](setup-checklist.md#stage-4--are-frames-being-captured)
before drawing conclusions.

### Then confirm it on the hardware

A decode is a hypothesis. The proof is the roof moving:

```
> keep 0
> tx 0 4
```

## If it is a fixed code

The good case. Most cheap remotes are EV1527, PT2262, HT6P20 or a close relative:
24 bits, two pulse widths in a 1:3 ratio, a sync gap around 31× the short pulse,
and no cryptography at all. The code is a constant and replaying it is the whole
solution.

Next steps:

1. Record the three codes in the table above.
2. Confirm each one moves the roof (`keep` / `tx`).
3. Measure travel time → [behaviour.md](behaviour.md).
4. Build the MQTT daemon → [home-assistant.md](home-assistant.md).

The daemon can then synthesise frames from the bit patterns rather than replaying
stored pulse arrays, which is tidier and lets it retransmit with clean timings.

## If it is a rolling code

Then the remote is doing something more interesting, and a captured frame is
worthless a moment after it was sent. Replaying it will do nothing — the receiver
tracks a counter and rejects anything it has already seen.

Pergola and awning motors commonly use **Somfy RTS**, which is why the analyser
looks for its fingerprint specifically: 433.42 MHz rather than 433.92, 2560 µs
hardware sync pulses, a 4550 µs software sync, and 56 Manchester bits at a 640 µs
half-period. Nice Flor-S, Came, and several others work similarly.

Somfy RTS is well documented and its algorithm is understood: a 16-bit rolling
counter and a checksum, obfuscated by XOR-chaining the frame bytes, with the
counter increasing on every press. That means a transmitter **can** be built — but
it is a different project from replaying a fixed code:

- The ESP32 must *become* a remote, holding its own address and counter, and
  persist that counter across reboots (NVS) or the receiver will reject it.
- The motor must be taught to accept that new remote, using the physical pairing
  procedure in its manual (typically holding the PROG button on an existing
  remote).
- The counter must never go backwards. Losing it means re-pairing.

Practical options, best first:

1. **Pair a new virtual remote.** Implement the protocol, give the ESP32 its own
   address, pair it with the motor. Clean, reversible, and the existing remote
   keeps working. This is what ESPHome's `somfy_rts` component and the various
   Arduino Somfy libraries do.
2. **Wire into the wall switch.** If there is a wired control point, relays across
   its contacts sidestep RF entirely. Often the most reliable answer, and immune to
   protocol changes.
3. **Solder onto the remote's buttons.** Three optocouplers or transistors across
   the remote's switch contacts, driven by the ESP32. Crude, but the rolling code
   is generated by the remote itself, so it simply works. A good fallback when the
   protocol turns out to be undocumented.

Whichever way it goes, record the finding here — the capture that proved it is
worth keeping too.

## Raw reference capture

Once you have one clean frame per button, paste the analyser output here. Future
you will want the exact timings without re-running anything, and having them in
git means a re-flash or a lost SD card is not a setback.

```
(paste `python3 pergola_analyze.py captures/*.jsonl` output here)
```
