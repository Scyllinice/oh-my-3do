# Oh My 3DO

![Oh My 3DO logo](assets/logo-menu.png)

Oh My 3DO is a replacement `boot.iso` menu for the open-source [FCare/SataTo3DO
ODE](https://github.com/FCare/SataTo3DO). It sorts folders and games by name
instead of FAT insertion order, with folders first and case-insensitive natural
sorting: `Disc 2` comes before `Disc 10`. No firmware update is required.

Features include disc queues, automatic multi-disc grouping, favorites, letter
navigation, saved settings, PNG covers, video previews, and NVRAM backup and
restore, plus optional menu music. PC tools can identify and organize a USB
library, download covers, and build a game information database. The current
version is **0.14.0**.

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
| `library.3di` | Search index generated on the PC by `index-media.py` |

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

Game lists hide `.iso` and `.cue` extensions from titles and show an ISO or CUE
label beside each entry. Both formats remain selectable, including when they
share a title. For BIN/CUE games, select the CUE; it describes the tracks and
references the companion data files. BIN files are hidden throughout the browser.
Full filenames and original TOC IDs are retained. Long selected names scroll after a pause;
folder changes reset the animation. Parent navigation restores selection. The
previous listing remains visible while a folder loads. A read failure hides the
listing and prevents selection until the connection recovers. The whole
directory is sorted before paging. Normal text shows seven rows; large filenames
show six.

At a USB root, support entries are hidden: `boot.iso`, `menu-demo.iso`, their
`.sha256` files, `ohmy3do.ini`, `favorites.txt`, `last-position.txt`,
`recent.txt`, `diagnostics.txt`, and the `covers`/`previews`/`music`/`saves` folders.
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
Same-folder queues use one fresh directory listing and add every disc in place,
avoiding repeated walks through the folder tree.
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
path limits. The loading artwork stays visible until startup settings and the
saved location are ready, so the browser first appears at its saved size.

Tools → Recent games lists up to ten recent launch selections, newest first.
**A** opens the containing folder and selects the image; **Start** reloads the
list. Paths are recorded in `recent.txt` just before handing control to a game,
so a failed mount can still appear in the list. Repeating a selection moves it
to the top without duplicates. Reads and verified writes share one trip to the
USB root, and repeating the newest cached selection skips the history write.
Long paths may reduce the number of entries;
paths of 1,024 bytes or more are not recorded. Missing paths do not launch a
game, and malformed lists are not overwritten automatically.

Tools → Find a game opens a controller keyboard. Use the arrows to choose a
character, **A** to enter it, **X** to erase, **C** to clear, and **Start** to
search. `SP` enters a space. Search matches game-image names across folders on the
selected USB drive, ignoring ASCII case. An empty query lists all games.

Run the updated `index-media.py` on the PC before using Find:

```powershell
py index-media.py M:/
```

The same command creates cover/preview associations and `library.3di` at the
USB root, even when there is no artwork. Find loads this checked file once and
keeps it in RAM. It does not recursively scan folders on the console. Matching
runs once per submitted query; drawing and navigating results use a stored match
list. The initial index read still requires a USB-root round trip.

Results show the selected game's folder to distinguish duplicate names.
**A** opens that folder and selects the game; it does not launch automatically.
**Start** or **B** returns to the keyboard, **X** reloads the index, and Left/Right
or L/R pages through results. Regenerate the index after adding or moving games.
Search covers the selected USB volume; enter a volume before searching from a
multi-device root. The index supports 2,048 games and 128 KiB of cached paths.
Support folders and the menu ISOs are excluded. A missing or invalid index shows
setup instructions instead of starting a slow directory scan.

Both position recording and recent-game recording can be disabled in Settings. Existing INIs use enabled defaults for these new options. Holding X
at startup skips these USB extras. Each USB volume has its own files; startup
restoration uses the boot volume's position file. Recent paths retain volume
labels where needed. A regular history-file failure does not prevent launch,
while an uncertain transport or failed folder restoration blocks it.

## Display preferences and configuration

Tools → Settings exposes every INI setting: additional screen
margins (0–12 pixels), larger game/queue titles, teal/high-contrast/amber
themes, the folder compatibility workaround, USB covers, video previews,
remembered position, recent games, and menu music. Left/Right or A changes the selected
value. Music and display changes take effect immediately. Save the remaining
options and cold boot to apply them. The settings list spans two pages; keep
pressing Down to reach the remaining actions, or press C to save from either page.

**C** or **Save changes to USB** writes all nine settings, closes/reopens the
file and verifies every byte. B returns without writing; unsaved display
previews remain active for this boot, and edited values remain on the settings
screen. Only a successful save persists after reboot. The unsaved marker
identifies changes that have not been saved. **X** or **Discard unsaved
changes** restores the last successfully saved values, including the display
preview.

The margin moves the menu into an inset area while keeping text at native pixel
size. Antialiased text uses brighter colors and larger glyphs for TV readability.
Game titles use a 12-pixel font, or 14 pixels and six list rows with large titles
enabled. Controls and system tools use a bold 10-pixel font.
Restore display defaults resets only the display settings.

The save writes a canonical INI containing all nine settings and replaces
comments/formatting. It requires a successfully
loaded config. Startup bypass, an invalid config, unsupported identity or a
failed transport prevents saving. The firmware cannot truncate files, so the
menu writes a fixed 512-byte plain-text file with Windows line endings, an
explicit `; End of Oh My 3DO settings.` comment, and space padding. It contains
no NUL bytes. Edit the values above the end comment; text below it is ignored.
Existing NUL-padded INIs still load and become readable text on the next save.
Older menu versions need the trailing space padding removed on a PC before
loading this new format. Do not interpret a failed save as persistent.

You can also edit `ohmy3do.ini` on a PC:

```ini
[compatibility]
folder_workaround=1

[artwork]
usb_covers=1
video_previews=0

[display]
screen_margin=0
large_titles=0
theme=0

[library]
remember_position=1
recent_games=1

[audio]
menu_music=1
```

`folder_workaround=0` is normal navigation; `1` enables the workaround for
FCare's directory-local ID collision bug. Keep the existing value if folder
navigation already works. `Compat on` appears at the bottom of the browser when
the workaround is enabled. `usb_covers` and `video_previews` are 0/1; previews
default off. Margin is 0–12; large titles is 0/1; theme is 0 teal, 1 high
contrast, 2 amber. The two library settings are 0/1. Old configs containing only
the earlier keys remain valid and use display defaults. Unknown/duplicate
settings or invalid values reject the config. PC text must stay below 512 bytes unless it retains all nine settings and the
end comment within the first 512 bytes;
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

## USB library organizer

`organize-library.py` runs on a PC with Python 3.9+. It is included in
`media-tools.zip` alongside its `library-catalog.py`, `index-media.py`, and
`resize-covers.py` dependencies. Keep the extracted tools together. Install
Pillow for cover conversion:

```powershell
py -m pip install Pillow
py organize-library.py scan M:/ --quick --plan library-plan.json
```

Run this from the extracted tools folder on the PC. Replace `M:/` with the USB
drive. Scanning leaves USB files unchanged and produces `library-plan.json` and
a searchable `library-plan.html` preview. The preview shows original/proposed
paths, identification confidence, category, genre, publisher, year, and unresolved matches.

The first scan downloads the [Libretro/Redump identification DAT](https://github.com/libretro/libretro-database)
and the [LaunchBox metadata archive](https://forums.launchbox-app.com/topic/30123-launchbox-games-db-external-scraper/),
currently about 104 MiB for the latter. Only 3DO records are retained in the
parsed catalog; subsequent scans use a local PC cache. Metadata can be incomplete
or inaccurate, and missing fields remain unknown. It describes games rather
than guaranteeing dates or publishers for each regional pressing. Source URLs and download
checksums accompany the generated database. No account or API key is needed.

Open the HTML preview before applying changes. The layout is
`<category>/<region>/<title>/<disc image>`, with discs of the same edition in
one folder. Categories are `Games`, `Demos`, `Samplers`, `Applications`,
`Multimedia`, and `Educational`. For example:

```text
Games/USA/D (USA)/D (USA) (Disc 1).iso
Games/USA/D (USA)/D (USA) (Disc 2).iso
Samplers/USA/3DO Interactive Sampler CD 2, The (USA)/...
Multimedia/USA/20th Century Video Almanac (USA)/...
Educational/USA/Learning title (USA)/...
```

Classification uses catalog title labels, release type, and genre. Explicit demo
labels go to Demos; sampler discs go to Samplers; utilities go to Applications;
multimedia/reference discs such as atlases and almanacs go to Multimedia. Full
game compilations remain Games, and games containing full-motion video are not
classified as multimedia solely because they contain video.
Educational software, including educational games, goes to Educational. Clear
reference titles take precedence over a broad Education genre, and explicit
demo/sampler labels take precedence over the educational theme.

Uncertain classifications are marked **category-review** and stay in place,
even with `--accept-name-matches` or a verified checksum. Examples include mixed
sampler/utility discs, missing genre information, and music or life-simulation
labels that do not establish a category. The HTML preview
shows the evidence for each choice. These are catalog-based classifications;
the script does not inspect the content inside a disc image to determine its type.

Choose categories in a separate PC-side `categories.json` file, using the current
relative disc paths copied from the preview:

```json
{
  "Unsorted/3DO Game Guru.iso": "Applications",
  "Unsorted/Interactive reference disc.iso": "Multimedia",
  "Unsorted/Educational game.iso": "Educational"
}
```

```powershell
py organize-library.py scan M:/ --quick --categories categories.json --plan library-plan.json
```

Category choices are saved in the USB library database during apply and retained
by later scans of the same identified title at its new path. After moving files,
omit `--categories` to reuse those choices, or update the file with current paths
to change them. Identification overrides (`--overrides`) remain separate: a
category choice cannot identify an unmatched disc. Old plans must be regenerated
with this version before apply. Existing cached source downloads are reused
when the parsed catalog is upgraded to include release type.

`--quick` matches filenames without reading entire disc images. These matches
are marked **review**, and moving them requires an explicit option:

```powershell
py organize-library.py apply M:/ --plan library-plan.json --accept-name-matches
```

Without `--quick`, scanning reads each image/data file and compares complete
SHA-1 hashes and sizes against the identification catalog. This can take a long
time on a large USB drive. Checksum and explicit manual matches are marked
**ready**; apply these without `--accept-name-matches`. The default DAT describes
raw Redump dumps. Converted 2048-byte-sector ISOs often cannot match those
checksums and still need a filename match or manual identification. `--dat`
accepts a local clrmamepro-format DAT, including one for the ISO format in use.

Ambiguous versions, fuzzy suggestions, broken/shared CUE sets, and destination
collisions stay in place. No duplicate images are deleted. To resolve a match,
put its relative USB path and exact catalog name in a PC-side overrides file,
then rescan:

```json
{"Unsorted/game.iso": "Doom (USA, Japan)"}
```

```powershell
py organize-library.py scan M:/ --quick --overrides overrides.json --plan library-plan.json
```

Release labels such as `(Alt)`, `(Rev 1)`, and `(Beta)` are retained, including
labels present only in the DAT's track filenames. If distinct dumps still share
a release name, filenames gain a stable eight-character code such as
`Game (USA) [Variant 1A2B3C4D].iso`. The code comes from the catalog's data-file
checksums, not scan order. Identical copies and serial-only catalog duplicates
do not get separate variant codes. Filename-only scans leave unnamed variants
ambiguous unless the filename or an explicit override identifies the code.
Rescan to generate a new plan after updating the organizer.

Disc suffixes such as `- Disc 1`, `Disk 02`, and `CD3` are recognized as well as
`(Disc 1)`. Candidates are restricted to the matching disc number. Disc numbers
and variant codes survive filename shortening. CUE companions receive the same
variant label and updated references. Opaque codes identify individual dumps;
they do not establish which differently coded discs belong to the same edition.
Such discs stay in the common game folder and can be queued manually.

When the filename has no region, clearly named parent folders such as `US`,
`USA`, `1 US`, or `Japan` narrow filename candidates. A region in the filename
takes precedence. Conflicting folder hints do not choose a region, and a known
catalog serial that conflicts with the inferred region remains ambiguous.
Folder hints still produce unverified filename matches requiring review.

Bracketed product/hub codes are matched against catalog serials within the
same title and disc number. An exact, unique serial match can identify a release
without reading the whole image, but does not verify its contents. If a code is
absent from the DAT, the organizer can make a **game-level** match when title,
region, and shared game metadata agree. It preserves the code in the filename
and marks the exact edition unresolved, without assigning a Redump revision.
For example, `Dragon's Lair [DH 65003].iso` and
`Dragon's Lair [DH 65003-2].iso` under `1 US` remain separate as
`Dragon's Lair (USA) [DH 65003].iso` and
`Dragon's Lair (USA) [DH 65003-2].iso`. Their matching code-specific covers are
preferred. The database records `variant`, `catalog_name`, and
`edition_unresolved`; unresolved game-level entries have no catalog edition name.

CUE sets move together. Companion files receive names derived from the disc
and track number, and their CUE `FILE` references are updated. Comments, track
directives, line endings, and supported source encodings are preserved. Only
sibling-file references are supported; CUE paths into other folders need manual
attention. Case-insensitive file references work on FAT and on a Linux-hosted
test directory. A CUE and its referenced ISO are treated as one move group.

Apply downloads available [Libretro box scans](https://github.com/libretro-thumbnails/libretro-thumbnails),
prepares 84×105 RGB PNGs, and links them to the correct game paths. Existing
cover and preview associations are preserved. Missing or ambiguous artwork is
reported without blocking other games. `--no-covers` skips downloads. Use
`--offline` on scan/apply to work from existing caches; `scan --refresh-catalog`
refreshes the identification and metadata sources.

Favorites, recent games, and the selected saved game are updated to the new
paths. If a saved folder loses its contents, the saved position resets to the
USB root. The search and artwork indices are rebuilt automatically. Original
empty game folders are retained. `boot.iso`, INI settings, save backups, and
music are left in place.

The USB database lives in `.ohmy3do/library.sqlite`, with a readable
`.ohmy3do/library.json` export. SQLite's `games` table contains paths, titles,
regions, disc numbers, genre, publisher, year, developer, match type, and cover
associations, plus category, classification evidence, and a category-review flag.
The `games` table includes supported disc images from all six categories.
These information fields are available for PC tools; the current
console menu uses the generated search/artwork indices and does not yet display
genre, publisher, or year.

Apply prints an undo journal path. To restore original file paths and support
files, use that relative path on the same USB root:

```powershell
py organize-library.py undo M:/ .ohmy3do/transactions/<printed-id>.json
```

The journal is written before moves and support-file replacements, including
CUE edits. Apply attempts to undo a failed operation automatically. After an
interruption, keep the journal and run undo before rescanning. Undo checks for
later edits and refuses to replace changed files. Journals remain on USB after
undo. Download caches remain on the PC. On Linux/macOS, replace `py` with
`python3`; `--cache <folder>` before the subcommand selects a different PC cache.

## PNG covers and video previews

Game media belongs in `/covers` and `/previews` on USB, outside `boot.iso`. PNG
covers can be copied directly: no image conversion or ISO rebuild is needed. The
menu decodes them into the cover area and preserves their aspect ratio.
Transparency is blended against the artwork background. PNGs can be up to
1024×1024 pixels and 512 KiB. For faster loading, prepare opaque 84×105 RGB
PNGs with the tool below; these match the artwork panel at native size and
avoid resizing and alpha blending on the console. Earlier versions reduced
covers to 64×80 and then stretched them; native PNG covers now retain all 84×105
pixels. Smaller PNGs and legacy 64×80 converted covers use smooth enlargement.
For sharper detail, prepare covers again from the original artwork, since an
already reduced image cannot recover discarded detail.
RGB, RGBA, grayscale, indexed, 1/2/4/8/16-bit and interlaced PNGs
are supported where valid for their color type. JPEGs still need conversion.

The menu keeps six decoded covers in RAM (about 104 KiB of pixel data). Returning
to a cached cover skips USB access, PNG decoding, and the normal selection wait.
Cache entries are separated by USB volume. Press **X** in the browser to refresh
after replacing cover files; reconnecting clears the cache as well. Cache
allocation is optional, so a memory shortage does not prevent a cover loading.
First-time loads still need the firmware's USB file access and a verified return
to the game folder. LOCODE restores check ancestor folder records directly where
safe, retaining full enumeration for compatibility detours and final verification.

To resize or convert artwork, use `resize-covers.py` from the release's
`media-tools.zip`. It needs Python 3.9+ and Pillow. On Windows:

```sh
py -m pip install Pillow
# Check existing artwork without changing it:
py resize-covers.py "E:/covers" --check
py resize-covers.py "Downloads/cover.jpg" "E:/covers/Road Rash.png"
# Or process a folder, preserving image basenames:
py resize-covers.py "Downloads/artwork" "E:/covers"
py index-media.py E:/
```

The tool creates 84×105 RGB PNGs, matching the cover texture used by the menu.
It preserves aspect ratio with dark padding, applies EXIF rotation, and blends
transparency against the artwork background. JPEG, PNG, WebP, BMP, TIFF, and GIF
are supported through Pillow; animated inputs use the first frame. Originals
stay intact. Existing covers require `--overwrite`; `--recursive` includes
subfolders in a flat output folder. Duplicate output names are rejected before
conversion. Keep output outside the input folder and match game basenames.

On Linux/macOS, replace `py` with `python3`. From a source checkout, the tool is
in `tools/`. Docker includes its dependencies after rebuilding the image:

```sh
./tools/dev-container build
./tools/dev-container resize-covers artwork iso/covers
```

Copy the prepared covers to USB and regenerate its media index. Existing valid
PNGs can still be copied directly; resizing is optional. `--check` reports each
image's dimensions, mode, and size, and exits with status 1 if preparation is
recommended. `index-media.py` also warns about PNGs different from 84×105 without
changing the artwork. Prepare replacements in a separate folder, then copy the
prepared PNGs into `covers/` and regenerate the index. Diagnostic exports include
the last cover load's milliseconds, ODE command count, and cache-hit flag.

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
`media-tools.zip` with the portable Python tools, a quick-start guide and an original
moving test-pattern clip. Rename that clip to match a game's basename for an
initial preview test without installing FFmpeg. From a source checkout, run:

```sh
python3 tools/index-media.py /path/to/USB
```

On Windows, run `py tools/index-media.py E:/`, substituting the USB drive
letter. With the portable ZIP, run `py index-media.py E:/` from its extracted
folder. The indexer scans game paths and writes small `index.3di` files plus
`library.3di` for Find, leaving the media files unchanged. The firmware does not
list PNGs or preview files in its browser, so these indices are still needed. Existing valid associations are
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

Enable **Video previews** in Tools → Settings, save, and cold boot
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

## Menu music

Music can be replaced from USB without rebuilding the ISO. Put the prepared
track at `music/menu.3dm` on the USB drive. The menu loads and verifies the whole
track before playback, then the 3DO DSP decodes it directly from RAM. Music
performs no USB reads while playing, so cover loading does not compete with an
audio stream. This adds a one-time load when music starts, after the browser has
restored and displayed the saved folder. Press a controller button during music
loading to cancel it.

Prepare a track with the Docker image, which includes FFmpeg and the pinned
[3at native audio encoder](https://github.com/trapexit/3at):

```sh
./tools/dev-container build
./tools/dev-container music artwork/music.mp3 --preview iso/music-preview.wav
```

Copy `iso/music/` to the USB root, then cold boot. The optional WAV is a decoded
listening copy of the converted track. It does not need to be copied to USB.
Neither an artwork index nor another ISO rebuild is needed for future tracks.
Install version 0.10.0 or newer once to enable USB music loading.

The portable `media-tools.zip` includes `prepare-music.py`. With Python 3.9+,
FFmpeg and 3at installed on PATH, Windows can use:

```sh
py prepare-music.py "music.mp3" "M:/music/menu.3dm" --preview "music-preview.wav"
```

The default is mono ADP4 at 11,025 Hz, with a 768 KiB compressed-data cap. That
fits about **2 minutes 22 seconds**, including a complete 1:49 track. This is a
lossy conversion with less high-frequency detail than the source. `--rate 22050`
keeps more detail but fits about **1 minute 11 seconds**. Oversized tracks are
rejected rather than silently shortened. The converter preserves the source,
pads to DMA alignment, applies tiny endpoint fades, and adds a checksum and end
marker to protect against firmware EOF errors.

Music is enabled by default. **Tools → Settings → Menu music**
changes playback immediately; press **C** to save. Turning it off releases audio
RAM. Turning it back on reloads the file from the selected USB volume; folder
navigation uses the same restoration and compatibility checks as USB artwork.
The settings row shows whether the active music is from USB or bundled. Holding
X at startup skips music. On a PC, `menu_music=0` under `[audio]` disables it.

An original synthesized 20-second loop remains bundled for the demo and as a
fallback when USB music is missing or invalid. No external recordings are in
that loop. A cancelled load stays silent, and an uncertain USB transport blocks
browsing without attempting fallback disc reads. Audio resources are released
before any game mount. Allocation or DSP failures leave the menu usable and
silent; toggle music off/on to retry. Larger tracks leave less RAM for other
optional media. Console playback still needs hardware testing.

The diagnostics export includes `music_source` (0 none, 1 bundled, 2 USB) and
`usb_music_result` (0 not attempted, 1 loaded, -1 unavailable/invalid, -2 transport
or folder failure, -3 cancelled), alongside the music setting and playback result.
The prepared file layout is described in [docs/media-format.md](docs/media-format.md).

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
are bounded. No in-game overlay, automatic return-to-menu, NVRAM
formatting, defragmentation, save deletion or Unicode collation is provided.
Image formats visible to the browser depend on the installed firmware.

## Testing and repository setup

Portable host tests (music conversion tests need FFmpeg and 3at on PATH):

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
