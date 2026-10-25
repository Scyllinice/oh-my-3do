# USB menu media formats

Integers in the binary layouts below are unsigned big-endian. The menu reads
these files using LOCODE E1/E3/E7/E5, with E6 buffer setup. It does not seek or
write media files.
Firmware may create an empty missing file even when opening it for a read.

Support paths such as `covers/index.3di` are relative to `.ohmy3do/` at the USB
root. Game-path keys inside indices remain USB-volume-relative.

## Association index

`covers/index.3di` and `previews/index.3di` share this layout:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `OM3DIDX1` |
| 8 | 4 | Entry count, at most 2048 |
| 12 | 4 | Payload bytes, at most 131072 |
| 16 | 4 | Standard CRC32 of payload |
| 20 | 4 | Media location: 0 = root media folders, 1 = beside each disc |

Each payload row is two 16-bit lengths followed by a NUL-terminated USB-relative
game path and media basename. Lengths include terminators. Rows sort by ASCII
case-insensitive path, with no duplicate paths. A media basename is at most 96
UTF-8 bytes, has no path separators/control characters, and ends in `.png`,
`.3dc` or `.3dv`. Covers accept PNG or prepared 3DC; previews accept 3DV.
`tools/index-media.py` builds root-folder associations without converting media.
Desktop USB preparation copies referenced media to a `.ohmy3do/` folder beside
each disc and writes location flag 1 in the root association indices. Game-path
keys remain relative to the USB volume; only media-file lookup changes.
Other flag values are rejected.

## PNG cover

PNGs are decoded directly, with chunk CRC, zlib completion, exact decoded-length
and IEND checks. The IEND CRC is verified with a poisoned read buffer to reject
stale EOF data. Limits are 1024×1024 pixels and 524288 bytes. The streaming
decoder retains a 32 KiB inflate dictionary, decoder state, a scanline buffer and
small scaling tables, rather than an entire source-size bitmap. Area
sampling fits covers into 96×120, with letterboxing and alpha compositing over
RGB `(8,16,33)`. Optional gamma and background-color metadata do not change this.

## Prepared cover

The library tools produce `.3dc` files directly in the console's RGB555 format:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `OM3DCOV2` |
| 8 | 2 | Width, 96 |
| 10 | 2 | Height, 120 |
| 12 | 4 | Compressed payload bytes, 1–32768 |
| 16 | 4 | CRC32 of compressed payload |
| 20 | 4 | Decoded bytes, 23040 |
| 24 | variable | One zlib stream, row-major big-endian RGB555 |
| after payload | 8 | `END3DC02` |

The decoder writes directly into the cover CEL. It checks the exact compressed
and decoded lengths, CRC and poisoned footer. No PNG scanline filters, scaling
or color callbacks run. The 96×120 display pixels match a prepared PNG after
RGB555 conversion.

### Full-screen cover

`NAME-large.3dc`, beside the indexed `NAME.png` or `NAME.3dc` (in `covers/`, or
in the `.ohmy3do/` beside a prepared drive's disc), is the same layout at 192×240:
width 192, height 240, decoded bytes 92160, and at most 93184 compressed bytes.
The menu reads it only when X opens the full-screen view, into a separate CEL it
frees on the next button. Indexers ignore `-large.3dc` names when matching
covers to games.

## Block-codec preview

`previews/Title.3dv`: 160×120 RGB555 at exactly 30 fps, up to 450 frames
(15 seconds), with optional audio. Converters size each clip to a budget,
1,400 KiB by default.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `OM3DVID5` |
| 8 | 2 | Width, 160 |
| 10 | 2 | Height, 120 |
| 12 | 4 | Payload bytes, at most 4194304 |
| 16 | 4 | Word checksum of the payload (below) |
| 20 | 2 | Frame count, 1–450 |
| 22 | 2 | Frames per second, always 30 |
| 24 | 4 | Video bytes: frame table plus packets, without audio or padding |
| 28 | 4 | Audio payload bytes, zero for silent clips |
| 32 | 4 | Audio sample rate, 11025 or zero when silent |
| 36 | 4 | Audio sample count, equal to audio bytes |
| 40 | payload | Frame table, audio, video packets, zero padding to a 4-byte boundary |
| after payload | 8 | `END3DV05` |

The frame table holds frame_count + 1 offsets into the video bytes, as if the
packets followed the table directly; the audio stored between them is not
counted. The payload length is a multiple of four. Its checksum runs over
big-endian 32-bit words: `a += word; b += a` from zero, modulo 2³², giving
`a XOR (b rotated by 16 bits)`.

Each packet updates the previous frame in place (black before the first) by
coding the 1,200 4×4 blocks in raster order. A packet is a series of op bytes,
`mode << 6 | (run − 1)`, each covering `run` (1–64) blocks:

| Mode | Name | Data |
| --- | --- | --- |
| 0 | SKIP | none: the blocks stay as they are |
| 1 | MOTION | one byte `(dx + 8) << 4 \| (dy + 8)` for the run of at most 32 blocks; each block copies the 4×4 block at that offset |
| 2 | SOLID | one colour for the run |
| 3 | GRAD | per block: colours a and b, then 32 bits of 2-bit indexes (MSB first) into a, b, mix(a, b), mix(b, a) |

A colour is either one byte with the top bit set, naming entry `byte & 127` of a
128-entry cache of recent colours, or a two-byte big-endian RGB555 literal, which
is then stored in the cache at the next round-robin position. The cache carries
over from frame to frame and resets with the frame when the clip loops.
`mix(a, b)` is `((2a + b) × 11) >> 5` per 5-bit channel. A MOTION run copies from
the frame as it was before the run started, so the decoder can copy a run a
strip at a time; it still sees every block drawn by earlier ops in the frame.
Runs are limited to 32 blocks so the parts of a run in two block rows never
overlap.
`src/preview_v5.h` is the reference, `src/preview_v5.c` the C decoder and
`src/preview_v5_arm.s` the console's decoder, which `tests/test_preview_asm.py`
checks against it.

The encoder also keeps every frame within a decode budget: about 200,000 ARM60
clocks, roughly 20 ms on a console, measured against the assembly decoder.

The menu accepts `OM3DVID5` and the streamed `OM3DVID7` format below. Older
formats must be converted from the original videos.

The menu reads the audio first, then places whole packets in up to eight
allocations, video RAM before DRAM, each followed by 1,024 readable bytes so a
damaged packet cannot read past its buffer. It keeps 64 KiB of DRAM and 32 KiB
of video RAM free, and keeps the longest prefix of frames (with matching audio)
that fits. It reads the whole file to verify the checksum and footer before
playing. Packets decode as frames are shown, and a packet that does not end
exactly at its table offset stops playback.

Audio is signed 8-bit mono PCM at 11025 Hz, of length
`align4(ceil(frame_count * 11025 / 30))` when audio is present, otherwise zero.
The converter pads short audio with silence and applies short fades at the
endpoints. Audio lives in MEMTYPE_AUDIO
storage; `varmono8.dsp` routes to both channels of `directout.dsp`. The menu restarts video and its sample
together at every loop boundary, closes audio before freeing its borrowed PCM,
and releases the entire clip on input. Sound-off skips PCM allocation. The
footer is read into a poisoned buffer because firmware reports requested read
lengths even at EOF. Playback never streams from USB, and loading never
modifies `.3dv` files.

## Streamed preview (`OM3DVID7`)

The format the desktop app makes: 320×240 RGB555 at 30 fps, the whole source
up to a minute (1,800 frames), with optional sound, about 200 KB per second.
The menu streams it from the drive while it plays rather than loading it
first, so it needs the media-beside-the-discs layout (`.ohmy3do` in each
game's folder): the menu reopens the file by the same relative path on every
loop. `src/preview_v7.h` is the reference.

The file is read from the start in 2 KiB blocks and never seeks (the
firmware's seek does nothing). It is a 2,048-byte header block, then chunks,
each padded with zeros to a multiple of 2,048 bytes.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `OM3DVID7` |
| 8 | 2 | Width, 320 |
| 10 | 2 | Height, 240 |
| 12 | 2 | Frames per second, 30 |
| 14 | 2 | Frames per chunk, 1–30 (10) |
| 16 | 4 | Frames, 1–1800 |
| 20 | 2 | Chunks: frames ÷ frames per chunk, rounded up |
| 22 | 2 | Preroll: chunks to read before playing |
| 24 | 2 | 4×4 entries the clip defines, at most 8,192 |
| 26 | 2 | 2×2 entries the clip defines, at most 4,096 |
| 28 | 4 | Sound rate, 11025 or zero when silent |
| 32 | 4 | Largest chunk in bytes, a multiple of 2,048 |
| 36 | 4 | Nominal bytes per second |
| 40 | 4 | Word checksum of the whole header block, with this field zero |
| 44 | 2 | Chunk slots the encoder assumed the player has, at least the preroll |
| 46 | 18 | Zero |
| 64 | 2 × chunks | Each chunk's length in 2 KiB blocks |

The rest of the block is zero. The word checksum is the preview checksum
described above.

A chunk holds its frames' sound, the dictionary entries first used in it, and
its frames' packets:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | Word checksum of the rest of the padded chunk |
| 4 | 2 | Chunk number |
| 6 | 2 | Frames in this chunk (the last may hold fewer) |
| 8 | 2 | First 4×4 entry it defines |
| 10 | 2 | 4×4 entries it defines |
| 12 | 2 | First 2×2 entry it defines |
| 14 | 2 | 2×2 entries it defines |
| 16 | 4 | Sound samples, `floor(end × 11025 / 30) − floor(start × 11025 / 30)` for its frames, or zero |
| 20 | 4 | Video bytes: the frame table and packets |
| 24 | | Signed 8-bit sound, padded with zeros to a word |
| | 40 each | 4×4 entries, ready to copy: two screen line pairs of four words (the upper line's pixel in each word's high half), then the TV picture's two rows of two pixels, each the rounded average of a 2×2 square |
| | 12 each | 2×2 entries: one line pair of two words, then the average of the four pixels in the high half of a word |
| | 4 × (frames + 1) | Packet offsets from the start of this table |
| | | Packets, then zero padding |

Entries are numbered in the order the chunks define them and never change.
A packet may use the entries its chunk or earlier chunks define. On every
loop the numbering starts again from zero, and the same entries are defined
again with the same pixels.

A packet is big-endian 16-bit words covering the 4,800 blocks of 4×4 pixels
in raster order (80 across, 60 down):

| Word | Meaning |
| --- | --- |
| `0x0000`–`0x1fff` | The block shows that 4×4 entry. |
| `0x2000`–`0x3fff` | `(word & 0x1fff) + 1` blocks stay as they are. |
| `0x4000`–`0x5fff` | `(word & 0x1fff) + 1` 4×4 entry numbers follow, one word each, below `0x2000`. |
| `0x6000`–`0x7fff` | `(word & 0x1fff) + 1` quad blocks follow, 3 words each: four 12-bit 2×2 entry numbers (top-left, top-right, bottom-left, bottom-right), most significant first. |

Other words are invalid, and a packet must cover exactly 4,800 blocks. Each
loop starts from a black picture.

### Playing

A reader thread with its own XBUS requests reads chunks into fixed-size
slots (as many as the clip assumed, up to eight, video RAM first). It
transfers at a priority above the menu's, so each finished transfer is
followed up at once, and checks a chunk and stores its entries below the
menu's priority, in the menu's spare time between frames. Playback starts
when every slot holds a chunk. A slot is reused once its frames are decoded
and its sound is in the sound ring. If a chunk is late, the picture and the
sound pause together until it arrives, and Diagnostics counts the stall.

Sound plays from a three-second ring that the DSP loops over; the menu copies
each chunk's samples into it ahead of the play position. The clock that
picks the frame due comes from the sound's start, less any pauses.

The decoder keeps what each block shows (the map) and draws only changed
blocks. The browser's TV picture is drawn from 2×2 averages of each entry,
160×120, which the cel engine shows at its native size. Full screen ignores
`screen_margin` and draws the video 1:1, as the game itself fills the TV: each
new frame is decoded straight into the back screen, then shown, and the SPORT
hardware copies that screen into the other buffer in the same vertical blank,
so the next frame starts from it. Switching between the two redraws the
picture from the map once.

The console's decoder, `src/preview_v7_arm.s`, checks each op against the
blocks and bytes left, so a damaged packet cannot write outside the picture;
entry numbers need no check, because the dictionaries always have room for
every number the format can express (8,192 and 4,096). Every chunk's checksum
is checked before use. `tests/test_preview_v7.py` runs the assembly against
the C decoder, `src/preview_v7.c`.

### Encoding

`tools/preview7-encoder.c` trains the two dictionaries on the clip, finds
each block's nearest 4×4 entry and its nearest four 2×2 entries, and chooses
per block whatever costs least in error plus bytes plus decode time. Error is
weighed as the eye sees it: brightness counts more than colour, and blocks in
flat areas (skies, water, skin) count more than busy ones, which hide
blockiness. It searches for the setting that averages the requested rate,
keeps every frame within about 230,000 ARM60 clocks of decoding, and keeps
chunks within the slot size. When a chunk could only fit by giving up
quality, as after a scene change, the encoder first sends the entries it
needs in the previous chunk's spare room; the decoder only needs an entry
before its first use. The preroll is every slot: the menu fills them all
before playing, and reads the next loop only once a loop has played, holding
the last frame silently while the slots refill, so the clip's end never
competes with reading. The encoder checks a model of a reader that manages
275 KB/s alongside playback and 400 KB/s during that hold: it must keep ahead
of playback within a loop, and a loop may hold for at most 2 seconds. It checks every file it
writes by decoding it with `src/preview_v7.c`. The desktop app and
`tools/prepare-preview.py` use the same settings, and add a 0.4-second fade-in
from the first frame to a source that does not start from black, which gives
that first picture's entries a chunk to arrive in.

## Save names

`.ohmy3do/save-names.txt` is ASCII text below 32768 bytes, written by the PC
tools. After an optional `#` comment, each line is `=FILE<TAB>Title` for an exact
NVRAM filename or `*PREFIX<TAB>Title` for files whose names start with PREFIX
(from patterns such as `SLOT%d` in a game's program). Names are matched without
regard to ASCII case; an exact name beats the longest prefix. Titles from several
discs that share a name are joined with ` / `. The names come from
`/NVRAM/NAME` strings in eligible ISO files, scanning contained files between
64 bytes and 3 MiB. CUE/BIN images are not scanned for these names. The results
are cached in `save-names-cache.json` keyed by filename, size and modification
time, so moving a disc between folders without changing those values reuses
the cache. Renaming it or changing its size/timestamp triggers another scan.

`.ohmy3do/saves-learned.txt`, written by the menu, uses the `=FILE<TAB>Title`
form; its names take precedence. When a game launches, the menu writes
`.ohmy3do/save-snapshot.txt`:

```
OH3DO_SAVESNAP1
Title
FILE<TAB>size<TAB>crc32 (8 hex digits)
...
```

with one line per NVRAM file. At the next startup it compares NVRAM with the
snapshot, adds each new or changed file that has no name yet to
`saves-learned.txt` under that title, and empties the snapshot. Snapshot titles
use the browser's cleaned game title, capped at 63 bytes. PC-generated titles
separately strip trailing parenthesized groups (including region and disc tags)
before applying the same 63-byte cap.

## Optional game descriptions

The desktop writes `.ohmy3do/info/xxxxxxxx.txt` for titles with descriptions.
`xxxxxxxx` is the CRC32 of the ASCII-case-folded UTF-8 relative disc path.
Each file contains `OM3DINFO1`, the complete relative path, a printable ASCII
description of at most 2048 bytes, and an eight-digit hexadecimal CRC32 of
`path + newline + description + newline`, each on its own newline-terminated line.
The reader verifies the full path as well as the checksum, so a filename collision
cannot display another game's description. Files are bounded to 12287 bytes and
loaded only on a details request; they do not enlarge the resident metadata index.
The desktop retains the original Unicode description in `library.json`.
