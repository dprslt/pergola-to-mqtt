# pergola-to-mqtt — working notes

## PlatformIO lives in a venv at the repo root

**There is no `pio` on `PATH`. Never run a bare `pio ...` command — it will fail
with "command not found".** Use `.venv/bin/pio` instead:

```bash
.venv/bin/pio --version          # PlatformIO Core, version 6.1.19
.venv/bin/pio device list        # find the ESP32's serial port
```

Or activate the venv first, in which case plain `pio` works for that shell:

```bash
source .venv/bin/activate
pio --version
```

### Why a venv, and why not the system Python

The system `python3` on this machine is **3.14**, which is ahead of what
PlatformIO Core supports. The venv is built on **pyenv's 3.11.12** instead:

```bash
~/.pyenv/versions/3.11.12/bin/python3 -m venv .venv
.venv/bin/python -m pip install platformio
```

That is the whole recreation procedure if `.venv` is ever deleted. `.venv/` is
gitignored, so it does not travel with the repo — expect to rebuild it on a fresh
clone. Do not "fix" a missing `pio` by installing it globally or with Homebrew;
rebuild the venv.

### The commands from the README, in venv form

The README and `docs/setup-checklist.md` are written with bare `pio` for
readability. Translate them:

| Doc says | Actually run |
|---|---|
| `pio run -t upload` | `.venv/bin/pio run -t upload` |
| `pio device monitor -b 115200` | `.venv/bin/pio device monitor -b 115200` |
| `pio run` | `.venv/bin/pio run` |

All of them need `cd firmware/sniffer` first — that is where `platformio.ini`
lives. `monitor_speed` is already set to 115200 there, so `-b 115200` is
redundant but harmless.

The **first** `pio run` downloads the `espressif32` platform and the Xtensa
toolchain — several hundred MB and several minutes. That is expected, not a hang.

### Monitoring without PlatformIO

`screen` is built into macOS and needs no toolchain, which makes it the quickest
way to confirm the serial path works before anything is flashed:

```bash
ls /dev/cu.usb*                        # find the port
screen /dev/cu.usbserial-0001 115200   # exit with Ctrl-A then K then y
```

Use `/dev/cu.*`, never `/dev/tty.*` — the `tty` variants block waiting for
carrier detect and appear to hang.

## The host tools use a *separate* venv

`tools/` has its own venv, per `docs/setup-checklist.md` Stage 5:

```bash
cd tools
python3 -m venv .venv && . .venv/bin/activate && pip install -r requirements.txt
```

Two venvs on purpose: the root one is the firmware toolchain, pinned to 3.11 by
PlatformIO's constraints; `tools/` is plain Python and has no such limit. Do not
merge them.

## Hardware gotcha that will bite

The Dupont loom uses **non-standard colours — brown is VCC, red is GDO0.** The
ribbon is bonded and cannot be reordered, so it keeps strict resistor-code
sequence instead of the colour convention. Connecting red to a power rail ties
GDO0 to 3.3 V and shorts it against the CC1101's output driver.

Full pin map and rationale: `docs/hardware.md`.
