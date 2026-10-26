# om3dvid

A video codec for 3DO programs: full-screen 320×240 video at 30 fps, with
optional sound, streamed from slow storage while it plays and decoded by the
console's ARM60 within a frame's time. It was made for the video previews in
Oh My 3DO's game menu, and is meant for any 3DO menu, game or demo that wants
video.

The format is OM3DVID7; [FORMAT.md](FORMAT.md) specifies it. This folder has
everything to make, check, decode and play it:

- on the **3DO**: a decoder (C, with ARM assembly for speed), a streaming
  player, and helpers for Portfolio memory, threads and sound;
- on a **PC**: an encoder and a reader that decodes clips to RGB, as C, a
  Python module and a Rust crate.

Sound is optional throughout. A clip may be silent, and a program chooses
whether and how to play the sound a clip carries.

MIT licensed (see [LICENSE](LICENSE)).

## What is here

| File | Runs on | What it is |
| --- | --- | --- |
| `include/om3dvid7.h`, `src/om3dvid7_decoder.c` | both (C89) | The format's constants, header and chunk checks, and the decoder |
| `src/om3dvid7_decoder_arm.s` | 3DO | The decoder's inner loops in ARM assembly, used automatically when the decoder is compiled with the 3DO SDK's compiler |
| `include/om3dvid7_player.h`, `src/om3dvid7_player.c` | both (C89) | The streaming player: reading chunks into slots, playing frames and sound in step, looping |
| `include/om3dvid7_portfolio.h`, `src/om3dvid7_portfolio.c` | 3DO | Player helpers for Portfolio: memory of each kind, starting a reader thread, sleeping |
| `include/om3dvid7_sound.h`, `src/om3dvid7_sound.c` | 3DO | Playing 8-bit sound through the audio folio, including the player's looping sound ring |
| `include/om3dvid7_encoder.h`, `src/om3dvid7_encoder.c` | PC (C99) | The encoder |
| `include/om3dvid7_reader.h`, `src/om3dvid7_reader.c` | PC (C99) | Decodes a whole clip in memory to RGB frames and gives its sound |
| `python/om3dvid7.py` | PC | Python: encode, check, decode (standard library only) |
| `rust/` | PC | Rust: the `om3dvid7` crate (encode, check, decode) |
| `tests/` | | Tests for all of the above |

The encoder and reader use no C runtime: all memory is the caller's. That
lets them build into a Windows DLL that depends on nothing, and lets separate
encoders or readers run on separate threads.

## Playing video on the 3DO

Compile `om3dvid7_decoder.c`, `om3dvid7_decoder_arm.s` and
`om3dvid7_player.c` into your program, with `om3dvid7_portfolio.c` and
`om3dvid7_sound.c` if you want their help (the 3DO SDK's project makefile
assembles `.s` files in `src/` along with the C). The player knows nothing
about where the clip comes from or how your program runs: you give it an
`Om3dvid7PlayerHost` of callbacks.

```c
#include "om3dvid7_player.h"
#include "om3dvid7_portfolio.h"
#include "om3dvid7_sound.h"

static Om3dvid7Player player;
static Om3dvid7Sound sound;

/* Your program's file access: open the clip at its start (the player calls
 * this again for each loop), read its next 2048 bytes, close it. Each
 * returns 0, or -1 on failure. */
static int clip_open(void *context);
static int clip_read_block(void *context, unsigned char *block);
static int clip_close(void *context);

int start_video(const unsigned char *header_block /* the file's first 2048 bytes */)
{
    Om3dvid7PlayerHost host = {0};
    host.context = 0; /* your own data, passed to every callback */
    host.open = clip_open;
    host.read_block = clip_read_block;
    host.close = clip_close;
    host.allocate = om3dvid7_portfolio_allocate; /* video memory for slots, and so on */
    host.release = om3dvid7_portfolio_release;
    /* Sound: leave these out (or pass 0 below) for silent playback. */
    host.sound_context = &sound;
    host.sound_start = om3dvid7_sound_player_start;
    host.sound_pause = om3dvid7_sound_player_pause;
    host.sound_stop = om3dvid7_sound_player_stop;
    return om3dvid7_player_open(&player, &host, header_block, 1);
}
```

Then, on every pass of your main loop, with `now` in milliseconds:

```c
if (!player.playing)
    om3dvid7_player_play(&player, now); /* starts the clock and the sound */
result = om3dvid7_player_decode(&player, om3dvid7_player_due(&player, now),
                                OM3DVID7_SCREEN, screen_bitmap);
om3dvid7_player_update(&player, now, result == OM3DVID7_PLAYER_NOT_READY);
/* result < 0: the clip is damaged; close it */
```

`OM3DVID7_SCREEN` decodes into a 320×240 screen bitmap; `OM3DVID7_TV` decodes
the 160×120 picture of 2×2 averages into `player.thumb`, cel memory a menu
can show small. Switching between them redraws the new picture once.
`om3dvid7_player_close` stops everything and frees the memory.

Without a reader thread (the setup above), the player reads chunks inside
`om3dvid7_player_decode` whenever a slot is free. That is simplest, but
decoding waits for every read. To overlap them, give the host `start_reader`
and `stop_reader` callbacks: `start_reader` starts a thread (for example with
`om3dvid7_portfolio_start_thread`) that calls `om3dvid7_player_read` in a loop,
sleeping a few milliseconds whenever it answers
`OM3DVID7_PLAYER_READ_LATER`, until it answers `OM3DVID7_PLAYER_READ_STOP`;
then it calls `om3dvid7_player_close_file` and, last,
`om3dvid7_player_reader_exit`. Only a thread that drives hardware directly
(Oh My 3DO's reads the USB drive over the XBUS) needs to be privileged. The
optional `pace` callback tells the thread when the chunk it is reading is not
needed for a while, so it can drop below your program's priority.
`include/om3dvid7_player.h` documents every callback, and Oh My 3DO's
`src/preview_stream.c` is a complete host.

Memory: `om3dvid7_player_bytes` gives the total for a clip: about 1.1 MB with
the default four 160 KiB slots. The slots, more than half of it, go in video
memory when there is room.

## Making and reading clips on a PC

The encoder takes 320×240 frames of 8-bit RGB in rows, and optionally signed
8-bit mono sound at 11025 Hz, `frames × 11025 / 30` samples (rounded down).
Scale and pad your source to 320×240 first; FFmpeg's
`scale=320:240:force_original_aspect_ratio=decrease,pad=320:240:(ow-iw)/2:(oh-ih)/2`
does it.

### Python

`python/om3dvid7.py` needs only the standard library, and the shared library
(`make` below) beside it, in `$OM3DVID7_LIBRARY`, or passed in.

```python
import om3dvid7

clip = om3dvid7.encode(frames, sound)        # sound may be None
open("clip.3dv", "wb").write(clip.data)
om3dvid7.check(clip.data)                    # ValueError if a player would refuse it

reader = om3dvid7.Reader(clip.data)
for rgb in reader:                           # every frame once, 320x240 RGB
    ...
number, small = reader.next(om3dvid7.TV)     # the 160x120 TV picture
sound = reader.audio()                       # b"" for a silent clip
```

`om3dvid7.Encoder` encodes a frame at a time; `read_header` and `check` are
pure Python and need no shared library.

### Rust

The `om3dvid7` crate in `rust/` compiles the C in with the `cc` crate.

```rust
let clip = om3dvid7::encode(&frames, Some(&sound), &om3dvid7::EncoderSettings::default())?;
om3dvid7::check(&clip.data)?;
let mut reader = om3dvid7::Reader::new(&clip.data)?;
let (number, rgb) = reader.next_frame(om3dvid7::Picture::Screen)?;
```

### C

`om3dvid7_encoder.h` and `om3dvid7_reader.h` describe the C interfaces the
bindings use; both work in caller-provided workspaces. Each has an `_abi`
function returning its interface version, so a program loading the shared
library can check it.

## Encoder settings

`om3dvid7_encoder_default_settings` (and `EncoderSettings` in the bindings)
gives the settings Oh My 3DO uses: about 200 KB/s, 10-frame chunks, 640 KiB
of slots and 160 KiB chunks, and frames the ARM60 decodes in under 230,000
clocks. A program that reads faster, or has more memory for slots, can raise
`rate`, `read_rate` or `slot_bytes`; one that decodes alongside heavier work
can lower `cycle_cap`. [FORMAT.md](FORMAT.md#encoding) explains what each
constrains.

## Building and testing

```
make            # build/om3dvid7.so (.dylib on macOS): encoder, decoder and reader
make test       # the C tests, then the Python tests
make test-rust  # the Rust crate's tests (needs cargo)
```

Nothing outside this folder is needed. `tests/test_decoder_arm.py`, which
checks the ARM assembly against the C decoder under an emulator, also needs
the 3DO SDK's assembler, the Unicorn emulator and Oh My 3DO's ARM60 clock
model, so it runs only in that repository.

A Windows DLL that needs no C runtime builds with clang and lld-link; Oh My
3DO's `tools/build-preview-encoder.py --windows` shows the flags.
