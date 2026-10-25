# Covers and video previews

Covers, gameplay clips and descriptions are optional. Prepare them in the
[desktop app](../desktop/README.md), then copy them to the console drive.
No ISO rebuild is needed.

## Prepare media

1. Choose a local media folder in **Artwork & previews**.
2. Use **Prepare missing media**, or select games in Library and prepare media
   for just those titles.
3. Review any unmatched games and assign a cover or video manually if needed.
4. Copy the games to the drive. For games already there, switch the view to
   **USB / SD drive** and choose **Update media & information…**.

The app can download covers and game information. For video sources, use your
own clips or download 3DO Video Snaps from EmuMovies with a supporting membership.
The app does not download gameplay videos or sign into EmuMovies.
Keep source videos and artwork on the PC so media can be rebuilt later.

Video conversion needs FFmpeg and ffprobe; the desktop app offers to download
them. MP4, AVI, MKV, MOV and WebM sources are supported.

## View media on the console

Pause on a game to load its cover and preview. Loading waits until you stop
moving so browsing stays responsive.

- **Tap X:** switch cover/video.
- **Hold X:** expand the current cover or playing preview.
- **Any button in full-screen view:** return to the game list.

Enable **Show covers**, **Video previews** and **Preview sound** in **Tools →
Settings**. New setups have previews enabled by default. Save changes and cold
boot where prompted.

![Video preview playing in the menu](images/gameplay-preview.png)

## Quality and loading

Prepared previews play at 160×120 and 30 fps, with optional audio, for up to
15 seconds. Full-screen viewing enlarges the same clip to 320×240, so it will
look softer than the original video. Higher-quality source clips can improve
conversion, but do not increase the playback resolution.

The desktop app prepares larger covers for full-screen viewing. Use **Rebuild
media quality** to recreate covers and previews from available originals.
Missing originals are skipped; existing media is retained.

A preview loads before playback, so the first loop may take time to start.
Moving to another game cancels loading. Press **L+R** to cancel and open Tools.
A cancelled or missing preview leaves the cover available.

After manually replacing media, [rebuild the drive indexes](media-tools.md#covers-and-indexes)
and restart the console, or press X at the drive root to refresh. For failed loads,
see [troubleshooting](troubleshooting.md).

## Manual setup and technical references

Manual PNG covers and prepared `.3dv` clips belong under `.ohmy3do/covers/` and
`.ohmy3do/previews/`. They need matching game names and updated indexes.

See [manual media preparation](media-tools.md) for Python commands, EmuMovies
imports and file layout. [Media formats](media-format.md) documents the binary
formats and size limits; [decoder timing](emulator-and-bench.md) covers performance
measurement.

[Project overview](../README.md)
