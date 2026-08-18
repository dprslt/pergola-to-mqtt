# How the pergola behaves

The unit is a **Green Outside "Actual"**, 3 × 4 m — a bioclimatic pergola with
motorised louvres and an integrated light bar, controlled by a 3-button 433 MHz
remote.

The remote has three buttons. The pergola has more than three behaviours, because
one button does two things at once. This page is where the measured behaviour
lives, and it is what the MQTT daemon's command model has to be built on.

**Status: mostly not yet measured.** Fill in the tables as you go.

## The unit

| | Value |
|---|---|
| Manufacturer | Green Outside |
| Model | Actual |
| Size | 3 × 4 m |
| Roof | Motorised bioclimatic louvres |
| Lighting | Integrated light bar, no dimming observed |
| Control | 3-button 433 MHz remote; no app, cloud or wired API |
| Motor / receiver make | _TBD — check the label on the beam or motor housing_ |

The motor make is worth chasing down: bioclimatic pergolas are usually assembled
from bought-in motors and RF receivers, so the protocol belongs to whoever made the
receiver, not to Green Outside. If the label says Somfy, Nice, Came or BFT, that
answers the fixed-versus-rolling question before a single capture — see
[remote-protocol.md](remote-protocol.md).

## The buttons

| Button | Documented effect | Side effect |
|---|---|---|
| open | louvres open | none observed |
| stop | stops travel | none observed |
| close | louvres close | **also switches the light on** |

That side effect is the only genuinely awkward thing about this pergola, and it
shapes the whole command design.

## Answered: `stop` turns the light off

**Reading A.** Measured on 2026-08-18 with the remote in hand:

> **Result:** starting a **close** switches the light **on**; pressing **stop**
> switches it **off**. A second `close` does not toggle it.

So a "close without leaving the light on" sequence is `close`, wait, `stop` — which
is what the daemon does anyway, because the auto-stop below is mandatory for other
reasons.

Still open, and none of it blocks the daemon:

- [x] Does `open` affect the light at all? — no
- [x] Can the light be controlled independently of the roof in any way? — **yes,
      when the roof is closed.** A `close` sent while the roof is already at its
      closed end stop lights the bar without moving anything, and a `stop` clears
      it. That is enough for a real Home Assistant `light` entity. From any other
      position the same `close` also closes the roof, and there is no reliable way
      to know which case you are in. `0xF3A751` would give unconditional light
      control if it works, but it has never been transmitted — see
      [remote-protocol.md](remote-protocol.md).
- [x] Does the motor stop itself at the end of travel, or does it need a `stop`? —
      **it needs a `stop`**, and this is more serious than it sounds. See
      [the lockout](#a-full-open-must-be-followed-by-a-stop).
- [ ] Does pressing `close` while already closing do anything?
- [x] Is there any feedback at all? — **no.** The remote's encoder is
      transmit-only, and the pergola has a **physical wired button** that moves the
      roof without emitting any RF, so even a permanent 315 MHz receiver would miss
      changes made at the wall.

## A full open must be followed by a stop

**If the roof reaches fully open and no `stop` is sent, it cannot be closed
again.** It latches, and a `stop` is the only way out.

This contradicts the assumption in [the command model](#the-command-model) below
that the motor simply stops itself at the end stop. It does stop, but the
controller stays in a state where `close` is ignored until a `stop` clears it.

It is a lockout, not a tidiness preference. Anything issuing an `open` owns the
`stop` that terminates it — scripts, automations and manual serial sessions
included, not just the daemon. `firmware/daemon` schedules the stop *when the move
starts* rather than when travel ends, so a busy loop or a dropped MQTT connection
cannot skip it. There is deliberately no flag to disable it.

**Never send a bare `open` and walk away.**

### The obligation survives a reset

Scheduling the stop up front covers a crash and a busy loop, because both leave
the scheduler running. It does not cover losing power: `autoStopAtMs_` is in RAM,
so a reset inside the ~6.8 s between the open and its stop would once have dropped
the obligation entirely and left the roof latched with nothing left to notice.

That window is also when the radio and WiFi draw their transmit current, which is
exactly when a marginal supply browns out — see [hardware.md](hardware.md), section
Power. The likeliest cause of a reset coincides with the only window where a reset
does lasting harm.

So the obligation is written to NVS before the open can go out, and cleared only
once a `stop` has actually been transmitted:

- `firmware/daemon/include/durable_state.h` owns the flag.
- `loop()` sets it from `CoverState_t::movePending()` *before* `nextTx()` can put
  the open on the air, so a reset between the two leaves it set. Failing towards
  "a stop is owed" is always the safe direction: a redundant stop costs one
  ~540 ms burst and moves nothing.
- `setup()` checks it before WiFi comes up and transmits the stop straight away.
  If the radio does not initialise, the flag stays set and the next boot retries.

This is also what makes the task watchdog safe to arm. A watchdog that reboots the
board mid-move would otherwise trade a hung daemon for a latched roof.

## Travel time

Needed for two things: reporting a plausible position to Home Assistant, and
timing the macros that work around the light quirk.

Time it with a stopwatch, three runs each, from a known end stop.

| Movement | Mean |
|---|---|
| Fully closed → fully open | **6.30 s** |
| Fully open → fully closed | **~6.0 s** |

Measured 2026-08-18. Individual run figures were not recorded separately, so the
table holds the means only — the close figure is the rougher of the two. These are
the values in `firmware/common/pergola/pergola_codes.h`
(`PERGOLA_TRAVEL_OPEN_MS` / `PERGOLA_TRAVEL_CLOSE_MS`); change them together.

Notes to capture while measuring:

- [ ] Is closing slower than opening? (Gravity and motor gearing often make them
      differ by a few seconds.)
- [ ] Is travel linear, or is there a slow start / soft stop at the ends?
- [ ] Does the motor need a pause between a `stop` and the opposite direction?
- [ ] Does travel time change with temperature or after rain?

If travel is not linear, position estimates from elapsed time will drift.
Recording *that* is more useful than pretending a single number is exact.

## The command model

Given the light quirk, the daemon should not expose raw button presses. It should
expose a small set of **macros**, each a sequence of button presses with delays:

```
macro := [ (button, hold_ms, wait_after_ms), ... ]
```

Everything the pergola can usefully do then falls out of that, and either reading
of the light question above is a change to the macro table rather than to the code:

| Macro | Sequence (Reading A) | Purpose |
|---|---|---|
| `open` | `open`, wait _travel_, `stop` | full open. The `stop` is **mandatory** — see [the lockout](#a-full-open-must-be-followed-by-a-stop) |
| `close` | `close`, wait _travel_, `stop` | full close **and** clear the light |
| `stop` | `stop` | halt where it is |
| `set_position(p)` | `open`/`close`, wait _travel × Δp_, `stop` | partial travel |
| `light_on` | `close`, wait 300 ms, `stop` | light on, roof effectively unmoved |
| `light_off` | `stop` | |

`light_on` is the ugly one: it necessarily nudges the roof for a few hundred
milliseconds. Whether that is acceptable depends on how fast the louvres move —
another reason the travel time above matters. If a 300 ms nudge is visible,
`light_on` may only be safe to offer when the roof is already closed.

Under Reading B the `close` macro becomes `close`, wait _travel_, `close`, and
`light_off` becomes a lone `close` — same structure, different table.

The light question is answered (Reading A), so the Reading A column above is the
live one — with one correction to it. `light_on` does **not** need to nudge the
roof: sent while the roof is already closed, a bare `close` lights the bar and
moves nothing, because the motor is at its end stop. So:

| Macro | Sequence | Notes |
|---|---|---|
| `light_on` | `close` | Lights the bar. If the roof is not already closed, this also closes it |
| `light_off` | `stop` | Always safe |

`light_on` is **not** gated on the roof being closed. It cannot usefully be: the
daemon's position is dead-reckoned and the wired wall button moves the roof without
emitting any RF, so a position check would refuse when the roof really was closed
and permit when it was not. `firmware/daemon` exposes this as a real `light`
entity — see [home-assistant.md](home-assistant.md#how-the-light-works).

## Safety

The roof has pinch points, and an automation can command it when nobody is
looking.

- Keep a working physical remote. Do not rely on the ESP32 as the only control.
- `stop` must always be reachable from Home Assistant, and should be the safest
  path in the code — no macro, no delay, one command.
- Do not schedule unattended travel without thinking about what could be in the
  way: furniture, a parasol, a cat.
- Open-loop control means a missed command leaves Home Assistant's idea of the
  state wrong. Prefer showing "unknown" over showing a confident lie.
