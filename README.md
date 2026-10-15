# Oh My 3DO

![Oh My 3DO logo](assets/logo-menu.png)

Oh My 3DO is a replacement `boot.iso` menu for the open-source [FCare/SataTo3DO
ODE](https://github.com/FCare/SataTo3DO). It sorts folders and games by name
instead of FAT insertion order, with folders first and case-insensitive natural
sorting: `Disc 2` comes before `Disc 10`. No firmware update is required.

Features include disc queues, automatic multi-disc grouping, favorites, letter
navigation, saved settings, PNG covers, video previews, and NVRAM backup and
restore. The current version is **0.8.0**.

![Browser layout preview](docs/images/browser.png)

Layout preview rendered from the menu drawing code.

## Install

Download `boot.iso` from the [latest
release](https://github.com/Scyllinice/oh-my-3do/releases/latest). Back up the
existing `boot.iso` on the USB drive, replace it with the download, and cold
boot the console. Keep games in their existing folders.

Keep an existing `ohmy3do.ini` to preserve settings. For a fresh setup, copy the
release's INI beside `boot.iso`. Favorites, covers, previews, and save backups
are optional; their setup is described below.

The release also includes `menu-demo.iso`, an offline interface demo. Copy it to
the USB drive as `boot.iso` to preview the layout without sending ODE commands.
It does not launch games. Stock emulators may run the demo but lack the vendor
commands needed by the normal browser.

## Build

Docker must be running. The first build downloads the SDK and tools. The default
SDK revision is pinned to `4e370299bd273bb13089d41fa33c5e863dc6d405`.

```sh
./tools/dev-container build
./tools/dev-container test
./tools/dev-container run make test-sdk test-covers
./tools/dev-container menu
./tools/dev-container demo
```

Outputs in `iso/`:

| Output | Purpose |
| --- | --- |
| `boot.iso` | Browser and launcher |
| `menu-demo.iso` | Offline UI demo; does not launch games |
| `*.iso.sha256` | Image checksums |
| `ohmy3do.ini` | Editable startup configuration |
| `favorites.txt` | Editable favorites template |
| `covers/` | Raw PNGs or converted thumbnails and checked index |
| `previews/` | Optional converted silent clips and checked index |
| `saves/` | Copy this folder to USB for save backups |

Builds preserve existing `iso/ohmy3do.ini` and `iso/favorites.txt`. Source
examples are `ohmy3do.ini` and `favorites.example.txt`. Build audits and signed
LaunchMe executables are retained in `build/console-menu/` and
`build/console-demo/`. The ISO includes the complete pinned SDK bootstrap
runtime and its startup scripts. Each build checks every template file before
adding the menu and artwork, and records component hashes in
`build/console-menu/runtime-audit.json`.

The container targets `linux/amd64` because the legacy compilers are x86
binaries. It includes Norcroft, Python, Wine, ImageMagick and `3it`, and mounts
the workspace with your host UID/GID. Docker's socket and physical devices are
not mounted.

## Controls

| Control | Browser |
| --- | --- |
| Up / Down | Move selection; hold to repeat |
| Left / Right or L / R | Page through the list |
| A | Open a folder or launch a game; offer a matching disc set for Disc 1 |
| B | Parent folder |
| C | Add selected image to the queue when released |
| Hold C + Up | Focus the alphabet strip from anywhere in the list |
| Hold C + Left / Right | Jump to the previous/next initial letter without adding a disc |
| Start | Review the disc queue |
| X | Clear an active find filter; otherwise refresh / retry connection |
| L + R together | Tools: saves, backups, favorites, recent games, find, configuration, help, diagnostics |
| Hold X at startup | Skip USB configuration, artwork, favorites and settings writes |

The browser uses a proportional, mixed-case font, a dedicated artwork panel,
rounded selection cards and controller button badges. The Tools screen has two
columns of four cards; the arrows move between them. Cover images and video
previews share the larger artwork panel.

The browser hides `.iso` and `.cue` extensions for display only. Full filenames
and original TOC IDs are retained. Long selected names scroll after a pause;
folder changes reset the animation. Parent navigation restores selection. The
previous listing remains visible while a folder loads. A read failure hides the
listing and prevents selection until the connection recovers. The whole
directory is sorted before paging. Normal text shows seven rows; large filenames
show six.

At a USB root, support entries are hidden: `boot.iso`, `menu-demo.iso`, their
`.sha256` files, `ohmy3do.ini`, `favorites.txt`, `last-position.txt`,
`recent.txt`, `diagnostics.txt`, and the `covers`/`previews`/`saves` folders.
Identically named entries inside game folders remain visible. Volume selectors
are never hidden. The compatibility workaround retains access to hidden entries
when needed.

## Disc queue and automatic grouping

The queue is intended for discs from **the same multi-disc game**, added in
their required order. Changing the mounted image leaves the first game's program
running; it is not a reliable way to start an unrelated game. Advance when the
game requests the next disc. The menu stops running after launching a game, so
it cannot show a live mounted-disc overlay or detect a game's swap prompt.

Press C on images to add up to 16 discs, then Start to review them:

| Control | Queue review |
| --- | --- |
| A / Start | Apply the displayed order and launch |
| X | Remove the selected disc |
| C | Toggle move mode; Up/Down moves that disc, C or B finishes |
| Hold C, press X | Clear the complete queue |
| B | Return to browsing |

Queue edits happen in menu memory. Before launch, the menu validates every path,
clears the firmware queue, and re-adds discs in the displayed order. It resolves
fresh directory IDs and restores/verifies the browser folder between visits.
This supports discs in different folders and volumes. A missing path prevents
launch; an uncertain append/clear blocks further launch until a confirmed clear.
An XBUS timeout or failed directory restoration requires a console restart. The
queue is not saved across power cycles.

Selecting a recognized Disc 1 with an empty queue offers a matching set before
launch. It recognizes explicit `Disc`, `Disk` and `CD` markers with numbers,
including names such as `D (Disc 1).iso` and `Game CD1.cue`. All entries must be
in the **same folder**, have matching surrounding names, contain unique disc
numbers, and run continuously from 1 through N. Region/revision differences,
duplicate disc numbers, gaps, ambiguous markers and oversized sets suppress the
offer. Files in separate Disc 1/Disc 2 subfolders must be queued manually.

On the proposed-set screen: **A** accepts into queue review, **X** launches only
the originally selected image, and **B** cancels. Acceptance does not launch the
game automatically. The menu cannot establish that matching filenames contain
compatible game data; review the suggestion.

## Favorites and letter navigation

Put `favorites.txt` beside `boot.iso`, with one path relative to that USB volume
per line. Use forward slashes and include the actual extension. For example:

```text
# Favorites
Games/Road Rash.iso
Games/D/D (Disc 1).iso
```

Open Tools → Favorites. **A** opens the containing folder and selects the image;
press A again to launch or review a detected disc set. **C** adds the game
currently selected in the browser; **X** removes the highlighted favorite. Both
edits save and verify `favorites.txt` immediately, creating it if missing.
**Start** reloads the list. Select a game before opening Tools to add it.

**R** enters move mode. Up/Down moves the highlighted favorite; **A or R** saves
the new order and verifies it. **B** cancels without writing. Before saving, the
menu re-reads the USB list and checks that it still matches the list opened for
editing. If another edit is detected, cancel and reload instead of overwriting
it.

Missing paths leave the original folder intact when restoration succeeds. A
transport/restoration failure disables browsing until restart. Files are read on
demand, not automatically at startup. Each USB volume can have its own
favorites. Do not include virtual USB device labels in favorites paths.

The list supports up to 64 entries in fewer than 8,192 bytes, with each path
under 1,024 bytes. Blank lines, `#`/`;` comment lines, CRLF/LF and a UTF-8 BOM
are accepted. Traversal components, absolute paths, backslashes, embedded
controls, duplicate paths ignoring ASCII case, and oversized lists are rejected
as a whole. Edits re-read the file first, reject invalid/unreadable lists and
case-insensitive duplicates, and write a canonical path list, replacing
comments/formatting. A NUL terminator hides old file tails because firmware
cannot truncate files. No favorites are saved without pressing an edit button.
The firmware may create an empty missing text file even for a read request.

The alphabet strip below the browser lists **# and A–Z**. Focus it with **C +
Up**, Up from the first entry, or Down from the last entry. Left/Right chooses a
letter, A jumps to its first matching visible name, and B cancels. Down returns
to the first list entry. Letters without matches are dimmed; confirming one
leaves the strip open. `#` covers numeric and other initials. The strip follows
the current name filter and never launches or queues a game.

Hold C and use Left/Right for quick jumps between available initials. A plain C
tap adds a disc only on release, so either alphabet shortcut avoids queueing.

## Remembered position, recent games and finding names

The menu records the last folder and selected entry in `last-position.txt` on
each USB volume. After two seconds without a selection change, the menu saves a
changed position and verifies it. It also saves before launch. On startup it
resolves the folder by name using fresh directory IDs and selects the saved
entry. A missing or renamed folder leaves the startup listing available. A
malformed position file disables automatic position writes for the session;
delete that file on a PC to start fresh. A failed write also stops automatic
position saves for the session. Position data supports the browser's depth and
path limits.

Tools → Recent games lists up to ten recent launch selections, newest first.
**A** opens the containing folder and selects the image; **Start** reloads the
list. Paths are recorded in `recent.txt` just before handing control to a game,
so a failed mount can still appear in the list. Repeating a selection moves it
to the top without duplicates. Long paths may reduce the number of entries;
paths of 1,024 bytes or more are not recorded. Missing paths do not launch a
game, and malformed lists are not overwritten automatically.

Tools → Find a game opens a controller keyboard. Use the arrows to choose a
character, **A** to enter it, **X** to erase, **C** to clear, and **Start** to
apply. `SP` enters a space. The filter matches names in the current folder,
ignoring ASCII case. It does not recursively search the drive. **X in the
browser** clears an active filter; changing folders clears it too. **B** cancels
keyboard edits.

Both position recording and recent-game recording can be disabled in Appearance
& settings. Existing INIs use enabled defaults for these new options. Holding X
at startup skips these USB extras. Each USB volume has its own files; startup
restoration uses the boot volume's position file. Recent paths retain volume
labels where needed. A regular history-file failure does not prevent launch,
while an uncertain transport or failed folder restoration blocks it.

## Display preferences and configuration

Tools → Appearance & settings exposes every INI setting: additional screen
margins (0–12 pixels), larger game/queue titles, teal/high-contrast/amber
themes, the folder compatibility workaround, USB covers, video previews,
remembered position, and recent games. Left/Right or A changes the selected
value. Display values preview immediately. Save the remaining options and cold
boot to apply them.

**C** or **Save changes to USB** writes all eight settings, closes/reopens the
file and verifies every byte. B returns without writing; unsaved display
previews remain active for this boot, and edited values remain on the settings
screen. Only a successful save persists after reboot. The unsaved marker
identifies changes that have not been saved. **X** or **Discard unsaved
changes** restores the last successfully saved values, including the display
preview.

The margin scales the menu into an inset area; larger titles use a 12-pixel font
and six list rows. Controls, help and system tools retain their compact font.
Restore display defaults resets only the display settings.

The save writes a canonical INI containing the selected compatibility, artwork
and display values and replaces comments/formatting. It requires a successfully
loaded config. Startup bypass, an invalid config, unsupported identity or a
failed transport prevents saving. The firmware cannot truncate files, so the
menu writes a fixed 512-byte, NUL-padded file. The config loader stops at that
terminator and ignores any older tail beyond it. Do not interpret a failed save
as persistent.

You can also edit `ohmy3do.ini` on a PC:

```ini
[compatibility]
folder_workaround=1

[artwork] usb_covers=1 video_previews=0

[display] screen_margin=0 large_titles=0 theme=0

[library]
remember_position=1
recent_games=1
```

`folder_workaround=0` is normal navigation; `1` enables the workaround for
FCare's directory-local ID collision bug. Keep the existing value if folder
navigation already works. `Compat on` appears at the bottom of the browser when
the workaround is enabled. `usb_covers` and `video_previews` are 0/1; previews
default off. Margin is 0–12; large titles is 0/1; theme is 0 teal, 1 high
contrast, 2 amber. The two library settings are 0/1. Old configs containing only
the earlier keys remain valid and use display defaults. Unknown/duplicate
settings or invalid values reject the config. PC text must stay below 512 bytes;
CRLF, LF, UTF-8 BOM and `#`/`;` comments are accepted.

The workaround first selects a safe existing file with the directory command to
change the remembered ID without mounting it or changing the playlist. It can
instead use a safe sibling detour. Intermediate folders are not displayed. With
no safe alternative it reports an error; the separate firmware patch is the
complete fix. See [firmware/README.md](firmware/README.md).

## Save manager and backup-slot details

Tools → Save manager lists files in the console's `/NVRAM` filesystem, their
sizes, and used/free space. Viewing and X refresh are read-only. **A** prepares
a backup of one save; **C** prepares all files, including system settings.

Choose one of eight USB slots. The menu reads and lists the **existing slot
contents before replacement**. C switches between existing and proposed
contents; A proceeds to a separate confirmation, and B returns. Missing/corrupt
slots are identified as empty or invalid; their contents are not presented as
valid saves. Confirming replaces the chosen slot and verifies the completed
archive by closing/reopening and comparing every byte.

Copy `iso/saves/` to the USB root first: the firmware cannot create directories.
Before the first backup or restore in a session, an automatic check writes two
patterns to `saves/usb-test.bin` and verifies both. It does not change NVRAM.
The test file can be deleted on a PC; there is no standalone Tools test option.

Tools → USB backups validates an archive and lists its contents. Select one save
to restore, review its filename/size/type and replacement warning, then confirm.
**Before any NVRAM write**, the menu checks space, captures all current saves,
writes them to `saves/recovery.3ds` and verifies that USB backup. Restoring from
that archive uses `recovery2.3ds` to preserve its source. Both recovery files
are listed alongside `slot01.3ds`–`slot08.3ds`.

Restore allocates space before overwriting, retains the exact length and file
type, reads the file back, and takes a fresh snapshot to check that unrelated
saves are unchanged. It never deletes other saves to make room. The confirmation
warns that the recovery archive is replaced. Keep power on until completion:
replacement is **not atomic**, and interruption can damage the selected save.
Keep a verified USB backup on your PC before restoring saves.

Archives preserve filenames, four-byte types, exact sizes and uncompressed data,
with per-file and whole-payload CRC32 checks. Limits are 128 files and 64 KiB
per archive. They are custom file archives, not raw NVRAM dumps, emulator `.srm`
images, Nintendo 3DS files or game-progress editors. Invalid names, checksums,
duplicates and lengths reject the whole archive. Files are self-delimiting so an
old USB tail does not require unsupported truncate/seek commands.

## PNG covers and video previews

Game media belongs in `/covers` and `/previews` on USB, outside `boot.iso`. PNG
covers can be copied directly: no image conversion or ISO rebuild is needed. The
menu decodes them into the cover area and preserves their aspect ratio.
Transparency is blended against the artwork background. PNGs can be up to
1024×1024 pixels and 512 KiB; smaller files, around 128–256 pixels tall, reduce
loading time. RGB, RGBA, grayscale, indexed, 1/2/4/8/16-bit and interlaced PNGs
are supported where valid for their color type. JPEGs still need conversion.

The filename matches the game's basename, ignoring ASCII case. For example:

```text
USB/
  boot.iso
  ohmy3do.ini
  Games/
    Road Rash.iso
  covers/
    Road Rash.png
    index.3di
  previews/
    Road Rash.3dv
    index.3di
```

After copying PNGs, generate the media indices on a PC. Python 3 is enough;
Docker and FFmpeg are not needed for PNG covers. The release includes a portable
`media-tools.zip` with both Python tools, a quick-start guide and an original
moving test-pattern clip. Rename that clip to match a game's basename for an
initial preview test without installing FFmpeg. From a source checkout, run:

```sh
python3 tools/index-media.py /path/to/USB
```

On Windows, run `py tools/index-media.py E:/`, substituting the USB drive
letter. With the portable ZIP, run `py index-media.py E:/` from its extracted
folder. The indexer scans game paths and writes small `index.3di` files, leaving
the media files unchanged. The firmware does not list PNGs or preview files in
its browser, so these indices are still needed. Existing valid associations are
preserved when their game and media files still exist, including older `.3dc`
covers; a matching PNG basename takes precedence. Games in different folders
with the same basename share the matching artwork. Each index supports up to
2,048 associations / 128 KiB. Media basenames must fit 96 UTF-8 bytes. No
artwork or footage is downloaded automatically.

Video previews are small **silent 64×80 RGB555 loops**, fitted into the artwork
area. They are converted on the PC; the console does not play MP4/MOV files. The
default clip is 24 frames at 6 fps (four seconds, 240 KiB). The limits are 32
frames and 1–12 fps. Fewer frames reduce loading time and memory use.

```sh
./tools/dev-container build
./tools/dev-container preview artwork/road-rash.mp4 'iso/previews/Road Rash.3dv'
# Optional shorter clip from a chosen point:
./tools/dev-container preview artwork/road-rash.mp4 'iso/previews/Road Rash.3dv' --start 10 --frames 12 --fps 6
```

Copy the generated `.3dv` into `/previews` on USB, then run the indexer against
that drive. Alternatively, with FFmpeg installed locally:

```sh
python3 tools/prepare-preview.py input.mp4 '/path/to/USB/previews/Road Rash.3dv'
python3 tools/index-media.py /path/to/USB
```

Enable **Video previews** in Tools → Appearance & settings, save, and cold boot
(or add `video_previews=1` under `[artwork]` in the USB INI). Old INIs keep
previews disabled. `usb_covers` controls static covers independently.

Covers start loading after 350 ms without a selection change; previews wait 1.2
seconds. A whole clip is checked and loaded into RAM before playback, so looping
needs no USB reads and uses at most 320 KiB for frame data. Pressing a button
cancels an in-progress media load or stops playback. Selecting another game
releases the clip; a stopped clip waits for a new selection before retrying.
Missing, corrupt or cancelled previews leave the static cover available. Actual
USB loading time depends on the firmware and folder navigation. The diagnostics
export includes `preview_result` and `preview_load_ms` for the last attempt. The
binary layouts are documented in [docs/media-format.md](docs/media-format.md).

For faster cover reads, convert artwork to the compact `.3dc` format:

```sh
./tools/dev-container cover 'Games/Road Rash.iso' artwork/road-rash.jpg
```

That exports a 64×80 RGB555 `.3dc` plus its index into `iso/covers`; see
[assets/covers/README.md](assets/covers/README.md). Copy those files to USB.
`tools/dev-container media-index USBROOT` also runs the indexer for a directory
inside the mounted project. Rebuilding the ISO exports the project's legacy
cover associations into `iso/covers`. Generate raw-media indices against the
actual USB drive after copying the media files.

Media access temporarily visits the selected USB root and restores the original
folder using fresh listings and the compatibility workaround. A failed restore
blocks browsing. Cold boot or refresh at the USB root reloads both indices.
Holding X at startup bypasses USB media access. The project logo/loading screen
and offline demo covers stay embedded in the ISO.

## Diagnostics and compatibility limits

L+R → Diagnostics has three pages (Left/Right): reported identity/revision and
raw `93` reply; active settings and session results; XBUS replies, command
count, last fault, timeout and navigation state. Viewing diagnostics sends no
commands and remains available after a transport failure. USB write-check
results appear only after the automatic check has run.

**C** exports a verified `diagnostics.txt` to the USB root. The report includes
the raw identity reply, command/reply and retained fault, active settings, queue
state, save results, and position/history results. It snapshots these values
before export I/O changes the bus counters. Export is unavailable after a
transport timeout or failed directory restoration, with startup bypass, or at
the virtual volume selector; viewing the screens still works. Result codes are
`1` for success, `0` for not attempted, and `-1` for failure. Export replaces
the previous report. It is padded to a fixed 4 KiB, so blank lines at the end
are expected.

`LOCODE 1.1.0` matches the reference source but is hard-coded there, along with
boot flags. It does not identify the exact firmware binary, board or flash size.
USB file operations are gated to `LOCODE`; that gate is not proof that every
binary using the name behaves identically. Hold X at boot for recovery without
these optional file operations. No firmware dumping or flashing is performed.

The reference firmware opens files with read/write access and creates missing
files even on reads. Reads can misreport EOF and writes can misreport short
writes. Text buffers are fully cleared before reading; binary inputs are bounded
and checksummed. Verified writes poison every requested readback byte with the
complement of its expected value before comparing actual data. No unsupported
create/seek/truncate/rename commands are used. Read-only USB storage can reject
these operations. All XBUS requests have a three-second timeout; a partial FIFO
exchange requires console restart rather than blind retries.

Other limits: 2,048 entries per directory, 32 navigation levels, ASCII natural
sorting/case folding, and `?` display for unsupported characters. Queue paths
are bounded. No in-game overlay, automatic return-to-menu, sound, NVRAM
formatting, defragmentation, save deletion or Unicode collation is provided.
Image formats visible to the browser depend on the installed firmware.

## Testing and repository setup

Portable host tests:

```sh
make test
```

Optional firmware-function tests and actual-draw layout previews need pinned
reference sources, downloaded separately into ignored `third_party/`:

```sh
./tools/fetch-references
make test-firmware
python3 tools/preview-menu.py
```

The firmware reference test reproduces the original folder-ID bug and checks
navigation against both the original and patched implementations. Host tests
cover sorting, queues, disc grouping, favorites, recent games, settings, media
loading, USB failures, and save backup and restore.

Preview files in `build/menu-preview-*` render the actual draw/input routines
with host graphics stubs and the bundled bitmap font. These are layout previews;
console testing is still needed to check hardware rendering and behavior.

SDK checks verify the formatter ABI and inspect ARM stack allocations against
the 32 KiB task stack. ISO builds preserve the complete bootstrap runtime,
compare packed payloads with their sources, and verify both the ISO and the
retail LaunchMe signature. Build audits record source and tool hashes.

GitHub Actions is disabled. Tests and builds run locally, with no hosted
workflow runners, artifact storage, or scheduled CI jobs. `.gitignore` excludes
generated builds, reference checkouts, personal artwork, game images, saves,
caches, and credentials. `.dockerignore` limits the build context to the
required source, vendored dependencies, tests, configuration, and build tools.

See [CONTRIBUTING.md](CONTRIBUTING.md), [LICENSE](LICENSE),
[THIRD_PARTY.md](THIRD_PARTY.md) and [asset provenance](assets/README.md).
Dependencies retain their own terms. This project is a new menu implementation;
it does not patch the original compiled Fixel menu.
