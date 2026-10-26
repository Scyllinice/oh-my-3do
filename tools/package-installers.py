#!/usr/bin/env python3
"""Build per-user installers from the prepared, licensed desktop package folders.

Usage: package-installers.py [--platform Windows-x64|Linux-x64|all]
                             [--webview-bootstrapper FILE]

Reads build/desktop-<platform>/ (made by package-desktop.py) and writes to
iso/:
  Windows: OhMy3DO-Library-Windows-x64-v<version>-Setup.exe, an NSIS setup
    that installs for the current user and bundles Microsoft's WebView2
    bootstrapper (pass it with --webview-bootstrapper). Needs makensis, so
    run it in the image from tools/installer.Dockerfile.
  Linux: OhMy3DO-Library-Linux-x64-v<version>-Install.run, a shell script
    with a gzipped tar of the package appended.
Each installer gets a .sha256 file beside it.
"""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
TEMPLATES = ROOT / "tools/installers"

WEBVIEW_SOURCE = "https://go.microsoft.com/fwlink/p/?LinkId=2124703"
MAX_WEBVIEW_BYTES = 8 * 1024 * 1024

# Where the installer image keeps NSIS's own license, which ships in the setup.
NSIS_LICENSE = Path("/usr/share/doc/nsis/copyright")

# Characters the installer scripts can't safely carry in a file name.
UNSAFE_NAME_CHARACTERS = '\r\n"$'


def nsis_string(value):
    """Escape a value for a double-quoted NSIS string ($ and " are special)."""
    return str(value).replace("$", "$$").replace('"', '$\\"')


def executable_name(platform):
    if platform == "Windows-x64":
        return "OhMy3DOLibrary.exe"
    return "OhMy3DOLibrary"


def payload(platform):
    """Return (package folder, its BUILD.json) after checking the package is
    complete: built for `platform`, every dependency's license notice
    present, the executable matching its recorded hash, and no symbolic
    links or file names the installers can't handle."""
    folder = ROOT / "build" / ("desktop-" + platform)
    info = json.loads((folder / "BUILD.json").read_text())
    if info["platform"] != platform or info["dependencies_without_separate_notice_files"]:
        raise ValueError("Prepare a complete licensed desktop package first")

    exe = folder / executable_name(platform)
    if hashlib.sha256(exe.read_bytes()).hexdigest() != info["binary_sha256"]:
        raise ValueError("Prepared executable does not match BUILD.json")

    for path in folder.rglob("*"):
        if path.is_symlink():
            raise ValueError("Installer payload cannot contain symbolic links")
        relative_name = path.relative_to(folder).as_posix()
        if path.is_file() and any(c in relative_name for c in UNSAFE_NAME_CHARACTERS):
            raise ValueError("Unsupported installer payload filename")
    return folder, info


def checksum(path):
    """Write `path`.sha256 in sha256sum's format and return the digest."""
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    path.with_name(path.name + ".sha256").write_text(f"{digest}  {path.name}\n")
    return digest


def read_webview_bootstrapper(webview):
    """Return the bootstrapper's bytes after a basic check that it is a
    Windows program of a plausible size."""
    if not webview or not webview.is_file():
        raise SystemExit(f"Supply --webview-bootstrapper from Microsoft: {WEBVIEW_SOURCE}")
    runtime = webview.read_bytes()
    if not runtime.startswith(b"MZ") or not 1024 < len(runtime) <= MAX_WEBVIEW_BYTES:
        raise ValueError("Invalid WebView2 bootstrapper")
    return runtime


def nsis_file_lists(folder):
    """Return the NSIS lines that install and uninstall the package's files.

    The executable is installed first. The uninstall list leaves out the
    executable and the licenses folder, which windows.nsi removes itself.
    """
    paths = sorted(p for p in folder.rglob("*") if p.is_file())
    exe = folder / "OhMy3DOLibrary.exe"
    paths.remove(exe)
    paths.insert(0, exe)

    install = []
    uninstall = []
    for path in paths:
        relative = path.relative_to(folder)
        if relative.parent == Path("."):
            directory = ""
        else:
            directory = "\\" + str(relative.parent).replace("/", "\\")

        install.append(f'  SetOutPath "$INSTDIR{nsis_string(directory)}"')
        install.append(f'  File "{nsis_string(path)}"')
        if relative.parts[0] != "licenses" and relative.name != "OhMy3DOLibrary.exe":
            uninstall.append(f'  Delete "$INSTDIR\\{nsis_string(relative)}"')
    return install, uninstall


def windows(webview):
    """Build the Windows setup program with NSIS."""
    if not shutil.which("makensis"):
        raise SystemExit("NSIS is required: run this script in tools/installer.Dockerfile")
    if not NSIS_LICENSE.is_file():
        raise SystemExit("NSIS dependency notices are required: use tools/installer.Dockerfile")
    runtime = read_webview_bootstrapper(webview)
    folder, info = payload("Windows-x64")

    # Write the file lists that windows.nsi includes.
    work = ROOT / "build/installer-Windows-x64"
    work.mkdir(exist_ok=True)
    install, uninstall = nsis_file_lists(folder)
    (work / "install-files.nsh").write_text("\n".join(install) + "\n")
    (work / "uninstall-files.nsh").write_text("\n".join(uninstall) + "\n")

    # Compile, passing everything windows.nsi needs as /D definitions.
    # -WX treats warnings as errors; -V2 shows only warnings and errors.
    output = ROOT / "iso" / f'OhMy3DO-Library-Windows-x64-v{info["app_version"]}-Setup.exe'
    definitions = {
        "OUTPUT": output,
        "VERSION": info["app_version"],
        "PAYLOAD": folder,
        "ICON": ROOT / "assets/app-icon.ico",
        "WEBVIEW": webview.resolve(),
        "NSIS_LICENSE": NSIS_LICENSE,
        "INSTALL_FILES": work / "install-files.nsh",
        "UNINSTALL_FILES": work / "uninstall-files.nsh",
    }
    command = ["makensis", "-WX", "-V2"]
    command += [f"-D{key}={value}" for key, value in definitions.items()]
    command.append(str(TEMPLATES / "windows.nsi"))
    subprocess.run(command, check=True)

    # Record what went into the setup, for the release notes and audits.
    result = {
        "format": "NSIS per-user setup",
        "payload_revision": info["revision"],
        "binary_sha256": info["binary_sha256"],
        "webview_bootstrapper_sha256": hashlib.sha256(runtime).hexdigest(),
        "webview_source": WEBVIEW_SOURCE,
        "sha256": checksum(output),
    }
    (work / "BUILD.json").write_text(json.dumps(result, indent=2) + "\n")
    print("Built:", output)


def add_tar_file(tar, name, data, mode):
    """Add a file to `tar` with a fixed owner and date (the defaults), so the
    archive is the same every time it is built."""
    entry = tarfile.TarInfo(name)
    entry.size = len(data)
    entry.mode = mode
    tar.addfile(entry, io.BytesIO(data))


def linux_archive(folder):
    """Return a gzipped tar of the package plus uninstall.sh.

    The gzip timestamp is zero and the tar entries carry no dates, so the
    same package always gives the same bytes.
    """
    archive = io.BytesIO()
    with gzip.GzipFile(fileobj=archive, mode="wb", mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w") as tar:
            for path in sorted(folder.rglob("*")):
                if not path.is_file():
                    continue
                mode = 0o755 if path.name == "OhMy3DOLibrary" else 0o644
                add_tar_file(tar, path.relative_to(folder).as_posix(), path.read_bytes(), mode)

            uninstaller = (TEMPLATES / "linux-uninstall.sh").read_bytes()
            add_tar_file(tar, "uninstall.sh", uninstaller, 0o755)
    return archive.getvalue()


def check_linux_archive(data, folder):
    """Read back every member and compare it with its source file."""
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        for member in tar.getmembers():
            if member.name == "uninstall.sh":
                expected = TEMPLATES / "linux-uninstall.sh"
            else:
                expected = folder / member.name
            if tar.extractfile(member).read() != expected.read_bytes():
                raise ValueError("Installer payload differs: " + member.name)


def linux():
    """Build the self-extracting Linux installer: linux-header.sh (with the
    version and the archive's hash filled in) followed by the archive."""
    folder, info = payload("Linux-x64")
    data = linux_archive(folder)
    check_linux_archive(data, folder)

    header = (TEMPLATES / "linux-header.sh").read_text()
    header = header.replace("@@VERSION@@", info["app_version"])
    header = header.replace("@@PAYLOAD_SHA256@@", hashlib.sha256(data).hexdigest())

    output = ROOT / "iso" / f'OhMy3DO-Library-Linux-x64-v{info["app_version"]}-Install.run'
    output.write_bytes(header.encode() + data)
    output.chmod(0o755)
    checksum(output)
    print("Built:", output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=["Windows-x64", "Linux-x64", "all"], default="all")
    parser.add_argument("--webview-bootstrapper", type=Path)
    args = parser.parse_args()

    (ROOT / "iso").mkdir(exist_ok=True)
    if args.platform in ("Windows-x64", "all"):
        windows(args.webview_bootstrapper)
    if args.platform in ("Linux-x64", "all"):
        linux()


if __name__ == "__main__":
    main()
