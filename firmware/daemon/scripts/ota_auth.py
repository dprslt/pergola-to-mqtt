"""Append espota's --auth flag, late enough that it survives.

Split out of inject_secrets.py for one reason: ordering. A `pre:` script runs
before the platform configures the upload target, and whatever it appends to
UPLOADERFLAGS is replaced by that configuration. espota then runs with auth=''
and the device answers "Authenticating...FAIL", which reads like a wrong password
rather than a flag that never arrived.

A `post:` script runs after that configuration, so the append sticks. The value
itself, and the refusal to accept a password containing '$', both stay in
inject_secrets.py where the .env resolution lives.
"""

Import("env")  # noqa: F821

if env.subst("$UPLOAD_PROTOCOL") == "espota":  # noqa: F821
    auth = env.get("PERGOLA_OTA_AUTH")  # noqa: F821
    if auth:
        env.Append(UPLOADERFLAGS=["--auth=" + auth])  # noqa: F821
