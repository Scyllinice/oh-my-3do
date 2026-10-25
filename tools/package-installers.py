#!/usr/bin/env python3
"""Build per-user installers from the prepared, licensed desktop package folders."""
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


def nsis_string(value):
    return str(value).replace("$", "$$").replace('"', '$\\"')


def payload(platform):
    folder = ROOT / "build" / ("desktop-" + platform)
    info = json.loads((folder / "BUILD.json").read_text())
    if info["platform"] != platform or info["dependencies_without_separate_notice_files"]:
        raise ValueError("Prepare a complete licensed desktop package first")
    exe = folder / ("OhMy3DOLibrary.exe" if platform == "Windows-x64" else "OhMy3DOLibrary")
    if hashlib.sha256(exe.read_bytes()).hexdigest() != info["binary_sha256"]:
        raise ValueError("Prepared executable does not match BUILD.json")
    for path in folder.rglob("*"):
        if path.is_symlink():
            raise ValueError("Installer payload cannot contain symbolic links")
        if path.is_file() and any(c in path.relative_to(folder).as_posix() for c in '\r\n"$'):
            raise ValueError("Unsupported installer payload filename")
    return folder, info


def checksum(path):
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    path.with_name(path.name + ".sha256").write_text(f"{digest}  {path.name}\n")
    return digest


def windows(webview):
    if not shutil.which("makensis"):
        raise SystemExit("NSIS is required: run this script in tools/installer.Dockerfile")
    nsis_license = Path("/usr/share/doc/nsis/copyright")
    if not nsis_license.is_file():
        raise SystemExit("NSIS dependency notices are required: use tools/installer.Dockerfile")
    if not webview or not webview.is_file():
        raise SystemExit(
            "Supply --webview-bootstrapper from Microsoft: https://go.microsoft.com/fwlink/p/?LinkId=2124703"
        )
    runtime = webview.read_bytes()
    if not runtime.startswith(b"MZ") or not 1024 < len(runtime) <= 8 * 1024 * 1024:
        raise ValueError("Invalid WebView2 bootstrapper")
    folder, info = payload("Windows-x64")
    work = ROOT / "build/installer-Windows-x64"
    work.mkdir(exist_ok=True)
    paths = sorted(p for p in folder.rglob("*") if p.is_file())
    exe = folder / "OhMy3DOLibrary.exe"
    paths.remove(exe)
    paths.insert(0, exe)
    install = []
    uninstall = []
    for path in paths:
        relative = path.relative_to(folder)
        directory = (
            "" if relative.parent == Path(".") else "\\" + str(relative.parent).replace("/", "\\")
        )
        install += [
            f'  SetOutPath "$INSTDIR{nsis_string(directory)}"',
            f'  File "{nsis_string(path)}"',
        ]
        if relative.parts[0] != "licenses" and relative.name != "OhMy3DOLibrary.exe":
            uninstall.append(f'  Delete "$INSTDIR\\{nsis_string(relative)}"')
    (work / "install-files.nsh").write_text("\n".join(install) + "\n")
    (work / "uninstall-files.nsh").write_text("\n".join(uninstall) + "\n")
    output = ROOT / "iso" / f'OhMy3DO-Library-Windows-x64-v{info["app_version"]}-Setup.exe'
    definitions = {
        "OUTPUT": output,
        "VERSION": info["app_version"],
        "PAYLOAD": folder,
        "ICON": ROOT / "assets/app-icon.ico",
        "WEBVIEW": webview.resolve(),
        "NSIS_LICENSE": nsis_license,
        "INSTALL_FILES": work / "install-files.nsh",
        "UNINSTALL_FILES": work / "uninstall-files.nsh",
    }
    subprocess.run(
        [
            "makensis",
            "-WX",
            "-V2",
            *[f"-D{k}={v}" for k, v in definitions.items()],
            str(TEMPLATES / "windows.nsi"),
        ],
        check=True,
    )
    result = {
        "format": "NSIS per-user setup",
        "payload_revision": info["revision"],
        "binary_sha256": info["binary_sha256"],
        "webview_bootstrapper_sha256": hashlib.sha256(runtime).hexdigest(),
        "webview_source": "https://go.microsoft.com/fwlink/p/?LinkId=2124703",
        "sha256": checksum(output),
    }
    (work / "BUILD.json").write_text(json.dumps(result, indent=2) + "\n")
    print("Built:", output)


def linux():
    folder, info = payload("Linux-x64")
    archive = io.BytesIO()
    with gzip.GzipFile(fileobj=archive, mode="wb", mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w") as tar:
            for path in sorted(folder.rglob("*")):
                if not path.is_file():
                    continue
                entry = tarfile.TarInfo(path.relative_to(folder).as_posix())
                data = path.read_bytes()
                entry.size = len(data)
                entry.mode = 0o755 if path.name == "OhMy3DOLibrary" else 0o644
                tar.addfile(entry, io.BytesIO(data))
            data = (TEMPLATES / "linux-uninstall.sh").read_bytes()
            entry = tarfile.TarInfo("uninstall.sh")
            entry.size = len(data)
            entry.mode = 0o755
            tar.addfile(entry, io.BytesIO(data))
    data = archive.getvalue()
    # Read back every payload member before appending it to the shell header.
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        for member in tar.getmembers():
            expected = (
                (TEMPLATES / "linux-uninstall.sh")
                if member.name == "uninstall.sh"
                else folder / member.name
            )
            if tar.extractfile(member).read() != expected.read_bytes():
                raise ValueError("Installer payload differs: " + member.name)
    header = (
        (TEMPLATES / "linux-header.sh")
        .read_text()
        .replace("@@VERSION@@", info["app_version"])
        .replace("@@PAYLOAD_SHA256@@", hashlib.sha256(data).hexdigest())
    )
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
