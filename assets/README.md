# Menu logo

`logo-source.png` is the original transparent logo generated with the built-in
image generation tool (generate mode). `logo-menu.png` is the trimmed,
133 x 48 console-sized version; `logo-menu.cel` is the packed, uncoded 16-bit
3DO cel loaded once at startup. The source PNG retains its original alpha.
The console cel uses binary transparency rather than full alpha blending.

Final generation prompt:

> Use case: logo-brand. Create a finished raster logo asset for the homebrew
> 3DO optical drive menu project named exactly "OH MY 3DO". Horizontal logo
> lockup, readable when reduced to about 140 pixels wide on a 320x240 CRT menu.
> Bold compact geometric 1990s console lettering and a simple original
> dimensional optical-disc/orbit emblem. Restrained cyan/teal accent with warm
> ivory letters and a small amber accent, designed for a deep midnight navy
> menu background. Clean hard silhouettes, chunky strokes, strong spacing,
> subtle dimensional shading, no fine hairlines, no small text. Transparent
> background. Only the text OH MY 3DO, no slogan, no official manufacturer
> logo, no mockup or surrounding scene. The horizontal mark should occupy most
> of the image width with little empty padding.

To regenerate the console formats from the saved source, use ImageMagick on the
host, then the existing container's `3it` converter:

```sh
magick assets/logo-source.png -trim +repage -resize 148x48 assets/logo-menu.png
./tools/dev-container run 3it to-cel --bpp 16 --packed true \
  --transparent 0x00000000 --ccb-last set \
  --output-path /workspace/assets/logo-menu.cel /workspace/assets/logo-menu.png
```

Normal menu builds use the checked-in cel and do not require ImageMagick or
image generation. The build verifies that the packaged cel matches this asset.

## Loading screen

`loading-source.png` is new opaque 4:3 artwork generated with the built-in image
tool using `logo-source.png` as a branding reference. `loading-menu.png` is the
320 x 240 console version, with the composition reduced into a safe inset for
CRT overscan. `loading.banner` is the 16-bit RGB555 `APPSCRN` asset installed as
the boot disc's `BannerScreen`, replacing the toolkit's stock image. It is
displayed by the console's disc startup code before LaunchMe begins, without
adding an artificial loading delay in the menu.

The banner's binary header specifies height 240, width 320 and 153,600 pixel
bytes, matching the toolkit banner's header. The converter's info output labels
width/height in reverse and emits a spurious non-fullscreen warning; the binary
header was checked against Portfolio's `VideoImage` layout. The converted banner
was decoded back to PNG for visual inspection. The ISO build compares the entire
packaged banner payload to the saved asset and verifies its disc signature.

Final loading-screen prompt (built-in tool, reference-image workflow):

> Use case: ui-mockup. Asset type: finished full-screen startup/loading artwork
> for the OH MY 3DO homebrew optical-drive menu. Input image: reference branding
> only, the saved OH MY 3DO logo with optical disc/orbit emblem. Create a NEW
> 4:3 landscape splash composition matching this branding, suitable for reduction
> to 320x240 on a CRT. Deep midnight navy background, restrained luminous teal
> orbital arcs and a subtle dark blue horizon glow. A large centered OH MY 3DO
> ivory-and-teal dimensional logo with the optical-disc/orbit emblem, surrounded
> by generous negative space. Keep the entire logo inside the central 80 percent
> of the canvas width. A single smaller but clearly legible ivory caption below
> it reads exactly "LOADING..." in bold chunky simple uppercase console lettering.
> Keep all text and essential details within a 10 percent safe inset from every
> edge. Evoke premium early-1990s console startup artwork, restrained and polished,
> with broad clean shapes and strong contrast. No progress bar, no tiny text, no
> manufacturer marks, no additional words, no toolkit branding, no mockup device,
> no CRT frame. Full opaque background, no transparency. The artwork itself fills
> the image, with a 4:3 composition.

To regenerate the console files from the saved artwork:

```sh
magick assets/loading-source.png -resize 280x210 -background '#02081b' \
  -gravity center -extent 320x240 -alpha off assets/loading-menu.png
./tools/dev-container run 3it to-banner \
  --output-path /workspace/assets/loading.banner /workspace/assets/loading-menu.png
```

Both menu and demo builds use the saved banner, with no image generation or host
ImageMagick dependency during normal builds.

## Cover demonstration

`demo-cover.png` and `demo-cover.cel` reuse the existing loading artwork, fitted
into a 64 x 80 thumbnail. They demonstrate cover placement in the demo ISO and
host previews; they are not game box scans. No new image generation was used.
User-supplied game art is prepared and indexed separately in `assets/covers/`.

```sh
magick assets/loading-menu.png -thumbnail 64x80 -background '#081021' \
  -gravity center -extent 64x80 -alpha remove -alpha off assets/demo-cover.png
./tools/dev-container run 3it to-cel --bpp 16 --packed false \
  --ccb-last set --ccb-bgnd set -o /workspace/assets/demo-cover.cel \
  /workspace/assets/demo-cover.png
```

## Menu font and startup artwork

The menu font is generated by `tools/build-ui-font.py` from local DejaVu Sans
regular and bold TTFs. It stores 4-bit antialiased coverage; the renderer blends
the palette against each text surface and keeps glyphs at native pixel size.
The generated font is covered by `vendor/fonts/LICENSE.txt`.

`startup.cel` is an unpacked 16-bit CEL of `loading-menu.png`. It keeps the
loading artwork visible while startup configuration and the saved folder load;
it is freed before the browser appears. To regenerate it:

```sh
./tools/dev-container run 3it to-cel --bpp 16 --packed false --ccb-last set --ccb-bgnd set -o /workspace/assets/startup.cel /workspace/assets/loading-menu.png
```
