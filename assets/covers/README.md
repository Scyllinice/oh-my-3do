# Optional game covers

Add an image from inside the project with:

```sh
./tools/dev-container cover 'Games/Road Rash.iso' artwork/road-rash.jpg
```

Copy the resulting `iso/covers/` folder to the USB root. The updated menu reads
its `index.3di` and `.3dc` thumbnails without rebuilding `boot.iso`.

`index.json` maps complete USB-relative game paths to intermediate CEL basenames.
Keep the real extension in the key even though the menu hides it. The tool fits
the image into a 64 x 80 thumbnail and updates an existing case-insensitive path
association when rerun. Images are supplied by the user, not fetched from a site.

Delete an association from the JSON, run `./tools/dev-container covers`, and copy
the exported folder again to remove it. Unreferenced files are ignored. Cold boot
or refresh the USB root to reload the index. An empty object disables the artwork
sidebar. The demo
uses `assets/demo-cover.cel`, a reduced copy of the existing project loading
artwork, as demonstration art; it does not alter this catalogue.
