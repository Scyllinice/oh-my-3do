#!/usr/bin/env python3
"""Start the menu from the boot volume, so it runs from an ODE on any XBUS unit.

Usage: patch-app-startup.py APP_STARTUP AUDIT_JSON

After the startup script, Portfolio's shell decides what to run by asking the
first CD-ROM device for its status (LaunchApp() in src/app/shell/launchapp.c,
which opens OpenNamedDevice("cd-rom", 0)). With an ODE on the rear expansion
port, that first device is the internal drive. If it is empty, the shell runs
$RunNoCD and loops, and never starts $app/LaunchMe from the ODE.

AppStartup runs before that check and belongs to the menu disc, so this ends
it with $boot/LaunchMe, which starts the menu from whichever device the console
booted from: unit 0 (an internal ODE) or the expansion port. It runs after
prepare-runtime.py has checked the staged file is the SDK's, and refuses any
other AppStartup. The change is recorded in AUDIT_JSON.
"""
import hashlib
import json
from pathlib import Path
import sys

SDK_SHA256 = "8f24e47bd2578ea75d9f155c725125bb52493ae1c6b4150e77eedfe902411343"
ADDED = (
    b"\n"
    b"## Start the menu from the boot volume. The shell would otherwise ask the\n"
    b"## first CD-ROM device (the internal drive, when the ODE is on the expansion\n"
    b"## port) and stop there if it is empty.\n"
    b"$boot/LaunchMe\n"
)
PATCHED_SHA256 = "e3e6f94fdb4d890b474a553b85f7c63a6a88007739fc607a5fa191f0bca2ac2a"


def patch(app_startup, audit):
    original = app_startup.read_bytes()
    if hashlib.sha256(original).hexdigest() != SDK_SHA256:
        raise SystemExit(f"{app_startup} is not the SDK AppStartup this patch was made for")
    data = original + ADDED
    if hashlib.sha256(data).hexdigest() != PATCHED_SHA256:
        raise SystemExit("Patched AppStartup does not match the tested result")
    app_startup.write_bytes(data)

    record = json.loads(audit.read_text())
    record.setdefault("modifications", []).append({
        "path": "AppStartup",
        "change": "ends with $boot/LaunchMe, so the menu starts from the boot device "
                  "instead of depending on the first CD-ROM device (the internal drive "
                  "when the ODE is on the expansion port)",
        "reference": "Portfolio src/app/shell/launchapp.c LaunchApp()",
        "sdk_sha256": SDK_SHA256,
        "patched_sha256": PATCHED_SHA256,
    })
    audit.write_text(json.dumps(record, indent=2) + "\n")
    print("Patched AppStartup: starts the menu from the boot volume")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    patch(Path(sys.argv[1]), Path(sys.argv[2]))
