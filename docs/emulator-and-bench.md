# Testing without a console: the emulator and the ARM60 bench

Console trips are slow for the user, so two local tools stand in for most of
them. Neither replaces a console test. Each section says what the tool gets
wrong.

| Question | Tool |
| --- | --- |
| Does the menu boot, navigate, load and play media, export diagnostics? | Emulator (`tools/emulator/`) |
| What does a screen look like after a change? | Emulator screenshots |
| How long does the console's preview decoder take per frame? Is the ARM code bit-exact? | ARM60 bench (`tools/arm60-bench/`) |
| How long does the cel engine take to draw? | Neither. Only the console measures it (see below). |

## The emulator

Opera (the libretro 3DO core) runs the real `iso/boot.iso` headless. Opera
already emulates the FCare ODE (the USB drive the menu reads); a small patch
makes it behave like the real firmware:

- TOC listings continue after the given ID;
- file opens create missing files, as `FA_OPEN_ALWAYS` does;
- reads report the requested length even at end of file;
- `OPERA_FCARE_TOC_BUG=1` reproduces the firmware's same-folder-ID bug.
- `OPERA_FCARE_LOG_QUEUE=1` prints each disc the menu queues (`fcare queue N: PATH`)
  to the run's `log.txt`, to check multi-disc queues without playing to a disc swap.

### Setup (once)

```
tools/emulator/setup.sh
```

This clones Opera at commit 684926f into `build/emu/opera-libretro` and applies
`tools/emulator/opera-fcare.patch`. It builds the core and the headless runner,
`tools/emulator/runner.c` → `build/emu/runner`. It links the BIOS from
`BIOS=…`, by default `/mnt/d/3DO/panafz1.bin`. That is the user's own FZ-1
ROM: never copy it into the repository.

### A test run

```
tools/dev-container menu                     # build iso/boot.iso
tools/emulator/make-usb.sh                   # build/emu/usb: the drive
tools/emulator/run.sh tools/emulator/scripts/roadrash-preview.txt rr
```

- `make-usb.sh` links the user's library (`LIBRARY=/mnt/d/3DO`) read-only and
  copies everything the menu writes.
- Clips in `CLIPS=` (default `build/console-test/previews`) replace library
  previews of the same file name.
- It copies the library's settings, changing `screen_margin=0` to 8; an existing
  nonzero margin is preserved. Use a valid current INI in the test library.
- Media goes beside each disc, the layout desktop USB preparation makes. The
  menu streams OM3DVID7 previews only from this layout. `BESIDE=0` keeps media
  in the root `.ohmy3do/covers` and `previews` instead, where previews will not
  stream.

`run.sh` writes screenshots to `build/emu/shots/NAME/*.ppm`, and everything
the console played to `build/emu/shots/NAME/audio.wav` (16-bit stereo, 44100 Hz). Diagnostics that a
script exports appear in `build/emu/usb/.ohmy3do/diagnostics.txt`, in the same
format the user sends from the console. Emulation is deterministic: the same
BIOS, drive contents, build and script give the same frames, so two builds can
be compared frame for frame when those inputs are controlled.

`shot` writes the actual emulator video callback as PPM, rather than a host UI
simulation. Convert PPM to PNG losslessly for documentation. The release's
captures and their settings are shown in the [screenshot gallery](screenshots.md).

### Scripts

One command per line; `#` starts a comment. Frames are 1/60 s.

```
wait N               run N frames
press BUTTON [N]     hold N frames (default 4), then release for 4
hold BUTTON N        hold N frames
chord B1+B2 [N]      hold several together (L+R opens Tools)
shot NAME            save NAME.ppm
burst NAME N STEP    N shots, STEP frames apart
```

Buttons: `up down left right a b c p x l r`.

- `scripts/roadrash-preview.txt` goes to Games → USA → R → Road Rash. It plays
  the preview in the TV picture and in full screen (X held), then exports
  diagnostics.
- `scripts/roadrash-steady.txt` plays for about 25 s without input, then
  exports. Use it for steady-state skipped frames.
- `scripts/roadrash-fullscreen.txt` takes full-screen shots at fixed times.
- `scripts/boot.txt` takes boot screenshots.

### Pitfalls

- **Presses get dropped** when they come too close together. Use `press X 6`
  followed by `wait 24` or more, and check positions with `shot` before relying
  on a long sequence.
- **Navigation scripts assume a particular library layout.** Different regions,
  folder counts or `hide_adult` settings change the required number of presses.
  Check the selected row in a screenshot and adjust the script for the test drive.
- **The menu reopens where it last was.** `run.sh` deletes `last-position.txt`
  and `recent.txt` first; without that, scripts start in random places and may
  launch games.
- **Road Rash's clip fades in from black over its first second.** A shot that
  lands at the clip's loop point looks black. That is the video, not a drawing
  bug.

### What the emulator gets wrong

- **The CPU runs about 1.3× faster than the console.** Decode and load times
  are optimistic by that much.
- **ODE folder navigation is instant**; on the console it takes hundreds of
  milliseconds.
- **The cel engine costs nothing.** On the console, drawing is expensive. A
  320×240 source picture took about 16 ms of cel-engine time per frame,
  wherever it was stored and however it was scaled. With 160×120 clips, a whole
  playback redraw of the TV picture measured about 7 ms. Any change that draws more per frame must be
  measured on the console with the `preview_video_ms`/`preview_video_draws`
  and `preview_light_draws`/`preview_draw_ms` diagnostics. This is why the
  320×240 30 fps experiment looked fine in the emulator and skipped a third of
  its frames on the console.

## The ARM60 bench

It answers how many clocks the console's preview decoder
(`src/preview_v5_arm.s`) spends on each frame of a clip, and whether its output
matches the C decoder (`src/preview_v5.c`).

```
tools/arm60-bench/run.sh path/to/clip.3dv [FRAMES] [--profile]
```

It runs in the development container, which has armasm/armcc/armlink and
Unicorn 2.1.1:

1. builds `bench.c` with the real assembly decoder;
2. runs it under Unicorn, big-endian;
3. charges every instruction by the ARM60 datasheet's S/N/I cycles, with
   N = 2 clocks;
4. decodes the same clip with the C decoder and compares every frame.

Example output for the Road Rash test clip:

```
mean 112k clocks (26.8% of a frame), worst 200k (48.0%, frame 379)
console estimate: 11.4 ms mean, 20.5 ms worst, of 33.3 ms per frame
output matches the C decoder
```

The console estimate multiplies by 1.28: the console measured that much more
than the count, from video DMA and bus contention. It has held well: for this
clip the console reported 11.8 ms per frame. `--profile` splits the clocks by
symbol, though most of the decoder is one assembly routine.

The encoder (`tools/preview-encoder.c`) uses a cost model fitted to these
counts (`block_cycles`). It caps each frame at `CYCLE_CAP` (200,000 clocks,
about 20 ms on the console), leaving the rest of the 33 ms for drawing. After
changing the decoder, re-measure with the bench and refit those constants.

### What the bench gets wrong

- **CPU only:** no cel-engine drawing, no USB reads, no menu work.
- **Contiguous packets:** the menu splits clips into a few memory chunks, but
  each packet is contiguous, so the bench matches that.

## Menu drawing checks

`python3 tools/preview-menu.py` runs the actual drawing routines with host
stubs. It checks incremental browser rendering against full redraws across
26 margin/font combinations, plus full-screen covers/video, contextual Help
and the Tools home screen across 65 mode/margin combinations. It also exercises
selection changes, messages changed in the same buffer, unsaved settings,
Help round trips, frame changes and audio loops.

Static screens prepare both framebuffers once, then continue polling input
without repainting. Full-screen video still advances its decoder and audio
clock on every poll, but presents only when its decoded image changes. Inline
video also skips duplicate presentations once both buffers have the current
caption; scrolling text, notices and selection changes still redraw.

The host fixture's drawing-call counts for 1,000 unchanged polls are:

| Screen | Before | After |
| --- | ---: | ---: |
| Full-screen cover | 78,000 | 156 |
| Contextual Help | 84,000 | 168 |
| Tools home | 304,000 | 608 |
| Unchanged full-screen video frame | 1,002 | 4 |
| Unchanged inline video frame | 72,370 | 1,654 |

These are drawing-call counts, not console frame-rate measurements. The normal
browser already performs zero draws while idle. Live screens such as
Diagnostics continue refreshing. Measure cel-engine time on the console;
the emulator does not model that cost.

## The decoder's own tests

`tests/test_preview_asm.py` (part of `make test-sdk`) checks the assembly
against the C decoder on random valid packets, damaged packets and
out-of-frame motion vectors. It also checks that nothing is written outside
the frame or read more than 1 KiB past a packet. Run
`tools/dev-container run make test-sdk` after any change to either decoder.
