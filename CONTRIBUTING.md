# Contributing

Build/test instructions are in [the build guide](docs/building.md), menu usage
is in [the menu guide](docs/menu-guide.md), and hardware limits are in
[Diagnostics](docs/troubleshooting.md). Please include the
menu version, console model, reported ODE identity/revision, compatibility setting,
and a small directory/file-name example when reporting a problem. Report whether
an operation failed before launch, during a disc swap, or during USB/NVRAM I/O.
Never upload game ISOs, personal saves, or credentials with an issue.

Run `make test` for changes to portable logic. Use `./tools/fetch-references`,
`make test-firmware` and `python3 tools/preview-menu.py` for navigation/UI work.
Use the Docker toolchain for `make test-sdk`, both ISO builds and signature
checks. Host previews are layout simulations, not console screenshots.

Keep console C code C89-compatible. Use quoted SDK headers for 3DO compilation;
Norcroft's built-in angle-bracket stdio/stdarg headers have an incompatible
argument-list ABI. Keep TOC IDs attached to their original names after sorting.

Some controller tests extract functions from `src/main.c`. Update their stubs
when adding fields or dependencies. `tests/test_stack_usage.py` checks stack
budgets; keep large paths and save archives off the 32 KiB task stack.
See `docs/emulator-and-bench.md` for the emulator and ARM60 timing harnesses.

Menu and desktop releases have independent versions and tags:

- **Menu:** `menu-vX.Y.Z`, containing the boot/demo ISOs, default settings and
  portable media tools. Update `MENU_VERSION` in `src/main.c`, `SCANNER_VERSION`
  in `tools/organize-library.py`, and `assets/media-tools-README.txt` together.
- **Desktop:** `desktop-vX.Y.Z`, containing installers and portable packages.
  Update the package versions in both desktop Cargo manifests and lockfiles,
  plus `desktop/shell/tauri.conf.json`. The shared Rust engine follows the
  desktop version; it does not determine which menu version is downloaded.

The combined **v1.0.0 is final**. Do not replace its tag or assets. Each new
stream starts above 1.0.0 and can advance without changing the other stream.
See [the release guide](docs/releasing.md) for version checks, staging and
publication. Desktop packages include exact dependency notices and source
revision in `BUILD.json`.

Do not blindly retry timed-out XBUS operations or claim a USB/NVRAM write worked
from an acknowledgement alone. Preserve the recovery-backup ordering and readback
checks. Document hardware tests separately from host tests and compilation.
