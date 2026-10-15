# Directory-local folder ID fix

The first console test reported that an ISO launched successfully, but the first
folder sometimes would not open until another folder was entered and exited.
This matches a reproducible bug in the linked FCare firmware's `setTocLevel()`.
The ODE reports LOCODE 1.1.0, matching hard-coded reference-source values;
this does not identify the exact installed binary.

A subsequent user hardware test confirmed that the external-config menu
workaround works on the installed ODE. Its precise firmware build is still
unknown; the firmware patch itself has not been flashed or tested there.

The source regression now also exercises USB artwork's root/folder round trip,
including repeated IDs and rejection of a changed restored listing. It sends no
playlist clear/add/launch operations. USB cover loading still needs a console test.

## Cause and patch

The firmware remembers the last entered folder in `current_toc`. It uses equality
with the next requested TOC ID as a reason to skip navigation. However, the lower
24 bits of an ID are an ordinal in the **current** directory, not a global folder
identifier. Folder 1 can contain another folder 1. The second request is a real
navigation operation even when its numeric ID equals the previous request.

The command handler replies with a normal status even when this equality check
skips the operation. The menu then reloads the unchanged directory. Returning from
a sibling clears the low ID bits, which explains why that sequence can unblock it.

`0001-relative-folder-ids.patch` changes the condition from:

```c
if (current_toc != index) {
```

to:

```c
if (current_toc != index || getTocEntry(index) != 0) {
```

Nonzero IDs always select an entry in the current directory. The original guard
remains for zero-entry device/root selections, and the separate parent-directory
branch is unchanged. The menu's sorting, stored IDs, launch commands, and disc
queue need no protocol changes for this fix.

## Validation

With the unmodified FCare checkout in `third_party/SataTo3DO`:

```sh
make test-firmware
```

This test extracts the **actual upstream `setTocLevel()` function**, compiles it
with host filesystem/device stubs, and runs it before and after applying the
patch in a temporary directory. It does not replace the navigation function with
a separately written simulation. The checkout itself is not modified by the test.

Verified against FCare commit `0c28fa484ce4b6537d32737f0e49566047e5a173`:

| Scenario | Original function | Patched function |
| --- | --- | --- |
| First folder blocked by a stale matching ID | Fails | Passes |
| Child folder reusing its parent's entry ID | Fails | Passes |
| Enter sibling, go back, enter first folder | Passes | Passes |
| Go back and re-enter the same folder | Passes | Passes |
| Repeated zero-entry device selection | Passes | Passes |
| Single-device root initialization | Passes | Passes |

This establishes the source-level defect and patch behavior. It does not prove
the installed firmware binary is identical, or replace RP2040 compilation and
hardware testing.

## Applying and building

From the project root, after running the regression test:

```sh
git -C third_party/SataTo3DO apply --check ../../firmware/0001-relative-folder-ids.patch
git -C third_party/SataTo3DO apply ../../firmware/0001-relative-folder-ids.patch
```

Building a matching UF2 also requires the board's existing configuration:
flash size/PICO_BOARD and the GPIO-related options such as `USE_DIR_IN_HIGH` and
`USE_HS_FC`. Use the settings appropriate to the installed hardware. The upstream
default is a Waveshare RP2040-Plus with 4 MB flash; this is not a confirmed match
for the user's ODE. No UF2 is supplied or flashed as part of this source fix.

The corrected firmware needs to be flashed to the RP2040. Replacing `boot.iso`
alone does not install this patch. The menu now also offers an optional
[USB-configured workaround](../README.md#optional-folder-workaround) without
flashing. It uses a file selection to update the remembered ID without entering
a folder, or a safe sibling-and-parent detour. These behaviors are verified
against the actual source function by the same regression test. They depend on
an alternate entry and leave the underlying firmware defect unresolved.
