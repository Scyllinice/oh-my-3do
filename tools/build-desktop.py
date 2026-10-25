#!/usr/bin/env python3
"""Build the Tauri desktop app and its shared Rust engine.

The finished app is a single native program: no Node or Python runtime is
shipped with it.

How it builds depends on where it runs:

  * Windows on Windows, or Linux with the Tauri prerequisites installed:
    the local Rust toolchain builds it directly.
  * Linux without those prerequisites: a Docker image provides them.
  * --windows on Linux: a Docker image cross-compiles the Windows app with
    cargo-xwin (which downloads Microsoft's SDK headers and libraries).

With --test, the engine's and the shell's test suites run first.
"""
import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The Rust target name for 64-bit Windows built with the Microsoft toolchain.
WINDOWS_TARGET = "x86_64-pc-windows-msvc"

# The engine (a library plus its tests) and the Tauri shell that packages it
# into the app people run.
ENGINE_MANIFEST = ROOT / "desktop/Cargo.toml"
SHELL_MANIFEST = ROOT / "desktop/shell/Cargo.toml"


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--target-dir", type=Path, help="where Cargo keeps its build output")
    parser.add_argument("--cargo", help="the cargo program to use for a local build")
    parser.add_argument("--test", action="store_true", help="run the test suites before building")
    parser.add_argument(
        "--windows", action="store_true", help="cross-compile Windows x64 with cargo-xwin in Docker"
    )
    return parser.parse_args()


def app_file_name(windows):
    """The name of the built program inside Cargo's release folder."""
    return "oh-my-3do-library.exe" if windows else "oh-my-3do-library"


def path_flags(source_root, cargo_home):
    """Compiler flags that hide this machine's folder names in the program.

    Rust embeds source paths in panic messages; replacing the build machine's
    folders with fixed names keeps personal paths out of released builds and
    makes builds from different machines match.
    """
    return [
        f"--remap-path-prefix={source_root}=oh-my-3do",
        f"--remap-path-prefix={cargo_home}=cargo",
    ]


def static_runtime_flags():
    """Windows builds include the C runtime, so the app runs without installing it."""
    return ["-C", "target-feature=+crt-static"]


def build_in_docker(args, windows):
    """Build (and optionally test) inside a Docker image with every prerequisite."""
    if not shutil.which("docker"):
        raise SystemExit(
            "Install Docker, or install Tauri build prerequisites and a Rust toolchain locally"
        )

    image = "oh3do-desktop-windows" if windows else "oh3do-desktop-tauri"
    dockerfile = "desktop-windows.Dockerfile" if windows else "desktop-tauri.Dockerfile"
    subprocess.run(
        ["docker", "build", "-t", image, "-f", str(ROOT / "tools" / dockerfile), str(ROOT)],
        check=True,
    )

    default_target = "build/windows-target" if windows else "build/tauri-linux-target"
    target = (args.target_dir or ROOT / default_target).resolve()
    # Downloaded crates and Windows SDK files are kept between builds here.
    cache = ROOT / "build/xwin-cache"
    target.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)

    flags = path_flags("/src", "/cache/cargo")
    if windows:
        flags = static_runtime_flags() + flags

    # Inside the container the repository is /src, build output /target and
    # the cache /cache. Files are created as the current user, not root.
    # fmt: off
    cargo_in_docker = [
        "docker", "run", "--rm",
        "--user", f"{os.getuid()}:{os.getgid()}",
        "-v", f"{ROOT}:/src",
        "-v", f"{target}:/target",
        "-v", f"{cache}:/cache",
        "-w", "/src",
        "-e", "HOME=/cache",
        "-e", "CARGO_HOME=/cache/cargo",
        "-e", "CARGO_BUILD_JOBS=3",
        "-e", "XWIN_CACHE_DIR=/cache/xwin",
        "-e", "XWIN_ACCEPT_LICENSE=1",
        # Cargo reads several flags from one variable, separated by 0x1F.
        "-e", "CARGO_ENCODED_RUSTFLAGS=" + "\x1f".join(flags),
        image,
        "cargo",
    ]
    # fmt: on
    if windows:
        cargo_in_docker.append("xwin")

    common = ["--locked", "--target-dir", "/target"]
    if windows:
        common += ["--target", WINDOWS_TARGET]

    if args.test:
        for manifest in (ENGINE_MANIFEST, SHELL_MANIFEST):
            in_container = "/src/" + manifest.relative_to(ROOT).as_posix()
            test_command = cargo_in_docker + ["test", "--manifest-path", in_container] + common
            if windows:
                run_windows_tests(test_command, target)
            else:
                subprocess.run(test_command, check=True)

    shell_in_container = "/src/" + SHELL_MANIFEST.relative_to(ROOT).as_posix()
    subprocess.run(
        cargo_in_docker + ["build", "--release", "--manifest-path", shell_in_container] + common,
        check=True,
    )
    platform_folder = WINDOWS_TARGET if windows else ""
    print("Desktop app:", target / platform_folder / "release" / app_file_name(windows))


def run_windows_tests(test_command, target):
    """Build the Windows test programs, then run them if this Linux can.

    Linux cannot run Windows programs itself, but WSL can hand them to
    Windows. Elsewhere the programs are only listed, to run on a Windows PC.
    """
    # "--no-run" builds the tests without running them; the JSON messages
    # say where each test program was written.
    output = subprocess.run(
        test_command + ["--no-run", "--message-format=json-render-diagnostics"],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    ).stdout
    programs = []
    for message in map(json.loads, output.splitlines()):
        is_test_program = (
            message.get("reason") == "compiler-artifact"
            and message.get("executable")
            and message["profile"]["test"]
        )
        if is_test_program:
            # The path is inside the container; map it back to this machine.
            programs.append(target / Path(message["executable"]).relative_to("/target"))

    running_in_wsl = Path("/proc/sys/fs/binfmt_misc/WSLInterop").exists()
    if running_in_wsl:
        for program in programs:
            subprocess.run([str(program)], check=True)
    else:
        print("Run Windows test executables on Windows:", *programs, sep="\n  ")


def missing_tauri_prerequisites():
    """Whether this Linux lacks the WebKit and GTK libraries Tauri needs."""
    check = subprocess.run(["pkg-config", "--exists", "webkit2gtk-4.1", "gtk+-3.0"])
    return check.returncode != 0


def build_locally(args):
    """Build (and optionally test) with the Rust toolchain on this machine."""
    on_windows = os.name == "nt"
    if on_windows:
        default_cargo = str(Path.home() / ".cargo/bin/cargo.exe")
    else:
        default_cargo = shutil.which("cargo")
    cargo = args.cargo or default_cargo
    if not cargo:
        raise SystemExit("Install the stable Rust toolchain first")

    # Windows keeps build output outside the repository, where paths are
    # shorter and antivirus scanning is less likely to slow the build.
    if on_windows:
        default_target = Path(tempfile.gettempdir()) / "oh3do-native-build"
    else:
        default_target = ROOT / "build/native-target"
    target = (args.target_dir or default_target).resolve()

    flags = path_flags(ROOT, Path.home() / ".cargo")
    if on_windows:
        flags = static_runtime_flags() + flags
    environment = os.environ.copy()
    environment["CARGO_ENCODED_RUSTFLAGS"] = "\x1f".join(flags)

    if args.test:
        for manifest in (ENGINE_MANIFEST, SHELL_MANIFEST):
            subprocess.run(
                [cargo, "test", "--locked", "--manifest-path", str(manifest)]
                + ["--target-dir", str(target)],
                cwd=ROOT,
                env=environment,
                check=True,
            )

    subprocess.run(
        [cargo, "build", "--release", "--locked", "--manifest-path", str(SHELL_MANIFEST)]
        + ["--target-dir", str(target)],
        cwd=ROOT,
        env=environment,
        check=True,
    )
    print("Desktop app:", target / "release" / app_file_name(on_windows))


def main():
    args = parse_arguments()

    # The engine embeds the save cheat database; stop early if the generated
    # file no longer matches its source.
    subprocess.run(
        [os.environ.get("PYTHON", "python3"), str(ROOT / "tools/build-save-cheats.py"), "--check"],
        check=True,
    )

    if args.windows and os.name != "nt":
        build_in_docker(args, windows=True)
    elif os.name == "posix" and missing_tauri_prerequisites():
        build_in_docker(args, windows=False)
    else:
        build_locally(args)


if __name__ == "__main__":
    main()
