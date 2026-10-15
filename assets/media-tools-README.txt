Oh My 3DO media tools

Indexing existing PNG covers needs Python 3 only. Resizing/conversion needs
Python 3.9+ and Pillow. Video conversion also needs FFmpeg on PATH.
The tools do not rebuild boot.iso or download games, music or video.
The optional organizer downloads game metadata and cover artwork.

ORGANIZE A USB LIBRARY

Keep all extracted scripts together on the PC. Python 3.9+ is required; install
Pillow for covers with: py -m pip install Pillow

Preview without modifying USB files:
  py organize-library.py scan M:/ --quick --plan library-plan.json

Open library-plan.html to review proposed moves and game information. Games use
Games/<region>/<game title>/, with discs of the same edition together. The first
scan downloads a Redump DAT and LaunchBox's metadata archive (about 104 MiB),
cached on the PC. Subsequent scans use the cache. Metadata fields may be missing.

Apply the reviewed filename matches, download covers, and build the database:
  py organize-library.py apply M:/ --plan library-plan.json --accept-name-matches

Without --quick, scan reads the entire data files and compares their SHA-1 and
size to the DAT. Checksum/manual matches can be applied without accepting name
matches. Converted ISOs often differ from raw Redump hashes. Filename matches
are unverified; ambiguous or fuzzy suggestions never move automatically.

Resolve a match with a PC-side overrides.json object, for example:
  {"Unsorted/game.iso": "Doom (USA, Japan)"}
Then scan again with --overrides overrides.json. --dat can select a local
clrmamepro-format DAT; --launchbox selects an existing Metadata.zip.

CUE companions move and are renamed together; FILE references are updated.
Only sibling-file references are supported. Shared/missing tracks, ambiguous
matches, and destination collisions stay in place. No duplicate images are
deleted. Existing artwork links are preserved; downloaded covers are native
84x105 PNGs. Favorites, recent games, saved selection and media/search indices
are updated. boot.iso, settings, NVRAM backups and music stay in place.

The game information database is USB/.ohmy3do/library.sqlite, with a readable
library.json beside it. It contains genre, publisher, year, developer and
identification/source information. Current console menus do not display those
metadata fields; search and covers work through the generated .3di indices.

Apply prints a journal path. Undo using the printed path relative to USB:
  py organize-library.py undo M:/ .ohmy3do/transactions/<printed-id>.json

Failed apply operations attempt automatic undo. After an interruption, keep the
journal and run undo before rescanning. Undo refuses to overwrite later edits.
Original empty game folders and journals are retained. --no-covers on apply skips
artwork; --offline uses cached data only. scan --refresh-catalog refreshes source
downloads. --cache PATH before the subcommand selects a PC cache location.

Data sources (downloaded separately; not bundled or covered by our MIT license):
  https://github.com/libretro/libretro-database
  https://gamesdb.launchbox-app.com/Metadata.zip
  https://github.com/libretro-thumbnails/libretro-thumbnails

Full details: https://github.com/Scyllinice/oh-my-3do#usb-library-organizer

PREPARE COVERS

Install the image library once (Windows):
  py -m pip install Pillow

Audit existing covers without changing files:
  py resize-covers.py "E:/covers" --check
This reports dimensions, mode and size. Exit status 1 means some images would
benefit from preparation; 0 means every image is already 84x105 RGB PNG.

Resize one image and give it the game's basename:
  py resize-covers.py "Downloads/cover.jpg" "E:/covers/Road Rash.png"

Or convert a whole folder, keeping each image's basename:
  py resize-covers.py "Downloads/artwork" "E:/covers"

The output is a 84x105 RGB PNG. Images are fitted without cropping, with dark
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
  USB/covers/Road Rash.png

PNGs: up to 1024x1024 pixels and 512 KiB. Prepared 84x105 RGB PNGs avoid
console-side resizing and alpha blending. The indexer warns about larger PNGs
without changing them. The menu caches six decoded covers for repeat visits.
Regenerate the index after adding, removing or renaming games/media.

Windows example (change E:/ to the USB drive):
  py index-media.py E:/

Linux/macOS example:
  python3 index-media.py /path/to/USB

The indexer writes index.3di in covers/ and previews/, plus library.3di at the
USB root. Find uses library.3di instead of scanning folders on the console.
Regenerate these indices after changing the games or media on the drive.
Prepare covers from original artwork for the sharpest detail; previously reduced
64x80 files cannot recover detail when enlarged. Video previews remain 64x80.
It preserves existing valid associations, including converted .3dc covers.
It does not convert or modify PNGs. The same game basename in different folders
shares its matching media. Cold boot or refresh at USB root reloads indices.

REPLACEABLE MENU MUSIC (MENU 0.10.0 OR NEWER)

Music is loaded completely into RAM, then decoded by the 3DO DSP. It does not
stream from USB, so artwork reads do not compete with music file reads.

Install FFmpeg and the separate 3at native audio encoder, and put both on PATH:
  https://ffmpeg.org/download.html
  https://github.com/trapexit/3at

Convert the complete track, optionally making a decoded WAV listening copy:
  py prepare-music.py "song.mp3" "M:/music/menu.3dm" --preview "listen.wav"

Use Python 3.9+. Default: 11025 Hz mono ADP4, up to about 2 minutes 22 seconds.
--rate 22050 allows about 1 minute 11 seconds at higher fidelity. Tracks that
exceed the 768 KiB cap are rejected without shortening or replacing the output.

No media index is needed for music. Cold boot or turn Menu music off and back on
to reload it. The menu's original synthesized sample is the missing/invalid-file
fallback; cancelled loads stay silent. The on/off setting still saves to the INI.
Audio DSP, memory availability and long-track playback need a console test.

The project Docker image includes both dependencies:
  ./tools/dev-container music artwork/song.mp3 --preview iso/listen.wav
Copy the resulting iso/music folder to USB. Future tracks require no ISO rebuild.

SILENT VIDEO PREVIEWS

For a first test without FFmpeg, copy the included original moving test pattern
"demo-preview.3dv" into USB previews/, rename it to match one game's basename
(e.g. Road Rash.3dv), then run the indexer and enable previews as described below.

Convert a local clip; the output is a small 64x80 RAM-playable animation:
  py prepare-preview.py input.mp4 "E:/previews/Road Rash.3dv"
  py index-media.py E:/

Default: 24 frames at 6 fps (four seconds, 240 KiB). Optional:
  py prepare-preview.py input.mp4 "E:/previews/Road Rash.3dv" --start 10 --frames 12 --fps 6

Limits: 1-32 frames, 1-12 fps. There is no audio or direct MP4 playback.
Enable Video previews in Tools > Settings, save, then cold boot.
Keep the existing INI and folder-workaround setting. Alternatively add
video_previews=1 under [artwork] in the USB ohmy3do.ini.

A preview waits for the selection to settle, loads completely into RAM and
loops without USB reads. Pressing a button cancels loading/stops playback.
Missing/invalid clips leave the cover available. Loading speed depends on
firmware; console verification is still needed for these new media features.

Project and full Docker workflow:
https://github.com/Scyllinice/oh-my-3do
FFmpeg downloads (separate software, not bundled):
https://ffmpeg.org/download.html

These Python tools are covered by the included MIT license.
