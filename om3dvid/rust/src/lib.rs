//! OM3DVID7 for Rust: make streamed 3DO video clips, check them, and decode
//! them.
//!
//! OM3DVID7 is the 320x240, 30 fps streamed video format of the om3dvid
//! library (see `om3dvid/include/om3dvid7.h` for the layout). This crate
//! compiles the library's C encoder, decoder and reader in (see build.rs),
//! and checks files in Rust.
//!
//! ```no_run
//! # fn main() -> Result<(), om3dvid7::Error> {
//! # let frames: Vec<Vec<u8>> = vec![];
//! let mut encoder = om3dvid7::Encoder::new(frames.len())?;
//! for frame in &frames {
//!     encoder.add_frame(frame)?; // 320x240 RGB, 3 bytes a pixel, in rows
//! }
//! // Sound is optional: signed 8-bit mono at 11025 Hz, samples_for(frames)
//! // of them, or None for a silent clip.
//! let clip = encoder.finish(None, &om3dvid7::EncoderSettings::default())?;
//! om3dvid7::check(&clip.data)?; // what a player would refuse
//!
//! let mut reader = om3dvid7::Reader::new(&clip.data)?;
//! let (number, rgb) = reader.next_frame(om3dvid7::Picture::Screen)?;
//! # Ok(()) }
//! ```
//!
//! Sound is optional throughout: a clip may be silent, and a player decides
//! for itself whether and how to play the sound it carries.

use std::fmt;

pub const WIDTH: usize = 320;
pub const HEIGHT: usize = 240;
pub const FPS: usize = 30;
pub const AUDIO_RATE: usize = 11025;
pub const MAX_FRAMES: usize = 1800;
pub const MAX_ENTRIES4: usize = 8192;
pub const MAX_ENTRIES2: usize = 4096;
pub const MAX_CHUNK_FRAMES: usize = 30;

/// One 320x240 frame of 8-bit RGB: what the encoder takes and the reader
/// gives at full size.
pub const FRAME_BYTES: usize = WIDTH * HEIGHT * 3;

/// The 160x120 TV picture of 2x2 averages that a menu shows small.
pub const TV_WIDTH: usize = 160;
pub const TV_HEIGHT: usize = 120;
pub const TV_FRAME_BYTES: usize = TV_WIDTH * TV_HEIGHT * 3;

/// The file's layout: a 2048-byte header block, whose chunk table starts at
/// byte 64, then chunks padded to 2048 bytes, each starting with a 24-byte
/// header. A 4x4 dictionary entry is 40 bytes, a 2x2 one 12.
pub const BLOCK_BYTES: usize = 2048;
const CHUNK_TABLE: usize = 64;
const CHUNK_HEADER: usize = 24;
const ENTRY4_BYTES: usize = 40;
const ENTRY2_BYTES: usize = 12;

/// The native interface versions this crate speaks.
const ENCODER_ABI: u32 = 2;
const READER_ABI: u32 = 1;

/// What went wrong, in words.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error(String);

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

fn fail<T>(message: impl Into<String>) -> Result<T> {
    Err(Error(message.into()))
}

/// The sound samples a clip of `frames` frames carries (rounded down).
pub fn samples_for(frames: usize) -> usize {
    frames * AUDIO_RATE / FPS
}

/// The format's check value of `data` (big-endian 32-bit words), or None if
/// it is not whole words.
pub fn word_sum(data: &[u8]) -> Option<u32> {
    if !data.len().is_multiple_of(4) {
        return None;
    }
    let (mut a, mut b) = (0u32, 0u32);
    for word in data.chunks_exact(4) {
        a = a.wrapping_add(u32::from_be_bytes([word[0], word[1], word[2], word[3]]));
        b = b.wrapping_add(a);
    }
    Some(a ^ b.rotate_left(16))
}

// ---- The native code ----------------------------------------------------

/// What the encoder aims for (Om3dvid7EncoderSettings in om3dvid7_encoder.h).
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EncoderSettings {
    /// Bytes per second the file should average.
    pub rate: u32,
    /// Bytes per second the console's reader is assumed to manage.
    pub read_rate: u32,
    /// Memory the player is assumed to have for chunks.
    pub slot_bytes: u32,
    /// The largest padded chunk, a multiple of 2048.
    pub max_chunk: u32,
    pub chunk_frames: u32,
    /// Dictionary sizes for 4x4 and 2x2 blocks.
    pub entries4: u32,
    pub entries2: u32,
    /// Modelled ARM60 decode clocks per frame.
    pub cycle_cap: u32,
}

impl Default for EncoderSettings {
    /// The settings Oh My 3DO uses: about 200 KB/s, 10-frame chunks.
    fn default() -> Self {
        let mut settings = EncoderSettings {
            rate: 0,
            read_rate: 0,
            slot_bytes: 0,
            max_chunk: 0,
            chunk_frames: 0,
            entries4: 0,
            entries2: 0,
            cycle_cap: 0,
        };
        // SAFETY: settings is the C structure, all 32-bit fields in order.
        unsafe { native::om3dvid7_encoder_default_settings(&mut settings) };
        settings
    }
}

mod native {
    use super::EncoderSettings;

    extern "C" {
        // om3dvid/src/om3dvid7_encoder.c
        pub fn om3dvid7_encoder_abi() -> u32;
        pub fn om3dvid7_encoder_default_settings(settings: *mut EncoderSettings);
        pub fn om3dvid7_encoder_workspace_bytes(frames: u32) -> u64;
        pub fn om3dvid7_encoder_begin(workspace: *mut u8, bytes: u64, frames: u32) -> i32;
        pub fn om3dvid7_encoder_add_frame(workspace: *mut u8, rgb: *const u8) -> i32;
        pub fn om3dvid7_encoder_finish(
            workspace: *mut u8,
            settings: *const EncoderSettings,
            audio: *const i8,
            samples: u64,
            out: *mut u8,
            capacity: u64,
            stats: *mut u32,
        ) -> i64;

        // om3dvid/src/om3dvid7_reader.c
        pub fn om3dvid7_reader_abi() -> u32;
        pub fn om3dvid7_reader_workspace_bytes() -> u64;
        pub fn om3dvid7_reader_open(
            workspace: *mut u8,
            bytes: u64,
            data: *const u8,
            size: u64,
        ) -> i32;
        pub fn om3dvid7_reader_frames(workspace: *const u8) -> u32;
        pub fn om3dvid7_reader_audio_rate(workspace: *const u8) -> u32;
        pub fn om3dvid7_reader_next(workspace: *mut u8, picture: i32, rgb: *mut u8) -> i32;
        pub fn om3dvid7_reader_rewind(workspace: *mut u8);
        pub fn om3dvid7_reader_audio(workspace: *mut u8, out: *mut i8, capacity: u64) -> i64;
    }
}

// ---- Encoding ---------------------------------------------------------------

/// What the encoder reports about a clip it made.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Statistics {
    /// The quality setting chosen, times 1000.
    pub lambda_x1000: u32,
    /// The slowest frame's modelled ARM60 decode clocks.
    pub slowest_frame_clocks: u32,
    /// Dictionary entries used.
    pub entries4: u32,
    pub entries2: u32,
    /// Chunks a player reads before it starts playing.
    pub preroll: u32,
    /// Chunk buffers the player is assumed to have.
    pub slots: u32,
    /// The largest chunk, in bytes.
    pub largest_chunk: u32,
    pub chunks: u32,
}

/// An encoded clip: the file's bytes, its frame count, and the encoder's
/// statistics.
#[derive(Debug, Clone)]
pub struct EncodedClip {
    pub data: Vec<u8>,
    pub frames: usize,
    pub statistics: Statistics,
}

/// Encodes one clip of up to `max_frames` frames.
pub struct Encoder {
    workspace: Vec<u8>,
    max_frames: usize,
    frames: usize,
}

impl Encoder {
    pub fn new(max_frames: usize) -> Result<Encoder> {
        // SAFETY: plain calls into the encoder.
        if unsafe { native::om3dvid7_encoder_abi() } != ENCODER_ABI {
            return fail("Unsupported OM3DVID7 encoder ABI");
        }
        if max_frames == 0 || max_frames > MAX_FRAMES {
            return fail(format!(
                "A clip holds 1 to {MAX_FRAMES} frames, not {max_frames}"
            ));
        }
        // SAFETY: plain arithmetic in the encoder.
        let bytes = unsafe { native::om3dvid7_encoder_workspace_bytes(max_frames as u32) };
        let mut workspace = vec![0u8; bytes as usize];
        // SAFETY: workspace holds `bytes` writable bytes.
        let status = unsafe {
            native::om3dvid7_encoder_begin(workspace.as_mut_ptr(), bytes, max_frames as u32)
        };
        if status != 0 {
            return fail("Preview encoder setup failed");
        }
        Ok(Encoder {
            workspace,
            max_frames,
            frames: 0,
        })
    }

    /// The frames added so far.
    pub fn frames(&self) -> usize {
        self.frames
    }

    /// Adds the next frame: FRAME_BYTES of 320x240 RGB, in rows.
    pub fn add_frame(&mut self, rgb: &[u8]) -> Result<()> {
        if rgb.len() != FRAME_BYTES {
            return fail(format!(
                "A frame is {FRAME_BYTES} bytes of 320x240 RGB, not {}",
                rgb.len()
            ));
        }
        if self.frames == self.max_frames {
            return fail("Preview encoder rejected a frame");
        }
        // SAFETY: rgb is one whole picture; the workspace has room for it.
        if unsafe { native::om3dvid7_encoder_add_frame(self.workspace.as_mut_ptr(), rgb.as_ptr()) }
            != 0
        {
            return fail("Preview encoder rejected a frame");
        }
        self.frames += 1;
        Ok(())
    }

    /// Encodes the frames added. `audio` is signed 8-bit mono at 11025 Hz,
    /// exactly samples_for(frames) bytes, or None for a silent clip.
    pub fn finish(
        &mut self,
        audio: Option<&[u8]>,
        settings: &EncoderSettings,
    ) -> Result<EncodedClip> {
        if self.frames == 0 {
            return fail("Video did not produce complete frames");
        }
        let samples = samples_for(self.frames);
        if audio.is_some_and(|audio| audio.len() != samples) {
            return fail("Preview sound does not match its frames");
        }

        // Room for 10% over the target rate, plus 2 MiB for headers.
        let capacity = settings.rate as usize * self.frames / FPS * 11 / 10 + (2 << 20);
        let mut out = vec![0u8; capacity];
        let mut stats = [0u32; 8];
        let audio_pointer = audio.map_or(std::ptr::null(), |audio| audio.as_ptr() as *const i8);
        let audio_samples = audio.map_or(0, |_| samples as u64);

        // SAFETY: the workspace holds the frames added; audio holds `samples`
        // bytes; out holds `capacity` writable bytes; stats has room for 8.
        let size = unsafe {
            native::om3dvid7_encoder_finish(
                self.workspace.as_mut_ptr(),
                settings,
                audio_pointer,
                audio_samples,
                out.as_mut_ptr(),
                capacity as u64,
                stats.as_mut_ptr(),
            )
        };
        if size < 0 {
            return fail(format!("Preview encoder failed (error {size})"));
        }
        out.truncate(size as usize);
        Ok(EncodedClip {
            data: out,
            frames: self.frames,
            statistics: Statistics {
                lambda_x1000: stats[0],
                slowest_frame_clocks: stats[1],
                entries4: stats[2],
                entries2: stats[3],
                preroll: stats[4],
                slots: stats[5],
                largest_chunk: stats[6],
                chunks: stats[7],
            },
        })
    }
}

/// Encodes `frames` (whole 320x240 RGB frames, one after another) with
/// optional `audio` in one call.
pub fn encode(
    frames: &[u8],
    audio: Option<&[u8]>,
    settings: &EncoderSettings,
) -> Result<EncodedClip> {
    let count = frames.len() / FRAME_BYTES;
    if count == 0 || count > MAX_FRAMES || !frames.len().is_multiple_of(FRAME_BYTES) {
        return fail("Invalid preview frames");
    }
    let mut encoder = Encoder::new(count)?;
    for frame in frames.chunks(FRAME_BYTES) {
        encoder.add_frame(frame)?;
    }
    encoder.finish(audio, settings)
}

// ---- Decoding ---------------------------------------------------------------

/// The picture a reader decodes into.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Picture {
    /// The full 320x240 screen (FRAME_BYTES of RGB).
    Screen,
    /// The 160x120 TV picture of 2x2 averages (TV_FRAME_BYTES of RGB).
    Tv,
}

impl Picture {
    /// The bytes of one RGB frame of this picture.
    pub fn rgb_bytes(self) -> usize {
        match self {
            Picture::Screen => FRAME_BYTES,
            Picture::Tv => TV_FRAME_BYTES,
        }
    }

    fn code(self) -> i32 {
        match self {
            Picture::Screen => 0,
            Picture::Tv => 1,
        }
    }
}

/// Decodes a clip frame by frame into RGB pictures, exactly as a 3DO shows
/// them. After the last frame it starts over, as players loop.
pub struct Reader<'a> {
    workspace: Vec<u8>,
    /// The clip, which the native reader decodes from.
    data: &'a [u8],
    frames: usize,
    audio_rate: usize,
}

impl<'a> Reader<'a> {
    /// Opens a clip (the whole file's bytes). Checks the header and that the
    /// chunk table matches the file; chunks are checked as they are reached.
    pub fn new(data: &'a [u8]) -> Result<Reader<'a>> {
        // SAFETY: plain calls into the reader.
        if unsafe { native::om3dvid7_reader_abi() } != READER_ABI {
            return fail("Unsupported OM3DVID7 reader ABI");
        }
        // SAFETY: plain arithmetic in the reader.
        let bytes = unsafe { native::om3dvid7_reader_workspace_bytes() };
        let mut workspace = vec![0u8; bytes as usize];
        // SAFETY: the workspace holds `bytes` writable bytes, and the reader
        // borrows data for as long as it lives (its heap buffer never moves).
        let status = unsafe {
            native::om3dvid7_reader_open(
                workspace.as_mut_ptr(),
                bytes,
                data.as_ptr(),
                data.len() as u64,
            )
        };
        if status != 0 {
            return fail("Not a readable OM3DVID7 clip");
        }
        // SAFETY: the workspace holds an open reader.
        let (frames, audio_rate) = unsafe {
            (
                native::om3dvid7_reader_frames(workspace.as_ptr()) as usize,
                native::om3dvid7_reader_audio_rate(workspace.as_ptr()) as usize,
            )
        };
        Ok(Reader {
            workspace,
            data,
            frames,
            audio_rate,
        })
    }

    /// The clip's frame count.
    pub fn frames(&self) -> usize {
        self.frames
    }

    /// Its sound's sample rate: 11025, or 0 when silent.
    pub fn audio_rate(&self) -> usize {
        self.audio_rate
    }

    pub fn has_sound(&self) -> bool {
        self.audio_rate != 0
    }

    /// Decodes the next frame into `rgb` (picture.rgb_bytes() long, in rows).
    /// Returns the frame's number within the clip.
    pub fn next_into(&mut self, picture: Picture, rgb: &mut [u8]) -> Result<usize> {
        if rgb.len() != picture.rgb_bytes() {
            return fail("The picture buffer is the wrong size");
        }
        // SAFETY: the workspace holds an open reader whose clip (self.data)
        // is still borrowed; rgb holds a whole picture.
        let number = unsafe {
            native::om3dvid7_reader_next(
                self.workspace.as_mut_ptr(),
                picture.code(),
                rgb.as_mut_ptr(),
            )
        };
        if number < 0 {
            return fail("Damaged OM3DVID7 clip");
        }
        Ok(number as usize)
    }

    /// Decodes the next frame. Returns (its number in the clip, RGB bytes).
    pub fn next_frame(&mut self, picture: Picture) -> Result<(usize, Vec<u8>)> {
        let mut rgb = vec![0u8; picture.rgb_bytes()];
        let number = self.next_into(picture, &mut rgb)?;
        Ok((number, rgb))
    }

    /// Starts again from the first frame.
    pub fn rewind(&mut self) {
        // SAFETY: the workspace holds an open reader.
        unsafe { native::om3dvid7_reader_rewind(self.workspace.as_mut_ptr()) };
    }

    /// The clip's whole sound: signed 8-bit mono at audio_rate() (empty when
    /// silent).
    pub fn audio(&mut self) -> Result<Vec<u8>> {
        // SAFETY: the workspace holds an open reader; NULL asks for the count.
        let count = unsafe {
            native::om3dvid7_reader_audio(self.workspace.as_mut_ptr(), std::ptr::null_mut(), 0)
        };
        if count < 0 {
            return fail("Damaged OM3DVID7 clip");
        }
        let mut samples = vec![0u8; count as usize];
        // SAFETY: samples holds `count` writable bytes.
        unsafe {
            native::om3dvid7_reader_audio(
                self.workspace.as_mut_ptr(),
                samples.as_mut_ptr() as *mut i8,
                count as u64,
            )
        };
        Ok(samples)
    }

    /// The clip being read.
    pub fn data(&self) -> &'a [u8] {
        self.data
    }
}

/// Every frame of a clip once, as RGB bytes of `picture`.
pub fn decode(data: &[u8], picture: Picture) -> Result<Vec<Vec<u8>>> {
    let mut reader = Reader::new(data)?;
    (0..reader.frames())
        .map(|_| reader.next_frame(picture).map(|(_, rgb)| rgb))
        .collect()
}

// ---- Reading and checking files -------------------------------------------

/// A clip's header block (see om3dvid7.h).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Header {
    pub frames: usize,
    /// Frames per chunk (the last may have fewer).
    pub chunk_frames: usize,
    pub chunks: usize,
    /// Chunks a player reads before it starts playing.
    pub preroll: usize,
    /// Dictionary entries in the whole clip.
    pub entries4: usize,
    pub entries2: usize,
    /// 0 (silent) or 11025.
    pub audio_rate: usize,
    /// The largest chunk, in bytes.
    pub largest_chunk: usize,
    /// Nominal bytes per second.
    pub rate: usize,
    /// Chunk buffers the encoder assumed the player has.
    pub slots: usize,
    /// Each chunk's length in 2048-byte blocks.
    pub chunk_blocks: Vec<usize>,
}

impl Header {
    pub fn seconds(&self) -> f64 {
        self.frames as f64 / FPS as f64
    }

    pub fn has_sound(&self) -> bool {
        self.audio_rate != 0
    }
}

/// A big-endian 16-bit number at `at` (0 past the end; callers check the
/// length first).
fn be16(data: &[u8], at: usize) -> usize {
    match data.get(at..at + 2) {
        Some(bytes) => u16::from_be_bytes([bytes[0], bytes[1]]) as usize,
        None => 0,
    }
}

/// A big-endian 32-bit number at `at`.
fn be32(data: &[u8], at: usize) -> Result<u32> {
    match data.get(at..at + 4) {
        Some(bytes) => Ok(u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])),
        None => fail("Truncated streamed preview"),
    }
}

/// Reads and checks the header block at the start of `data`, as a player
/// does.
pub fn read_header(data: &[u8]) -> Result<Header> {
    if data.len() < BLOCK_BYTES {
        return fail("Truncated streamed preview");
    }
    if !data.starts_with(b"OM3DVID7") {
        return fail("Not an OM3DVID7 clip");
    }
    let chunk_frames = be16(data, 14);
    let frames = be32(data, 16)? as usize;
    let chunks = be16(data, 20);
    let preroll = be16(data, 22);
    let entries4 = be16(data, 24);
    let entries2 = be16(data, 26);
    let audio_rate = be32(data, 28)? as usize;
    let largest_chunk = be32(data, 32)? as usize;
    let rate = be32(data, 36)? as usize;
    let slots = be16(data, 44);

    let size_and_rate = (be16(data, 8), be16(data, 10), be16(data, 12));
    if size_and_rate != (WIDTH, HEIGHT, FPS)
        || !(1..=MAX_CHUNK_FRAMES).contains(&chunk_frames)
        || !(1..=MAX_FRAMES).contains(&frames)
        || chunks != frames.div_ceil(chunk_frames)
        || chunks > (BLOCK_BYTES - CHUNK_TABLE) / 2
        || preroll == 0
        || preroll > chunks
        || slots < preroll
        || entries4 > MAX_ENTRIES4
        || entries2 > MAX_ENTRIES2
        || (audio_rate != 0 && audio_rate != AUDIO_RATE)
        || largest_chunk == 0
        || !largest_chunk.is_multiple_of(BLOCK_BYTES)
        || data[46..CHUNK_TABLE].iter().any(|&byte| byte != 0)
        || data[CHUNK_TABLE + 2 * chunks..BLOCK_BYTES]
            .iter()
            .any(|&byte| byte != 0)
    {
        return fail("Invalid streamed preview header");
    }

    // The sum covers the header block with its own field (bytes 40-43) as zero.
    let mut block = data[..BLOCK_BYTES].to_vec();
    block[40..44].fill(0);
    if word_sum(&block) != Some(be32(data, 40)?) {
        return fail("Streamed preview header checksum mismatch");
    }
    Ok(Header {
        frames,
        chunk_frames,
        chunks,
        preroll,
        entries4,
        entries2,
        audio_rate,
        largest_chunk,
        rate,
        slots,
        chunk_blocks: (0..chunks)
            .map(|n| be16(data, CHUNK_TABLE + 2 * n))
            .collect(),
    })
}

/// Checks a whole clip as a player does: the header block and its sum, the
/// chunk table against the file, and every chunk's sum, numbering, entries
/// and frame table. (Packets are checked by the decoder as they play.)
pub fn check(data: &[u8]) -> Result<Header> {
    let header = read_header(data)?;

    // Walk the chunks, keeping count of the dictionary entries added so far.
    let mut position = BLOCK_BYTES;
    let mut entries4_so_far = 0usize;
    let mut entries2_so_far = 0usize;
    for (number, &blocks) in header.chunk_blocks.iter().enumerate() {
        let size = blocks * BLOCK_BYTES;
        if size == 0 || size > header.largest_chunk || position + size > data.len() {
            return fail("Invalid streamed preview chunk table");
        }
        let chunk = &data[position..position + size];

        let first_frame = number * header.chunk_frames;
        let frame_count = (header.frames - first_frame).min(header.chunk_frames);
        let audio = be32(chunk, 16)? as usize;
        let video = be32(chunk, 20)? as usize;
        let new4 = be16(chunk, 10);
        let new2 = be16(chunk, 14);
        // The chunk's header, its sound (padded to 4 bytes) and its new
        // dictionary entries come before the video.
        let video_start =
            CHUNK_HEADER + audio.next_multiple_of(4) + new4 * ENTRY4_BYTES + new2 * ENTRY2_BYTES;
        let expected_audio = if header.has_sound() {
            samples_for(first_frame + frame_count) - samples_for(first_frame)
        } else {
            0
        };

        if word_sum(&chunk[4..]) != Some(be32(chunk, 0)?)
            || be16(chunk, 4) != number
            || be16(chunk, 6) != frame_count
            || be16(chunk, 8) != entries4_so_far
            || be16(chunk, 12) != entries2_so_far
            || entries4_so_far + new4 > header.entries4
            || entries2_so_far + new2 > header.entries2
            || audio != expected_audio
            || video_start + video > size
            || video < (frame_count + 1) * 4
            || be32(chunk, video_start)? as usize != (frame_count + 1) * 4
            || be32(chunk, video_start + frame_count * 4)? as usize != video
        {
            return fail(format!("Invalid streamed preview chunk {number}"));
        }

        entries4_so_far += new4;
        entries2_so_far += new2;
        position += size;
    }

    if position != data.len() {
        return fail("Streamed preview has trailing data");
    }
    Ok(header)
}
