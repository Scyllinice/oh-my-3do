# Contributing

Build/run instructions and hardware limits are in README.md. Please include the
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

Do not blindly retry timed-out XBUS operations or claim a USB/NVRAM write worked
from an acknowledgement alone. Preserve the recovery-backup ordering and readback
checks. Document hardware tests separately from host tests and compilation.
