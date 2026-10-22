Oh My 3DO media tools (menu v1.0.2)

Indexing existing PNG covers needs only Python 3.9+ and its standard library.
Resizing/conversion also needs Pillow; video conversion needs FFmpeg on PATH.
The tools do not rebuild boot.iso or download games, music or video.
The optional organizer downloads game metadata and cover artwork.
Previews prefer matching regions, then a unique same-game clip from another
region. Covers remain region-specific. Ambiguous clips, demo/full-game
mismatches and incompatible disc labels are skipped. Existing media is kept.
The indexer also exports .ohmy3do/metadata.3di from library.json. The
menu shows genre, publisher and release date below the game list.
When only a year is known, it shows the year. Run index-media.py against an
existing staging library to export its metadata without rescanning checksums.

SUPPORT FILES
Keep boot.iso at the USB root. Settings, favorites/history/position,
diagnostics, indexes, covers, previews and save backups live in .ohmy3do/.
On a fresh drive, create .ohmy3do/ and copy ohmy3do.ini from this folder into
it. Game-path keys inside favorites and indexes stay relative to the USB
volume. Other support paths below are relative to .ohmy3do/.

ORGANIZE A USB LIBRARY

Keep all extracted scripts together on the PC. Python 3.9+ is required; install
Pillow for covers with: py -m pip install Pillow

Preview without modifying USB files:
  py organize-library.py scan M:/ --quick --plan library-plan.json

ZIP archives are not scanned or renamed into ISO files. Extract archives first
and keep their original BIN/CUE or ISO contents together. Empty disc files and
recognized archive headers disguised as disc images are blocked even with a
manual override. Apply checks again before any moves, including for older plans.
The standalone indexer omits empty/archive-disguised ISOs from Find and media
associations. These checks do not replace checksum verification of valid data.

Open library-plan.html to review proposed moves and game information. Discs use
<category>/<region>/<title>/, with discs of the same edition together. Categories
are Games, Demos, Samplers, Applications, Multimedia, Educational, Bonus Discs
and Adult. Adult is selected manually. The first scan downloads the full
Redump 3DO DAT and LaunchBox's metadata archive,
cached on the PC. Subsequent scans use the cache. Metadata fields may be missing.

Online scans upgrade the older limited catalog automatically, reusing LaunchBox
metadata. Offline scans warn when using a limited cache. --dat accepts XML/Logiqx
or clrmamepro DAT files, plain or ZIP. CUE metadata records are excluded from
checksum matching; every actual track remains required for a complete match.
Redump categories supply the default category; explicit choices take precedence.
Video maps to Multimedia, Coverdiscs to Samplers, and Add-Ons to Games.
Full scans report identical data-file checksums without deleting copies or
comparing CUE track layouts. A single unmatched release label is reported as
unverified-release instead of misleadingly claiming multiple matching releases.

REGISTER AN UNCATALOGUED DISC

Preview adding an existing disc without moving it or downloading metadata:
  py organize-library.py register M:/ "Games/USA/Example/Example.iso" --title "Example" --region USA --category Games
Repeat with --apply to update Find and the library database with an undo journal.
The disc remains unverified against the catalog. Source hashes are stored so an
unchanged registration survives future scans. Full scans compare SHA-1; quick
scans compare size/time only. Changed files need new identification/registration.
The command accepts valid ISO/CUE entries, preserves covers/previews, and rejects
empty/archive-disguised data. It does not certify signatures or bootability.
Use --category "Bonus Discs" for a prepared audio CUE. Ordinary scans leave
audio-only sets in place and point to prepare-audio-disc.py, avoiding false
identification as a same-named game.

The report has Matched, Needs a choice, Unidentified and File problems filters.
Matched filename entries already identify one release or a marked game-level
entry, but do not verify disc contents. Check a sample of proposed paths, then
accept the matched batch with --accept-name-matches. Unresolved entries stay
in place and do not block it. Dropdowns let you choose a release/category;
download overrides.json/categories.json, then scan with the corresponding flags.
Nothing is preselected. Download choices before closing the report. Editing the
report does not edit the plan or drive. New release choices may need category
choices on a subsequent scan. File problems need separate attention.
Rebuild a report from an existing plan without rescanning:
  py organize-library.py report library-plan.json
Names with (Disc 1 of 2), (EU-US), (EU-JP) and (GB) are recognized.

Catalog title labels, release type and genre determine the category. Full game
compilations remain Games; FMV gameplay alone does not make a disc multimedia.
Explicit atlases/almanacs and other reference media go to Multimedia. Uncertain
or mixed classifications show category-review and stay in place even when file
hashes match or --accept-name-matches is supplied. Educational software and
educational games go to Educational. Specific reference titles stay Multimedia,
and explicit demos/samplers keep those categories regardless of educational
theme. Music/life-simulation labels alone may require a category choice.
Bonus-disc labels and existing Bonus Discs folders (including 4 Bonus Discs)
classify extras separately. Manual category choices take precedence. Source
labels such as [CD-Audio] are retained in bonus filenames within menu limits;
CUE companions move with them. Unidentified extras still need a release match.

Copy/edit categories.example.json on the PC with current relative disc paths:
  {"Unsorted/3DO Game Guru.iso": "Applications"}
Then scan with the category choices:
  py organize-library.py scan M:/ --quick --categories categories.json --plan library-plan.json

Choices are stored during apply and reused for the same title at its new path.
Later scans can omit --categories; update paths in the file if using it again
after a move. Category choices do not identify unmatched discs. The database and
HTML preview include the category, evidence and review flag. Old plans must be
regenerated for this version. Older parsed catalogs are upgraded using existing
cached downloads, without requiring another download when those files exist.

Apply the reviewed filename matches, download covers, and build the database:
  py organize-library.py apply M:/ --plan library-plan.json --accept-name-matches

Without --quick, scan reads the entire data files and compares their SHA-1 and
size to the DAT, including reconstruction for standard converted ISOs (below).
Checksum/manual matches can be applied without accepting name matches.
Filename matches are unverified; ambiguous or fuzzy suggestions never move
automatically.

Variant labels (Alt, Rev, Beta, etc.) are retained even when the DAT places them
only in track filenames. Distinct dumps with the same name get stable filenames
such as Game (USA) [Variant 1A2B3C4D].iso. Identical copies still collide and stay
in place. A quick scan cannot choose between unnamed variants without a code or
explicit override. Generate a new scan plan after updating the organizer.
Trailing - Disc 1, Disk 02 and CD3 suffixes are recognized and restrict matching
to that disc. Disc numbers and variant codes survive shortened filenames. Codes
identify individual dumps, not a guaranteed multi-disc edition; differently
coded discs stay in the common game folder and can be queued manually.

If the filename has no region, explicit parent folders such as 1 US, USA or
Japan narrow filename candidates. Filename regions take precedence; conflicting
folder hints do not choose a region. These matches remain unverified.
Bracketed product/hub codes are checked against catalog serials. If the code is
unknown but title, region and shared game metadata agree, a game-level match can
preserve it without assigning an unsupported Redump edition. For example, the
DH 65003 and DH 65003-2 Dragon's Lair files keep separate filenames and matching
code-specific covers. The preview reports "exact edition unresolved" and the
database records that flag explicitly. Regenerate the scan plan after updating.

Resolve a match with a PC-side overrides.json object, for example:
  {"Unsorted/game.iso": "Doom (USA, Japan)"}
Then scan again with --overrides overrides.json. --dat can select a local
clrmamepro-format DAT; --launchbox selects an existing Metadata.zip.

VERIFY CONVERTED ISOS

Run a full scan into a separate plan without --quick or --overrides:
  py organize-library.py scan M:/ --plan verified-plan.json
Whole-sector MODE1/2048 ISOs with plausible catalog track sizes are hashed as
both their original bytes and reconstructed MODE1/2352 sectors in a single
read. Reconstruction is in memory; no images are changed or written.
  checksum: original data-file size and SHA-1 match the catalog.
  converted-checksum: complete single-track checksum matches after reconstruction.
  data-track: only the first track matches; remaining tracks are absent/unverified.
Checksum and converted-checksum matches are ready. Partial data-track matches
require review and --accept-name-matches. Shared checksums remain ambiguous.
Reconstruction assumes standard Mode 1 sectors starting at LBA 0. It does not
guess offsets, restore audio, repair modified images or trim/pad tracks.
Explicit overrides still take precedence and are labeled manual, so omit them
when checking guesses. --quick skips all image checksums and reconstruction.

Keep the supplied sector-codec-win64.dll / sector-codec.so beside the Python
tools for native acceleration on Windows x64 / Linux x64. No extra packages
or compiler are needed. Other platforms fall back to slower portable Python;
with a C compiler installed, build locally with:
  python3 build-sector-codec.py --output .
The helpers are PC tools and do not require a menu ISO update.

Matched discs with the same category, region and game/edition title consolidate
into a common game folder even when their source folders differ. Disc numbers
stay in filenames. Conflicts and unmatched discs still require review.

CUE companions move and are renamed together; FILE references are updated.
Only sibling-file references are supported. Shared/missing tracks, ambiguous
matches, and destination collisions stay in place. No duplicate images are
deleted. Single-track MODE1/2048 ISO CUE wrappers starting at zero are redundant. Their
ISO is the library entry; apply archives the original CUE in .ohmy3do/ignored-cues/
with undo support and updates CUE-based favorites and media links to the ISO.
BIN/CUE, audio/multiple tracks, gaps, extra indexes, unknown directives and
broken/shared sets remain intact. Separate ISO copies are not merged.
Existing artwork links are preserved; downloaded covers are native
96x120 RGB555 .3dc files, each with a 192x240 NAME-large.3dc for the menu's
full-screen cover view. The Adult category is only used when you choose it. Favorites, recent games, saved selection and media/search indices
are updated. boot.iso, settings, NVRAM backups and music stay in place.
Menu-written text is read up to its first NUL terminator, ignoring stale tails.
Invalid saved positions reset to the USB root with a warning; undo restores
the original file. Text changes are prepared before any discs are moved.

Clean up empty directories left after organizing:
  py organize-library.py clean-empty M:/
The command above previews changes. To remove the listed empty folders:
  py organize-library.py clean-empty M:/ --apply
Only empty directories are removed, deepest first; files are never deleted.
Folders with unmatched discs, notes or hidden files remain. Support folders,
hidden folders, Windows system folders, symlinks and junctions are skipped.
No scan plan, catalog or downloads are needed. Organizer undo still works and
recreates folders needed for the original files.

To import EmuMovies covers instead of downloading covers, add:
  --cover-dir "D:/EmuMovies/Panasonic_3DO/<front-cover-folder>"
Replace the placeholder with the actual front-box image folder. Avoid a parent
folder containing back covers, screenshots and logos. Subfolders are included.
Install Pillow: py -m pip install Pillow
PNG/JPEG/WebP/BMP/GIF/TIFF images become native 96x120 RGB555 .3dc covers with
192x240 -large.3dc companions. Covers are indexed
automatically. Exact names are preferred, then unique title matches
with compatible region/disc labels. A generic cover can serve all discs.
Ambiguous, missing and unreadable images are skipped. Existing USB covers and
original images stay intact. Local imports make no cover-download requests and
work with --offline. New files are included in the undo journal. --no-covers
skips all cover imports/downloads and cannot be combined with --cover-dir.
Use --cover-dir and --preview-dir together to import both. No ISO update needed.

The game information database is USB/.ohmy3do/library.sqlite, with a readable
library.json beside it. It contains genre, publisher, year, developer and
identification/source information. The console reads metadata.3di to display
genre, publisher and release date/year; it does not read SQLite or library.json.

Apply prints a journal path. Undo using the printed path relative to USB:
  py organize-library.py undo M:/ .ohmy3do/transactions/<printed-id>.json

Failed apply operations attempt automatic undo. After an interruption, keep the
journal and run undo before rescanning. Undo refuses to overwrite later edits.
Original empty game folders and journals are retained. --no-covers on apply skips
artwork; --offline uses cached data only. scan --refresh-catalog refreshes source
downloads. --cache PATH before the subcommand selects a PC cache location.

Data sources (downloaded separately; not bundled or covered by our MIT license):
  https://redump.info/datfile/3do/
  https://gamesdb.launchbox-app.com/Metadata.zip
  https://github.com/libretro-thumbnails/libretro-thumbnails

Full details: https://github.com/Scyllinice/oh-my-3do/blob/main/docs/library-organizer.md

PREPARE COVERS

Install the image library once (Windows):
  py -m pip install Pillow

Audit existing covers without changing files:
  py resize-covers.py "E:/.ohmy3do/covers" --check
This reports dimensions, mode and size. Exit status 1 means some images would
benefit from preparation; 0 means every image is already 96x120 RGB PNG.

Resize one image and give it the game's basename:
  py resize-covers.py "Downloads/cover.jpg" "E:/.ohmy3do/covers/Road Rash.png"

Or convert a whole folder, keeping each image's basename:
  py resize-covers.py "Downloads/artwork" "E:/.ohmy3do/covers"

The output is a 96x120 RGB PNG. Images are fitted without cropping, with dark
padding, transparency composited, and EXIF orientation corrected. JPEG, PNG,
WebP, BMP, TIFF and GIF inputs are supported when available in Pillow. Animated
inputs use the first frame. Originals are never replaced. Existing output files
require --overwrite; --recursive includes subfolders and produces a flat output.
Duplicate output basenames are rejected before conversion. Use a separate output
folder, and name each cover to match its game's .iso/.cue basename.

On Linux/macOS, use python3 instead of py (Pillow can also be installed through
the OS package manager). The project Docker image includes Pillow:
  ./tools/dev-container resize-covers artwork iso/covers

Then run index-media.py against the USB root as shown below. No ISO update is
needed to use the prepared covers.

PNG COVERS

Copy unmodified PNGs into the USB covers folder, using the game's basename:
  USB/Games/Road Rash.iso
  USB/.ohmy3do/covers/Road Rash.png

PNGs: up to 1024x1024 pixels and 512 KiB. Prepared 96x120 RGB PNGs avoid
console-side resizing and alpha blending. The indexer warns about larger PNGs
without changing them. The menu caches six decoded covers for repeat visits.
Regenerate the index after adding, removing or renaming games/media.

Windows example (change E:/ to the USB drive):
  py index-media.py E:/

Linux/macOS example:
  python3 index-media.py /path/to/USB

The indexer writes index.3di in covers/ and previews/, plus library.3di and
save-names.txt (Save manager game names read from the discs; the first run
inspects eligible ISO files, later runs reuse save-names-cache.json), all under .ohmy3do/. Find uses library.3di instead of scanning folders on the console.
Regenerate these indices after changing the games or media on the drive.
Prepare covers from original artwork for the sharpest detail. Video previews use
160x120. It preserves existing valid associations.
It does not convert or modify PNGs. The same game basename in different folders
shares its matching media. Cold boot or refresh at USB root reloads indices.

VIDEO PREVIEWS WITH SOUND

For a first test without FFmpeg, copy the included original moving test pattern
"demo-preview.3dv" (moving pattern and quiet test tone) into USB .ohmy3do/previews/, rename it to match one game's basename
(e.g. Road Rash.3dv), then run the indexer and enable previews as described below.

Convert a local clip; the output is a 160x120, 30 fps RAM-playable animation:
  py prepare-preview.py input.mp4 "E:/.ohmy3do/previews/Road Rash.3dv"
  py index-media.py E:/

New conversions (OM3DVID5) are up to 15 seconds at 30 fps with source audio, in
a 1,400 KiB payload budget; --budget-kib changes that. Decoder/frame buffers
and allocation guards require additional memory. Shorter sources stay shorter, and
--duration can request less than 15 seconds. Loading speed depends on storage
and firmware; conversion time depends on the PC and clip.
The encoder library is included (preview-encoder-win64.dll / preview-encoder.so,
or build it with "py build-preview-encoder.py --output ." where a C compiler is available).
The menu plays OM3DVID5 previews. The desktop app converts original videos
with Covers & previews > Rebuild media quality.
Audio is signed 8-bit 11025 Hz mono PCM.
FFmpeg and ffprobe must both be on PATH; no additional audio encoder is needed.

Optional:
  py prepare-preview.py input.mp4 "E:/.ohmy3do/previews/Road Rash.3dv" --start 10 --duration 10
Use --silent to omit audio.
Enable Video previews in Tools > Settings, save, then cold boot. Preview sound
can be toggled independently and saves as preview_sound=0/1 under [audio].
Keep the existing INI and folder-workaround setting. The menu has no
background music; menu_music is an unknown INI key and rejects the config.
Existing music files are left alone.

Bulk import from EmuMovies:
Download 3DO Video Snaps using EmuMovies Sync with a supporting membership,
or extract a downloaded 3DO video pack into a PC folder. Keep original game
filenames and keep that folder limited to 3DO clips. Install FFmpeg on PATH.
Scan the USB drive, review the HTML plan, then apply with:
  py organize-library.py apply M:/ --plan library-plan.json --accept-name-matches --preview-dir "C:/EmuMovies/3DO"
Optionally add --preview-start 5 to skip the first five seconds of each clip.
MP4/AVI/MKV/MOV/WebM clips in subfolders are supported. Exact names are preferred;
a unique normalized title match is allowed when known region/disc labels agree.
Generic clips without a disc label can serve every disc. Ambiguous/missing
clips and conversion failures are reported and skipped. Existing previews stay.
Imports use at most 15 seconds at 30 fps with source audio, trimming further
when needed to fit RAM.
--preview-duration 10 requests a shorter maximum; --preview-silent omits audio.
--replace-previews explicitly upgrades existing associations, preserving old media
files. A fresh scan/plan is recommended before applying. The preview index is
built automatically and new previews are covered by the organizer undo journal.
Original videos stay on the PC. Enable Video previews in Settings afterward.
Install boot.iso from the matching release; media changes need no ISO rebuild.
This tool does not log into EmuMovies or store account
credentials. Direct API integration needs separate developer access:
  https://emumovies.com/forums/topic/58984-new-2023-emumovies-api-available-to-developers/
  https://emumovies.com/faq/4-questions-about-emumovies-sync
Downloaded footage is not bundled with these tools.

A preview waits for the selection to settle, loads completely into RAM and
loops without USB reads. Pressing a button cancels loading/stops playback.
Missing/invalid clips leave the cover available. Loading speed depends on
firmware and storage. Covers and previews have been tested on a console, but
that does not guarantee playback on every ODE/firmware combination.

Project and full Docker workflow:
https://github.com/Scyllinice/oh-my-3do
FFmpeg downloads (separate software, not bundled):
https://ffmpeg.org/download.html

These Python tools are covered by the included MIT license.

AUDIO BONUS DISCS

Combine an audio-only WAV/CUE set into a single BIN/CUE pair:
  py prepare-audio-disc.py "D:/rips/album.cue" "D:/3DO/Bonus Discs/USA/Album (Audio CD)"

Select the resulting CUE in the menu. The output directory must be new.
Python 3.9+ is sufficient; no FFmpeg or ISO rebuild is needed. Audio must already
be uncompressed 44.1 kHz, 16-bit stereo with complete CD sectors. One WAV per
track is supported. Recorded INDEX 00/01 gaps are preserved; unsupported layouts
are rejected. Audio samples are copied without resampling or compression and
verified after writing. Original WAVs and CUEs stay in place. audio-disc.json
records source and output hashes. Reindex Find with index-media.py afterward.

These are audio CDs, not bootable game ISOs. The open-source ODE has audio-track
support, but playback on each installed firmware needs a hardware test. Unusual
first-track pregaps may expose firmware limitations. Test a conventional disc
before a 99-track bonus disc. Conversion does not certify a Redump release.

INCREMENTAL MEDIA AND USB PREPARATION

Run these commands from the extracted media-tools directory. Python 3.9+ is
required. Cover imports and review thumbnails use Pillow; preview imports need
FFmpeg and ffprobe on PATH. USB validation/copy uses the Python standard library.

Preview only the missing covers and videos for an already organized library:
  py library-maintenance.py update-media D:/3DO --emu-movies D:/EmuMovies

Review artwork-review.html, then repeat with --apply to import matching media:
  py library-maintenance.py update-media D:/3DO --emu-movies D:/EmuMovies --apply

Existing associations and media are preserved. Disc identities, registrations,
folder locations, Find, settings and saves stay unchanged. New game files must
first be organized or registered with organize-library.py, then indexed. The
update command reads the existing database; it does not checksum game images
or discover/re-identify newly added discs. Missing and ambiguous matches stay
unselected. A failed conversion is reported and does not block other matches.
Converted files are cached on the PC, keyed by source content and conversion
settings. Imports have an organizer undo journal; rollback covers media,
association indexes and both library databases.

--emu-movies accepts D:/EmuMovies or D:/EmuMovies/Panasonic_3DO. It uses Box for
covers and prefers Video_MP4_HD, then Video_MP4_HI_QUAL, then Video_MP4, once per
title. Alternate directories can be supplied with --cover-dir and --preview-dir.
Only the supplied front-cover directory is searched, avoiding logos/back covers.
New previews are 30 fps, at most 15 seconds, with sound when present. Existing
previews are kept. No account login or download occurs.

Create a fresh, searchable contact sheet of installed covers at any time:
  py library-maintenance.py review D:/3DO --report artwork-review.html

Validate staging and preview the files needed on a USB drive (replace M:/ with
its actual drive letter):
  py library-maintenance.py prepare-usb D:/3DO M:/ --report usb-copy-report.json

If Find is stale, rebuild it first:
  py index-media.py D:/3DO

Copy after reviewing the JSON report:
  py library-maintenance.py prepare-usb D:/3DO M:/ --report usb-copy-report.json --apply

Both roots must already exist and be separate. The staging library must
contain boot.iso, .ohmy3do/ohmy3do.ini and the Find index.
The command checks the boot volume header/length (not its cryptographic boot
signature), config syntax, Find paths/checksums, disc archive signatures/CUE
references, media associations, cover PNG chunks and preview frame tables and
checksums. This validates files; it cannot certify hardware playback or identify
which game is inside an image. Media absent from the index is not copied.

Only disc files, boot.iso, configuration, indexes, referenced covers/previews,
and available favorites/history/save files are eligible. PC library JSON and
SQLite databases, reports, caches, old CUE archives and undo/recovery folders
are excluded. Existing destination settings, favorites, history and save files
are preserved. Missing ones can be copied.
Nothing is deleted from the destination. Different existing game files block
copying rather than being overwritten. Updated boot/index/media files are
backed up on the PC beside the report. Each copy goes to a temporary file and
passes SHA-256 readback before replacement. Failed copies can be rerun.

Unchanged existing discs use size and exact timestamp as a fast shortcut;
otherwise matching-size files are hashed. Keep reusing the same copy report:
it also records verified source/destination sizes and timestamps, allowing fast
skips after a verified copy even when FAT rounds destination timestamps.
Preview runs preserve these copy receipts. Add --verify-discs to hash every
existing disc, including those whose size/timestamp agree. That reads the
entire matching destination library and can take a long time. Non-disc runtime
files are compared by content. The tool does not remove old games or stale
files from the USB drive. Reports, conversion cache and update backups must
stay outside both roots.

FAST PREPARED COVERS

Library imports and media updates prepare compressed native .3dc covers.
Resolution stays 96x120; the console skips PNG filtering and color conversion.
The standalone resize-covers.py command still produces PNGs, which remain
supported. Playback remains entirely in RAM; sound-off skips allocating audio.
