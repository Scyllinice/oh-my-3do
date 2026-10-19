# Command-line library organizer

This is the detailed Python CLI reference. For a graphical import and review
workflow, see the [desktop app](../desktop/README.md). For an existing organized
library, see [incremental media updates and USB preparation](library-maintenance.md).

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

The report separates **Matched**, **Needs a choice**, **Unidentified**, and
**File problems**. Filename matches already identify a single release or a
marked game-level entry, but do not verify the disc contents. Check a sample
of their proposed paths and accept them together with `--accept-name-matches`.
Unresolved entries stay in place and do not block that batch.

Choose a release or category from the report's dropdowns, then download the
choices as `overrides.json` or `categories.json`. Place those files beside the
tools and scan again with the corresponding flags. Nothing is selected
automatically, and editing the report does not change the plan or USB drive.
Download choices before closing the page. New release choices may expose
category choices on the next scan. File problems need attention separately.

Rebuild the report from an existing plan without rescanning or reading discs:

```powershell
py organize-library.py report library-plan.json
```

Names such as `(Disc 1 of 2)` and compound region labels such as `(EU-US)`
are supported. Different spellings and alternate titles remain suggestions
unless an exact normalized match exists; fuzzy suggestions are not applied.

The first scan downloads the [full Redump 3DO identification DAT](https://redump.info/datfile/3do/)
and the [LaunchBox metadata archive](https://forums.launchbox-app.com/topic/30123-launchbox-games-db-external-scraper/),
which contains metadata for multiple systems. Only 3DO records are retained in the
parsed catalog; subsequent scans use a local PC cache. Metadata can be incomplete
or inaccurate, and missing fields remain unknown. It describes games rather
than guaranteeing dates or publishers for each regional pressing. Source URLs and download
checksums accompany the generated database. No account or API key is needed.

The full catalog includes demos, coverdiscs, multimedia, educational software,
applications, and audio bonus discs. An online scan automatically upgrades an
older limited catalog cache while reusing downloaded LaunchBox metadata.
Offline scans can still use the older cache, with a warning about missing releases.
`--dat` accepts XML/Logiqx and clrmamepro DATs, either plain text or ZIP.
CUE records in a DAT are excluded from disc-data comparisons; all BIN tracks,
including audio tracks, remain part of checksum verification.

Open the HTML preview before applying changes. The layout is
`<category>/<region>/<title>/<disc image>`, with discs of the same edition in
one folder. Categories are `Games`, `Demos`, `Samplers`, `Applications`,
`Multimedia`, `Educational`, `Bonus Discs`, and `Adult`; Adult is only ever
chosen by hand (category overrides here, or the desktop app's Library page). Redump categories take precedence
over title/genre heuristics; explicit category choices take precedence over both.
`Video` maps to Multimedia, `Coverdiscs` to Samplers, and `Add-Ons` to Games.
Unclassified catalog types still need review. Compound regions use their first
listed region for the folder while retaining the full release name. For example:

```text
Games/USA/D (USA)/D (USA) (Disc 1).iso
Games/USA/D (USA)/D (USA) (Disc 2).iso
Samplers/USA/3DO Interactive Sampler CD 2, The (USA)/...
Multimedia/USA/20th Century Video Almanac (USA)/...
Educational/USA/Learning title (USA)/...
Bonus Discs/Japan/Game title (Japan)/...
```

Classification uses catalog title labels, release type, and genre. Explicit demo
labels go to Demos; sampler discs go to Samplers; utilities go to Applications;
multimedia/reference discs such as atlases and almanacs go to Multimedia. Full
game compilations remain Games, and games containing full-motion video are not
classified as multimedia solely because they contain video.
Educational software, including educational games, goes to Educational. Clear
reference titles take precedence over a broad Education genre, and explicit
demo/sampler labels take precedence over the educational theme.

Bonus discs have their own **Bonus Discs** category. Explicit bonus-disc labels
and existing folders named `Bonus Discs` (including numbered names such as
`4 Bonus Discs`) identify extras separately from the base game. A manual
category choice takes precedence. Bonus-disc filenames retain their source
labels, such as `[CD-Audio]`, within the menu's filename limits. This also keeps
their CUE companions separate from the main game's files. An unidentified
extra still needs a release match before it can move; the folder alone does
not identify its contents.

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
time on a large USB drive. For whole-sector 2048-byte ISOs with a plausible
catalog track size, the scanner also reconstructs standard Mode 1 headers,
EDC and ECC in memory and compares the resulting 2352-byte-sector checksum.
The source ISO stays unchanged. Both checksums are computed in one read, with
bounded memory and progress messages during long reads.

`checksum` identifies a byte-for-byte catalog dump. `converted-checksum`
identifies the complete single-track catalog image after reconstruction; both
are **ready** without `--accept-name-matches`. `data-track` identifies only the
first track of a multi-track catalog entry and remains **review**: absent audio
or other tracks have not been verified or restored. Accept it explicitly with
`--accept-name-matches` only after reviewing the missing-track warning. Shared
checksums across releases stay ambiguous even when the filename suggests one.
Modified images, nonstandard headers, trimmed sectors, gaps and Mode 2 data
are not repaired or guessed; unsuccessful reconstruction leaves the existing
filename/manual identification workflow available.

A single title with different release labels is reported as `unverified-release`,
with its catalog suggestion, rather than claiming that multiple releases matched.
A familiar name never certifies a modified, trimmed, or uncatalogued build.
Full scans also report matching data-file checksums across separate copies.
CUE layouts are not compared by that duplicate warning; nothing is deleted or
merged automatically, and destination collisions still require a decision.

To check existing guesses, write a separate plan without `--quick` or
`--overrides`. Explicit overrides still take precedence over identification
and are labeled `manual`, even during a full scan:

```powershell
py organize-library.py scan M:/ --plan verified-plan.json
```

The release ZIP includes native accelerators for Windows x64 and Linux x64.
Keep them beside the Python tools; no compiler or extra Python package is
needed on those systems. Other systems use a slower Python fallback. To build
an accelerator from a source checkout, run `python3 tools/build-sector-codec.py`.
From the extracted ZIP, run `python3 build-sector-codec.py --output .` with a C
compiler installed. The Windows cross-build uses Clang and LLD:
`python3 tools/build-sector-codec.py --windows`. These helpers run on the PC
and are never included in `boot.iso`. `--dat` also accepts a local
clrmamepro-format DAT, including one for the ISO format in use.

ZIP files are not scanned or renamed into ISOs; extract them first and preserve
their BIN/CUE or ISO contents. Empty data files and recognized archive headers
disguised as disc images are blocked before identification, even with an
explicit override. Apply repeats these checks before any moves, including for
older saved plans. The standalone indexer omits empty/archive-disguised ISOs
from Find and artwork associations. These checks do not replace checksum
verification or repair damaged images.

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
Matched discs with the same category, region and game/edition title are moved
into that common folder even when they started in separate folders. Disc
numbers remain in filenames; unresolved entries and destination conflicts
still need review. Empty source folders can be removed with `clean-empty`.

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

To add an uncatalogued or alternate build to Find and the database while keeping
its existing filename and folder, use `register`. It does not download metadata
or assign a catalog release. Preview first, then repeat with `--apply`:

```powershell
py organize-library.py register M:/ "Games/USA/Example/Example.iso" --title "Example" --region USA --category Games
py organize-library.py register M:/ "Games/USA/Example/Example.iso" --title "Example" --region USA --category Games --apply
```

Registration records the disc as `unverified`, hashes every source file, and
updates Find and the database with an undo journal. Disc files remain in place.
Existing covers/previews are preserved. Unchanged registrations survive later
scans; a full scan rechecks SHA-1, while a quick scan checks size and timestamp.
Changed files require fresh identification or explicit registration. ISO
registration checks the 3DO volume header and sector alignment, but does not
certify signatures or hardware bootability. Existing checksum-verified catalog
entries cannot be downgraded through `register`.

Audio-only CUEs stay in place during ordinary organization. They are reported
as audio discs rather than identified as the companion game by title. Use the
[audio-disc converter](#audio-bonus-discs) for WAV sets, then register the
resulting CUE with `--category "Bonus Discs"` to add its title to the database.

CUE sets move together. Companion files receive names derived from the disc
and track number, and their CUE `FILE` references are updated. Comments, track
directives, line endings, and supported source encodings are preserved. Only
sibling-file references are supported; CUE paths into other folders need manual
attention. Case-insensitive file references work on FAT and on a Linux-hosted
test directory. Useful CUEs and their data/audio files remain one move group.
A CUE containing only one MODE1/2048 ISO track at 00:00:00 is redundant. The
organizer uses its ISO and archives the CUE under `.ohmy3do/ignored-cues/` on
apply, with byte-for-byte undo. Existing CUE-based choices, favorites, history,
position, covers, and previews follow the ISO. BIN/CUE, audio, multiple tracks,
gaps, extra indexes, unknown directives, and broken/shared sets are retained.
The standalone indexer also omits verified redundant wrappers. This does not
merge separate ISO copies or resolve their destination collisions.

Apply downloads available [Libretro box scans](https://github.com/libretro-thumbnails/libretro-thumbnails),
prepares compressed 96×120 RGB555 `.3dc` covers and 192×240 `-large.3dc`
companions, and links them to the correct game paths. Existing
cover and preview associations are preserved. Missing or ambiguous artwork is
reported without blocking other games. `--no-covers` skips cover downloads and
imports. Use
`--offline` on scan/apply to work from existing caches; `scan --refresh-catalog`
refreshes the identification and metadata sources.

Use `--cover-dir` to import local front-box images instead of downloading covers.
Point it at the front-cover folder inside an EmuMovies download, rather than a
parent containing screenshots, logos, and back covers:

```powershell
py organize-library.py apply M:/ --plan library-plan.json --accept-name-matches --cover-dir "D:/EmuMovies/Panasonic_3DO/<front-cover-folder>"
```

Replace `<front-cover-folder>` with the actual folder name. Subfolders are
searched. PNG, JPEG, WebP, BMP, GIF, and TIFF are supported; install Pillow with
`py -m pip install Pillow`. Images become native 96×120 `.3dc` covers with
192×240 `-large.3dc` companions, are indexed automatically, and are included
in the undo journal. Originals and existing USB
covers are retained. Local imports work offline and do not fall back to network
downloads. Missing, ambiguous, or unreadable images are reported and skipped.
Matching follows the same exact-name/unique-title rules as preview imports,
including region and disc checks. A generic cover can serve every disc of a
game. The JSON export records the image's relative source filename.

To import downloaded gameplay clips during apply, add
`--preview-dir "C:/EmuMovies/3DO"`; see the EmuMovies workflow below. Existing
previews take precedence. New clips and their index associations are included
in the same undo journal as the disc moves.
`--cover-dir` and `--preview-dir` can be used together on the same apply command.
No ISO update is needed for either import feature.

Favorites, recent games, and the selected saved game are updated to the new
paths. If a saved folder loses its contents, the saved position resets to the
USB root. The search and artwork indices are rebuilt automatically. Original
empty game folders are retained. `boot.iso`, INI settings and save backups
are left in place.

Remove empty folders left after organizing with `clean-empty`. Preview first,
then add `--apply` to remove them:

```powershell
py organize-library.py clean-empty M:/
py organize-library.py clean-empty M:/ --apply
```

This removes empty folders from the bottom up without deleting files. Folders
containing unmatched discs, notes, artwork, or hidden files remain. Support
folders, hidden folders, Windows system folders, symlinks, and junctions are
skipped. No scan plan or downloads are needed. Organizer undo still works and
recreates folders needed to restore the moved discs.

Menu-written text ends at its first NUL terminator; old bytes after that point
are ignored when updating favorites, history, and saved position. An invalid
saved-position file is reset to the USB root with a warning, and undo restores
its original bytes. Text updates are prepared before any disc moves begin.

The USB database lives in `.ohmy3do/library.sqlite`, with a readable
`.ohmy3do/library.json` export. SQLite's `games` table contains paths, titles,
regions, disc numbers, genre, publisher, year, developer, match type, and cover
associations, plus category, classification evidence, and a category-review flag.
The `games` table includes supported disc images from all eight categories.
The console uses the generated search, artwork and metadata indices rather
than opening SQLite. Its selected-game panel displays genre, publisher and
release date or year when those fields are available.

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

## Audio bonus discs

`prepare-audio-disc.py` combines an audio-only WAV/CUE set into one BIN/CUE
pair, preserving its samples and recorded track gaps. It requires Python 3.9+
and CD-quality PCM WAVs; FFmpeg and an ISO rebuild are unnecessary.

```powershell
py prepare-audio-disc.py "D:/rips/album.cue" "D:/3DO/Bonus Discs/USA/Album (Audio CD)"
py index-media.py D:/3DO
```

Choose a new output folder, then select its CUE in the menu. Original files
remain in place. The converter verifies each output track and writes a local
`audio-disc.json` with source and output hashes. It supports one WAV per track
with INDEX 00/01; unsupported layouts are rejected.

These images need a playback test on the installed ODE firmware. The
[open-source firmware](https://github.com/FCare/SataTo3DO/blob/main/src/interfaces/MSC.c)
has audio-only BIN/CUE support, but unusual first-track pregaps can expose
firmware limitations. Conversion preserves the supplied rip; it does not verify
which catalog release it belongs to.

[Project overview](../README.md)
