#!/usr/bin/env python3
"""Describe a completed local developer build; consumed only in developer mode."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def write_manifest(component, artifact, version=None, platform=None):
    artifact = Path(artifact).resolve(strict=True)
    if component == "menu":
        version = (
            version
            or re.search(
                r'^#define MENU_VERSION\s+"([^"]+)"', (ROOT / "src/main.c").read_text(), re.M
            )[1]
        )
        if artifact.name != "boot.iso":
            raise ValueError("Menu artifact must be boot.iso")
        data = {"schema": 1, "component": "menu", "version": version, "artifact": artifact.name}
        filename = "menu-update.json"
    else:
        if platform not in ("Windows-x64", "Linux-x64", "macOS-arm64", "macOS-x64"):
            raise ValueError("Choose a desktop platform")
        with zipfile.ZipFile(artifact) as archive:
            info = json.loads(archive.read("OhMy3DO-Library/BUILD.json"))
            version = version or info["app_version"]
            if info["app_version"] != version or info["platform"] != platform:
                raise ValueError("Desktop package version/platform mismatch")
            member = "OhMy3DO-Library/OhMy3DOLibrary" + (
                ".exe" if platform.startswith("Windows") else ""
            )
            digest = hashlib.sha256(archive.read(member)).hexdigest()
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
        filename = f"desktop-update-{platform}.json"
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version):
        raise ValueError("Invalid build version")
    data["sha256"] = hashlib.sha256(artifact.read_bytes()).hexdigest()
    sidecar = artifact.with_name(artifact.name + ".sha256")
    if sidecar.read_text().split() != [data["sha256"], artifact.name]:
        raise ValueError("Build checksum sidecar is missing or stale")
    target = artifact.parent / filename
    with tempfile.NamedTemporaryFile(
        mode="w", dir=artifact.parent, delete=False, encoding="utf-8"
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
