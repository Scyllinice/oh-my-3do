#!/usr/bin/env python3
"""Inspect or stage one release stream. Never creates tags or publishes releases."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PLATFORMS = ("Windows-x64", "Linux-x64", "macOS-arm64", "macOS-x64")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def versions(root=ROOT):
    def find(path, pattern):
        match = re.search(pattern, (root / path).read_text(), re.M)
        if not match:
            raise ValueError(f"Missing version in {path}")
        return match[1]

    menu = find("src/main.c", r'^#define MENU_VERSION\s+"([^"]+)"')
    desktop = find("desktop/shell/Cargo.toml", r'^version\s*=\s*"([^"]+)"')
    for value in (menu, desktop):
        if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", value):
            raise ValueError(f"Invalid stable version: {value}")
    checks = {
        "menu media tools": (
            find("tools/organize-library.py", r"""^SCANNER_VERSION\s*=\s*['"]([^'"]+)['"]"""),
            menu,
        ),
        "menu tools README": (
            find("assets/media-tools-README.txt", r"^Oh My 3DO media tools \(menu v([^)]+)\)"),
            menu,
        ),
        "desktop core": (find("desktop/Cargo.toml", r'^version\s*=\s*"([^"]+)"'), desktop),
        "Tauri configuration": (
            json.loads((root / "desktop/shell/tauri.conf.json").read_text())["version"],
            desktop,
        ),
    }
    for lock in ("desktop/Cargo.lock", "desktop/shell/Cargo.lock"):
        text = (root / lock).read_text()
        for name in (
            ("oh-my-3do-library",)
            if lock == "desktop/Cargo.lock"
            else ("oh-my-3do-library", "oh-my-3do-desktop")
        ):
            match = re.search(r'name = "' + name + r'"\nversion = "([^"]+)"', text)
            if not match:
                raise ValueError(f"Missing {name} package in {lock}")
            checks[f"{lock}: {name}"] = (match[1], desktop)
    for label, (actual, expected) in checks.items():
        if actual != expected:
            raise ValueError(f"{label} is {actual}; expected {expected}")
    return {"menu": menu, "desktop": desktop}


def asset_names(component, version, platforms):
    if component == "menu":
        return [
            "boot.iso",
            "boot.iso.sha256",
            "menu-demo.iso",
            "menu-demo.iso.sha256",
            "media-tools.zip",
            "media-tools.zip.sha256",
            "ohmy3do.ini",
        ]
    names = []
    for platform in platforms:
        base = f"OhMy3DO-Library-{platform}-v{version}"
        files = [base + ".zip"]
        if platform == "Windows-x64":
            files.append(base + "-Setup.exe")
        elif platform == "Linux-x64":
            files.append(base + "-Install.run")
        for name in files:
            names.extend((name, name + ".sha256"))
    return names


def stage(component, platforms, output, root=ROOT):
    version = versions(root)[component]
    if tuple(map(int, version.split("."))) <= (1, 0, 0):
        raise ValueError("The combined 1.0.0 release is final; use a higher version")
    names = asset_names(component, version, platforms)
    if output.exists():
        raise ValueError(f"Staging destination already exists: {output}")
    # Validate the complete input set before creating the destination.
    for name in names:
        path = root / "iso" / name
        if not path.is_file() or path.is_symlink():
            raise ValueError(f"Missing release asset: {path}")
        if name.endswith(".sha256"):
            target = name.removesuffix(".sha256")
            if path.read_text().split() != [digest(root / "iso" / target), target]:
                raise ValueError(f"Stale or invalid checksum: {name}")
    if component == "desktop":
        for platform in platforms:
            archive = root / "iso" / f"OhMy3DO-Library-{platform}-v{version}.zip"
            with zipfile.ZipFile(archive) as z:
                info = json.loads(z.read("OhMy3DO-Library/BUILD.json"))
                if info["app_version"] != version or info["platform"] != platform:
                    raise ValueError(f"Package version/platform mismatch: {archive}")
    else:
        with zipfile.ZipFile(root / "iso/media-tools.zip") as archive:
            for member, source in [
                ("organize-library.py", "tools/organize-library.py"),
                ("README.txt", "assets/media-tools-README.txt"),
                ("ohmy3do.ini", "ohmy3do.ini"),
            ]:
                if archive.read(member) != (root / source).read_bytes():
                    raise ValueError(f"Rebuild media-tools.zip: stale {member}")
        # The builder preserves edited iso/ settings. Release the source defaults.
        if (root / "iso/ohmy3do.ini").read_bytes() != (root / "ohmy3do.ini").read_bytes():
            raise ValueError(
                "iso/ohmy3do.ini differs from source defaults; prepare release defaults first"
            )
        for mode in ("menu", "demo"):
            audit = root / f"build/console-{mode}/source.sha256"
            expected = digest(root / "src/main.c")
            if not audit.is_file() or not any(
                line.split()[0] == expected and line.split()[-1].endswith("/src/main.c")
                for line in audit.read_text().splitlines()
                if line.split()
            ):
                raise ValueError(f"Rebuild {mode}: source version does not match its build audit")
    output.mkdir(parents=True)
    for name in names:
        shutil.copy2(root / "iso" / name, output / name)
    for name in ("LICENSE", "THIRD_PARTY.md"):
        shutil.copy2(root / name, output / name)
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root, text=True))
    info = {
        "component": component,
        "version": version,
        "tag": f"{component}-v{version}",
        "revision": revision,
        "working_tree_dirty": dirty,
        "platforms": platforms if component == "desktop" else [],
    }
    (output / "BUILD.json").write_text(json.dumps(info, indent=2) + "\n")
    files = sorted(path for path in output.iterdir() if path.is_file())
    (output / "SHA256SUMS").write_text("".join(f"{digest(path)}  {path.name}\n" for path in files))
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("component", choices=("menu", "desktop"))
    parser.add_argument(
        "--stage",
        action="store_true",
        help="copy only this stream's verified assets to a new directory",
    )
    parser.add_argument(
        "--platform",
        choices=PLATFORMS,
        action="append",
        help="desktop platforms; defaults to Windows and Linux",
    )
    parser.add_argument("--output", type=Path, help="new staging directory (never overwritten)")
    args = parser.parse_args()
    if args.component == "menu" and args.platform:
        parser.error("--platform applies only to desktop releases")
    if args.output and not args.stage:
        parser.error("--output requires --stage")
    try:
        version = versions()[args.component]
        platforms = list(dict.fromkeys(args.platform or ["Windows-x64", "Linux-x64"]))
        info = {
            "component": args.component,
            "version": version,
            "tag": f"{args.component}-v{version}",
            "assets": asset_names(args.component, version, platforms),
        }
        if args.stage:
            output = args.output or ROOT / "build" / "releases" / info["tag"]
            info = stage(args.component, platforms, output)
            info["directory"] = str(output)
        print(json.dumps(info, indent=2))
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
