# The OM3DVID7 format

OM3DVID7 is a streamed video format for the 3DO: 320×240 RGB555 at 30 fps,
up to a minute (1,800 frames), with optional sound, at about 200 KB per
second. It is designed to be read from slow storage while it plays, and
decoded by the console's ARM60 within a frame's time.

The file is read from the start in 2 KiB blocks and never seeks: to loop, a
player opens it again. It is a 2,048-byte header block, then chunks of about a
third of a second each, each padded with zeros to a multiple of 2,048 bytes.
Numbers are unsigned and big-endian unless stated. `include/om3dvid7.h` is the
reference; this page explains it.

## Word checksum

Headers and chunks are checked with a word checksum over big-endian 32-bit
words: starting from `a = b = 0`, for each word `a += word; b += a` (modulo
2³²); the checksum is `a XOR (b rotated left by 16 bits)`.

## Header block

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `OM3DVID7` |
| 8 | 2 | Width, 320 |
| 10 | 2 | Height, 240 |
| 12 | 2 | Frames per second, 30 |
| 14 | 2 | Frames per chunk, 1–30 (10 by default) |
| 16 | 4 | Frames, 1–1800 |
| 20 | 2 | Chunks: frames ÷ frames per chunk, rounded up |
| 22 | 2 | Preroll: chunks to read before playing |
| 24 | 2 | 4×4 entries the clip defines, at most 8,192 |
| 26 | 2 | 2×2 entries the clip defines, at most 4,096 |
| 28 | 4 | Sound rate, 11025, or zero when silent |
| 32 | 4 | Largest chunk in bytes, a multiple of 2,048 |
| 36 | 4 | Nominal bytes per second |
| 40 | 4 | Word checksum of the whole header block, with this field zero |
| 44 | 2 | Chunk slots the encoder assumed the player has, at least the preroll |
| 46 | 18 | Zero |
| 64 | 2 × chunks | Each chunk's length in 2 KiB blocks, none longer than the largest |

The rest of the block is zero, and the chunks fill the rest of the file
exactly.

## Chunks

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
| 16 | 4 | Sound samples: `floor(end × 11025 / 30) − floor(start × 11025 / 30)` for its frames `start` to `end`, or zero |
| 20 | 4 | Video bytes: the frame table and packets |
| 24 | | Signed 8-bit mono sound, padded with zeros to a word |
| | 40 each | 4×4 entries (below) |
| | 12 each | 2×2 entries (below) |
| | 4 × (frames + 1) | Packet offsets, from the start of this table |
| | | Packets, then zero padding |

The first offset is `4 × (frames + 1)` and the last equals the video bytes.

## Dictionary entries

Entries are stored in the console decoder's own layout, so storing one is a
copy. Pixels are RGB555: red in bits 10–14, green in 5–9, blue in 0–4, bit 15
zero.

- A **4×4 entry** (40 bytes) is two screen line pairs of four 32-bit words
  (each word holds a pixel of the upper line in its high half and the pixel
  below it in its low half), then its TV picture: two rows of two pixels, each
  the rounded average of a 2×2 square (each channel `(sum + 2) >> 2`).
- A **2×2 entry** (12 bytes) is one line pair of two words, then the average
  of its four pixels in the high half of a word.

Entries are numbered in the order the chunks define them and are never
redefined with different pixels. A packet may use only the entries its chunk
or earlier chunks define. Every loop starts the numbering again from zero, and
the same entries are defined again with the same pixels.

## Packets

A packet is big-endian 16-bit words covering the 4,800 blocks of 4×4 pixels
in raster order (80 across, 60 down):

| Word | Meaning |
| --- | --- |
| `0x0000`–`0x1fff` | The block shows that 4×4 entry. |
| `0x2000`–`0x3fff` | `(word & 0x1fff) + 1` blocks stay as they are. |
| `0x4000`–`0x5fff` | `(word & 0x1fff) + 1` 4×4 entry numbers follow, one word each, below `0x2000`. |
| `0x6000`–`0x7fff` | `(word & 0x1fff) + 1` quad blocks follow, 3 words each: four 12-bit 2×2 entry numbers (top-left, top-right, bottom-left, bottom-right), most significant first. |

Other words are invalid, and a packet must cover exactly 4,800 blocks. Each
frame changes the previous one in place; each loop starts from a black
picture.

## Pictures

A decoder draws into either of two pictures, and keeps a map of what every
block shows so it can redraw the other picture whole after switching:

- **Screen**: a 320×240 3DO screen bitmap, two lines per 32-bit word: pixel
  (x, y) is halfword `y & 1` of word `(y / 2) × 320 + x`.
- **TV**: the 160×120 picture of 2×2 averages, in rows, which a menu can show
  small at its native size.

## Playing

A player reads the header block, then chunks in order into a few fixed-size
slots (the header says how many the encoder assumed, each at least the
largest chunk). It starts once the preroll is read, which the encoder makes
every slot, and reuses a slot once its frames are decoded and its sound is
queued. If a chunk is late, the picture and sound pause together until it
arrives. To keep the end of a loop from competing with reading, a player
reads the next loop only once a loop has played, and holds the last frame
while the slots refill.

## Encoding

`src/om3dvid7_encoder.c` trains the two dictionaries on the clip, finds each
block's nearest 4×4 entry and its nearest four 2×2 entries, and chooses per
block whatever costs least in error plus bytes plus decode time. Error is
weighed as the eye sees it: brightness counts more than colour, and blocks in
flat areas (skies, water, skin) count more than busy ones, which hide
blockiness. It searches for the setting that averages the requested rate,
keeps every frame within a decoding budget (about 230,000 ARM60 clocks by
default), and keeps chunks within the slot size. When a chunk could only fit
by giving up quality, as after a scene change, the encoder first sends the
entries it needs in the previous chunk's spare room; a decoder only needs an
entry before its first use.

The encoder checks a model of a reader that manages 275 KB/s alongside
playback and 400 KB/s while a loop holds: it must keep ahead of playback
within a loop, and a loop may hold for at most 2 seconds. It checks every
file it writes by decoding it with `src/om3dvid7_decoder.c`. Encoding is
deterministic: the same input always gives the same file.

A source that does not open from black plays better with a short fade-in from
black to its first frame (Oh My 3DO adds 12 frames), which gives that first
picture's entries a chunk to arrive in.
