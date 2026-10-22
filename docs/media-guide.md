# Covers and video previews

Prepare optional artwork for the console menu. The [desktop app](../desktop/README.md)
provides a graphical workflow; this guide also covers direct file setup and
the Python tools. Support-file paths below are relative to `.ohmy3do/` at the
USB root unless a full path is shown.

Game media belongs in `.ohmy3do/covers` and `.ohmy3do/previews` on USB, outside `boot.iso`. PNG
covers can be copied directly: no image conversion or ISO rebuild is needed. The
menu decodes them into the cover area and preserves their aspect ratio.
Transparency is blended against the artwork background. PNGs can be up to
1024×1024 pixels and 512 KiB. For faster loading, prepare opaque 96×120 RGB
PNGs with the tool below; these match the artwork panel at native size and
avoid resizing and alpha blending on the console. Smaller PNGs use smooth
enlargement.
The desktop app and library organizer now prepare compressed native `.3dc`
covers automatically. These keep the same 96×120 RGB555 display pixels while
skipping PNG filters, scaling and per-pixel conversion on the console.
Raw PNGs remain supported alongside prepared covers.

RGB, RGBA, grayscale, indexed, 1/2/4/8/16-bit and interlaced PNGs
are supported where valid for their color type. JPEGs still need conversion.

The menu caches up to six decoded covers (135 KiB of pixel data). Returning
to a cached cover skips USB access, PNG decoding, and the normal selection wait.
The cache is released before loading a preview; the displayed cover stays available.
Media and game-information indexes are read once per USB volume during a
session. Returning to the root reuses them, including an empty or missing index.
Press **X** at the filesystem root or cold boot to reload changed indexes.

Cache entries are separated by USB volume. Press **X** at the filesystem root to refresh
after replacing cover files; on a game with a preview, X switches cover/video
instead. Reconnecting clears the cache as well. Cache
allocation is optional, so a memory shortage does not prevent a cover loading.
First-time loads still need the firmware's USB file access and a verified return
to the browser folder. A game displayed as a single row is entered temporarily
for its media read, keeping file-open paths short; the browser and saved position
stay on the game list. LOCODE restores check ancestor folder records directly where
safe, retaining full enumeration for compatibility detours and final verification.

To resize or convert artwork, use `resize-covers.py` from the release's
`media-tools.zip`. It needs Python 3.9+ and Pillow. On Windows:

```sh
py -m pip install Pillow
# Check existing artwork without changing it:
py resize-covers.py "E:/.ohmy3do/covers" --check
py resize-covers.py "Downloads/cover.jpg" "E:/.ohmy3do/covers/Road Rash.png"
# Or process a folder, preserving image basenames:
py resize-covers.py "Downloads/artwork" "E:/.ohmy3do/covers"
py index-media.py E:/
```

The tool creates 96×120 RGB PNGs, matching the cover texture used by the menu.
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
recommended. `index-media.py` also warns about PNGs different from 96×120 without
changing the artwork. Prepare replacements in a separate folder, then copy the
prepared PNGs into `covers/` and regenerate the index. Diagnostic exports include
the last cover load's milliseconds, ODE command count, and cache-hit flag.

The filename matches the game's basename, ignoring ASCII case. For example:

```text
USB/
  boot.iso
  Games/
    Road Rash.iso
  .ohmy3do/
    ohmy3do.ini
    favorites.txt
    library.3di
    covers/
      Road Rash.png
      index.3di
    previews/
      Road Rash.3dv
      index.3di
    saves/
```

After copying PNGs, generate the media indices on a PC with Python 3.9+;
the standard library is enough. Docker and FFmpeg are not needed for PNG covers.
The release includes a portable
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
preserved when their game and media files still exist; a matching PNG basename
takes precedence. Games in different folders
with the same basename share the matching artwork. Each index supports up to
2,048 associations / 128 KiB. Media basenames must fit 96 UTF-8 bytes. No
footage is downloaded by these tools. The organizer can download covers and
import clips already saved on the PC.

![Road Rash video preview playing in the menu](images/gameplay-preview.png)

Road Rash preview in the menu v1.0.1 boot image in Opera: native 320×240, enlarged to 960×720.

The library uses a left-hand game list and a fixed right-hand media panel.
Portrait covers display at **96×120**; gameplay previews display at **160×120**,
with genre, publisher and release date below. Long game names scroll when selected.

Hold X on a cover to show it full-screen. Hold X on a playing preview to
expand it to 320×240 with its audio and playback position preserved. Any button
returns to the library; a tap on X there switches cover/video. Covers prepared by the desktop app or the
organizer include a 192×240 version, `NAME-large.3dc` beside `NAME.3dc`, for
this view; without one the menu doubles the 96×120 cover. Rebuild media quality
in the desktop app adds large covers to an existing library from its original
artwork.

Video previews are **160×120 RGB555 loops at 30 fps with optional game audio**,
fitted without stretching their aspect ratio. The full-screen view (X) doubles
them. The PC converts MP4/AVI/MKV/MOV/WebM sources; the console
plays the prepared `.3dv` file from RAM.

New previews (`OM3DVID5`) are **up to 15 seconds at 30 fps with source audio**,
in a **1,400 KiB payload budget**. Shorter sources stay shorter, and `--duration`
can request a shorter clip. The encoder chooses quality so each clip fills its budget;
`--budget-kib` changes it. On the console the clip is stored in video RAM first,
then main RAM, and is shortened when the console does not have enough
free memory. Decoder state, frame buffers and allocation guards need memory
beyond the payload budget. Loading time depends on storage, firmware and
folder navigation.

The format is a block codec written for the 3DO: each 4×4 block is kept, copied
from nearby in the previous frame, filled with one colour, or drawn from a
two-colour gradient, with a small cache of recent colours. The console decodes
it with a hand-written ARM60 routine. The encoder counts the decoder's cycles
and aims to keep each decode within an estimated 20 ms of the 33 ms
available per frame, giving up detail on busy frames to leave time for drawing.
This is a cycle-model target; real playback also depends on cel drawing and
other console work. When EmuMovies offers a higher-quality video
(`Video_MP4_HI_QUAL` or `Video_MP4_HD`), it is scaled down as the source,
which looks cleaner than the 320×240 download.

The encoder is a native library shared by `prepare-preview.py` and the
desktop app. In a source checkout, `prepare-preview.py` builds it on first use
when a C compiler is available. The release ZIP includes native libraries for
Windows and Linux x64; other systems can build one with
`python3 tools/build-preview-encoder.py` from a source checkout. Conversion time
depends on the PC and clip; the desktop app converts several clips at once.

The menu plays only `OM3DVID5` previews; clips made for earlier menu versions
show as failed previews until reconverted from the original videos. In the
desktop app, use **Covers & previews → Rebuild media quality** to reconvert
recorded originals. Old media files are retained, unavailable originals are
skipped, and the update has an undo journal.

Audio is signed 8-bit mono PCM at 11,025 Hz, with modest headroom and short
endpoint fades. Playback requires no USB reads. Audio and video restart
together each loop; changing selection stops both. Allocation or audio
initialization failures leave the menu usable; audio failure allows silent
video playback.

```sh
./tools/dev-container preview artwork/road-rash.mp4 'iso/previews/Road Rash.3dv'
# Explicit length and starting point:
./tools/dev-container preview artwork/road-rash.mp4 'iso/previews/Road Rash.3dv' --start 10 --duration 10
```

Copy the generated `.3dv` into `.ohmy3do/previews` on USB, then run the indexer
against that drive. With Python 3.9+, FFmpeg and ffprobe installed locally:

```powershell
py prepare-preview.py input.mp4 "M:/.ohmy3do/previews/Road Rash.3dv"
py index-media.py M:/
```

Use `--silent` to omit audio.

**Video previews** are enabled by default for new setups. Toggle them in
Tools → Settings, save, and cold boot (or set `video_previews=0/1` under
`[artwork]` in the INI). **Preview sound**
controls audio separately and saves as `preview_sound=0/1` under `[audio]`.
Existing explicit preview settings are preserved. There is no background-music setting;
`menu_music` is an unknown key and makes an INI invalid. Music files are not
read or deleted.

### Importing EmuMovies clips

Download the **3DO Video Snaps** to a separate PC folder using
[EmuMovies Sync](https://emumovies.com/faq/4-questions-about-emumovies-sync)
with a supporting membership, or extract a downloaded 3DO video pack there.
Keep that folder limited to 3DO clips and keep the original game-based filenames.
MP4, AVI, MKV, MOV, and WebM files are supported. Subfolders are searched.

Install FFmpeg on the PC and make sure `ffmpeg` and `ffprobe` are available on PATH. Scan the
drive, review the HTML plan, then apply it with the clip folder:

```powershell
py organize-library.py scan M:/ --quick --plan library-plan.json
py organize-library.py apply M:/ --plan library-plan.json --accept-name-matches --preview-dir "C:/EmuMovies/3DO"
```

The organizer prefers exact catalog/source names, then a unique normalized
game-title match. Matching regions are preferred; if none are available, a
unique clip from another region can be used. Disc labels and demo/full-game
labels must still agree. A generic clip without a disc label can serve all
discs of a game; duplicate clips or uncertain
matches are reported and skipped. It does not infer the edition of an ISO from
the clip. Rename or remove duplicate source clips before running a new scan and
apply if a match needs attention.

Imports use up to 15 seconds at 30 fps with source audio, shortening further to fit RAM.
Add `--preview-start 5`, `--preview-duration 10`, or `--preview-silent` as needed.
Clips are converted before USB changes, indexed automatically, and covered by
the organizer's undo journal. Existing previews are retained by default.
To upgrade existing silent clips, add **`--replace-previews`**:

```powershell
py organize-library.py apply M:/ --plan library-plan.json --accept-name-matches --preview-dir "D:/EmuMovies/Panasonic_3DO/Video_MP4" --replace-previews
```

This selects newly converted previews without deleting old media files or PC
originals. Explicit preview associations take precedence over older same-name
files during reindexing. Conversion failures are reported without blocking
other games. The database records each imported clip's filename and relative
source filename. Install the new `boot.iso` once; subsequent preview changes
require no ISO rebuild. Review a fresh plan before applying to reorganize files.

The tool does not log into EmuMovies or store account credentials. Direct API
integration requires separate developer access from
[EmuMovies](https://emumovies.com/forums/topic/58984-new-2023-emumovies-api-available-to-developers/).
Gameplay video files are not included in this repository or its releases.
Documentation screenshots include captured frames from the user's local clips.

Uncached covers start loading after 700 ms without a selection change;
previews wait 1.6 seconds. A whole clip is loaded into RAM before playback, so
looping needs no USB reads. Its packets go into video RAM first, then DRAM, keeping the
longest prefix that fits when free memory is low. Turning preview sound off also
skips allocating PCM. Longer clips may take longer to load and can be cancelled. During loading,
a D-pad or face-button press cancels; L+R cancels and opens Tools. A single
shoulder leaves time to complete L+R. Other menu actions stop playback; holding
X expands it without restarting it. Selecting another game
releases the clip; a stopped clip waits for a new selection before retrying.
The menu verifies the payload checksum, footer and frame table before
starting. Frames decode as they play; a bad packet stops the preview and
restores the static cover.
Missing or cancelled previews leave the static cover available. Actual
USB loading time depends on the firmware and folder navigation. The diagnostics
export includes total load time, read/CRC time, checks/first-frame time,
folder-navigation time, audio/display setup time, payload size and command count
for the last attempt. `preview_result=-3` means a later frame failed to decode,
`-4` means an allocation failed, and `-5` means controller input cancelled the
load. `-1` means a file/read check failed. Allocation and file failures also show
a brief message in the library. Exports include free DRAM and the largest
reported free block immediately before loading.
The binary layouts are documented in [docs/media-format.md](media-format.md).

For faster cover reads, convert artwork to the compact `.3dc` format:

```sh
./tools/dev-container cover 'Games/Road Rash.iso' artwork/road-rash.jpg
```

That exports a 96×120 prepared `.3dc` plus its index into `iso/covers`; see
[assets/covers/README.md](../assets/covers/README.md). Copy those files to USB.
`tools/dev-container media-index USBROOT` also runs the indexer for a directory
inside the mounted project. Rebuilding the ISO exports the project's built-in
cover associations into `iso/covers`. Generate raw-media indices against the
actual USB drive after copying the media files.

Media access temporarily visits the selected USB root and restores the original
folder using fresh listings and the compatibility workaround. A failed restore
blocks browsing. Cold boot or refresh at the USB root reloads the media and
metadata indices.
Holding X at startup bypasses USB media access. The project logo/loading screen
and offline demo covers stay embedded in the ISO.

[Project overview](../README.md)
