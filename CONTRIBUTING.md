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

The menu, desktop app and media tools share one release version. Update
`MENU_VERSION` in `src/main.c`, `SCANNER_VERSION` in `tools/organize-library.py`,
the app version in `desktop/Cargo.toml` and `desktop/Cargo.lock`, and the release
links and version labels in the documentation together. Build and validate all
packages before publishing a release. Desktop packaging includes the exact
dependency notices and source revision in `BUILD.json`.

Do not blindly retry timed-out XBUS operations or claim a USB/NVRAM write worked
from an acknowledgement alone. Preserve the recovery-backup ordering and readback
checks. Document hardware tests separately from host tests and compilation.
