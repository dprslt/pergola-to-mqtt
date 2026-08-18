#!/usr/bin/env python3
"""Hardware tests for firmware/daemon. Needs a flashed board and a live broker.

Unlike test_analyze.py and test_capture.py, nothing here runs without hardware.
These exercise the one property that cannot be unit tested: that an obligation the
daemon has taken on survives losing the CPU part way through.

    python3 hwtest_daemon.py --case owed-stop-reset       # no roof movement
    python3 hwtest_daemon.py --case owed-stop-open --yes  # MOVES THE ROOF
    python3 hwtest_daemon.py --case watchdog              # selftest build only
    python3 hwtest_daemon.py --case watchdog-recovery     # selftest build only

Resets are driven through the serial adapter's RTS line rather than by pulling
power, so the serial link survives and the recovery boot is observable. The board
reports `reset reason 1` (ESP_RST_POWERON) for these, the same class a brownout
produces, so RAM really is cleared the way a supply failure would clear it. What it
does not reproduce is a sagging rail catching a flash write half done; NVS's own
atomicity covers that, not our code.

The two watchdog cases need a build with -DPERGOLA_WDT_SELFTEST, which adds a
`pergola/selftest/set` topic that can hang loop() on purpose:

    .venv/bin/pio run -d firmware/daemon -e esp32dev-selftest -t upload

Put the normal firmware back afterwards. The script says so when it finishes.
"""
import argparse
import glob
import pathlib
import sys
import time

import serial
import paho.mqtt.client as mqtt

CODE_OPEN, CODE_STOP, CODE_CLOSE = "0xF3A758", "0xF3A754", "0xF3A752"
REPO = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_ENV = REPO / "firmware" / "daemon" / ".env"

# The watchdog is armed at 30 s. Allow it a wide margin: a false failure here would
# send someone looking for a watchdog bug that is not there.
WDT_WAIT_S = 60


def read_env(path):
    values = {}
    for line in pathlib.Path(path).read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            key, _, value = line.partition("=")
            values[key.strip()] = value.strip().strip("\"'")
    return values


def find_port(explicit):
    if explicit:
        return explicit
    # The board is a CP2102N. Pick the only match, or make the user choose.
    found = sorted(glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/ttyUSB*"))
    if len(found) == 1:
        return found[0]
    if not found:
        sys.exit("no USB serial port found; pass --port")
    sys.exit(f"several USB serial ports; pass --port. Saw: {', '.join(found)}")


class Device:
    """The serial side: a live log, and the ability to reset the board."""

    def __init__(self, port):
        self.log = []
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        # Neither line asserted: DTR drives IO0 and RTS drives EN on these boards,
        # and leaving DTR high here would hold the chip in the bootloader.
        self.ser.setDTR(False)
        self.ser.setRTS(False)

    def pump(self, seconds, until=None, echo=True):
        end = time.time() + seconds
        seen = ""
        while time.time() < end:
            chunk = self.ser.read(2048).decode("utf-8", "replace")
            if chunk:
                seen += chunk
                self.log.append(chunk)
                if echo:
                    sys.stdout.write(chunk)
                    sys.stdout.flush()
                if until and until in seen:
                    return True
        return False

    def reset(self):
        self.ser.setRTS(True)
        time.sleep(0.12)
        self.ser.setRTS(False)

    def text(self):
        return "".join(self.log)

    def last_boot(self):
        """Everything from the most recent boot banner on."""
        full = self.text()
        return full[full.rindex("rst:"):] if "rst:" in full else full

    def close(self):
        self.ser.close()


class Broker:
    def __init__(self, env):
        self.state = {}
        self.client = mqtt.Client()
        if env.get("PERGOLA_MQTT_USER"):
            self.client.username_pw_set(
                env["PERGOLA_MQTT_USER"], env.get("PERGOLA_MQTT_PASSWORD", "")
            )
        self.client.on_connect = lambda c, u, f, rc: c.subscribe("pergola/#")
        self.client.on_message = self._on_message
        self.client.connect(
            env["PERGOLA_MQTT_HOST"], int(env.get("PERGOLA_MQTT_PORT", 1883)), 30
        )
        self.client.loop_start()
        time.sleep(2)  # let the retained topics arrive before anyone reads them

    def _on_message(self, client, userdata, msg):
        self.state[msg.topic] = msg.payload.decode("utf-8", "replace")

    def send(self, topic, payload):
        self.client.publish(topic, payload)

    def close(self):
        self.client.loop_stop()


def await_ready(dev, label="waiting for the daemon to finish booting"):
    print(f"--- {label} (opening the serial port reset the board) ---")
    if not dev.pump(50, until="# mqtt: connected"):
        sys.exit("!! the daemon never reported an MQTT connection")
    dev.pump(3)


def report(name, checks, extra=()):
    print(f"\n\n================ {name} ================")
    for line in extra:
        print(f"  {line}")
    if extra:
        print("  " + "-" * 38)
    for label, ok in checks:
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}")
    print("=" * (34 + len(name)))
    return all(ok for _, ok in checks)


# --------------------------------------------------------------------------
# cases
# --------------------------------------------------------------------------

def case_owed_stop_reset(dev, broker, args):
    """No roof movement. A close against the closed end stop lights the bar and
    moves nothing, but it still books an obligation."""
    await_ready(dev)
    if broker.state.get("pergola/recovery") != "none":
        sys.exit(f"!! recovery topic is {broker.state.get('pergola/recovery')!r},"
                 " expected 'none' -- reboot the board and retry")

    print("\n--- CLOSE (roof must be fully closed: lights the bar, moves nothing) ---")
    broker.send("pergola/roof/set", "CLOSE")
    if not dev.pump(10, until="# mqtt: CLOSE"):
        sys.exit("!! the daemon never acknowledged the CLOSE")
    # The NVS write lands a few ms after that print; the close transmit then runs for
    # ~540 ms and the stop follows immediately. 300 ms is clear of both edges.
    time.sleep(0.30)

    print("\n--- RESET (flag written, close part sent, no stop yet) ---")
    dev.reset()
    dev.pump(50, until="# mqtt: connected")
    dev.pump(3)

    after = dev.last_boot()
    return report("OWED STOP / RESET", [
        ("flag survived the reset", "# nvs: stop_owed=YES" in after),
        ("recovery path entered", "# recovery: a stop was owed" in after),
        (f"stop transmitted ({CODE_STOP})", f"# tx: {CODE_STOP}" in after),
        ("stop went out before WiFi",
         "# recovery: stop sent" in after
         and after.index("# recovery: stop sent") < after.index("# wifi: connecting")),
        ("flag cleared afterwards",
         "stop_owed=YES" not in after.split("# recovery: stop sent")[-1]),
        ("offline transmit flushed to the broker", "# mqtt: flushed" in after),
        ('last_command carries "recovery":true',
         '"recovery":true' in broker.state.get("pergola/last_command", "")),
        ("recovery topic reads stop-sent",
         broker.state.get("pergola/recovery") == "stop-sent"),
    ], extra=[
        f"recovery topic : {broker.state.get('pergola/recovery')}",
        f"last_command   : {broker.state.get('pergola/last_command')}",
    ])


def case_owed_stop_open(dev, broker, args):
    """MOVES THE ROOF. A real open, interrupted part way."""
    if not args.yes:
        sys.exit("!! this case moves the roof. Re-run with --yes once someone has"
                 " eyes on the pergola and a real remote in reach.")
    await_ready(dev)

    print("\n--- OPEN, resetting 2 s in. THE ROOF IS ABOUT TO MOVE ---")
    broker.send("pergola/roof/set", "OPEN")
    if not dev.pump(10, until="# mqtt: OPEN"):
        sys.exit("!! the daemon never acknowledged the OPEN")
    time.sleep(2.0)

    print("\n--- RESET (roof mid-travel, un-stopped) ---")
    dev.reset()
    dev.pump(50, until="# mqtt: connected")
    dev.pump(3)
    after = dev.last_boot()

    # Re-anchor. The light-on path sends a bare CLOSE with no auto-stop, so the roof
    # runs to its closed end stop and the position estimate is true again. Doing this
    # through roof/set instead would schedule a stop from the believed position -- 0
    # after the reset -- and halt the roof a second into travel.
    print("\n--- closing fully to re-anchor (light-on path, no auto-stop) ---")
    broker.send("pergola/light/set", "ON")
    dev.pump(9)
    broker.send("pergola/light/set", "OFF")
    dev.pump(4)

    return report("OWED STOP / OPEN INTERRUPT", [
        ("flag survived the reset", "# nvs: stop_owed=YES" in after),
        ("recovery path entered", "# recovery: a stop was owed" in after),
        (f"stop transmitted ({CODE_STOP})", f"# tx: {CODE_STOP}" in after),
        ("stop went out before WiFi",
         "# recovery: stop sent" in after
         and after.index("# recovery: stop sent") < after.index("# wifi: connecting")),
        ("position estimate reset to 0, as documented",
         "position=0%" in after),
        ("roof left closed", broker.state.get("pergola/roof/state") == "closed"),
    ], extra=[
        f"recovery topic : {broker.state.get('pergola/recovery')}",
        f"roof           : {broker.state.get('pergola/roof/state')}"
        f" @ {broker.state.get('pergola/roof/position')}%",
    ])


def _hang(dev, broker, payload, label):
    print(f"\n--- {label} ---")
    broker.send("pergola/selftest/set", payload)
    if not dev.pump(10, until="# selftest: hanging"):
        sys.exit("!! no selftest acknowledgement. Is this the -e esp32dev-selftest"
                 " build? A normal build does not subscribe to that topic.")
    print(f"\n--- loop() is wedged. Waiting up to {WDT_WAIT_S} s for the watchdog ---")
    rebooted = dev.pump(WDT_WAIT_S, until="# pergola-to-mqtt daemon")
    if rebooted:
        dev.pump(25, until="# mqtt: connected")
        dev.pump(2)
    return rebooted


def case_watchdog(dev, broker, args):
    """No roof movement, nothing owed. Does the watchdog reboot a wedged loop()?"""
    await_ready(dev)
    rebooted = _hang(dev, broker, "HANG", "hanging loop() with nothing owed")
    after = dev.last_boot()
    reason = ""
    for line in after.splitlines():
        if "# boot: reset reason" in line:
            reason = line.strip()
    # trigger_panic is on, so the panic handler is what records the reset. IDF 4.x
    # reports TASK_WDT (6); PANIC (4) and WDT (7) are the neighbouring encodings and
    # all three mean the watchdog did its job.
    code = reason.rsplit(" ", 1)[-1] if reason else ""
    return report("WATCHDOG", [
        ("the wedged loop was rebooted", rebooted),
        ("reset reason is a watchdog or panic, not a power-on",
         code in {"4", "6", "7"}),
        ("the daemon came back up", "# mqtt: connected" in after),
        ("nothing was owed, so no recovery ran",
         "# recovery:" not in after),
    ], extra=[f"reset line     : {reason or '<none seen>'}"])


def case_watchdog_recovery(dev, broker, args):
    """No roof movement. Books an obligation with no RF, then wedges loop(). The
    watchdog reboot must discharge it -- the two features working together."""
    await_ready(dev)
    rebooted = _hang(dev, broker, "OWE-AND-HANG",
                     "marking a stop owed, then hanging loop()")
    after = dev.last_boot()
    return report("WATCHDOG + RECOVERY", [
        ("the wedged loop was rebooted", rebooted),
        ("the obligation survived the watchdog reboot",
         "# nvs: stop_owed=YES" in after),
        (f"stop transmitted ({CODE_STOP})", f"# tx: {CODE_STOP}" in after),
        ("recovery reported success", "# recovery: stop sent" in after),
        ("recovery topic reads stop-sent",
         broker.state.get("pergola/recovery") == "stop-sent"),
    ], extra=[f"recovery topic : {broker.state.get('pergola/recovery')}"])


CASES = {
    "owed-stop-reset": (case_owed_stop_reset, False),
    "owed-stop-open": (case_owed_stop_open, True),
    "watchdog": (case_watchdog, False),
    "watchdog-recovery": (case_watchdog_recovery, False),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--case", required=True, choices=sorted(CASES))
    ap.add_argument("--port", help="serial port; auto-detected when there is one")
    ap.add_argument("--env", default=str(DEFAULT_ENV),
                    help="daemon .env, for the broker credentials")
    ap.add_argument("--yes", action="store_true",
                    help="required by cases that move the roof")
    args = ap.parse_args()

    fn, moves = CASES[args.case]
    if moves:
        print("!! this case MOVES THE ROOF. Pinch points; keep a real remote in reach.\n")

    env = read_env(args.env)
    for required in ("PERGOLA_MQTT_HOST",):
        if not env.get(required):
            sys.exit(f"!! {required} missing from {args.env}")

    dev = Device(find_port(args.port))
    broker = Broker(env)
    try:
        ok = fn(dev, broker, args)
    finally:
        broker.close()
        dev.close()

    if args.case.startswith("watchdog"):
        print("\nReminder: this ran on the selftest build. Put the normal firmware"
              " back:\n  .venv/bin/pio run -d firmware/daemon -e esp32dev-ota -t upload")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
