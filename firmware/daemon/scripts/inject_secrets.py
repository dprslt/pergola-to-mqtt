"""Supply WiFi and MQTT credentials to the build without putting them in the tree.

The daemon needs an SSID, a passphrase and a broker to be useful, and this repo
is public. Rather than keep those in a header that merely happens to be
gitignored, they are resolved at build time from:

  1. `firmware/daemon/.env`  -- KEY=VALUE lines, gitignored. The everyday path.
  2. The process environment  -- overrides the file. For CI and one-offs.

Every name is prefixed `PERGOLA_` so that a stray `MQTT_HOST` exported for some
unrelated tool cannot silently flash the wrong broker into the pergola.

Why a generated header rather than -D flags
-------------------------------------------
The obvious implementation appends -DMQTT_PASSWORD=... to CPPDEFINES. Do not:
SCons expands $NAME inside construction variables, so a password containing
"$o3V" arrives at the compiler four characters shorter. The failure is silent --
the build succeeds and only the broker complains, which reads as a server-side
problem. Escaping as $$ does not fix it (the value is substituted more than
once) and SCons' Literal() does not either (PlatformIO substitutes these flags
itself). Writing the values into a header and force-including it keeps them out
of SCons' hands completely, which is the only version that survives an arbitrary
password.

The header lands in $BUILD_DIR, i.e. under .pio/ -- build output, gitignored, and
wiped by `pio run -t clean`. It is no more exposed than firmware.bin next to it,
which necessarily contains the same strings.

This keeps secrets out of the repo, not out of the firmware. Anyone holding the
board can read them back. If the image itself must be clean, the answer is
runtime provisioning into NVS -- see docs/home-assistant.md, section "Secrets".
"""

import os
import shutil
import sys

Import("env")  # noqa: F821 -- injected by PlatformIO's SCons runner

PREFIX = "PERGOLA_"

# name -> (required, default). Defaults apply only to genuinely optional fields.
FIELDS = {
    "WIFI_SSID": (True, None),
    "WIFI_PASSWORD": (True, None),
    "MQTT_HOST": (True, None),
    "MQTT_PORT": (False, "1883"),
    "MQTT_USER": (False, ""),
    "MQTT_PASSWORD": (False, ""),
    # Optional, and empty means OTA stays off entirely rather than open. The
    # daemon refuses to expose an unauthenticated flash endpoint on something
    # that moves a roof, so there is no unsafe default to fall into here.
    "OTA_PASSWORD": (False, ""),
}

# Emitted as an integer literal, not a string: PubSubClient's setServer() takes
# a uint16_t port, and "1883" would not convert.
NUMERIC = {"MQTT_PORT"}


def read_env_file(path):
    """Parse KEY=VALUE lines. No dependency on python-dotenv, which PlatformIO's
    bundled interpreter is not guaranteed to have."""
    values = {}
    if not os.path.isfile(path):
        return values
    with open(path, encoding="utf-8") as handle:
        for lineno, raw in enumerate(handle, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                raise SystemExit(f"{path}:{lineno}: expected KEY=VALUE")
            key, _, value = line.partition("=")
            key, value = key.strip(), value.strip()
            # Strip one matched pair of surrounding quotes, so both
            # PERGOLA_WIFI_PASSWORD=hunter2 and ="hunter 2" work.
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            values[key] = value
    return values


def resolve():
    from_file = read_env_file(os.path.join(env["PROJECT_DIR"], ".env"))  # noqa: F821

    resolved, missing = {}, []
    for name, (required, default) in FIELDS.items():
        key = PREFIX + name
        value = os.environ.get(key, from_file.get(key))  # environment wins
        if value is None or value == "":
            if required:
                missing.append(key)
                continue
            value = default
        resolved[name] = value

    if missing:
        raise SystemExit(
            "inject_secrets: missing credentials -- "
            + ", ".join(missing)
            + "\n\n  cp .env.example .env   and fill it in, or export the names.\n"
            '  See docs/home-assistant.md, section "Secrets".\n'
        )

    # An unfilled placeholder is worse than a missing value: the build succeeds
    # and the board silently fails to associate, which looks like a hardware or
    # antenna problem. This happened -- a secrets.h carrying FILL-ME-IN in both
    # WiFi fields was migrated across and flashed without complaint. Treat any
    # value still equal to the template's, plus the usual sentinels, as absent.
    example = read_env_file(os.path.join(env["PROJECT_DIR"], ".env.example"))  # noqa: F821
    generic = {"fill-me-in", "changeme", "change-me", "todo", "xxx", "none"}

    # The example-equality rule applies only to required fields. For optional
    # ones the template shows the real default -- MQTT_PORT=1883 is correct, not
    # unfilled -- so matching it there means nothing.
    unfilled = [
        PREFIX + name
        for name, value in resolved.items()
        if value
        and (
            (FIELDS[name][0] and value == example.get(PREFIX + name))
            or value.strip().lower() in generic
        )
    ]
    if unfilled:
        raise SystemExit(
            "inject_secrets: placeholder left unfilled -- "
            + ", ".join(unfilled)
            + "\n\n  These still hold the template value. Put real ones in .env;\n"
            "  flashing a placeholder fails at association with no useful error.\n"
        )
    return resolved


def c_string(value):
    """Quote for C. Only backslash and double quote need escaping here; the
    values are credentials, not arbitrary binary."""
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return '"' + escaped + '"'


values = resolve()

lines = [
    "// Generated by scripts/inject_secrets.py. Do not edit, do not commit.",
    "#pragma once",
    "",
]
for name, value in values.items():
    if name in NUMERIC:
        if not value.isdigit():
            raise SystemExit(f"inject_secrets: {PREFIX}{name} must be a number")
        lines.append(f"#define {name} {int(value)}")
    else:
        lines.append(f"#define {name} {c_string(value)}")

# The espota transport needs the same password on its command line. Injecting it
# here keeps it out of shell history, but it goes through SCons substitution on the
# way -- the identical trap this file documents for -DMQTT_PASSWORD, and with the
# identical symptom: a $ in the value silently shortens it and the upload just
# fails to authenticate. Refuse rather than mangle.
if env.subst("$UPLOAD_PROTOCOL") == "espota":  # noqa: F821
    ota = values["OTA_PASSWORD"]
    if not ota:
        raise SystemExit(
            "inject_secrets: over-the-air upload needs PERGOLA_OTA_PASSWORD set.\n\n"
            "  The daemon does not start ArduinoOTA without it, so there is nothing\n"
            "  listening. Set it in .env, flash once over USB, then use -e esp32dev-ota.\n"
        )
    if "$" in ota:
        raise SystemExit(
            "inject_secrets: PERGOLA_OTA_PASSWORD contains '$', which SCons expands\n"
            "  inside upload flags -- the value would reach espota truncated and the\n"
            "  upload would fail authentication for no visible reason. Use a password\n"
            "  without '$'; letters and digits are the safe set here.\n"
        )
    # Stashed rather than appended. UPLOADERFLAGS set from a pre-script is
    # overwritten when the platform configures the upload target afterwards -- the
    # observed symptom was espota reporting auth='' and the device answering
    # "Authenticating...FAIL". scripts/ota_auth.py appends it as a post-script,
    # which runs late enough to survive.
    env["PERGOLA_OTA_AUTH"] = ota  # noqa: F821

build_dir = env.subst("$BUILD_DIR")  # noqa: F821
os.makedirs(build_dir, exist_ok=True)
header = os.path.join(build_dir, "pergola_secrets.h")
content = "\n".join(lines) + "\n"

previous = None
if os.path.isfile(header):
    with open(header, encoding="utf-8") as handle:
        previous = handle.read()

if previous != content:
    with open(header, "w", encoding="utf-8") as handle:
        handle.write(content)
    os.chmod(header, 0o600)

    # SCons cannot see a force-included header as a dependency -- it is not in
    # any translation unit's #include graph, and the command line is unchanged
    # because only the file's *contents* differ. So an incremental build after
    # editing .env silently keeps the previous credentials in the image: you fix
    # the password, upload, and flash the old one, then blame the password. Drop
    # the project objects so they recompile. Only src/ needs it -- the framework
    # and library objects are force-included too but never reference these
    # macros, so their output cannot depend on them.
    shutil.rmtree(os.path.join(build_dir, "src"), ignore_errors=True)

# CCFLAGS rather than CFLAGS/CXXFLAGS: it is the one SCons applies to both C and
# C++ translation units. The path itself contains no $, so substituting it is safe.
env.Append(CCFLAGS=["-include", header])  # noqa: F821

# Confirm what was found without ever echoing a value.
print(
    "inject_secrets: wifi credentials set, broker {}:{} ({}), ota {}".format(
        values["MQTT_HOST"],
        values["MQTT_PORT"],
        "anonymous" if not values["MQTT_USER"] else "authenticated",
        "enabled" if values["OTA_PASSWORD"] else "DISABLED (no password)",
    ),
    file=sys.stderr,
)
