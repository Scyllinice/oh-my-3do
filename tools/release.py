#!/usr/bin/env python3
"""Inspect or stage one release stream. Never creates tags or publishes releases.

Usage: release.py menu|desktop [--stage] [--platform PLATFORM ...] [--output FOLDER]

The menu (boot.iso and its media tools) and the desktop app are released
separately, each with its own version and tag ("menu-v1.0.3",
"desktop-v1.0.5"). Without --stage this prints the stream's version, tag and
the files its release needs.

--stage checks those files in iso/ (present, checksums current, packages
built from the current sources and versions) and copies them, with the
license files, a BUILD.json and a SHA256SUMS, into a new folder
(build/releases/<tag>/ by default). An existing folder is never touched.
Tagging and uploading are done by hand; see docs/releasing.md.
"""
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
DEFAULT_PLATFORMS = ["Windows-x64", "Linux-x64"]

# Version 1.0.0 was the last release of both streams together.
COMBINED_RELEASE = (1, 0, 0)

# MAJOR.MINOR.PATCH with no leading zeros and no pre-release suffix.
STABLE_VERSION = r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"

MENU_ASSETS = [
    "boot.iso",
    "boot.iso.sha256",
    "menu-demo.iso",
    "menu-demo.iso.sha256",
    "media-tools.zip",
    "media-tools.zip.sha256",
    "ohmy3do.ini",
]

# Files in media-tools.zip that must match the checkout, as
# (name in the zip, path in the checkout).
MEDIA_TOOLS_CHECKED_FILES = [
    ("organize-library.py", "tools/organize-library.py"),
    ("README.txt", "assets/media-tools-README.txt"),
    ("ohmy3do.ini", "ohmy3do.ini"),
]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def find_version(root, path, pattern):
    """Return the first capture of `pattern` (a multi-line regex) in a file."""
    match = re.search(pattern, (root / path).read_text(), re.M)
    if not match:
        raise ValueError(f"Missing version in {path}")
    return match[1]


def lock_file_versions(root, expected):
    """Return {label: (version, expected)} for the project's own packages in
    both Cargo.lock files."""
    packages = {
        "desktop/Cargo.lock": ("oh-my-3do-library",),
        "desktop/shell/Cargo.lock": ("oh-my-3do-library", "oh-my-3do-desktop"),
    }
    checks = {}
    for lock, names in packages.items():
        text = (root / lock).read_text()
        for name in names:
            match = re.search(r'name = "' + name + r'"\nversion = "([^"]+)"', text)
            if not match:
                raise ValueError(f"Missing {name} package in {lock}")
            checks[f"{lock}: {name}"] = (match[1], expected)
    return checks


def versions(root=ROOT):
    """Return {"menu": version, "desktop": version}.

    The menu's version comes from src/main.c and the desktop app's from
    desktop/shell/Cargo.toml. Raises ValueError unless both are stable
    versions and every other place that repeats them agrees.
    """
    menu = find_version(root, "src/main.c", r'^#define MENU_VERSION\s+"([^"]+)"')
    desktop = find_version(root, "desktop/shell/Cargo.toml", r'^version\s*=\s*"([^"]+)"')
    for value in (menu, desktop):
        if not re.fullmatch(STABLE_VERSION, value):
            raise ValueError(f"Invalid stable version: {value}")

    # Every other copy of a version: {label: (found, expected)}.
    checks = {
        "menu media tools": (
            find_version(
                root,
                "tools/organize-library.py",
                r"""^SCANNER_VERSION\s*=\s*['"]([^'"]+)['"]""",
            ),
            menu,
        ),
        "menu tools README": (
            find_version(
                root,
                "assets/media-tools-README.txt",
                r"^Oh My 3DO media tools \(menu v([^)]+)\)",
            ),
            menu,
        ),
        "desktop core": (
            find_version(root, "desktop/Cargo.toml", r'^version\s*=\s*"([^"]+)"'),
            desktop,
        ),
        "Tauri configuration": (
            json.loads((root / "desktop/shell/tauri.conf.json").read_text())["version"],
            desktop,
        ),
    }
    checks.update(lock_file_versions(root, desktop))

    for label, (actual, expected) in checks.items():
        if actual != expected:
            raise ValueError(f"{label} is {actual}; expected {expected}")
    return {"menu": menu, "desktop": desktop}


def asset_names(component, version, platforms):
    """The files in iso/ that make up a release, each followed by its
    .sha256 file (except ohmy3do.ini, the editable settings)."""
    if component == "menu":
        return list(MENU_ASSETS)

    # Desktop: a portable zip per platform, plus an installer on Windows
    # and Linux.
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


def check_assets_and_checksums(root, names):
    """Every asset must be a real file, and every .sha256 must match the
    file it names (in sha256sum's "digest  name" format)."""
    for name in names:
        path = root / "iso" / name
        if not path.is_file() or path.is_symlink():
            raise ValueError(f"Missing release asset: {path}")
        if name.endswith(".sha256"):
            target = name.removesuffix(".sha256")
            if path.read_text().split() != [digest(root / "iso" / target), target]:
                raise ValueError(f"Stale or invalid checksum: {name}")


def check_desktop_packages(root, version, platforms):
    """Each portable zip must be this version, built for its platform."""
    for platform in platforms:
        archive = root / "iso" / f"OhMy3DO-Library-{platform}-v{version}.zip"
        with zipfile.ZipFile(archive) as package:
            info = json.loads(package.read("OhMy3DO-Library/BUILD.json"))
            if info["app_version"] != version or info["platform"] != platform:
                raise ValueError(f"Package version/platform mismatch: {archive}")


def build_audit_matches(audit, main_source_digest):
    """Whether a build's source.sha256 audit lists src/main.c with this digest."""
    for line in audit.read_text().splitlines():
        parts = line.split()
        if parts and parts[0] == main_source_digest and parts[-1].endswith("/src/main.c"):
            return True
    return False


def check_menu_build(root):
    """The menu's files must have been built from the current sources."""
    with zipfile.ZipFile(root / "iso/media-tools.zip") as archive:
        for member, source in MEDIA_TOOLS_CHECKED_FILES:
            if archive.read(member) != (root / source).read_bytes():
                raise ValueError(f"Rebuild media-tools.zip: stale {member}")

    # The builder preserves edited iso/ settings. Release the source defaults.
    if (root / "iso/ohmy3do.ini").read_bytes() != (root / "ohmy3do.ini").read_bytes():
        raise ValueError(
            "iso/ohmy3do.ini differs from source defaults; prepare release defaults first"
        )

    # Each console build records the hashes of its sources; boot.iso and
    # menu-demo.iso must come from today's src/main.c.
    expected = digest(root / "src/main.c")
    for mode in ("menu", "demo"):
        audit = root / f"build/console-{mode}/source.sha256"
        if not audit.is_file() or not build_audit_matches(audit, expected):
            raise ValueError(f"Rebuild {mode}: source version does not match its build audit")


def git_output(root, *arguments):
    return subprocess.check_output(["git", *arguments], cwd=root, text=True)


def stage(component, platforms, output, root=ROOT):
    """Check and copy one stream's release into the new folder `output`.

    Everything is checked before the folder is created, so a failed check
    leaves nothing behind. Returns the BUILD.json contents.
    """
    version = versions(root)[component]
    if tuple(map(int, version.split("."))) <= COMBINED_RELEASE:
        raise ValueError("The combined 1.0.0 release is final; use a higher version")
    names = asset_names(component, version, platforms)
    if output.exists():
        raise ValueError(f"Staging destination already exists: {output}")

    # Validate the complete input set before creating the destination.
    check_assets_and_checksums(root, names)
    if component == "desktop":
        check_desktop_packages(root, version, platforms)
    else:
        check_menu_build(root)

    output.mkdir(parents=True)
    for name in names:
        shutil.copy2(root / "iso" / name, output / name)
    for name in ("LICENSE", "THIRD_PARTY.md"):
        shutil.copy2(root / name, output / name)

    info = {
        "component": component,
        "version": version,
        "tag": f"{component}-v{version}",
        "revision": git_output(root, "rev-parse", "HEAD").strip(),
        "working_tree_dirty": bool(git_output(root, "status", "--porcelain")),
        "platforms": platforms if component == "desktop" else [],
    }
    (output / "BUILD.json").write_text(json.dumps(info, indent=2) + "\n")

    files = sorted(path for path in output.iterdir() if path.is_file())
    sums = "".join(f"{digest(path)}  {path.name}\n" for path in files)
    (output / "SHA256SUMS").write_text(sums)
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
        # Drop repeated --platform values, keeping their order.
        platforms = list(dict.fromkeys(args.platform or DEFAULT_PLATFORMS))
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
