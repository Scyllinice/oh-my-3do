Oh My 3DO media tools

PNG covers need Python 3 only. Video conversion also needs FFmpeg on PATH.
The tools do not rebuild boot.iso or download artwork/video.

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
Enable Video previews in Tools > Configuration, save, then cold boot.
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
