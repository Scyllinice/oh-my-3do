#!/usr/bin/env python3
"""Describe a completed local developer build; consumed only in developer mode.

Writes the update manifest the desktop app reads in developer mode (see
updates.rs, local_release): menu-update.json beside a built boot.iso, or
desktop-update-<platform>.json beside a desktop package ZIP. The manifest
records the version and SHA-256 of the build (and, for the desktop, of the
executable inside it), after checking the build's checksum sidecar.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]

DESKTOP_PLATFORMS = ("Windows-x64", "Linux-x64", "macOS-arm64", "macOS-x64")

# A plain "X.Y.Z" version, without leading zeros.
VERSION_PATTERN = r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"


def menu_version():
    """The menu's version, from MENU_VERSION in src/main.c."""
    source = (ROOT / "src/main.c").read_text()
    return re.search(r'^#define MENU_VERSION\s+"([^"]+)"', source, re.M)[1]


def menu_manifest(artifact, version):
    """The manifest for a built boot.iso."""
    if artifact.name != "boot.iso":
        raise ValueError("Menu artifact must be boot.iso")
    data = {"schema": 1, "component": "menu", "version": version, "artifact": artifact.name}
    return data, "menu-update.json"


def desktop_manifest(artifact, version, platform):
    """The manifest for a desktop package ZIP. Checks the package's
    BUILD.json matches the version and platform, and its executable."""
    if platform not in DESKTOP_PLATFORMS:
        raise ValueError("Choose a desktop platform")
    with zipfile.ZipFile(artifact) as archive:
        info = json.loads(archive.read("OhMy3DO-Library/BUILD.json"))
        version = version or info["app_version"]
        if info["app_version"] != version or info["platform"] != platform:
            raise ValueError("Desktop package version/platform mismatch")
        extension = ".exe" if platform.startswith("Windows") else ""
        executable = archive.read("OhMy3DO-Library/OhMy3DOLibrary" + extension)
        digest = hashlib.sha256(executable).hexdigest()
        if info["binary_sha256"] != digest:
            raise ValueError("Desktop executable does not match BUILD.json")
    if artifact.name != f"OhMy3DO-Library-{platform}-v{version}.zip":
        raise ValueError("Unexpected desktop package filename")
    data = {
        "schema": 1,
        "component": "desktop",
        "platform": platform,
        "version": version,
        "artifact": artifact.name,
        "binary_sha256": digest,
    }
    return data, f"desktop-update-{platform}.json"


def write_atomically(target, data):
    """Writes JSON to `target` through a temporary file, so a reader never
    sees a half-written manifest."""
    with tempfile.NamedTemporaryFile(
        mode="w", dir=target.parent, delete=False, encoding="utf-8"
    ) as handle:
        temporary = Path(handle.name)
        json.dump(data, handle, indent=2)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    try:
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)


def write_manifest(component, artifact, version=None, platform=None):
    """Writes the manifest for a build ("menu" or "desktop") and returns its path."""
    artifact = Path(artifact).resolve(strict=True)
    if component == "menu":
        version = version or menu_version()
        data, filename = menu_manifest(artifact, version)
    else:
        data, filename = desktop_manifest(artifact, version, platform)
        version = data["version"]
    if not re.fullmatch(VERSION_PATTERN, version):
        raise ValueError("Invalid build version")

    # The build's own checksum file must agree.
    data["sha256"] = hashlib.sha256(artifact.read_bytes()).hexdigest()
    sidecar = artifact.with_name(artifact.name + ".sha256")
    if sidecar.read_text().split() != [data["sha256"], artifact.name]:
        raise ValueError("Build checksum sidecar is missing or stale")

    target = artifact.parent / filename
    write_atomically(target, data)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("component", choices=["menu", "desktop"])
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--version")
    parser.add_argument("--platform")
    args = parser.parse_args()
    print(write_manifest(args.component, args.artifact, args.version, args.platform))


if __name__ == "__main__":
    main()
