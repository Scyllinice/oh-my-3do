Oh My 3DO media tools

Indexing existing PNG covers needs Python 3 only. Resizing/conversion needs
Python 3.9+ and Pillow. Video conversion also needs FFmpeg on PATH.
The tools do not rebuild boot.iso or download artwork, music or video.

PREPARE COVERS

Install the image library once (Windows):
  py -m pip install Pillow

Resize one image and give it the game's basename:
  py resize-covers.py "Downloads/cover.jpg" "E:/covers/Road Rash.png"

Or convert a whole folder, keeping each image's basename:
  py resize-covers.py "Downloads/artwork" "E:/covers"

The output is a 64x80 RGB PNG. Images are fitted without cropping, with dark
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

PNGs: up to 1024x1024 pixels and 512 KiB. Smaller files load faster.
Regenerate the index after adding, removing or renaming games/media.

Windows example (change E:/ to the USB drive):
  py index-media.py E:/

Linux/macOS example:
  python3 index-media.py /path/to/USB

The indexer changes only index.3di files in covers/ and previews/.
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
