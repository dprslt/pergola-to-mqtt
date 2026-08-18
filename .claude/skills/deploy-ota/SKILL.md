---
name: deploy-ota
description: Deploy a firmware update to the pergola daemon over the air, using the esp32dev-ota environment. Use when asked to flash, deploy, update, or ship firmware to the pergola / the ESP32 / the roof controller, or when a code change to firmware/daemon or firmware/common needs to reach the device. Covers the pre-flight safety check, the upload, and verifying the device came back.
---

# Deploying an over-the-air update to the pergola daemon

The daemon controls a roof that moves on command and has pinch points. An update
ends in a reboot. Read the safety section before uploading anything.

## Before you touch anything

**Ask the user before uploading.** An update reboots the device. That is normally
harmless, but only the user knows whether anyone is standing under the pergola.

**Do not send a movement command to test the result.** `open`, `close`, `stop` and
the position slider all move a physical roof. Verification below is entirely
passive: a status page, a serial log and MQTT topics. If a functional test really is
needed, say so and let the user run it.

## Pre-flight

Run these first. They are all read-only.

```bash
# 1. Both firmwares must build. common/ is shared, so a daemon change can break the
#    sniffer and you would not otherwise notice.
.venv/bin/pio run -d firmware/daemon
.venv/bin/pio run -d firmware/sniffer

# 2. Is OTA even enabled on the running device? No password means no espota, no
#    status page and no Home Assistant link.
grep -q '^PERGOLA_OTA_PASSWORD=.\+' firmware/daemon/.env && echo "ota configured" \
  || echo "NO OTA PASSWORD -- see the fallback section"

# 3. Is the device reachable, and is it safe to reboot it right now?
curl -s --max-time 8 http://pergola.local/ | tr -d '\n' | grep -o 'Stop owed[^<]*<[^>]*>[^<]*'
```

**If "Stop owed" says YES, do not upload.** A stop is outstanding: the roof may be
sitting latched open. The daemon refuses to service OTA in that state anyway, so the
upload would fail with a confusing timeout rather than a clear error. Tell the user,
and let them clear it (a `stop` from Home Assistant or the real remote).

Note the reported firmware version from that page. You will compare it afterwards.

## Upload

```bash
.venv/bin/pio run -d firmware/daemon -e esp32dev-ota -t upload
```

That is the whole command. No password on it: `scripts/ota_auth.py` appends `--auth`
from the same `.env` value the firmware is built with.

`upload_port` defaults to `pergola.local`. If mDNS does not resolve, get the address
from the `pergola/ip` MQTT topic or the Home Assistant "IP address" diagnostic
entity, then add `--upload-port <ip>`.

**Uploads fail sometimes.** A dropped attempt is safe: the ESP32 writes to the
inactive partition and only switches to a verified image, so a failure leaves the
running firmware untouched. Retry once or twice before investigating. Observed rate
on this link is roughly one failure in two or three attempts, usually part way
through with `Error Uploading`.

## Verify

The device reboots itself. Give it about 15 seconds, then:

```bash
# Version should have changed if you bumped FIRMWARE_VERSION; everything else
# should read healthy. Radio "ready", Broker "connected", Stop owed "no".
curl -s --max-time 8 http://pergola.local/ | sed 's/<[^>]*>/ /g' | tr -s ' '
```

A reboot is expected to show `reset reason 3` (software reset) in the serial log and
to keep the NVS values. `stop_owed` must read `no` and the position must be whatever
it was before, not reset to 0.

If you need the boot log, note that **opening the serial port resets the board**, so
you get a fresh boot rather than the one you were trying to inspect. That is usually
fine, but it also means you cannot retrospectively read the boot you just caused.

## Regression check after a firmware change

If the change touched the cover state machine, the owed-stop handling or the
watchdog, run the hardware test that covers it. It moves nothing:

```bash
cd tools && python3 hwtest_daemon.py --case owed-stop-reset
```

Eight checks, all of which must pass. It commands a `close` against the closed end
stop (lights the bar, moves nothing), resets the board mid-transmit, and asserts
that the owed stop survived and was transmitted before WiFi came up.

The heavier cases are in `tools/README.md`. `--case owed-stop-open` moves the roof
and needs `--yes`; the two watchdog cases need the `esp32dev-selftest` build and a
reflash afterwards. Ask the user before running any of those.

## When OTA is not available

If `.env` has no `PERGOLA_OTA_PASSWORD`, the running device is not listening and no
amount of espota will reach it. Bootstrap over USB once:

1. Add `PERGOLA_OTA_PASSWORD=` with letters and digits only to
   `firmware/daemon/.env`. A `$` is refused by `inject_secrets.py`, because SCons
   expands it inside the upload flags and espota then fails to authenticate with no
   useful error.
2. Flash over USB: `.venv/bin/pio run -d firmware/daemon -t upload --upload-port <tty>`
   (find the port with `.venv/bin/pio device list`; it is a CP2102N).
3. From then on use the OTA command above.

USB is also the recovery path if an image turns out to be broken.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `Authenticating...FAIL` | Password mismatch between `.env` and the running image. The running image was built with the old value; reflash over USB. |
| `auth: ''` in the espota debug line | The `--auth` flag never arrived. `scripts/ota_auth.py` must be registered as a **post**-script; a pre-script's `UPLOADERFLAGS` is replaced when the platform configures the upload target. |
| Upload times out with no response | Device unreachable, or it is declining OTA because a stop is owed. Check the status page. |
| `Error Uploading` part way | Usually transient. Retry. The running image is unaffected. |
| Status page unreachable but MQTT works | No OTA password in the flashed image, so the web server was never started. |

## Related

- `docs/home-assistant.md`, section Secrets — the seven fields and why OTA fails closed
- `docs/behaviour.md`, section "The obligation survives a reset" — why OTA is refused mid-move
- `CLAUDE.md` — the roof safety rules that outrank any of this
