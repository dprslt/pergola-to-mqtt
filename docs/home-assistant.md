# Home Assistant integration

**Status: designed, not built.** The daemon is phase 6. This is the design it
should be built to, written down now so the earlier phases capture the right
measurements.

Nothing here should be implemented until
[remote-protocol.md](remote-protocol.md) has a fixed-or-rolling verdict and
[behaviour.md](behaviour.md) has the light question answered. Both change what the
daemon has to do.

## Shape

One ESP32, permanently powered, running:

- WiFi + an MQTT client
- the same CC1101 driver as the sniffer, in transmit mode
- MQTT discovery so Home Assistant creates the entities itself
- a macro engine (button sequences with delays) rather than raw button presses

Deliberately **not** ESPHome. ESPHome is the obvious choice for a fixed code —
`remote_transmitter` with a raw pulse list is about ten lines of YAML, and if the
capture turns out to be a simple EV1527 that is a perfectly good place to stop.
Custom firmware wins once the timed macros and the light quirk get involved,
because that logic wants to live next to the radio rather than in HA automations
that break when the network hiccups mid-sequence.

If the remote turns out to be Somfy RTS, revisit this: ESPHome has a `somfy_rts`
component, and reusing it beats reimplementing rolling-code bookkeeping.

## Entities

Home Assistant's `cover` platform fits a pergola roof almost exactly — it has
open, close, stop, and an optional position.

| Entity | Platform | Notes |
|---|---|---|
| Roof | `cover` | open / close / stop, plus estimated position |
| Light bar | `light` | on/off; on sends `close`, off sends `stop` |
| Last command | `sensor` | diagnostics: what was sent, and when |

> **There is deliberately no position-confidence entity.**
>
> Two versions of such an entity existed briefly and both were removed. The reason is that the `ON` state can be silently
> false: the pergola has a **wired wall button** that moves the roof emitting no RF,
> so any such flag stays green while the real position has changed completely. An
> indicator that reads "position is fine" when it cannot know that invites exactly
> the trust it cannot earn, and is worse than no indicator at all.
>
> The daemon publishes `pergola/roof/position` and nothing about how much to believe
> it. Treat the percentage as an estimate at all times. The honest way to know where
> the roof is remains looking at it.
>
> Fixing this properly needs hardware the project does not have: a limit or reed
> sensor on the louvres, or current sensing on the motor supply. Both `position` and
> `state` would then be measurements rather than estimates, and this whole caveat
> would disappear.

### A reboot loses the position

On boot the daemon assumes the roof is **closed**. That is a guess, and it is often
wrong for a reason that catches you out in practice: **opening the serial port resets
the ESP32.** Any `pio device monitor` session, or any script that asserts DTR/RTS,
silently reverts the estimate to 0%.

Observed live during bring-up: the roof was opened from Home Assistant, the
mandatory auto-stop fired correctly, and then a serial connect reset the board and
the daemon went back to reporting "closed" while the roof was open.

Commands still work in that state, because open/close deliberately carry **no**
"already there" short-circuit — see the note in `cover_state.cpp`. An earlier version
did, and the consequence was a *dropped* close command whenever the daemon wrongly
believed the roof was already shut.

To re-anchor after any reboot, drive to an end: a full close, or a full open (which
auto-stops).

### How the light works

The light bar has no code of its own, but it is still genuinely controllable:

| Want | Send | Why it works |
|---|---|---|
| Light **on** | `close` | With the roof **already closed** it is at its end stop, so a `close` lights the bar and moves nothing |
| Light **off** | `stop` | A `stop` clears the light, and against a stopped motor does nothing else |

**Light-on is unconditional, and that is deliberate.**

An earlier version refused unless the daemon believed the roof was closed. That
guard was removed, because it could not do the job it looked like it was doing:
the believed position is dead-reckoned, and the pergola's **wired wall button moves
the roof without emitting any RF**. So the belief can be stale in either direction —
the guard would refuse while the roof genuinely was closed, and permit while it was
not. It failed precisely in the case it existed for, while making the code look
careful. **Nothing in this daemon gates behaviour on a position it cannot verify.**

The honest consequence: **if the roof is not actually closed, turning the light on
closes it.** Exactly what pressing `close` on the physical remote would do. There is
no auto-stop on this path, so it closes fully and the light stays on.

Light-off is always safe.

One interaction to keep in mind: a full close through the **cover** entity ends with
the mandatory auto-stop, which switches the light **off**. To end up closed with the
light on, close first and then turn the light on — the second command re-lights the
bar and, with the roof already at its end stop, moves nothing.

`0xF3A751` is the predicted fourth code and would give unconditional light control
if it works, but it has never been transmitted
([remote-protocol.md](remote-protocol.md)).

Connectivity is covered by the `availability_topic` on every entity rather than a
separate radio-state sensor — HA greys the whole device out on the last will, which
is what that entity was for.

## Topics

```
pergola/roof/set              <- OPEN | CLOSE | STOP
pergola/roof/position/set     <- 0..100
pergola/roof/state            -> open | opening | closed | closing | stopped
pergola/roof/position         -> 0..100  (estimated, see below)
pergola/light/set             <- ON | OFF   (ON sends close; OFF sends stop)
pergola/light/state           -> ON | OFF   (inferred from what was sent)
pergola/availability          -> online | offline   (MQTT will message)
pergola/last_command          -> JSON: command, code, repeats, uptime_ms
```

Publish state topics with **retain**, so Home Assistant recovers the last known
state after a restart. Set `availability` as the LWT so a crashed ESP32 shows as
unavailable rather than as whatever it last claimed.

## Discovery

Published once at boot to `homeassistant/<component>/<object_id>/config`, retained,
so HA needs no YAML at all.

```json
{
  "name": "Pergola roof",
  "unique_id": "pergola_roof",
  "device": {
    "identifiers": ["pergola_cc1101"],
    "name": "Pergola",
    "manufacturer": "pergola-to-mqtt",
    "model": "ESP32 + CC1101"
  },
  "command_topic": "pergola/roof/set",
  "state_topic": "pergola/roof/state",
  "position_topic": "pergola/roof/position",
  "set_position_topic": "pergola/roof/position/set",
  "availability_topic": "pergola/availability",
  "payload_open": "OPEN",
  "payload_close": "CLOSE",
  "payload_stop": "STOP",
  "state_open": "open",
  "state_opening": "opening",
  "state_closed": "closed",
  "state_closing": "closing",
  "device_class": "awning",
  "optimistic": false
}
```

Sharing one `device.identifiers` across the roof, light and diagnostic entities
groups them under a single device in HA.

`device_class: awning` is closer to the truth than `shutter` — it gets the right
icon and the right assumptions in the UI.

## No feedback means no truth

The pergola sends nothing back. Not a position, not an end-stop signal, not an
acknowledgement. The daemon knows only what it *commanded*.

That has consequences worth designing for rather than papering over:

- **Position is an estimate**, integrated from commanded travel time and the
  measured full-travel duration in [behaviour.md](behaviour.md). It drifts.
- **A lost RF frame desynchronises everything.** The daemon thinks the roof moved;
  it did not. Nothing detects this.
- **The physical remote is invisible.** Someone using it moves the roof without the
  ESP32 knowing.

Mitigations, in order of value:

1. **Re-synchronise at the end stops.** A full open or full close is the only time
   position is known for certain — the motor's own limit switches make it true.
   Snap the estimate to 0 or 100 there, and prefer full travel over partial where
   it does not matter.
2. **Report `unknown` rather than guess.** After a boot, or after any command whose
   outcome is uncertain, publish an unknown position instead of a confident lie. A
   blank dial is more useful than a wrong one.
3. **Expose the estimate as an estimate.** Name the diagnostic sensor so it is
   obvious the number is derived, not measured.
4. **Optionally, listen.** The CC1101 is a transceiver, and the sniffer already
   decodes the remote. Staying in RX between transmissions lets the daemon see
   presses of the *physical* remote and update its own state accordingly. This is
   the one mitigation that actually closes the loop, and it costs almost nothing
   given the capture code already exists.

Mitigation 4 is the interesting one and worth building early rather than bolting on
later — it turns the daemon from write-only into something that at least observes
the same world the user is in.

## Timing vs WiFi

The transmit path has a real timing budget: the CC1101 samples the async input at
8× the data rate, so the error in each pulse must stay under ⅛ of a bit period —
about 26 µs at 4.8 kBaud. See
[cc1101/04-async-serial-ook.md](cc1101/04-async-serial-ook.md#2-jitter).

The WiFi stack does not respect that. It runs ISRs and can stall a task for
hundreds of microseconds at moments you do not control, which lands directly
inside the pulse widths being generated.

Three defences, and the ESP32 makes the first two easy:

1. **Pin transmission to core 1**, leaving WiFi and lwIP on core 0 — the Arduino
   default split. Do not transmit from an MQTT callback; queue the macro and let
   the radio task run it.
2. **Use the RMT peripheral** to clock the whole waveform out of hardware. The
   ESP32's RMT is built for exactly this and removes the jitter problem entirely
   rather than mitigating it. Strongly preferred over
   `delayMicroseconds()` loops for the permanent installation — the sniffer's
   bit-banged `tx` is fine for a bring-up test, not for something that runs for
   years.
3. **Never hold a lock or allocate** inside the pulse loop.

## Duty cycle and neighbours

⚠️ This remote is on **315 MHz, which is not an EU licence-free SRD band.** The
EN 300 220 10% duty-cycle allowance this section originally cited covers
433.05–434.79 MHz and does not apply. The remote already transmits on 315 MHz, so
replaying it puts nothing new on air — but do not assume EN 300 220 gives you cover.

- **Match the remote: 12 repeats.** Not three or four, as first guessed here — the
  measured remote sends its word about a dozen times per press, and the receiver
  expects it (`PERGOLA_TX_REPEATS`). One command is ~540 ms of keying.
- Rate-limit commands. Home Assistant can produce a burst of position updates from
  a dragged slider; coalesce them rather than transmitting each one.
- Never transmit a continuous carrier.

## Secrets

The repo is public, so credentials are kept out of the source tree entirely
rather than in a header that merely happens to be gitignored.
`firmware/daemon/scripts/inject_secrets.py` runs before each build and resolves
each field from two places, in this order:

1. `firmware/daemon/.env` — `KEY=VALUE` lines, gitignored. The everyday path.
2. The process environment — overrides the file. For CI and one-offs:
   `PERGOLA_MQTT_HOST=10.0.0.5 pio run -t upload`.

Every name carries a `PERGOLA_` prefix so that an `MQTT_HOST` exported for some
unrelated tool cannot quietly flash the wrong broker into the pergola.
`firmware/daemon/.env.example` lists the fields with no values.

`PERGOLA_WIFI_SSID`, `PERGOLA_WIFI_PASSWORD` and `PERGOLA_MQTT_HOST` are
required; a build missing any of them fails in under a second and names which.
`PERGOLA_MQTT_PORT` defaults to 1883, and an empty `PERGOLA_MQTT_USER` selects
an anonymous broker.

### Why a generated header and not `-D` flags

The script writes the resolved values into `pergola_secrets.h` under
`$BUILD_DIR` (so, inside the gitignored `.pio/`) and force-includes it with
`-include`. The obvious implementation — appending `-DMQTT_PASSWORD=...` to
`CPPDEFINES` — is a trap worth recording, because it was tried here first:

SCons expands `$NAME` inside construction variables, so a password containing
`$o3V` reaches the compiler four characters shorter. **The build succeeds**; only
the broker complains, which reads as a server-side problem and costs an evening.
Escaping as `$$` does not fix it — the value is substituted more than once — and
SCons' `Literal()` does not either, because PlatformIO substitutes these flags
itself. A generated header keeps the values out of SCons' hands completely, which
is the only version that survives an arbitrary password.

The header is build output: wiped by `pio run -t clean`, and no more exposed than
the `firmware.bin` beside it, which necessarily contains the same strings.

That approach has one consequence the script has to handle explicitly. SCons
cannot see a force-included header as a dependency — it is in no translation
unit's `#include` graph, and the command line does not change when only the
file's *contents* do. Left alone, an incremental build after editing `.env`
keeps the previous credentials in the image: you fix the password, upload, and
flash the old one, then blame the password. The script therefore compares the
header it is about to write against the one on disk and drops `$BUILD_DIR/src`
when they differ, forcing a recompile. Only the project objects need it — the
framework and library objects are force-included too, but never reference these
macros.

**This keeps secrets out of the repo, not out of the firmware.** A `-D` is not
encryption — the values sit in the flash image as plain strings, readable by
anyone who can pull the image off the board. What it buys is that they are
absent from the tree, from diffs and from anything you might paste elsewhere. If
the image itself has to be clean, the answer is runtime provisioning into NVS,
which also survives a WiFi password change without a rebuild.

## Build order

1. ✅ WiFi + MQTT + discovery.
2. ✅ Transmit, verified against the real roof — all three codes move it.
3. ✅ The command model from [behaviour.md](behaviour.md), including the
   **mandatory stop** after every open.
4. ✅ Position estimation, dead-reckoned from the measured travel times. No
   confidence flag is published — see the note above.
5. ❌ **RX-side listening for physical remote presses — deliberately not built.**
   The pergola has a **physical wired button** that moves the roof without emitting
   any RF, so a receiver would catch remote presses and still miss wall presses.
   Partial coverage that reads as full coverage is worse than none: it would make
   the position estimate look authoritative while leaving it just as wrong. The
   daemon is transmit-only and says so.

Steps 1–4 are built and compiling. Not yet run against a live broker —
`PERGOLA_MQTT_HOST` and the broker credentials need real values first.
