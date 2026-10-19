# Menu logo

`logo-source.png` is the approved ivory/lime wordmark with a tilted disc and
orange signal rays. The transparent source and `app-icon-source.png` were
finished with the built-in image generation tool in edit mode from
`branding/approved-concept.png`. The [saved prompts](branding/prompts.md)
record both edits. Source PNGs retain their alpha.

`logo-menu.png` is the trimmed 109 x 34 console export, drawn at native size
in the browser and half size in Tools. `logo-menu.cel` is its packed, uncoded
16-bit CEL, loaded once at startup; it uses binary transparency. The desktop
uses `logo-desktop.png` (766 x 240), with linear filtering for its header.

`app-icon.png` is a 256 x 256 window icon. `app-icon.ico` contains 16, 24, 32,
48, 64, 128 and 256 pixel sizes. `app-icon.rc` and its compiled `app-icon.res`
embed those sizes into the Windows executable so Explorer and the taskbar use
the same disc mark. The precompiled resource is checked in, so normal native
and cross builds need no additional resource compiler.

Original concept direction (the production edit prompts are linked above):

> Use case: logo-brand. Create a finished raster logo asset for the homebrew
> 3DO optical drive menu project named exactly "OH MY 3DO". Horizontal logo
> lockup, readable when reduced to about 140 pixels wide on a 320x240 CRT menu.
> Bold compact geometric 1990s console lettering and a simple original
> tilted optical-disc emblem. Restrained lime accent with warm
> ivory letters and a small orange signal accent, designed for a deep midnight navy
> menu background. Clean hard silhouettes, chunky strokes, strong spacing,
> subtle dimensional shading, no fine hairlines, no small text. Transparent
> background. Only the text OH MY 3DO, no slogan, no official manufacturer
> logo, no mockup or surrounding scene. The horizontal mark should occupy most
> of the image width with little empty padding.

To regenerate the exports from the saved sources, use ImageMagick and the
existing SDK and Windows cross-build containers:

```sh
python3 tools/prepare-branding.py --console --windows-resource
```

Normal menu builds use the checked-in cel and do not require ImageMagick or
image generation. The build verifies that the packaged cel matches this asset.
Omit the two flags to export only PNGs and the ICO. Startup exports replace
only the logo panel and preserve the exact black mask used for live static;
dark logo pixels are raised above RGB555 black to keep them opaque.

## Loading screen

The Gecko TV startup screen ("Tuning in") combines the generated `logo-source.png`
with shapes, bitmap text and TV static: the logo on a black caption box, a green
channel display and a tuning bar. The screen layout was composed without an
additional image-generation step. It comes in two versions:

- `loading-menu.png` has the static and a quarter-filled bar baked in. It
  becomes `loading.banner`, the 16-bit RGB555 `APPSCRN` asset installed as the
  boot disc's `BannerScreen`, which the console shows before LaunchMe begins.
- `startup-menu.png` leaves the static area pure black (0) and has no bar; every
  other pixel is non-zero. It becomes `startup.cel`, which the menu shows while
  it connects and reads the USB drive. The cel's BGND flag is clear, so the
  black area is transparent: the menu draws moving static behind it and fills
  the tuning bar a quarter per startup stage.

The banner's binary header specifies height 240, width 320 and 153,600 pixel
bytes, matching the toolkit banner's header. The converter's info output labels
width/height in reverse and emits a spurious non-fullscreen warning; the binary
header was checked against Portfolio's `VideoImage` layout. The ISO build
compares the entire packaged banner payload to the saved asset and verifies its
disc signature.

To regenerate the console files from the two PNGs:

```sh
./tools/dev-container run 3it to-banner \
  --output-path /workspace/assets/loading.banner /workspace/assets/loading-menu.png
./tools/dev-container run 3it to-cel --bpp 16 --packed false --ccb-last set \
  --ccb-bgnd unset -o /workspace/assets/startup.cel /workspace/assets/startup-menu.png
```

Both menu and demo builds use the saved banner, with no image generation or host
ImageMagick dependency during normal builds.

## Cover demonstration

`demo-cover.png` and `demo-cover.cel` reuse the earlier teal loading artwork, fitted
into a 64 x 80 thumbnail. They demonstrate cover placement in the demo ISO and
host previews; they are not game box scans. No new image generation was used.
User-supplied game art is prepared and indexed separately in `assets/covers/`.

The README and [screenshot gallery](../docs/screenshots.md) use actual emulator
captures from the release ISO, including game artwork and footage from the
user's local library. They are separate from this synthetic demo artwork.

To rebuild the CEL from the saved demo PNG:

```sh
./tools/dev-container run 3it to-cel --bpp 16 --packed false \
  --ccb-last set --ccb-bgnd set -o /workspace/assets/demo-cover.cel \
  /workspace/assets/demo-cover.png
```

## Menu font and startup artwork

The menu font is Ark Pixel, a bitmap font drawn pixel by pixel at 10 and 12 px.
`tools/build-ui-font.py ARK_10PX_TTF ARK_12PX_TTF` converts the proportional
Latin TTFs from the [Ark Pixel releases](https://github.com/TakWolf/ark-pixel-font/releases)
(2026.10.16) into `src/ui_font_data.h`: 12 px for normal and large text, 10 px
for small text, and the 10 px design doubled for headings. Every pixel is fully
on or off. Rasterizing an outline font to 1-bit looked blocky on a real console
and antialiasing it looked soft; a hand-drawn bitmap font avoids both. Glyphs
keep their native pixel size. The generated font is covered by
`vendor/fonts/ark-pixel-OFL.txt` (SIL Open Font License 1.1).

`startup.cel` is an unpacked 16-bit CEL of `startup-menu.png` (see Loading
screen). It stays visible while startup configuration and the saved folder load
and is freed before the browser appears.

## Channel-changing gecko

`channel-gecko-source.png` is a generated 4×4 sheet of the gecko in an orange
recliner with a remote. The [saved edit prompt](channel-gecko-prompt.txt) records
the approved pose and animation. `tools/prepare-channel-gecko.py` uses ImageMagick
to reduce it to sixteen 32×32 frames in `src/channel_gecko_data.h`: 8,192 bytes
of 4-bit pixels plus a shared 64-byte palette. Frames advance every 20 ms after
a selection change; idle browsing leaves the first frame visible. The header
is embedded in both menu and demo builds, so the animation needs no USB reads.

```sh
python3 tools/prepare-channel-gecko.py
```
