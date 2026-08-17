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

## Open question: what turns the light off?

This needs one experiment with the remote in hand, because two readings of "press
close, wait a bit, then press off" lead to different implementations:

- **Reading A — `stop` clears the light.** Pressing `close` starts the roof
  closing and switches the light on; pressing `stop` halts travel and switches the
  light off. A "close without leaving the light on" macro is then
  `close`, wait, `stop`.
- **Reading B — `close` toggles the light.** A second `close` press switches the
  light off again. The macro is then `close`, wait, `close`, and `stop` is purely
  a travel command.

They are distinguishable in about a minute:

1. Press `close`. Note the light comes on.
2. Press `stop`. **Does the light go off?**
   - Yes → Reading A.
   - No → press `close` again. If the light goes off now, Reading B.
3. Separately: with the roof already fully closed and the light on, press `close`
   again. Does anything happen to the light? To the motor?

Record the answer here:

> **Result:** _TBD_

Also worth establishing while you are there:

- [ ] Does `open` affect the light at all?
- [ ] Can the light be controlled independently of the roof in any way?
- [ ] Does the motor stop itself at the end of travel, or does it need a `stop`?
- [ ] Does pressing `close` while already closing do anything (restart? stop?)?
- [ ] Is there any feedback at all — a wall panel, an LED, an end-stop click?

The last one matters more than it looks. Without feedback, the daemon is
open-loop: it knows what it *commanded*, never what the roof actually did. See
[home-assistant.md](home-assistant.md#no-feedback-means-no-truth).

## Travel time

Needed for two things: reporting a plausible position to Home Assistant, and
timing the macros that work around the light quirk.

Time it with a stopwatch, three runs each, from a known end stop.

| Movement | Run 1 | Run 2 | Run 3 | Mean |
|---|---|---|---|---|
| Fully closed → fully open | _TBD_ s | _TBD_ s | _TBD_ s | _TBD_ s |
| Fully open → fully closed | _TBD_ s | _TBD_ s | _TBD_ s | _TBD_ s |

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
| `open` | `open` | full open, motor stops itself at the end stop |
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

**Do not build the macro table until the light question is answered.** Guessing
here produces a daemon that half-works in a way that is annoying to debug.

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
