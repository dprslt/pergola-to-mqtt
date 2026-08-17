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
| Pergola roof | `cover` | open / close / stop, plus estimated position |
| Pergola light | `light` | on/off only; no dimming available |
| Last command | `sensor` | diagnostics: what was sent, and when |
| Radio state | `binary_sensor` | connectivity, via the MQTT birth/will topics |

## Topics

```
pergola/roof/set              <- OPEN | CLOSE | STOP
pergola/roof/position/set     <- 0..100
pergola/roof/state            -> open | opening | closed | closing | stopped
pergola/roof/position         -> 0..100  (estimated, see below)
pergola/light/set             <- ON | OFF
pergola/light/state           -> ON | OFF
pergola/availability          -> online | offline   (MQTT will message)
pergola/last_command          -> JSON: macro, timestamp, result
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

433.05–434.79 MHz is licence-free in the EU under EN 300 220 with a **10% duty
cycle** cap. A real remote transmits a few tens of milliseconds and repeats a
handful of times, which is far inside the limit; a retry loop that never gives up
is not.

- Cap retries. Three or four repeats per command, matching what the remote does.
- Rate-limit commands. Home Assistant can produce a burst of position updates from
  a dragged slider; coalesce them rather than transmitting each one.
- Never transmit a continuous carrier.

## Secrets

WiFi and MQTT credentials go in a `secrets.h` that is gitignored — the repo is
public. Provide a `secrets.h.example` with the field names and no values.

## Build order

1. WiFi + MQTT + discovery, with transmission stubbed to a log line. Get the
   entities appearing in Home Assistant first; it is the part most likely to
   surprise you.
2. Single-command transmit (`STOP`), verified against the real roof.
3. The macro engine, with the table from [behaviour.md](behaviour.md).
4. Position estimation and end-stop re-synchronisation.
5. RX-side listening for physical remote presses (mitigation 4).
