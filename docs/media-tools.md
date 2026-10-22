# Manual media preparation

Use this reference if you prefer Python tools to the desktop app. For the
usual workflow, see the [media guide](media-guide.md). Commands assume the tools
from `media-tools.zip` have been extracted together. Replace drive letters and
paths with your own.

## Covers and indexes

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
prepared PNGs into `covers/` and regenerate the index.

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

## Convert a video

With Python 3.9+, FFmpeg and ffprobe installed:

```powershell
py prepare-preview.py input.mp4 "M:/.ohmy3do/previews/Road Rash.3dv"
py index-media.py M:/
```

Use `--start 10` to choose a starting point, `--duration 10` for a shorter clip,
or `--silent` to omit audio. Prepared clips are up to 15 seconds at 30 fps.

From a source checkout, Docker can run the conversion:

```sh
./tools/dev-container preview artwork/road-rash.mp4 'iso/previews/Road Rash.3dv'
```

## Importing EmuMovies clips

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
Documentation screenshots include captured gameplay clips.

## Prepared cover format

The desktop app and organizer prepare native `.3dc` covers, including larger
versions for full-screen viewing. To prepare one from a source checkout:

```sh
./tools/dev-container cover 'Games/Road Rash.iso' artwork/road-rash.jpg
```

See [cover assets](../assets/covers/README.md) for the output layout and
[media formats](media-format.md) for binary layouts, codecs and size limits.
