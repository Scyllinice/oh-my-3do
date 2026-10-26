#!/usr/bin/env python3
"""Exercise Tauri executable replacement, frontend-ready acknowledgement and
rollback in disposable directories. Run under Xvfb/D-Bus.

The desktop app updates itself by starting a copy of the new executable with
--apply-desktop-update PLAN, where PLAN is a JSON file naming the executable
to replace, the staged new one and the support files to swap in. This test
writes such plans by hand and checks four cases:

  restart + success   the new version is installed, started, and its
                      interface reports it is ready
  restart + rollback  the "new version" can't start, so the original
                      executable and files are put back
  on exit + success   the update is applied when the app closes, and no
                      app process is left running afterwards
  on exit + rollback  as above, with the failed update rolled back

Each case runs in its own folder under build/desktop-update-smoke/, using the
Docker-built Linux binary. See docs/desktop-development.md for the docker
command that provides the virtual display and D-Bus session.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "build/desktop-update-smoke"
BINARY = ROOT / "build/tauri-linux-target/release/oh-my-3do-library"

HELPER_TIMEOUT_SECONDS = 60

# A process ID that is never running, so the updater doesn't wait for a
# "parent" app to exit first.
MISSING_PARENT_PID = 99999999

# The contents of the settings file the update must not touch.
PERSONAL_SETTINGS = "Personal preferences must stay unchanged"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def running_commands():
    """Yield (pid, program path) for every running process, from /proc."""
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            program = (entry / "cmdline").read_bytes().split(b"\0")[0]
        except OSError:
            continue  # the process exited while we looked
        yield int(entry.name), program


def cleanup(exe):
    """Stop any process still running from `exe` (the relaunched app)."""
    for pid, program in running_commands():
        if program == str(exe).encode():
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                pass


class Case:
    """The files for one test case, all inside its own work folder."""

    def __init__(self, restart, fail):
        self.restart = restart
        self.fail = fail
        name = ("restart-" if restart else "on-exit-") + ("rollback" if fail else "success")
        self.work = BASE / name
        self.exe = self.work / "OhMy3DOLibrary"  # the installed app to replace
        self.stage = self.work / "staged"  # the downloaded new version
        self.previous = self.work / "previous"  # where the old version is kept
        self.ready = self.work / "ready"  # created once the new interface starts
        self.support = self.work / "asset-readme"  # the downloaded new README
        self.readme = self.work / "README.md"  # the installed README
        self.settings = self.work / "native-settings.json"
        self.cache = self.work / "cache/oh-my-3do"
        self.process_log = self.work / "process.log"


def prepare(case):
    """Lay out an installed app, a staged update and the app's settings."""
    case.work.mkdir(parents=True, exist_ok=True)

    # Remove what an earlier run left behind.
    stale_log = case.work / "install.log"
    for path in [case.exe, case.stage, case.previous, case.ready, stale_log]:
        if path.exists():
            path.unlink()

    # The "installed" app: the real binary with extra bytes, so its hash
    # differs from the update's.
    shutil.copy2(BINARY, case.exe)
    with case.exe.open("ab") as stream:
        stream.write(b"ORIGINAL VERSION")

    # The update: the real binary, or for rollback cases an ELF header with
    # nothing behind it, which fails to start.
    shutil.copy2(BINARY, case.stage)
    if case.fail:
        case.stage.write_bytes(b"\x7fELF" + bytes(100))
        case.stage.chmod(0o755)

    case.support.write_text("Updated release documentation")
    case.readme.write_text("Original release documentation")
    case.settings.write_text(PERSONAL_SETTINGS)

    # Keep the relaunched app offline and quiet.
    case.cache.mkdir(parents=True, exist_ok=True)
    (case.cache / "native-settings.json").write_text(
        json.dumps(dict(offline=True, disable_update_checks=True))
    )


def write_plan(case):
    """Write the install plan the updater reads, and return (its path, the plan)."""
    update_work = case.cache / "self-update/test"
    update_work.mkdir(parents=True, exist_ok=True)

    plan = dict(
        restart=case.restart,
        executable=str(case.exe),
        staged=str(case.stage),
        previous=str(case.previous),
        ready=str(case.ready),
        log=str(update_work / "install.log"),
        original_sha256=sha(case.exe),
        new_sha256=sha(case.stage),
        parent=MISSING_PARENT_PID,
        assets=[dict(relative="README.md", staged=str(case.support), sha256=sha(case.support))],
    )
    path = case.work / "install.json"
    path.write_text(json.dumps(plan))
    return path, plan


def start_updater(case, plan_path):
    """Start the update helper with its settings folders inside the case's
    work folder and software rendering (there is no GPU in the container)."""
    env = os.environ.copy()
    env.update(
        XDG_CACHE_HOME=str(case.work / "cache"),
        XDG_CONFIG_HOME=str(case.work / "config"),
        XDG_DATA_HOME=str(case.work / "data"),
        WEBKIT_DISABLE_DMABUF_RENDERER="1",
        LIBGL_ALWAYS_SOFTWARE="1",
    )
    return subprocess.Popen(
        [str(BINARY), "--apply-desktop-update", str(plan_path)],
        env=env,
        stdout=open(case.process_log, "w"),
        stderr=subprocess.STDOUT,
    )


def check_rolled_back(case, result, original):
    """The update failed, and everything is as it was before."""
    assert result != 0
    assert sha(case.exe) == original
    assert case.readme.read_text() == "Original release documentation"
    assert not case.previous.exists()
    print(
        "Updater failure: launch error restored original executable and support files.",
        flush=True,
    )


def check_installed(case, result, plan, original):
    """The update succeeded: the new files are in place, the new interface
    started, and the old executable was kept."""
    assert result == 0, (result, case.process_log.read_text())
    assert case.ready.exists()
    assert sha(case.exe) == plan["new_sha256"]
    assert sha(case.previous) == original
    assert case.readme.read_text() == "Updated release documentation"
    print(
        "Updater success: replaced executable and docs, relaunched actual Tauri UI, "
        "received frontend-ready acknowledgement; previous executable retained.",
        flush=True,
    )


def check_result_report(case):
    """The updater's last-result.json, which the app shows on its next start."""
    report = json.loads((case.cache / "self-update/last-result.json").read_text())
    assert report["success"] == (not case.fail)
    assert report["previous_intact"] == case.fail
    assert report["executable"] == str(case.exe)
    assert report["detail"]


def check_nothing_left_running(case):
    """An update on exit must not leave the app open afterwards."""
    time.sleep(2)
    for _, program in running_commands():
        assert program != str(case.exe).encode(), "Update on exit left the UI running"
    print(
        "Update on exit: hidden startup check completed and no application process remains; "
        "result persisted.",
        flush=True,
    )


def run_case(restart, fail):
    case = Case(restart, fail)
    prepare(case)
    original = sha(case.exe)
    plan_path, plan = write_plan(case)

    helper = start_updater(case, plan_path)
    try:
        result = helper.wait(timeout=HELPER_TIMEOUT_SECONDS)
        if fail:
            check_rolled_back(case, result, original)
        else:
            check_installed(case, result, plan, original)
        check_result_report(case)
        if not restart:
            check_nothing_left_running(case)
        assert case.settings.read_text() == PERSONAL_SETTINGS
    finally:
        cleanup(case.exe)
        if helper.poll() is None:
            helper.kill()
            helper.wait()


def main():
    for restart, fail in [(True, False), (True, True), (False, False), (False, True)]:
        run_case(restart, fail)


if __name__ == "__main__":
    main()
