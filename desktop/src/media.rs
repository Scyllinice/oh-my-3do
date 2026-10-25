//! Covers and video previews: finding them, converting them to the formats
//! the console menu reads, and checking prepared files.
//!
//! - Covers come from the person's own files, an EmuMovies media folder, or
//!   the libretro box art server. Each becomes a small 96x120 cover and a
//!   192x240 full-screen cover, both stored as compressed 15-bit pictures
//!   ("OM3DCOV2", see [`prepared_cover`]).
//! - Previews come from video clips (the person's own, or EmuMovies). FFmpeg
//!   turns a clip into 320x240 frames at 30 fps and 11025 Hz sound, and the
//!   C encoder compiled in from `tools/preview7-encoder.c` packs them into a
//!   streamed preview ("OM3DVID7"). The older in-memory format ("OM3DVID5",
//!   160x120) can still be encoded and checked.
//!
//! Converted files are cached under the cache folder by a checksum of the
//! source and the conversion settings, so each is only converted once.
//! [`validate`] checks a prepared file the same way the menu does before
//! it is copied to a drive.

use crate::{
    catalog,
    disc::{self as d, rx},
    fsutil as f,
    model::*,
    Job,
};
use anyhow::{bail, Context, Result};
use flate2::{write::ZlibEncoder, Compression};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::{
    collections::HashMap,
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    time::{Duration, Instant},
};

/// A cover or preview converted for the console.
#[derive(Clone)]
pub struct Prepared {
    /// The name it gets on the drive, from a checksum of its contents.
    pub name: String,
    /// Where the converted file is in the cache.
    pub path: PathBuf,
    /// Where it came from (a file path or URL), recorded in the library.
    pub source: String,
    /// A cover's 192x240 full-screen version, stored as `large_name(name)`.
    pub large: Option<PathBuf>,
}

/// The full-screen cover's name beside NAME.png or NAME.3dc:
/// NAME-large.3dc.
pub fn large_name(name: &str) -> String {
    let without_extension = &name[..name.len().saturating_sub(4)];
    format!("{without_extension}-large.3dc")
}

/// Prepared media, by (disc's library path, "cover" or "preview").
pub type Outputs = HashMap<(String, String), Prepared>;

// ---- Running FFmpeg and ffprobe ----

/// How long one run of a video tool may take.
const TOOL_TIME_LIMIT: Duration = Duration::from_secs(180);

/// How much of a video tool's error output is kept for the error message.
const MAX_TOOL_ERROR_BYTES: u64 = 64 * 1024;

/// A command for `exe`. On Windows, it runs without opening a console
/// window.
fn command(exe: &Path) -> Command {
    #[allow(unused_mut)]
    let mut command = Command::new(exe);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        command.creation_flags(CREATE_NO_WINDOW);
    }
    command
}

/// Starts `exe` with its output and errors piped back and no input.
fn spawn_tool(exe: &Path, args: &[String]) -> Result<std::process::Child> {
    Ok(command(exe)
        .args(args)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .stdin(Stdio::null())
        .spawn()?)
}

/// Reads up to `limit` bytes from a pipe on a thread of its own, so a tool
/// never stalls with a full pipe while another one is being read.
fn read_on_thread(
    mut pipe: impl Read + Send + 'static,
    limit: u64,
) -> std::thread::JoinHandle<std::io::Result<Vec<u8>>> {
    std::thread::spawn(move || {
        let mut bytes = Vec::new();
        (&mut pipe).take(limit).read_to_end(&mut bytes)?;
        Ok(bytes)
    })
}

/// Runs `exe` and returns its output, at most `max` bytes. Stops it if the
/// job is canceled or it runs longer than [`TOOL_TIME_LIMIT`].
fn capture(exe: &Path, args: &[String], max: usize, job: &Job) -> Result<Vec<u8>> {
    let mut child = spawn_tool(exe, args)?;
    let output = read_on_thread(child.stdout.take().unwrap(), max as u64 + 1);
    let errors = read_on_thread(child.stderr.take().unwrap(), MAX_TOOL_ERROR_BYTES);

    let started = Instant::now();
    let status = loop {
        if job.check().is_err() || started.elapsed() > TOOL_TIME_LIMIT {
            let _ = child.kill();
            let _ = child.wait();
            let _ = output.join();
            let _ = errors.join();
            bail!("Video conversion canceled or timed out")
        }
        if let Some(status) = child.try_wait()? {
            break status;
        }
        std::thread::sleep(Duration::from_millis(50));
    };

    let bytes = output
        .join()
        .map_err(|_| anyhow::anyhow!("Video output thread failed"))??;
    let error_text = errors
        .join()
        .map_err(|_| anyhow::anyhow!("Video error thread failed"))??;
    if !status.success() {
        bail!(
            "Video tool failed: {}",
            String::from_utf8_lossy(&error_text)
        )
    }
    if bytes.len() > max {
        bail!("Video output exceeds expected bounds")
    }
    Ok(bytes)
}

/// Runs `exe` and hands each complete `size`-byte block of its output to
/// `each`, up to `max` blocks. A partial block at the end is dropped.
/// Returns how many blocks there were.
fn stream(
    exe: &Path,
    args: &[String],
    size: usize,
    max: usize,
    job: &Job,
    mut each: impl FnMut(&[u8]) -> Result<()>,
) -> Result<usize> {
    let mut child = spawn_tool(exe, args)?;
    let mut output = child.stdout.take().unwrap();
    let errors = read_on_thread(child.stderr.take().unwrap(), MAX_TOOL_ERROR_BYTES);

    let mut block = vec![0u8; size];
    let mut count = 0;
    let result = (|| -> Result<()> {
        loop {
            job.check()?;

            // Fill the block (a read can return less than asked).
            let mut filled = 0;
            while filled < size {
                let read = output.read(&mut block[filled..])?;
                if read == 0 {
                    break;
                }
                filled += read;
            }
            if filled < size {
                return Ok(());
            }

            if count == max {
                bail!("Video output exceeds expected bounds")
            }
            each(&block)?;
            count += 1;
        }
    })();

    if result.is_err() {
        let _ = child.kill();
    }
    let status = child.wait()?;
    let error_text = errors
        .join()
        .map_err(|_| anyhow::anyhow!("Video error thread failed"))??;
    result?;
    if !status.success() {
        bail!(
            "Video tool failed: {}",
            String::from_utf8_lossy(&error_text)
        )
    }
    Ok(count)
}

/// Command-line arguments as owned strings.
fn args(list: &[&str]) -> Vec<String> {
    list.iter().map(|arg| arg.to_string()).collect()
}

/// Finds FFmpeg and ffprobe: in the folder chosen in the preferences, or
/// else on the PATH. Both must be in the same folder.
pub fn video_exes(prefs: &Preferences) -> Result<(PathBuf, PathBuf)> {
    let extension = if cfg!(windows) { ".exe" } else { "" };

    let mut folders = vec![];
    if !prefs.video_tools.is_empty() {
        folders.push(PathBuf::from(&prefs.video_tools));
    }
    if let Ok(path) = std::env::var("PATH") {
        folders.extend(std::env::split_paths(&path));
    }

    for folder in folders {
        let ffmpeg = folder.join(format!("ffmpeg{extension}"));
        let ffprobe = folder.join(format!("ffprobe{extension}"));
        if ffmpeg.is_file() && ffprobe.is_file() {
            return Ok((ffmpeg, ffprobe));
        }
    }
    bail!(
        "Video tools are missing. \
         Use Get video tools or choose their folder under Tools & settings."
    )
}

// ---- Covers ----

/// The menu's small cover and full-screen cover sizes.
const COVER_SIZE: (u16, u16) = (96, 120);
const LARGE_COVER_SIZE: (u16, u16) = (192, 240);

/// The menu's background color, behind art that does not fill the cover.
const COVER_BACKGROUND: [u8; 3] = [8, 16, 33];

/// The largest image file accepted as cover art.
const MAX_COVER_SOURCE_BYTES: u64 = 32 * 1024 * 1024;

/// Reads cover art (any common image format) and fits it inside
/// `width` x `height`, centered on the menu's background color. Turns the
/// picture upright if its EXIF data says it was taken sideways, and blends
/// any transparency onto the background.
fn cover_image(source: &Path, width: u32, height: u32) -> Result<image::RgbImage> {
    if fs::metadata(source)?.len() > MAX_COVER_SOURCE_BYTES {
        bail!("Oversized cover input")
    }

    // Limits keep a huge or malicious image from using too much memory.
    let mut reader = image::ImageReader::open(source)?.with_guessed_format()?;
    let mut limits = image::Limits::default();
    limits.max_alloc = Some(128 * 1024 * 1024);
    limits.max_image_width = Some(8192);
    limits.max_image_height = Some(8192);
    reader.limits(limits);
    let image = apply_exif_orientation(source, reader.decode()?);

    let fitted = image
        .resize(width, height, image::imageops::FilterType::Lanczos3)
        .to_rgba8();
    let mut canvas = image::RgbImage::from_pixel(width, height, image::Rgb(COVER_BACKGROUND));
    let left = (width - fitted.width()) / 2;
    let top = (height - fitted.height()) / 2;

    for (x, y, pixel) in fitted.enumerate_pixels() {
        // Blend by alpha onto the background, rounding to nearest.
        let alpha = pixel[3] as u32;
        let rgb = std::array::from_fn(|channel| {
            let art = pixel[channel] as u32 * alpha;
            let background = COVER_BACKGROUND[channel] as u32 * (255 - alpha);
            ((art + background + 127) / 255) as u8
        });
        canvas.put_pixel(left + x, top + y, image::Rgb(rgb));
    }
    Ok(canvas)
}

/// Turns a photo upright according to its EXIF orientation tag (values 2-8
/// mean mirrored and/or rotated). Images without one are unchanged.
fn apply_exif_orientation(source: &Path, image: image::DynamicImage) -> image::DynamicImage {
    let Ok(file) = fs::File::open(source) else {
        return image;
    };
    let Ok(exif) = exif::Reader::new().read_from_container(&mut std::io::BufReader::new(file))
    else {
        return image;
    };
    let orientation = exif
        .get_field(exif::Tag::Orientation, exif::In::PRIMARY)
        .and_then(|field| field.value.get_uint(0));

    match orientation {
        Some(2) => image.fliph(),
        Some(3) => image.rotate180(),
        Some(4) => image.flipv(),
        Some(5) => image.rotate90().fliph(),
        Some(6) => image.rotate90(),
        Some(7) => image.rotate270().fliph(),
        Some(8) => image.rotate270(),
        _ => image,
    }
}

/// Makes a 96x120 PNG cover (the older cover format).
pub fn cover(source: &Path, target: &Path) -> Result<()> {
    let picture = cover_image(source, COVER_SIZE.0 as u32, COVER_SIZE.1 as u32)?;
    let mut bytes = std::io::Cursor::new(Vec::new());
    image::DynamicImage::ImageRgb8(picture).write_to(&mut bytes, image::ImageFormat::Png)?;
    f::atomic(target, bytes.get_ref())
}

/// Makes the menu's 96x120 cover, ready to show without decoding a PNG.
pub fn fast_cover(source: &Path, target: &Path) -> Result<()> {
    prepared_cover(source, target, COVER_SIZE)
}

/// The menu's full-screen cover: the same art at 192x240.
pub fn large_cover(source: &Path, target: &Path) -> Result<()> {
    prepared_cover(source, target, LARGE_COVER_SIZE)
}

/// Writes a cover in the menu's own format ("OM3DCOV2"): 15-bit pixels
/// (5 bits each of red, green and blue, big-endian), compressed with zlib.
/// All numbers are big-endian.
///
/// ```text
/// bytes  0-7   "OM3DCOV2"
/// bytes  8-11  width, height (16 bits each)
/// bytes 12-15  compressed size
/// bytes 16-19  CRC-32 of the compressed data
/// bytes 20-23  uncompressed size (width × height × 2)
/// bytes 24-    compressed pixels, then "END3DC02"
/// ```
fn prepared_cover(source: &Path, target: &Path, (width, height): (u16, u16)) -> Result<()> {
    let canvas = cover_image(source, width as u32, height as u32)?;

    let mut raw = Vec::with_capacity(width as usize * height as usize * 2);
    for pixel in canvas.pixels() {
        let red = pixel[0] as u16 >> 3;
        let green = pixel[1] as u16 >> 3;
        let blue = pixel[2] as u16 >> 3;
        let rgb555 = (red << 10) | (green << 5) | blue;
        raw.extend_from_slice(&rgb555.to_be_bytes());
    }

    let mut compressor = ZlibEncoder::new(Vec::new(), Compression::best());
    compressor.write_all(&raw)?;
    let compressed = compressor.finish()?;

    let mut bytes = b"OM3DCOV2".to_vec();
    bytes.extend_from_slice(&width.to_be_bytes());
    bytes.extend_from_slice(&height.to_be_bytes());
    bytes.extend_from_slice(&(compressed.len() as u32).to_be_bytes());
    bytes.extend_from_slice(&crc32fast::hash(&compressed).to_be_bytes());
    bytes.extend_from_slice(&(raw.len() as u32).to_be_bytes());
    bytes.extend_from_slice(&compressed);
    bytes.extend_from_slice(b"END3DC02");
    f::atomic(target, &bytes)
}

/// Reads a cover for showing in the app: a prepared `.3dc` cover, or any
/// common image format.
pub fn cover_display(path: &Path) -> Result<image::DynamicImage> {
    let is_prepared = path
        .extension()
        .is_some_and(|extension| extension.eq_ignore_ascii_case("3dc"));
    if !is_prepared {
        let mut reader = image::ImageReader::open(path)?.with_guessed_format()?;
        let mut limits = image::Limits::default();
        limits.max_alloc = Some(64 * 1024 * 1024);
        reader.limits(limits);
        return Ok(reader.decode()?);
    }

    validate(path)?;
    let data = fs::read(path)?;
    let width = u16::from_be_bytes(data[8..10].try_into()?);
    let height = u16::from_be_bytes(data[10..12].try_into()?);
    let raw = if data.starts_with(b"OM3DCOV2") {
        let mut decompressor = flate2::read::ZlibDecoder::new(&data[24..data.len() - 8]);
        let mut raw = Vec::new();
        decompressor.read_to_end(&mut raw)?;
        raw
    } else {
        data[24..].to_vec()
    };

    // Each 5-bit channel scaled up to 0-255.
    let mut image = image::RgbImage::new(width as u32, height as u32);
    for (pixel, bytes) in image.pixels_mut().zip(raw.chunks_exact(2)) {
        let value = u16::from_be_bytes([bytes[0], bytes[1]]);
        let channel = |shift: u16| (((value >> shift) & 31) * 255 / 31) as u8;
        *pixel = image::Rgb([channel(10), channel(5), channel(0)]);
    }
    Ok(image::DynamicImage::ImageRgb8(image))
}

// ---- In-memory previews (OM3DVID5) ----

/// RAM for a preview's video and audio together; matches tools/prepare-preview.py.
pub const PREVIEW_BYTES: usize = 1400 * 1024;

/// OM3DVID5: 160x120 at 30 fps, up to 15 seconds.
const PREVIEW_WIDTH: usize = 160;
const PREVIEW_HEIGHT: usize = 120;
const PREVIEW_FPS: usize = 30;
const PREVIEW_FRAMES: usize = 15 * PREVIEW_FPS;
/// One 160x120 frame of 24-bit RGB.
const PREVIEW_FRAME: usize = PREVIEW_WIDTH * PREVIEW_HEIGHT * 3;
/// Worst-case OM3DVID5 packet: every block GRAD with literal colors.
const PREVIEW_PACKET: usize = 1200 * 12;

/// The sound sample rate of every preview.
const AUDIO_RATE: usize = 11025;

extern "C" {
    // tools/preview-encoder.c, compiled in by build.rs
    fn oh3do_preview_encode(
        frames: *const u8,
        count: std::os::raw::c_uint,
        lambda_first: f64,
        split: std::os::raw::c_uint,
        lambda: f64,
        out: *mut u8,
        capacity: std::os::raw::c_ulong,
        decoded: *mut u8,
    ) -> std::os::raw::c_long;
}

/// The OM3DVID5 video section for 160x120 RGB24 frames, as close to
/// `budget` bytes as it can get without going over.
///
/// The encoder trades size for quality with a number called lambda: smaller
/// means better quality and a bigger file. This finds the smallest lambda
/// whose result fits. Size jumps between nearby lambdas, so the first frames
/// then use the next smaller lambda to fill what is left of the budget.
pub fn encode_preview(frames: &[u8], budget: usize, job: &Job) -> Result<Vec<u8>> {
    let count = frames.len() / PREVIEW_FRAME;
    if count == 0 || count > PREVIEW_FRAMES || !frames.len().is_multiple_of(PREVIEW_FRAME) {
        bail!("Invalid preview frames")
    }
    let mut out = vec![0u8; (count + 1) * 4 + count * PREVIEW_PACKET];

    // Encodes with lambda `first` for the first `split` frames and `rest`
    // for the others, and returns the size.
    let mut encoded_size = |first: f64, split: usize, rest: f64| -> Result<usize> {
        job.check()?;
        // SAFETY: frames holds count whole frames and out.len() bytes are writable.
        let size = unsafe {
            oh3do_preview_encode(
                frames.as_ptr(),
                count as std::os::raw::c_uint,
                first,
                split as std::os::raw::c_uint,
                rest,
                out.as_mut_ptr(),
                out.len() as std::os::raw::c_ulong,
                std::ptr::null_mut(),
            )
        };
        if size < 0 {
            bail!("Preview encoder failed (error {size})")
        }
        Ok(size as usize)
    };
    let mut fits = |lambda: f64| -> Result<bool> { Ok(encoded_size(lambda, 0, lambda)? <= budget) };

    // Step 1: from a typical lambda, step by 4x until one lambda fits
    // (`high`) and the next better one does not (`low`).
    const BEST_LAMBDA: f64 = 0.01;
    const WORST_LAMBDA: f64 = 100000.;
    let mut low = 0.5f64;
    let mut high = 0.5f64;
    if fits(high)? {
        loop {
            if low <= BEST_LAMBDA {
                // Even the best quality fits.
                low = high;
                break;
            }
            low = (low / 4.).max(BEST_LAMBDA);
            if !fits(low)? {
                break;
            }
            high = low;
        }
    } else {
        loop {
            low = high;
            high *= 4.;
            if high > WORST_LAMBDA {
                bail!("Preview cannot fit RAM budget")
            }
            if fits(high)? {
                break;
            }
        }
    }

    // Step 2: narrow the gap by halves (on a log scale) to within 2%.
    while high / low > 1.02 {
        let middle = (low * high).sqrt();
        if fits(middle)? {
            high = middle;
        } else {
            low = middle;
        }
    }

    // Step 3: use the better lambda for as many leading frames as fit.
    let mut split = 0;
    if low < high {
        let mut fitting = 0;
        let mut too_many = count + 1;
        while too_many - fitting > 1 {
            let middle = (fitting + too_many) / 2;
            if encoded_size(low, middle, high)? <= budget {
                fitting = middle;
            } else {
                too_many = middle;
            }
        }
        split = fitting;
    }

    let size = encoded_size(low, split, high)?;
    out.truncate(size);
    Ok(out)
}

// ---- Streamed previews (OM3DVID7) ----

/// OM3DVID7: streamed 320x240 at 30 fps, the whole clip up to a minute, at
/// about 200 KB/s (see src/preview_v7.h). The settings match the console's
/// reader and decoder: tools/prepare-preview.py uses the same ones.
const STREAM_WIDTH: usize = 320;
const STREAM_HEIGHT: usize = 240;
/// One 320x240 frame of 24-bit RGB.
const STREAM_FRAME: usize = STREAM_WIDTH * STREAM_HEIGHT * 3;
const STREAM_MAX_FRAMES: usize = 1800;

/// The streamed encoder's settings (the C struct of the same layout).
#[repr(C)]
struct Preview7Params {
    /// Target bytes per second.
    rate: u32,
    /// The drive's read speed the stream must keep within.
    read_rate: u32,
    /// The console's buffer for each chunk.
    slot_bytes: u32,
    max_chunk: u32,
    chunk_frames: u32,
    /// Dictionary sizes for 4x4 and 2x2 blocks.
    entries4: u32,
    entries2: u32,
    /// Decoding work per frame the console's CPU can afford.
    cycle_cap: u32,
}

const STREAM_PARAMS: Preview7Params = Preview7Params {
    rate: 200 * 1024,
    read_rate: 275 * 1024,
    slot_bytes: 640 * 1024,
    max_chunk: 160 * 1024,
    chunk_frames: 10,
    entries4: 8192,
    entries2: 4096,
    cycle_cap: 230_000,
};

/// Part of the cached file's name: a change of format or settings re-encodes.
pub const PREVIEW_KEY: &str = "om3dvid7-320x240-30fps-full-200k-160c-percept-lead";

/// Part of a converted cover's cached name, likewise.
const COVER_KEY: &str = "native-v3-160x120-fast96-cap15-512k";

/// A source that does not open from black gets this many frames fading in
/// from black to its first frame, which gives the first picture's dictionary
/// entries a chunk to arrive in (tools/prepare-preview.py does the same).
const STREAM_LEAD_FRAMES: usize = 12;

/// A first frame darker than this mean value already opens from black.
const DARK_FRAME: u64 = 6;

/// The fade-in for a source whose first frame is `first`, or none.
fn lead_frames(first: &[u8]) -> Vec<Vec<u8>> {
    let total: u64 = first.iter().map(|&value| value as u64).sum();
    if total < DARK_FRAME * first.len() as u64 {
        return vec![];
    }

    (0..STREAM_LEAD_FRAMES)
        .map(|step| {
            first
                .iter()
                .map(|&value| (value as usize * step / STREAM_LEAD_FRAMES) as u8)
                .collect()
        })
        .collect()
}

extern "C" {
    // tools/preview7-encoder.c, compiled in by build.rs
    fn oh3do_preview7_workspace(frames: u32) -> u64;
    fn oh3do_preview7_begin(workspace: *mut u8, bytes: u64, frames: u32) -> i32;
    fn oh3do_preview7_frame(workspace: *mut u8, rgb: *const u8) -> i32;
    fn oh3do_preview7_encode(
        workspace: *mut u8,
        params: *const Preview7Params,
        audio: *const i8,
        samples: u64,
        out: *mut u8,
        capacity: u64,
        stats: *mut u32,
    ) -> i64;
}

/// Physical memory free now, or 8 GiB where the platform will not say.
fn memory_available() -> u64 {
    #[cfg(target_os = "linux")]
    if let Ok(text) = fs::read_to_string("/proc/meminfo") {
        let available_kib = text
            .lines()
            .find(|line| line.starts_with("MemAvailable:"))
            .and_then(|line| line.split_whitespace().nth(1)?.parse::<u64>().ok());
        if let Some(kib) = available_kib {
            return kib * 1024;
        }
    }

    #[cfg(windows)]
    {
        /// Windows' MEMORYSTATUSEX.
        #[repr(C)]
        struct MemoryStatus {
            length: u32,
            load: u32,
            total_phys: u64,
            avail_phys: u64,
            total_page: u64,
            avail_page: u64,
            total_virtual: u64,
            avail_virtual: u64,
            avail_extended: u64,
        }
        extern "system" {
            fn GlobalMemoryStatusEx(status: *mut MemoryStatus) -> i32;
        }
        let mut status = MemoryStatus {
            length: std::mem::size_of::<MemoryStatus>() as u32,
            load: 0,
            total_phys: 0,
            avail_phys: 0,
            total_page: 0,
            avail_page: 0,
            total_virtual: 0,
            avail_virtual: 0,
            avail_extended: 0,
        };
        // SAFETY: status is a MEMORYSTATUSEX with its length set.
        if unsafe { GlobalMemoryStatusEx(&mut status) } != 0 {
            return status.avail_phys;
        }
    }

    8 << 30
}

/// How many previews may encode at once, and how many are.
struct EncodeCount {
    running: usize,
    /// 0 until the first encode sets it.
    limit: usize,
}

/// The encode count, and a signal for when an encode finishes.
static ENCODES: (std::sync::Mutex<EncodeCount>, std::sync::Condvar) = (
    std::sync::Mutex::new(EncodeCount {
        running: 0,
        limit: 0,
    }),
    std::sync::Condvar::new(),
);

/// Memory to allow for each encode: a minute-long clip needs about 600 MB
/// of workspace, plus room for everything else.
const MEMORY_PER_ENCODE: u64 = 900 << 20;

/// Permission to run one preview encode, given back when dropped.
///
/// Encodes at once are limited by memory. The limit is set once, from the
/// memory free when the first encode starts, between 1 and 16.
struct EncodeSlot;

impl EncodeSlot {
    /// Waits for a free slot (or for the job to be canceled).
    fn take(job: &Job) -> Result<EncodeSlot> {
        let mut count = ENCODES.0.lock().unwrap();
        if count.limit == 0 {
            count.limit = ((memory_available() / MEMORY_PER_ENCODE) as usize).clamp(1, 16);
        }
        while count.running >= count.limit {
            job.check()?;
            count = ENCODES
                .1
                .wait_timeout(count, Duration::from_millis(200))
                .unwrap()
                .0;
        }
        count.running += 1;
        Ok(EncodeSlot)
    }
}

impl Drop for EncodeSlot {
    fn drop(&mut self) {
        ENCODES.0.lock().unwrap().running -= 1;
        ENCODES.1.notify_one();
    }
}

/// Sound samples covering `frames` frames of video.
fn samples_for_frames(frames: usize) -> usize {
    frames * AUDIO_RATE / PREVIEW_FPS
}

/// Encodes the `count` frames added to a workspace, with signed 8-bit
/// 11025 Hz sound for exactly those frames, if any.
fn encode_workspace(workspace: &mut [u8], count: usize, audio: Option<&[u8]>) -> Result<Vec<u8>> {
    let samples = samples_for_frames(count);
    if audio.is_some_and(|audio| audio.len() != samples) {
        bail!("Preview sound does not match its frames")
    }

    // Room for 10% over the target rate, plus 2 MiB.
    let capacity = STREAM_PARAMS.rate as usize * count / PREVIEW_FPS * 11 / 10 + (2 << 20);
    let mut out = vec![0u8; capacity];
    let mut stats = [0u32; 8];
    let audio_pointer = audio.map_or(std::ptr::null(), |audio| audio.as_ptr() as *const i8);
    let audio_samples = audio.map_or(0, |_| samples as u64);

    // SAFETY: the workspace holds count frames; audio holds samples bytes;
    // out holds capacity writable bytes; stats has room for 8 words.
    let size = unsafe {
        oh3do_preview7_encode(
            workspace.as_mut_ptr(),
            &STREAM_PARAMS,
            audio_pointer,
            audio_samples,
            out.as_mut_ptr(),
            capacity as u64,
            stats.as_mut_ptr(),
        )
    };
    if size < 0 {
        bail!("Preview encoder failed (error {size})")
    }
    out.truncate(size as usize);
    Ok(out)
}

/// A streamed preview of 320x240 RGB24 frames already in memory (tests and
/// tools; preview() streams frames from FFmpeg instead).
pub fn encode_stream(frames: &[u8], audio: Option<&[u8]>) -> Result<Vec<u8>> {
    let count = frames.len() / STREAM_FRAME;
    if count == 0 || count > STREAM_MAX_FRAMES || !frames.len().is_multiple_of(STREAM_FRAME) {
        bail!("Invalid preview frames")
    }

    let mut workspace = new_workspace(count)?;
    for frame in frames.chunks(STREAM_FRAME) {
        add_frame(&mut workspace, frame)?;
    }
    encode_workspace(&mut workspace, count, audio)
}

/// A streamed encoder's workspace with room for `frames` frames.
fn new_workspace(frames: usize) -> Result<Vec<u8>> {
    // SAFETY: plain arithmetic in the encoder.
    let bytes = unsafe { oh3do_preview7_workspace(frames as u32) } as usize;
    let mut workspace = vec![0u8; bytes];
    // SAFETY: workspace holds bytes writable bytes.
    let status =
        unsafe { oh3do_preview7_begin(workspace.as_mut_ptr(), bytes as u64, frames as u32) };
    if status != 0 {
        bail!("Preview encoder setup failed")
    }
    Ok(workspace)
}

/// Adds one 320x240 RGB24 frame to a workspace.
fn add_frame(workspace: &mut [u8], frame: &[u8]) -> Result<()> {
    // SAFETY: frame is one whole picture; the workspace was begun with
    // room for the frames added to it.
    if unsafe { oh3do_preview7_frame(workspace.as_mut_ptr(), frame.as_ptr()) } != 0 {
        bail!("Preview encoder rejected a frame")
    }
    Ok(())
}

/// What ffprobe says about a video clip.
struct ClipInfo {
    has_sound: bool,
    /// How much of it to use: its length, at most a minute (60 seconds if
    /// it does not say).
    seconds: f64,
}

/// Asks ffprobe about a clip. Refuses one with no video.
fn probe_clip(ffprobe: &Path, source: &Path, job: &Job) -> Result<ClipInfo> {
    let mut probe_args = args(&[
        "-v",
        "error",
        "-show_streams",
        "-show_format",
        "-of",
        "json",
    ]);
    probe_args.push(source.to_string_lossy().into_owned());
    let info: Value = serde_json::from_slice(&capture(ffprobe, &probe_args, 1024 * 1024, job)?)?;

    let streams = info["streams"].as_array().context("No video streams")?;
    if !streams.iter().any(|stream| stream["codec_type"] == "video") {
        bail!("Source has no video stream")
    }
    let has_sound = streams.iter().any(|stream| stream["codec_type"] == "audio");

    let longest = (STREAM_MAX_FRAMES / PREVIEW_FPS) as f64;
    let seconds = info["format"]["duration"]
        .as_str()
        .and_then(|text| text.parse::<f64>().ok())
        .filter(|seconds| seconds.is_finite() && *seconds > 0.)
        .unwrap_or(60.)
        .min(longest);

    Ok(ClipInfo { has_sound, seconds })
}

/// FFmpeg arguments that turn the first `seconds` of a clip's first video
/// stream into at most `frames` raw 320x240 RGB24 frames at 30 fps, scaled
/// to fit and centered on the menu's background color.
fn video_arguments(source: &Path, seconds: f64, frames: usize) -> Vec<String> {
    let filter = format!(
        "fps={PREVIEW_FPS},scale={STREAM_WIDTH}:{STREAM_HEIGHT}:force_original_aspect_ratio=decrease:flags=area,\
         pad={STREAM_WIDTH}:{STREAM_HEIGHT}:(ow-iw)/2:(oh-ih)/2:color=0x081021"
    );
    let mut arguments = args(&["-v", "error", "-nostdin", "-i"]);
    arguments.push(source.to_string_lossy().into_owned());
    arguments.extend(args(&[
        "-map",
        "0:v:0",
        "-an",
        "-sn",
        "-t",
        &seconds.to_string(),
        "-vf",
        &filter,
        "-frames:v",
        &frames.to_string(),
        "-pix_fmt",
        "rgb24",
        "-f",
        "rawvideo",
        "pipe:1",
    ]));
    arguments
}

/// FFmpeg arguments that turn the first `seconds` of a clip's first sound
/// stream into raw signed 8-bit mono at 11025 Hz, a little quieter (to
/// avoid clipping) and with 10 ms fades at each end (to avoid clicks).
fn audio_arguments(source: &Path, seconds: f64) -> Vec<String> {
    let fade_out_start = (seconds - 0.01).max(0.);
    let filter = format!("volume=0.8,afade=t=in:d=0.01,afade=t=out:st={fade_out_start}:d=0.01");
    let mut arguments = args(&["-v", "error", "-nostdin", "-i"]);
    arguments.push(source.to_string_lossy().into_owned());
    arguments.extend(args(&[
        "-map",
        "0:a:0",
        "-vn",
        "-sn",
        "-t",
        &seconds.to_string(),
        "-ac",
        "1",
        "-ar",
        "11025",
        "-af",
        &filter,
        "-f",
        "s8",
        "pipe:1",
    ]));
    arguments
}

/// Converts a video clip into a streamed preview at `target`, with FFmpeg
/// (found as [`video_exes`] says).
///
/// Frames go straight from FFmpeg into the encoder's workspace, so a long
/// clip is never held in memory as raw frames. A clip that does not open
/// from black gets a short fade-in first (see [`lead_frames`]); its sound,
/// if any, is delayed to match.
pub fn preview(source: &Path, target: &Path, prefs: &Preferences, job: &Job) -> Result<()> {
    let (ffmpeg, ffprobe) = video_exes(prefs)?;
    let clip = probe_clip(&ffprobe, source, job)?;

    // Frames to ask FFmpeg for (two spare, in case the length is rounded
    // down), and room in the workspace for those plus a fade-in.
    let wanted_frames =
        ((clip.seconds * PREVIEW_FPS as f64).ceil() as usize + 2).min(STREAM_MAX_FRAMES);
    let room = (wanted_frames + STREAM_LEAD_FRAMES).min(STREAM_MAX_FRAMES);

    let _slot = EncodeSlot::take(job)?;
    let mut workspace = new_workspace(room)?;

    job.say("Reading preview frames");
    let mut lead = 0;
    let mut count = 0;
    let arguments = video_arguments(source, clip.seconds, wanted_frames);
    stream(
        &ffmpeg,
        &arguments,
        STREAM_FRAME,
        wanted_frames,
        job,
        |frame| {
            let mut pictures = if count == 0 {
                lead_frames(frame)
            } else {
                vec![]
            };
            lead += pictures.len();
            pictures.push(frame.to_vec());

            for picture in pictures {
                if count == room {
                    break;
                }
                add_frame(&mut workspace, &picture)?;
                count += 1;
            }
            Ok(())
        },
    )?;
    if count == lead {
        bail!("Incomplete video frames")
    }

    let audio = if clip.has_sound {
        read_audio(&ffmpeg, source, count, lead, job)?
    } else {
        vec![]
    };

    job.say(format!("Encoding preview: {count} frames"));
    let encoded = encode_workspace(&mut workspace, count, clip.has_sound.then_some(&audio[..]))?;
    drop(workspace);
    f::atomic(target, &encoded)?;

    job.say(format!(
        "Preview: {:.1} seconds at {PREVIEW_FPS} fps{}, {} KB",
        count as f64 / PREVIEW_FPS as f64,
        if clip.has_sound { " with audio" } else { "" },
        encoded.len() / 1024
    ));
    Ok(())
}

/// Reads a clip's sound for a preview of `count` frames whose first `lead`
/// frames are a fade-in: silence during the fade-in, then the clip's sound,
/// padded with silence to exactly the preview's length.
fn read_audio(
    ffmpeg: &Path,
    source: &Path,
    count: usize,
    lead: usize,
    job: &Job,
) -> Result<Vec<u8>> {
    let samples = samples_for_frames(count);
    let clip_frames = count - lead;
    let seconds = clip_frames as f64 / PREVIEW_FPS as f64;
    let sound = capture(
        ffmpeg,
        &audio_arguments(source, seconds),
        samples + 8192,
        job,
    )?;

    let silence = samples - samples_for_frames(clip_frames);
    let mut audio = vec![0; silence];
    audio.extend_from_slice(&sound[..sound.len().min(samples - silence)]);
    audio.resize(samples, 0);
    Ok(audio)
}

// ---- Finding media for a game ----

/// Every file under `root` with one of `extensions` (lowercase), sorted.
/// Links are skipped. Empty if the folder does not exist.
fn files(root: &Path, extensions: &[&str]) -> Result<Vec<PathBuf>> {
    if !root.is_dir() {
        return Ok(vec![]);
    }

    let mut paths = vec![];
    let entries = walkdir::WalkDir::new(root)
        .follow_links(false)
        .into_iter()
        .filter_entry(|entry| !f::is_link(entry.path()));
    for entry in entries {
        let entry = entry?;
        let extension = entry
            .path()
            .extension()
            .unwrap_or_default()
            .to_string_lossy()
            .to_ascii_lowercase();
        if entry.file_type().is_file() && extensions.contains(&extension.as_str()) {
            paths.push(entry.path().to_owned());
        }
    }
    paths.sort();
    Ok(paths)
}

/// The cover files in `paths` that belong to a game (see [`media_matches`]).
pub fn matches(title: &str, source: &Path, paths: &[PathBuf]) -> Vec<PathBuf> {
    media_matches(title, source, paths, false)
}

/// The preview clips in `paths` that belong to a game. A clip from another
/// region is accepted when there is none for the game's own region.
pub fn preview_matches(title: &str, source: &Path, paths: &[PathBuf]) -> Vec<PathBuf> {
    media_matches(title, source, paths, true)
}

/// The files in `paths` named for a game with title `title` and disc file
/// `source`:
///
/// 1. Files named exactly like the title, or else like the disc file
///    (ignoring letter case).
/// 2. Otherwise, files with the same title key (see [`d::title_key`]), the
///    same kind (game, demo, trial or sampler), no disc number or the same
///    one, and a compatible region (none given, or one in common).
/// 3. With `other_regions`, if no region is compatible, those from any
///    region.
///
/// More than one result means the choice is ambiguous.
fn media_matches(
    title: &str,
    source: &Path,
    paths: &[PathBuf],
    other_regions: bool,
) -> Vec<PathBuf> {
    let stem = |path: &Path| {
        path.file_stem()
            .unwrap_or_default()
            .to_string_lossy()
            .into_owned()
    };

    for name in [title.to_owned(), stem(source)] {
        let exact: Vec<PathBuf> = paths
            .iter()
            .filter(|path| stem(path).eq_ignore_ascii_case(&name))
            .cloned()
            .collect();
        if !exact.is_empty() {
            return exact;
        }
    }

    let kind = |name: &str| {
        rx(r"(?i)\b(demo|demonstration|trial|sampler)\b")
            .captures(name)
            .map(|captures| captures[1].to_lowercase())
            .unwrap_or_else(|| "game".into())
    };
    let same_game: Vec<PathBuf> = paths
        .iter()
        .filter(|path| {
            let name = stem(path);
            let disc = d::disc_number(&name);
            d::title_key(&name) == d::title_key(title)
                && kind(&name) == kind(title)
                && (disc.is_none() || disc == d::disc_number(title))
        })
        .cloned()
        .collect();
    let same_region: Vec<PathBuf> = same_game
        .iter()
        .filter(|path| {
            let regions = d::regions(&stem(path));
            regions.is_empty() || !regions.is_disjoint(&d::regions(title))
        })
        .cloned()
        .collect();

    // A regional fallback never resolves ambiguity among compatible clips.
    if same_region.is_empty() && other_regions {
        same_game
    } else {
        same_region
    }
}

/// The image formats accepted as cover art.
const COVER_EXTENSIONS: [&str; 8] = ["png", "jpg", "jpeg", "webp", "bmp", "gif", "tiff", "tif"];

/// The video formats accepted as preview clips.
const CLIP_EXTENSIONS: [&str; 5] = ["mp4", "avi", "mov", "mkv", "webm"];

/// EmuMovies' video folders, from lowest to highest quality.
const CLIP_FOLDERS: [&str; 3] = ["Video_MP4", "Video_MP4_HI_QUAL", "Video_MP4_HD"];

/// The cover art ("cover") or clips (anything else) in the EmuMovies folder
/// chosen in the preferences. For clips with the same name in several
/// quality folders, only the highest quality ones are kept.
fn read_sources(prefs: &Preferences, kind: &str) -> Result<Vec<PathBuf>> {
    if prefs.emumovies.is_empty() {
        return Ok(vec![]);
    }
    let mut root = PathBuf::from(&prefs.emumovies);
    if root.join("Panasonic_3DO").is_dir() {
        root.push("Panasonic_3DO");
    }

    if kind == "cover" {
        return files(&root.join("Box"), &COVER_EXTENSIONS);
    }

    // Later (higher quality) folders replace earlier ones' clips of the same
    // name.
    let mut clips: HashMap<String, Vec<PathBuf>> = HashMap::new();
    for folder in CLIP_FOLDERS {
        let mut by_name: HashMap<String, Vec<PathBuf>> = HashMap::new();
        for path in files(&root.join(folder), &CLIP_EXTENSIONS)? {
            let name = path.file_stem().unwrap().to_string_lossy().to_lowercase();
            by_name.entry(name).or_default().push(path);
        }
        clips.extend(by_name);
    }
    Ok(clips.into_values().flatten().collect())
}

/// [`read_sources`], remembered for the rest of the job.
fn sources(prefs: &Preferences, kind: &str, job: &Job) -> Result<Vec<PathBuf>> {
    let key = format!("{}:{kind}", prefs.emumovies);
    if let Some(paths) = job.media_sources.lock().unwrap().get(&key) {
        return Ok(paths.clone());
    }
    let paths = read_sources(prefs, kind)?;
    job.media_sources.lock().unwrap().insert(key, paths.clone());
    Ok(paths)
}

/// Where downloaded box art from `url` is cached.
fn artwork_cache_path(cache: &Path, url: &str) -> PathBuf {
    let key = format!("{:x}", Sha256::digest(url.as_bytes()));
    cache.join("artwork").join(format!("{key}.png"))
}

/// Downloads box art for `title` from the libretro thumbnail server, if
/// exactly one image there fits it. Returns the downloaded file and its URL.
fn download_cover(title: &str, cache: &Path, job: &Job) -> Result<Option<(PathBuf, String)>> {
    // The server's folder listing, cached.
    let list = cache.join("cover-list.html");
    catalog::download(catalog::ART, &list, 16 * 1024 * 1024, job)?;
    let html = fs::read_to_string(list)?;
    let names: Vec<String> = rx(r#"href="([^"]+\.png)""#)
        .captures_iter(&html)
        .filter_map(|captures| {
            urlencoding::decode(&captures[1])
                .ok()
                .map(|name| name.into_owned())
        })
        .filter(|name| !name.contains(['/', '\\']) && !name.starts_with('.'))
        .collect();

    let Some(name) = choose_cover(title, names) else {
        return Ok(None);
    };
    let url = format!("{}{}", catalog::ART, urlencoding::encode(&name));
    let path = artwork_cache_path(cache, &url);
    catalog::download(&url, &path, 32 * 1024 * 1024, job)?;
    Ok(Some((path, url)))
}

/// The box art name (from `names`) for `title`: the only exact match, or
/// else the only one with the same title key and a region in common. If
/// several fit, the only one for disc 1 (the set's box). None if it is
/// still ambiguous or nothing fits.
fn choose_cover(title: &str, names: Vec<String>) -> Option<String> {
    let stem = |name: &str| {
        Path::new(name)
            .file_stem()
            .unwrap()
            .to_string_lossy()
            .into_owned()
    };

    let exact: Vec<String> = names
        .iter()
        .filter(|name| stem(name).eq_ignore_ascii_case(title))
        .cloned()
        .collect();
    let candidates: Vec<String> = if !exact.is_empty() {
        exact
    } else {
        names
            .into_iter()
            .filter(|name| {
                let stem = stem(name);
                d::title_key(&stem) == d::title_key(title)
                    && !d::regions(&stem).is_disjoint(&d::regions(title))
            })
            .collect()
    };

    let first_disc: Vec<&String> = candidates
        .iter()
        .filter(|name| d::disc_number(name) == Some(1))
        .collect();
    if candidates.len() == 1 {
        Some(candidates[0].clone())
    } else if first_disc.len() == 1 {
        Some(first_disc[0].clone())
    } else {
        None
    }
}

/// Prepares a cover and preview for an entry being imported (see
/// [`prepare_missing`]).
pub fn prepare_entry(
    entry: &mut Entry,
    prefs: &Preferences,
    cache: &Path,
    out: &mut Outputs,
    job: &Job,
) -> Result<()> {
    prepare_missing(entry, prefs, cache, out, true, true, job)
}

/// Finds the original file a prepared cover or preview was made from, given
/// the `origin` recorded for it: a box art URL (downloaded again if needed,
/// unless offline), a full path, or (from older versions) a path relative to
/// the EmuMovies folder.
pub fn original_source(
    kind: &str,
    origin: &str,
    prefs: &Preferences,
    cache: &Path,
    job: &Job,
) -> Result<Option<PathBuf>> {
    if kind == "cover" && origin.starts_with("https://") {
        let path = artwork_cache_path(cache, origin);
        if !path.is_file() && !prefs.offline {
            catalog::download(origin, &path, 32 * 1024 * 1024, job)?;
        }
        return Ok(path.is_file().then_some(path));
    }

    let is_usable = |path: &Path| path.is_file() && !f::is_link(path);
    let path = PathBuf::from(origin);
    if path.is_absolute() {
        return Ok(is_usable(&path).then_some(path));
    }

    if !prefs.emumovies.is_empty() {
        // Older tools saved paths relative to the selected media folder.
        let root = PathBuf::from(&prefs.emumovies);
        let kind_folder = if kind == "cover" {
            "Box"
        } else {
            "Video_MP4_HI_QUAL"
        };
        for base in [&root, &root.join("Panasonic_3DO")] {
            for candidate in [base.join(origin), base.join(kind_folder).join(origin)] {
                if is_usable(&candidate) {
                    return Ok(Some(candidate));
                }
            }
        }
    }
    Ok(None)
}

/// Prepares the cover (if `want_cover`) and preview (if `want_preview`) for
/// an entry, adding them to `out`.
///
/// Uses the file the person chose, or else the only matching file in the
/// EmuMovies folder, or else (covers only, if allowed and online) box art
/// downloaded from libretro. A conversion that fails is reported and
/// skipped; the import goes on without that media.
pub fn prepare_missing(
    entry: &mut Entry,
    prefs: &Preferences,
    cache: &Path,
    out: &mut Outputs,
    want_cover: bool,
    want_preview: bool,
    job: &Job,
) -> Result<()> {
    for (kind, wanted) in [("cover", want_cover), ("preview", want_preview)] {
        if !wanted {
            continue;
        }
        job.check()?;

        let Some((source, origin)) = find_media_source(entry, kind, prefs, cache, job)? else {
            continue;
        };
        match convert_media(kind, &source, origin, prefs, cache, job) {
            Ok(prepared) => {
                out.insert((entry.target.clone(), kind.into()), prepared);
            }
            Err(error) => {
                job.check()?;
                job.say(format!("{kind} skipped: {error:#}"));
            }
        }
    }
    Ok(())
}

/// The file to make an entry's cover or preview (`kind`) from, and the
/// origin to record for it (see [`prepare_missing`]). None if there is no
/// single obvious choice.
fn find_media_source(
    entry: &Entry,
    kind: &str,
    prefs: &Preferences,
    cache: &Path,
    job: &Job,
) -> Result<Option<(PathBuf, String)>> {
    let chosen = if kind == "cover" {
        entry.cover.clone()
    } else {
        entry.preview.clone()
    };
    if let Some(path) = chosen {
        let origin = path.to_string_lossy().into_owned();
        return Ok(Some((path, origin)));
    }

    let paths = sources(prefs, kind, job)?;
    let candidates = if kind == "preview" {
        preview_matches(&entry.title, &entry.source, &paths)
    } else {
        matches(&entry.title, &entry.source, &paths)
    };
    if candidates.len() == 1 {
        let origin = candidates[0].to_string_lossy().into_owned();
        return Ok(Some((candidates[0].clone(), origin)));
    }

    if kind == "cover" && prefs.download_covers && !prefs.offline {
        return match download_cover(&entry.title, cache, job) {
            Ok(found) => Ok(found),
            Err(error) => {
                job.say(format!("Cover download skipped: {error:#}"));
                Ok(None)
            }
        };
    }
    Ok(None)
}

/// Converts a cover or preview (`kind`) from `source`, or reuses the cached
/// conversion. Covers also get their full-screen version.
fn convert_media(
    kind: &str,
    source: &Path,
    origin: String,
    prefs: &Preferences,
    cache: &Path,
    job: &Job,
) -> Result<Prepared> {
    if f::is_link(source) || !source.is_file() {
        bail!("Choose a real media file")
    }
    job.say(format!("Preparing {kind}: {}", source.display()));

    // Cached by the source's checksum and the conversion settings.
    let settings = if kind == "cover" {
        COVER_KEY
    } else {
        PREVIEW_KEY
    };
    let key_text = format!("{}:{kind}:{settings}", f::sha256(source, job)?);
    let key = format!("{:x}", Sha256::digest(key_text.as_bytes()));
    let extension = if kind == "cover" { "3dc" } else { "3dv" };
    let converted = cache.join("converted-native");
    let target = converted.join(format!("{key}.{extension}"));

    if !target.exists() {
        if kind == "cover" {
            fast_cover(source, &target)?;
        } else {
            preview(source, &target, prefs, job)?;
        }
    }
    validate(&target)?;

    let large = if kind == "cover" {
        let large = converted.join(format!("{key}-large.3dc"));
        if !large.exists() {
            large_cover(source, &large)?;
        }
        validate(&large)?;
        Some(large)
    } else {
        None
    };

    let name = format!("{kind}-{}.{extension}", &f::sha256(&target, job)?[..24]);
    Ok(Prepared {
        name,
        path: target,
        source: origin,
        large,
    })
}

// ---- Checking prepared files ----

/// Preview payload check, as in the menu's src/usb_media.c: over big-endian
/// words, a += w; b += a; the result is a ^ (b rotated by 16). It costs the
/// console about a tenth of CRC32.
pub fn word_sum(data: &[u8]) -> u32 {
    let mut a = 0u32;
    let mut b = 0u32;
    for word in data.chunks(4) {
        // A short last word is padded with zeros.
        let mut bytes = [0u8; 4];
        bytes[..word.len()].copy_from_slice(word);
        a = a.wrapping_add(u32::from_be_bytes(bytes));
        b = b.wrapping_add(a);
    }
    a ^ b.rotate_left(16)
}

/// The big-endian 32-bit number at `position`.
fn be(data: &[u8], position: usize) -> Result<u32> {
    let bytes = data
        .get(position..position + 4)
        .context("Truncated media")?;
    Ok(u32::from_be_bytes(bytes.try_into()?))
}

/// The big-endian 16-bit number at `position`.
fn be16(data: &[u8], position: usize) -> usize {
    u16::from_be_bytes([data[position], data[position + 1]]) as usize
}

/// Checks zlib data decompresses to exactly `expected` bytes and uses all
/// of its input.
fn inflate(data: &[u8], expected: usize) -> Result<()> {
    let mut decompressor = flate2::Decompress::new(true);
    let mut out = vec![0; expected + 1];
    let status = decompressor.decompress(data, &mut out, flate2::FlushDecompress::Finish)?;
    if status != flate2::Status::StreamEnd
        || decompressor.total_out() != expected as u64
        || decompressor.total_in() != data.len() as u64
    {
        bail!("Invalid compressed frame")
    }
    Ok(())
}

/// Checks a prepared media file as the menu would: a PNG cover, a prepared
/// cover (OM3DCOV2), or a preview (OM3DVID7 or OM3DVID5).
pub fn validate(path: &Path) -> Result<()> {
    let extension = path
        .extension()
        .unwrap_or_default()
        .to_string_lossy()
        .to_ascii_lowercase();
    let size_limit = if extension == "3dv" { 16384 } else { 1024 } * 1024 + 48;
    if fs::metadata(path)?.len() > size_limit {
        bail!("Oversized media")
    }
    let data = fs::read(path)?;

    if extension == "png" {
        return validate_png(&data);
    }

    if data.len() < 24 {
        bail!("Invalid media dimensions")
    }
    if data.starts_with(b"OM3DCOV2") {
        let large = path
            .to_string_lossy()
            .to_ascii_lowercase()
            .ends_with("-large.3dc");
        return validate_cover(&data, large);
    }
    if data.starts_with(b"OM3DVID7") {
        return validate_stream(&data);
    }
    validate_preview_v5(&data)
}

/// Checks a PNG cover: the signature, every chunk's length and CRC, an
/// IEND chunk at the very end, and a 96x120 picture that decodes.
fn validate_png(data: &[u8]) -> Result<()> {
    if data.len() > 512 * 1024 || !data.starts_with(b"\x89PNG\r\n\x1a\n") {
        bail!("Invalid PNG")
    }

    // Each chunk: length (4), type (4), data (length), CRC of type and data (4).
    let mut position = 8;
    let mut ended = false;
    while position + 12 <= data.len() {
        let length = be(data, position)? as usize;
        if position + 12 + length > data.len() {
            bail!("Truncated PNG")
        }
        let type_and_data = &data[position + 4..position + 8 + length];
        if crc32fast::hash(type_and_data) != be(data, position + 8 + length)? {
            bail!("PNG checksum mismatch")
        }
        if &data[position + 4..position + 8] == b"IEND" {
            if length != 0 || position + 12 != data.len() {
                bail!("Invalid PNG end")
            }
            ended = true;
            break;
        }
        position += length + 12;
    }

    let image = image::load_from_memory_with_format(data, image::ImageFormat::Png)?;
    let size = (image.width(), image.height());
    if !ended || size != (COVER_SIZE.0 as u32, COVER_SIZE.1 as u32) {
        bail!("Resize covers to 96x120 PNG")
    }
    Ok(())
}

/// Checks a prepared cover (see [`prepared_cover`]): the right size for a
/// small or `large` cover, consistent sizes, the end marker, the CRC, and
/// that it decompresses to exactly the right number of pixels.
fn validate_cover(data: &[u8], large: bool) -> Result<()> {
    let width = be16(data, 8);
    let height = be16(data, 10);
    let compressed = be(data, 12)? as usize;
    let (expected_size, limit) = if large {
        ((192, 240), 192 * 240 * 2 + 1024)
    } else {
        ((96, 120), 32768)
    };

    if (width, height) != expected_size
        || compressed == 0
        || compressed > limit
        || data.len() != 32 + compressed
        || be(data, 20)? as usize != width * height * 2
        || &data[24 + compressed..] != b"END3DC02"
        || crc32fast::hash(&data[24..24 + compressed]) != be(data, 16)?
    {
        bail!("Invalid prepared cover")
    }
    inflate(&data[24..24 + compressed], width * height * 2)
}

/// Checks an in-memory preview ("OM3DVID5"). All numbers are big-endian.
///
/// ```text
/// bytes  0-7   "OM3DVID5"
/// bytes  8-11  width, height (16 bits each): 160, 120
/// bytes 12-15  payload size (video, padding to 4 bytes, then audio)
/// bytes 16-19  word_sum of the payload
/// bytes 20-23  frame count, frames per second (16 bits each)
/// bytes 24-39  video size, audio size, sample rate, sample count
/// bytes 40-    payload: a frame table (offsets of each frame, then the
///              end), the frames, padding, the sound; then "END3DV05"
/// ```
fn validate_preview_v5(data: &[u8]) -> Result<()> {
    if !data.starts_with(b"OM3DVID5") || data.len() < 48 {
        bail!("Invalid preview signature")
    }
    let width = be16(data, 8);
    let height = be16(data, 10);
    let payload = be(data, 12)? as usize;
    let checksum = be(data, 16)?;
    let frames = be16(data, 20);
    let fps = be16(data, 22);

    if data.len() != 40 + payload + 8
        || data[data.len() - 8..] != *b"END3DV05"
        || !payload.is_multiple_of(4)
        || word_sum(&data[40..data.len() - 8]) != checksum
    {
        bail!("Invalid preview bounds/checksum")
    }
    if (width, height, fps) != (PREVIEW_WIDTH, PREVIEW_HEIGHT, PREVIEW_FPS)
        || frames == 0
        || frames > PREVIEW_FRAMES
    {
        bail!("Invalid preview size, rate or length")
    }

    let video = be(data, 24)? as usize;
    let audio = be(data, 28)? as usize;
    let rate = be(data, 32)?;
    let samples = be(data, 36)? as usize;
    // The sound for every frame, rounded up to whole 4-byte words.
    let expected_samples = ((frames * AUDIO_RATE).div_ceil(fps) + 3) & !3;
    let padding = video.next_multiple_of(4) - video;
    // Only looked at once the sizes are known to add up, so the slice is in
    // bounds.
    let padding_is_zero = || {
        data[40 + payload - padding..40 + payload]
            .iter()
            .all(|&byte| byte == 0)
    };
    let sound_consistent = if audio > 0 {
        rate == AUDIO_RATE as u32 && samples == audio && samples == expected_samples
    } else {
        samples == 0 && rate == 0
    };
    if payload > 4096 * 1024
        || video + padding + audio != payload
        || !padding_is_zero()
        || video <= (frames + 1) * 4
        || !sound_consistent
    {
        bail!("Invalid preview audio or RAM bounds")
    }

    // The frame table comes first; packets are checked by the menu's decoder
    // as they play. Offsets start just after the table, rise, and end at the
    // video's end.
    let table = &data[40..40 + (frames + 1) * 4];
    if be(table, 0)? as usize != (frames + 1) * 4 || be(table, frames * 4)? as usize != video {
        bail!("Invalid frame table")
    }
    for frame in 0..frames {
        if be(table, frame * 4)? >= be(table, (frame + 1) * 4)? {
            bail!("Invalid frame offset")
        }
    }
    Ok(())
}

/// The streamed preview's header block size, and the size its chunks are
/// measured in.
const STREAM_BLOCK: usize = 2048;

/// OM3DVID7, as the menu's preview7_header and preview7_chunk check it: the
/// header block and its sum, the chunk table against the file, and every
/// chunk's sum, numbering, entries and frame table. Packets are checked by
/// the decoder as they play.
///
/// The header is one 2048-byte block (big-endian numbers):
///
/// ```text
/// bytes  8-13  width, height, frames per second (16 bits each)
/// bytes 14-15  frames per chunk
/// bytes 16-19  frame count
/// bytes 20-23  chunk count, chunks to read before playing (16 bits each)
/// bytes 24-27  4x4 and 2x2 dictionary sizes (16 bits each)
/// bytes 28-35  sample rate (0: no sound), largest chunk in bytes
/// bytes 40-43  word_sum of the block (counting these bytes as zero)
/// bytes 44-45  chunk buffers the console needs
/// bytes 64-    each chunk's size in 2048-byte blocks (16 bits each),
///              then zeros
/// ```
///
/// Each chunk starts with its word_sum, number, frame count, the first
/// dictionary entries it adds and how many, and its audio and video sizes.
fn validate_stream(data: &[u8]) -> Result<()> {
    if data.len() < STREAM_BLOCK {
        bail!("Truncated streamed preview")
    }
    let chunk_frames = be16(data, 14);
    let frames = be(data, 16)? as usize;
    let chunks = be16(data, 20);
    let preroll = be16(data, 22);
    let entries4 = be16(data, 24);
    let entries2 = be16(data, 26);
    let rate = be(data, 28)?;
    let max_chunk = be(data, 32)? as usize;
    let slots = be16(data, 44);

    let size_and_rate = (be16(data, 8), be16(data, 10), be16(data, 12));
    if size_and_rate != (STREAM_WIDTH, STREAM_HEIGHT, PREVIEW_FPS)
        || !(1..=30).contains(&chunk_frames)
        || !(1..=STREAM_MAX_FRAMES).contains(&frames)
        || chunks != frames.div_ceil(chunk_frames)
        || chunks > (STREAM_BLOCK - 64) / 2
        || preroll == 0
        || preroll > chunks
        || slots < preroll
        || entries4 > 8192
        || entries2 > 4096
        || (rate != 0 && rate != AUDIO_RATE as u32)
        || max_chunk == 0
        || !max_chunk.is_multiple_of(STREAM_BLOCK)
        || data[46..64].iter().any(|&byte| byte != 0)
        || data[64 + 2 * chunks..STREAM_BLOCK]
            .iter()
            .any(|&byte| byte != 0)
    {
        bail!("Invalid streamed preview header")
    }

    let mut header = data[..STREAM_BLOCK].to_vec();
    header[40..44].fill(0);
    if word_sum(&header) != be(data, 40)? {
        bail!("Streamed preview header checksum mismatch")
    }

    // Walk the chunks, keeping count of the dictionary entries added so far.
    let mut position = STREAM_BLOCK;
    let mut entries4_so_far = 0usize;
    let mut entries2_so_far = 0usize;
    for number in 0..chunks {
        let size = be16(data, 64 + 2 * number) * STREAM_BLOCK;
        if size == 0 || size > max_chunk || position + size > data.len() {
            bail!("Invalid streamed preview chunk table")
        }
        let chunk = &data[position..position + size];

        let first_frame = number * chunk_frames;
        let frame_count = (frames - first_frame).min(chunk_frames);
        let audio = be(chunk, 16)? as usize;
        let video = be(chunk, 20)? as usize;
        let new4 = be16(chunk, 10);
        let new2 = be16(chunk, 14);
        // The chunk's 24-byte header, its sound (padded to 4 bytes) and its
        // new dictionary entries come before the video.
        let video_start = 24 + audio.next_multiple_of(4) + new4 * 40 + new2 * 12;
        let expected_audio = if rate != 0 {
            samples_for_frames(first_frame + frame_count) - samples_for_frames(first_frame)
        } else {
            0
        };

        if word_sum(&chunk[4..]) != be(chunk, 0)?
            || be16(chunk, 4) != number
            || be16(chunk, 6) != frame_count
            || be16(chunk, 8) != entries4_so_far
            || be16(chunk, 12) != entries2_so_far
            || entries4_so_far + new4 > entries4
            || entries2_so_far + new2 > entries2
            || audio != expected_audio
            || video_start + video > size
            || video < (frame_count + 1) * 4
            || be(chunk, video_start)? as usize != (frame_count + 1) * 4
            || be(chunk, video_start + frame_count * 4)? as usize != video
        {
            bail!("Invalid streamed preview chunk {number}")
        }

        entries4_so_far += new4;
        entries2_so_far += new2;
        position += size;
    }

    if position != data.len() {
        bail!("Streamed preview has trailing data")
    }
    Ok(())
}

// ---- Downloading FFmpeg ----

/// Where FFmpeg builds are published.
const FFMPEG_RELEASES_API: &str = "https://api.github.com/repos/BtbN/FFmpeg-Builds/releases/latest";
const FFMPEG_DOWNLOADS: &str = "https://github.com/BtbN/FFmpeg-Builds/releases/download/";

/// Downloads FFmpeg and ffprobe (Windows only; other systems use the
/// distribution's) into the cache, checks the download against the
/// provider's SHA-256, and unpacks it. Returns the folder holding the
/// programs.
pub fn install_video_tools(cache: &Path, job: &Job) -> Result<PathBuf> {
    let platform = match (std::env::consts::OS, std::env::consts::ARCH) {
        ("windows", "x86_64") => "win64-lgpl-shared-9.0.zip",
        ("linux", "x86_64") => "linux64-lgpl-9.0.tar.xz",
        _ => bail!(
            "Automatic download supports Windows/Linux x64. \
             Choose an existing FFmpeg/ffprobe folder on this platform."
        ),
    };
    if !platform.ends_with(".zip") {
        bail!(
            "Choose the FFmpeg/ffprobe folder supplied by the Linux distribution; \
             automatic archive installation is currently Windows-only"
        )
    }

    // Find this platform's build in the latest release.
    let response = reqwest::blocking::Client::new()
        .get(FFMPEG_RELEASES_API)
        .header("User-Agent", "OhMy3DO-Library")
        .send()?
        .error_for_status()?;
    let release: Value = response.json()?;
    let asset = release["assets"]
        .as_array()
        .context("No video tool assets")?
        .iter()
        .find(|asset| {
            asset["name"].as_str().is_some_and(|name| {
                name.starts_with("ffmpeg-n9.0-latest-") && name.ends_with(platform)
            })
        })
        .context("Compatible video tool download missing")?;
    let digest = asset["digest"]
        .as_str()
        .and_then(|digest| digest.strip_prefix("sha256:"))
        .context("Provider does not supply a SHA256 digest")?;
    if digest.len() != 64 {
        bail!("Invalid provider digest")
    }
    let url = asset["browser_download_url"]
        .as_str()
        .context("No video tool URL")?;
    if !url.starts_with(FFMPEG_DOWNLOADS) {
        bail!("Unexpected video tool provider")
    }

    let archive = cache.join("video-tools").join(format!("{digest}.zip"));
    catalog::download(url, &archive, 256 * 1024 * 1024, job)?;
    if f::sha256(&archive, job)? != digest {
        bail!(
            "Video tool download checksum failed; retry after removing {}",
            archive.display()
        )
    }

    let destination = cache.join("video-tools").join(digest);
    let programs = extract_video_tools(&archive, &destination, job)?;
    job.say("Video tools installed and checksum verified");
    Ok(programs)
}

/// Unpacks the FFmpeg ZIP into `destination`, refusing unsafe paths, links
/// and more than 1 GiB in total. Returns the folder holding ffmpeg.exe,
/// checking ffprobe.exe is beside it.
fn extract_video_tools(archive: &Path, destination: &Path, job: &Job) -> Result<PathBuf> {
    fs::create_dir_all(destination)?;
    let mut zip = zip::ZipArchive::new(fs::File::open(archive)?)?;
    let mut total_bytes = 0u64;
    let mut programs = None;

    for index in 0..zip.len() {
        job.check()?;
        let mut entry = zip.by_index(index)?;
        let relative = entry.enclosed_name().context("Unsafe video archive path")?;
        const FILE_TYPE_MASK: u32 = 0o170000;
        const SYMBOLIC_LINK: u32 = 0o120000;
        if entry
            .unix_mode()
            .is_some_and(|mode| mode & FILE_TYPE_MASK == SYMBOLIC_LINK)
        {
            bail!("Links in video tool archive")
        }
        total_bytes += entry.size();
        if total_bytes > 1024 * 1024 * 1024 {
            bail!("Video tools archive too large")
        }

        let path = destination.join(&relative);
        if entry.is_dir() {
            fs::create_dir_all(&path)?;
            continue;
        }
        let folder = path.parent().unwrap();
        fs::create_dir_all(folder)?;
        let mut temporary = tempfile::NamedTempFile::new_in(folder)?;
        std::io::copy(&mut entry, &mut temporary)?;
        temporary.persist(&path).map_err(|error| error.error)?;

        if path.file_name().is_some_and(|name| name == "ffmpeg.exe") {
            programs = Some(folder.to_owned());
        }
    }

    let programs = programs.context("FFmpeg not found in downloaded tools")?;
    if !programs.join("ffprobe.exe").is_file() {
        bail!("ffprobe is missing")
    }
    Ok(programs)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_sources_not_opening_from_black_get_a_fade_in() {
        assert!(lead_frames(&vec![2u8; STREAM_FRAME]).is_empty());
        let lead = lead_frames(&vec![200u8; STREAM_FRAME]);
        assert_eq!(lead.len(), STREAM_LEAD_FRAMES);
        assert!(lead[0].iter().all(|&v| v == 0));
        assert!(lead[STREAM_LEAD_FRAMES - 1]
            .iter()
            .all(|&v| v as usize == 200 * 11 / 12));
    }
}
