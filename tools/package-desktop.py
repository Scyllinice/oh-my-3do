#!/usr/bin/env python3
"""Package a prebuilt native GUI with exact dependency notices; not a runtime dependency.

Usage: package-desktop.py BINARY --platform PLATFORM [--revision TEXT] [--cargo CARGO]

Takes the desktop app built by build-desktop.py and produces:
  build/desktop-<platform>/   the package folder: the program, README,
                              license, icon and a licenses/ folder with the
                              notice files of every Rust crate it contains
  iso/OhMy3DO-Library-<platform>-v<version>.zip (+ .sha256)
                              the portable download, holding that folder
and updates the desktop update manifest (write-update-manifest.py).

Packaging stops if any crate's license notice can't be found, since the
app can't be distributed without them.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]

# The Rust target each platform is built for.
TARGET_TRIPLES = {
    "Windows-x64": "x86_64-pc-windows-msvc",
    "Linux-x64": "x86_64-unknown-linux-gnu",
    "macOS-arm64": "aarch64-apple-darwin",
    "macOS-x64": "x86_64-apple-darwin",
}

# The project's own crates, which are covered by LICENSE.
OWN_CRATES = ("oh-my-3do-library", "oh-my-3do-desktop")

FONT_LICENSES = ("ark-pixel-OFL.txt", "atkinson-hyperlegible-OFL.txt")

# Words that mark a file in a crate as a license or notice.
NOTICE_WORDS = ("license", "licence", "copyright", "copying", "notice")

# Files never copied as notices even when their names match (source code,
# fonts and images that merely have "license" in their names).
NOT_NOTICE_SUFFIXES = (".rs", ".c", ".h", ".ttf", ".woff", ".woff2", ".png", ".jpg", ".svg")

# How deep inside a crate to look for notices.
MAX_NOTICE_DEPTH = 4

USER_AGENT = "OhMy3DO-Desktop-Packager"
CC0_TEXT_URL = "https://raw.githubusercontent.com/spdx/license-list-data/main/text/CC0-1.0.txt"
LICENSE_CACHE = ROOT / "build/license-cache"


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--platform", choices=list(TARGET_TRIPLES), required=True)
    parser.add_argument("--revision", default="working-tree")
    parser.add_argument("--cargo", default="cargo")
    return parser.parse_args()


def app_version():
    """The desktop app's version, from its Cargo.toml."""
    text = (ROOT / "desktop/shell/Cargo.toml").read_text()
    return re.search(r'^version\s*=\s*"([^"]+)"', text, re.M).group(1)


def copy_app_files(binary, folder, name):
    """Start a fresh package folder with the program and its own documents."""
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)

    shutil.copy2(binary, folder / name)
    shutil.copy2(ROOT / "LICENSE", folder / "LICENSE")
    shutil.copy2(ROOT / "desktop/README.md", folder / "README.md")
    shutil.copy2(ROOT / "desktop/THIRD_PARTY.md", folder / "THIRD_PARTY.md")
    shutil.copy2(ROOT / "assets/app-icon.png", folder / "OhMy3DOLibrary.png")

    fonts = folder / "licenses" / "fonts"
    fonts.mkdir(parents=True)
    for font_license in FONT_LICENSES:
        shutil.copy2(ROOT / "vendor/fonts" / font_license, fonts / font_license)


def cargo_metadata(cargo, platform):
    """Ask Cargo for every crate the app uses when built for `platform`."""
    command = [
        cargo,
        "metadata",
        "--manifest-path",
        str(ROOT / "desktop/shell/Cargo.toml"),
        "--format-version",
        "1",
        "--locked",
        "--filter-platform",
        TARGET_TRIPLES[platform],
    ]
    result = subprocess.run(command, check=True, capture_output=True, text=True, encoding="utf-8")
    return json.loads(result.stdout)


def is_notice_file(path, relative):
    """Whether a file in a crate looks like a license or notice to copy:
    its name says so, or it is a text file in a fonts folder."""
    suffix = path.suffix.lower()
    if suffix in NOT_NOTICE_SUFFIXES:
        return False
    label = path.name.lower()
    if any(word in label for word in NOTICE_WORDS):
        return True
    return "fonts" in relative.parts and suffix in (".txt", ".md")


def copy_crate_notices(source, target):
    """Copy the notice files found in a crate's source folder; return their
    paths relative to it."""
    texts = []
    if not source.is_dir():
        return texts
    for path in source.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(source)
        if len(relative.parts) > MAX_NOTICE_DEPTH:
            continue
        if is_notice_file(path, relative):
            destination = target / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
            texts.append(relative.as_posix())
    return texts


def copy_declared_license_file(package, source, target, texts):
    """Copy the license file the crate's Cargo.toml names, unless already
    copied; return the names added."""
    if not package.get("license_file"):
        return []
    path = source / package["license_file"]
    if not path.is_file() or path.name in texts:
        return []
    target.mkdir(parents=True, exist_ok=True)
    shutil.copy2(path, target / path.name)
    return [path.name]


def copy_supplied_notices(crate, target):
    """Some published crates omit their license files. Keep pinned upstream
    notices beside the app sources (desktop/licenses/<crate>-<version>/)
    rather than requiring a mutable network fallback."""
    supplied = ROOT / "desktop/licenses" / crate
    texts = []
    if supplied.is_dir():
        for path in sorted(supplied.iterdir()):
            if path.is_file():
                target.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target / path.name)
                texts.append(path.name)
    return texts


def fetch_json(url):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def fetch_upstream_license(package, source, target):
    """Some workspace crates omit inherited license files from the crate
    tarball. Retrieve the exact repository revision recorded by Cargo at
    publication, through GitHub's license API.

    Answers are cached in build/license-cache/ so later packaging works
    offline. Returns the names added (none if the lookup isn't possible).
    """
    vcs_info = source / ".cargo_vcs_info.json"
    repository = package.get("repository") or ""
    match = re.match(r"https://github.com/([^/]+)/([^/#]+)", repository)
    if not vcs_info.is_file() or not match:
        return []
    revision = json.loads(vcs_info.read_text(encoding="utf-8")).get("git", {}).get("sha1", "")
    if not re.fullmatch("[0-9a-f]{40}", revision):
        return []

    owner, name = match.groups()
    cache = LICENSE_CACHE / f"{owner}-{name}-{revision}.json"
    cache.parent.mkdir(parents=True, exist_ok=True)
    try:
        if cache.exists():
            license_info = json.loads(cache.read_text(encoding="utf-8"))
        else:
            url = f"https://api.github.com/repos/{owner}/{name}/license?ref={revision}"
            license_info = fetch_json(url)
            cache.write_text(json.dumps(license_info), encoding="utf-8")

        text = base64.b64decode(license_info["content"])
        target.mkdir(parents=True, exist_ok=True)
        filename = "upstream-" + Path(license_info["path"]).name
        (target / filename).write_bytes(text)
        return [filename]
    except (OSError, ValueError, KeyError) as error:
        print("License lookup:", package["name"], error)
        return []


def copy_cc0_text(target):
    """CC0 crates often ship no license text; use SPDX's (cached after the
    first download)."""
    cache = LICENSE_CACHE / "CC0-1.0.txt"
    cache.parent.mkdir(parents=True, exist_ok=True)
    if not cache.exists():
        request = urllib.request.Request(CC0_TEXT_URL, headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(request, timeout=30) as response:
            cache.write_bytes(response.read())
    target.mkdir(parents=True, exist_ok=True)
    shutil.copy2(cache, target / "CC0-1.0.txt")
    return ["CC0-1.0.txt"]


def collect_notices(package, folder):
    """Copy one crate's notices into licenses/<crate>-<version>/, trying each
    source in turn until one provides something. Returns the file names."""
    crate = package["name"] + "-" + package["version"]
    source = Path(package["manifest_path"]).parent
    target = folder / "licenses" / crate

    texts = copy_crate_notices(source, target)
    texts += copy_declared_license_file(package, source, target, texts)
    if not texts:
        texts = copy_supplied_notices(crate, target)
    if not texts:
        texts = fetch_upstream_license(package, source, target)
    if not texts and package.get("license") == "CC0-1.0":
        texts = copy_cc0_text(target)
    return texts


def gather_dependency_notices(metadata, folder):
    """Copy the notices of every crate in the build.

    Returns (a list describing each crate and its notice files, the crates
    with no notice found).
    """
    used = {node["id"] for node in metadata["resolve"]["nodes"]}
    notices = []
    missing = []
    for package in metadata["packages"]:
        if package["name"] in OWN_CRATES or package["id"] not in used:
            continue
        texts = collect_notices(package, folder)
        if not texts:
            missing.append(package["name"] + "-" + package["version"])
        notices.append(
            {
                "name": package["name"],
                "version": package["version"],
                "license": package.get("license"),
                "repository": package.get("repository"),
                "notices": sorted(set(texts)),
            }
        )
    return notices, missing


def minimum_glibc(binary):
    """The newest glibc symbol version a Linux program needs (the oldest
    glibc it runs on), or None when objdump isn't available."""
    if not shutil.which("objdump"):
        return None
    result = subprocess.run(
        ["objdump", "-T", str(binary)], check=True, capture_output=True, text=True
    )
    versions = re.findall(r"GLIBC_([0-9.]+)", result.stdout)
    if not versions:
        return None
    return max(versions, key=lambda value: tuple(map(int, value.split("."))))


def build_info(args, version, folder, name, missing):
    """The package's BUILD.json, which the installers and release check read."""
    info = {
        "app_version": version,
        "platform": args.platform,
        "revision": args.revision,
        "binary_sha256": hashlib.sha256((folder / name).read_bytes()).hexdigest(),
        "cargo_lock_sha256": hashlib.sha256(
            (ROOT / "desktop/shell/Cargo.lock").read_bytes()
        ).hexdigest(),
        "runtime": "Tauri 2 + Rust; system WebView; no Node/Python runtime; FFmpeg downloaded separately",
        "dependencies_without_separate_notice_files": missing,
    }
    if args.platform == "Linux-x64":
        glibc = minimum_glibc(args.binary)
        if glibc:
            info["minimum_glibc"] = glibc
    return info


def write_archive(folder, platform, version):
    """Zip the package folder as OhMy3DO-Library/ and write its .sha256."""
    archive = ROOT / "iso" / f"OhMy3DO-Library-{platform}-v{version}.zip"
    archive.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(
        archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9, strict_timestamps=False
    ) as package:
        for path in sorted(folder.rglob("*")):
            if path.is_file():
                package.write(path, "OhMy3DO-Library/" + path.relative_to(folder).as_posix())

    sha = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(".zip.sha256").write_text(
        sha + "  " + archive.name + "\n", encoding="utf-8"
    )
    return archive


def update_manifest(archive, platform, version):
    """Record the new zip in the desktop app's update manifest."""
    command = [
        sys.executable,
        str(ROOT / "tools/write-update-manifest.py"),
        "desktop",
        "--artifact",
        str(archive),
        "--platform",
        platform,
        "--version",
        version,
    ]
    subprocess.run(command, check=True)


def main():
    args = parse_arguments()
    assert args.binary.is_file()
    version = app_version()
    folder = ROOT / "build" / ("desktop-" + args.platform)
    name = "OhMy3DOLibrary" + (".exe" if args.platform.startswith("Windows") else "")

    copy_app_files(args.binary, folder, name)
    metadata = cargo_metadata(args.cargo, args.platform)
    notices, missing = gather_dependency_notices(metadata, folder)

    (folder / "licenses").mkdir(exist_ok=True)
    (folder / "licenses/dependencies.json").write_text(
        json.dumps(notices, indent=2), encoding="utf-8"
    )
    info = build_info(args, version, folder, name, missing)
    (folder / "BUILD.json").write_text(json.dumps(info, indent=2), encoding="utf-8")
    if missing:
        raise SystemExit("Dependency notices are incomplete: " + ", ".join(missing))

    archive = write_archive(folder, args.platform, version)
    update_manifest(archive, args.platform, version)
    print("Packaged:", archive)
    print(
        "Dependency notices:",
        len(notices),
        "packages; without separate notice files:",
        ", ".join(missing),
    )


if __name__ == "__main__":
    main()
