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
#[derive(Clone)]
pub struct Prepared {
    pub name: String,
    pub path: PathBuf,
    pub source: String,
    /// A cover's 192x240 full-screen version, stored as `large_name(name)`.
    pub large: Option<PathBuf>,
}
/// The full-screen cover stored beside NAME.png or NAME.3dc.
pub fn large_name(name: &str) -> String {
    format!("{}-large.3dc", &name[..name.len().saturating_sub(4)])
}
pub type Outputs = HashMap<(String, String), Prepared>;
fn command(exe: &Path) -> Command {
    let mut c = Command::new(exe);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        c.creation_flags(0x08000000);
    }
    c
}
fn capture(exe: &Path, args: &[String], max: usize, job: &Job) -> Result<Vec<u8>> {
    let mut child = command(exe)
        .args(args)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .stdin(Stdio::null())
        .spawn()?;
    let stdout = child.stdout.take().unwrap();
    let stderr = child.stderr.take().unwrap();
    let out = std::thread::spawn(move || {
        let mut bytes = Vec::new();
        stdout
            .take(max as u64 + 1)
            .read_to_end(&mut bytes)
            .map(|_| bytes)
    });
    let err = std::thread::spawn(move || {
        let mut bytes = Vec::new();
        stderr
            .take(64 * 1024)
            .read_to_end(&mut bytes)
            .map(|_| bytes)
    });
    let start = Instant::now();
    let status = loop {
        if job.check().is_err() || start.elapsed() > Duration::from_secs(180) {
            let _ = child.kill();
            let _ = child.wait();
            let _ = out.join();
            let _ = err.join();
            bail!("Video conversion cancelled or timed out")
        };
        if let Some(s) = child.try_wait()? {
            break s;
        }
        std::thread::sleep(Duration::from_millis(50));
    };
    let bytes = out
        .join()
        .map_err(|_| anyhow::anyhow!("Video output thread failed"))??;
    let stderr = err
        .join()
        .map_err(|_| anyhow::anyhow!("Video error thread failed"))??;
    if !status.success() {
        bail!("Video tool failed: {}", String::from_utf8_lossy(&stderr))
    }
    if bytes.len() > max {
        bail!("Video output exceeds expected bounds")
    };
    Ok(bytes)
}
fn args(s: &[&str]) -> Vec<String> {
    s.iter().map(|s| s.to_string()).collect()
}
pub fn video_exes(prefs: &Preferences) -> Result<(PathBuf, PathBuf)> {
    let ext = if cfg!(windows) { ".exe" } else { "" };
    let mut dirs = vec![];
    if !prefs.video_tools.is_empty() {
        dirs.push(PathBuf::from(&prefs.video_tools));
    }
    if let Ok(path) = std::env::var("PATH") {
        dirs.extend(std::env::split_paths(&path));
    }
    for dir in dirs {
        let a = dir.join(format!("ffmpeg{ext}"));
        let b = dir.join(format!("ffprobe{ext}"));
        if a.is_file() && b.is_file() {
            return Ok((a, b));
        }
    }
    bail!("Video tools are missing. Use Get video tools or choose their folder under Tools & settings.")
}
fn cover_image(source: &Path, width: u32, height: u32) -> Result<image::RgbImage> {
    if fs::metadata(source)?.len() > 32 * 1024 * 1024 {
        bail!("Oversized cover input")
    }
    let mut reader = image::ImageReader::open(source)?.with_guessed_format()?;
    let mut limits = image::Limits::default();
    limits.max_alloc = Some(128 * 1024 * 1024);
    limits.max_image_width = Some(8192);
    limits.max_image_height = Some(8192);
    reader.limits(limits);
    let mut image = reader.decode()?;
    if let Ok(file) = fs::File::open(source) {
        if let Ok(exif) =
            exif::Reader::new().read_from_container(&mut std::io::BufReader::new(file))
        {
            if let Some(field) = exif.get_field(exif::Tag::Orientation, exif::In::PRIMARY) {
                if let Some(v) = field.value.get_uint(0) {
                    image = match v {
                        2 => image.fliph(),
                        3 => image.rotate180(),
                        4 => image.flipv(),
                        5 => image.rotate90().fliph(),
                        6 => image.rotate90(),
                        7 => image.rotate270().fliph(),
                        8 => image.rotate270(),
                        _ => image,
                    };
                }
            }
        }
    }
    let fitted = image
        .resize(width, height, image::imageops::FilterType::Lanczos3)
        .to_rgba8();
    let mut canvas = image::RgbImage::from_pixel(width, height, image::Rgb([8, 16, 33]));
    let (left, top) = ((width - fitted.width()) / 2, (height - fitted.height()) / 2);
    for (x, y, p) in fitted.enumerate_pixels() {
        let a = p[3] as u32;
        let bg = [8, 16, 33];
        let rgb = std::array::from_fn(|n| {
            ((p[n] as u32 * a + bg[n] as u32 * (255 - a) + 127) / 255) as u8
        });
        canvas.put_pixel(left + x, top + y, image::Rgb(rgb));
    }
    Ok(canvas)
}
pub fn cover(source: &Path, target: &Path) -> Result<()> {
    let mut bytes = std::io::Cursor::new(Vec::new());
    image::DynamicImage::ImageRgb8(cover_image(source, 96, 120)?)
        .write_to(&mut bytes, image::ImageFormat::Png)?;
    f::atomic(target, bytes.get_ref())
}
pub fn fast_cover(source: &Path, target: &Path) -> Result<()> {
    prepared_cover(source, target, 96, 120)
}
/// The menu's full-screen cover: the same art at 192x240.
pub fn large_cover(source: &Path, target: &Path) -> Result<()> {
    prepared_cover(source, target, 192, 240)
}
fn prepared_cover(source: &Path, target: &Path, width: u16, height: u16) -> Result<()> {
    let canvas = cover_image(source, width as u32, height as u32)?;
    let mut raw = Vec::with_capacity(width as usize * height as usize * 2);
    for p in canvas.pixels() {
        let pixel = ((p[0] as u16 >> 3) << 10) | ((p[1] as u16 >> 3) << 5) | (p[2] as u16 >> 3);
        raw.extend_from_slice(&pixel.to_be_bytes());
    }
    let mut z = ZlibEncoder::new(Vec::new(), Compression::best());
    z.write_all(&raw)?;
    let payload = z.finish()?;
    let mut bytes = b"OM3DCOV2".to_vec();
    bytes.extend_from_slice(&width.to_be_bytes());
    bytes.extend_from_slice(&height.to_be_bytes());
    bytes.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    bytes.extend_from_slice(&crc32fast::hash(&payload).to_be_bytes());
    bytes.extend_from_slice(&(raw.len() as u32).to_be_bytes());
    bytes.extend_from_slice(&payload);
    bytes.extend_from_slice(b"END3DC02");
    f::atomic(target, &bytes)
}
pub fn cover_display(path: &Path) -> Result<image::DynamicImage> {
    if !path
        .extension()
        .is_some_and(|s| s.eq_ignore_ascii_case("3dc"))
    {
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
        let mut z = flate2::read::ZlibDecoder::new(&data[24..data.len() - 8]);
        let mut raw = Vec::new();
        z.read_to_end(&mut raw)?;
        raw
    } else {
        data[24..].to_vec()
    };
    let mut image = image::RgbImage::new(width as u32, height as u32);
    for (p, bytes) in image.pixels_mut().zip(raw.chunks_exact(2)) {
        let value = u16::from_be_bytes([bytes[0], bytes[1]]);
        *p = image::Rgb([
            (((value >> 10) & 31) * 255 / 31) as u8,
            (((value >> 5) & 31) * 255 / 31) as u8,
            ((value & 31) * 255 / 31) as u8,
        ]);
    }
    Ok(image::DynamicImage::ImageRgb8(image))
}
/// RAM for a preview's video and audio together; matches tools/prepare-preview.py.
pub const PREVIEW_BYTES: usize = 1400 * 1024;
/// OM3DVID5: 160x120 at 30 fps, up to 15 seconds.
const PREVIEW_WIDTH: usize = 160;
const PREVIEW_HEIGHT: usize = 120;
const PREVIEW_FPS: usize = 30;
const PREVIEW_FRAMES: usize = 15 * PREVIEW_FPS;
const PREVIEW_FRAME: usize = PREVIEW_WIDTH * PREVIEW_HEIGHT * 3;
/// Worst-case OM3DVID5 packet: every block GRAD with literal colours.
const PREVIEW_PACKET: usize = 1200 * 12;
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
/// OM3DVID5 video section for 160x120 RGB24 frames: the smallest lambda (best
/// quality) that fits budget bytes. Size jumps between nearby lambdas, so the
/// first frames then use the next smaller lambda to fill what is left.
pub fn encode_preview(frames: &[u8], budget: usize, job: &Job) -> Result<Vec<u8>> {
    let count = frames.len() / PREVIEW_FRAME;
    if count == 0 || count > PREVIEW_FRAMES || frames.len() % PREVIEW_FRAME != 0 {
        bail!("Invalid preview frames")
    }
    let mut out = vec![0u8; (count + 1) * 4 + count * PREVIEW_PACKET];
    let mut size = |first: f64, split: usize, rest: f64| -> Result<usize> {
        job.check()?;
        // SAFETY: frames holds count whole frames and out.len() bytes are writable.
        let n = unsafe {
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
        if n < 0 {
            bail!("Preview encoder failed (error {n})")
        }
        Ok(n as usize)
    };
    // Bracket from a typical lambda, widening by 4x, then bisect to 2%.
    let (mut low, mut high) = (0.5f64, 0.5f64);
    if size(high, 0, high)? <= budget {
        loop {
            if low <= 0.01 {
                low = high; // best quality fits
                break;
            }
            low = (low / 4.).max(0.01);
            if size(low, 0, low)? > budget {
                break;
            }
            high = low;
        }
    } else {
        loop {
            low = high;
            high *= 4.;
            if high > 100000. {
                bail!("Preview cannot fit RAM budget")
            }
            if size(high, 0, high)? <= budget {
                break;
            }
        }
    }
    while high / low > 1.02 {
        let middle = (low * high).sqrt();
        if size(middle, 0, middle)? <= budget {
            high = middle
        } else {
            low = middle
        }
    }
    let mut split = 0;
    if low < high {
        let (mut fit, mut over) = (0, count + 1);
        while over - fit > 1 {
            let middle = (fit + over) / 2;
            if size(low, middle, high)? <= budget {
                fit = middle
            } else {
                over = middle
            }
        }
        split = fit;
    }
    let n = size(low, split, high)?;
    out.truncate(n);
    Ok(out)
}
/// OM3DVID7: streamed 320x240 at 30 fps, the whole clip up to a minute, at
/// about 200 KB/s (see src/preview_v7.h). The settings match the console's
/// reader and decoder: tools/prepare-preview.py uses the same ones.
const STREAM_WIDTH: usize = 320;
const STREAM_HEIGHT: usize = 240;
const STREAM_FRAME: usize = STREAM_WIDTH * STREAM_HEIGHT * 3;
const STREAM_MAX_FRAMES: usize = 1800;
#[repr(C)]
struct Preview7Params {
    rate: u32,
    read_rate: u32,
    slot_bytes: u32,
    max_chunk: u32,
    chunk_frames: u32,
    entries4: u32,
    entries2: u32,
    cycle_cap: u32,
}
const STREAM_PARAMS: Preview7Params = Preview7Params {
    rate: 200 * 1024,
    read_rate: 300 * 1024,
    slot_bytes: 640 * 1024,
    max_chunk: 160 * 1024,
    chunk_frames: 10,
    entries4: 8192,
    entries2: 4096,
    cycle_cap: 230_000,
};
/// Part of the cached file's name: a change of format or settings re-encodes.
pub const PREVIEW_KEY: &str = "om3dvid7-320x240-30fps-full-200k-160c";
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
        if let Some(kb) = text
            .lines()
            .find(|l| l.starts_with("MemAvailable:"))
            .and_then(|l| l.split_whitespace().nth(1)?.parse::<u64>().ok())
        {
            return kb * 1024;
        }
    }
    #[cfg(windows)]
    {
        #[repr(C)]
        struct Status {
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
            fn GlobalMemoryStatusEx(status: *mut Status) -> i32;
        }
        let mut status = Status { length: std::mem::size_of::<Status>() as u32, load: 0, total_phys: 0, avail_phys: 0,
            total_page: 0, avail_page: 0, total_virtual: 0, avail_virtual: 0, avail_extended: 0 };
        // SAFETY: status is a MEMORYSTATUSEX with its length set.
        if unsafe { GlobalMemoryStatusEx(&mut status) } != 0 {
            return status.avail_phys;
        }
    }
    8 << 30
}
/// Encodes at once are limited by memory: a minute-long clip needs about
/// 600 MB of workspace while it encodes. The limit is set once, from the
/// memory free when the first encode starts.
struct EncodeSlot;
static ENCODES: (std::sync::Mutex<(usize, usize)>, std::sync::Condvar) =
    (std::sync::Mutex::new((0, 0)), std::sync::Condvar::new());
impl EncodeSlot {
    fn take(job: &Job) -> Result<EncodeSlot> {
        let mut state = ENCODES.0.lock().unwrap();
        if state.1 == 0 {
            state.1 = ((memory_available() / (900 << 20)) as usize).clamp(1, 16);
        }
        while state.0 >= state.1 {
            job.check()?;
            state = ENCODES.1.wait_timeout(state, Duration::from_millis(200)).unwrap().0;
        }
        state.0 += 1;
        Ok(EncodeSlot)
    }
}
impl Drop for EncodeSlot {
    fn drop(&mut self) {
        ENCODES.0.lock().unwrap().0 -= 1;
        ENCODES.1.notify_one();
    }
}

/// Encode the count frames added to a workspace, with signed 8-bit 11025 Hz
/// sound for exactly those frames, if any.
fn encode_workspace(workspace: &mut [u8], count: usize, audio: Option<&[u8]>) -> Result<Vec<u8>> {
    let samples = count * 11025 / PREVIEW_FPS;
    if audio.is_some_and(|a| a.len() != samples) {
        bail!("Preview sound does not match its frames")
    }
    let capacity = STREAM_PARAMS.rate as usize * count / PREVIEW_FPS * 11 / 10 + (2 << 20);
    let mut out = vec![0u8; capacity];
    let mut stats = [0u32; 8];
    // SAFETY: the workspace holds count frames; audio holds samples bytes;
    // out holds capacity writable bytes; stats has room for 8 words.
    let n = unsafe {
        oh3do_preview7_encode(
            workspace.as_mut_ptr(),
            &STREAM_PARAMS,
            audio.map_or(std::ptr::null(), |a| a.as_ptr() as *const i8),
            audio.map_or(0, |_| samples as u64),
            out.as_mut_ptr(),
            capacity as u64,
            stats.as_mut_ptr(),
        )
    };
    if n < 0 {
        bail!("Preview encoder failed (error {n})")
    }
    out.truncate(n as usize);
    Ok(out)
}
/// A streamed preview of 320x240 RGB24 frames already in memory (tests and
/// tools; preview() streams frames from FFmpeg instead).
pub fn encode_stream(frames: &[u8], audio: Option<&[u8]>) -> Result<Vec<u8>> {
    let count = frames.len() / STREAM_FRAME;
    if count == 0 || count > STREAM_MAX_FRAMES || frames.len() % STREAM_FRAME != 0 {
        bail!("Invalid preview frames")
    }
    // SAFETY: plain arithmetic in the encoder.
    let bytes = unsafe { oh3do_preview7_workspace(count as u32) } as usize;
    let mut workspace = vec![0u8; bytes];
    // SAFETY: workspace holds bytes writable bytes; each frame is a whole picture.
    unsafe {
        if oh3do_preview7_begin(workspace.as_mut_ptr(), bytes as u64, count as u32) != 0 {
            bail!("Preview encoder setup failed")
        }
        for frame in frames.chunks(STREAM_FRAME) {
            if oh3do_preview7_frame(workspace.as_mut_ptr(), frame.as_ptr()) != 0 {
                bail!("Preview encoder rejected a frame")
            }
        }
    }
    encode_workspace(&mut workspace, count, audio)
}
/// Run exe and hand each complete `size`-byte block of its output to each(),
/// up to `max` blocks. Returns how many.
fn stream(exe: &Path, args: &[String], size: usize, max: usize, job: &Job, mut each: impl FnMut(&[u8]) -> Result<()>) -> Result<usize> {
    let mut child = command(exe)
        .args(args)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .stdin(Stdio::null())
        .spawn()?;
    let mut stdout = child.stdout.take().unwrap();
    let stderr = child.stderr.take().unwrap();
    let err = std::thread::spawn(move || {
        let mut bytes = Vec::new();
        stderr.take(64 * 1024).read_to_end(&mut bytes).map(|_| bytes)
    });
    let mut block = vec![0u8; size];
    let mut count = 0;
    let result = (|| -> Result<()> {
        loop {
            job.check()?;
            let mut filled = 0;
            while filled < size {
                let n = stdout.read(&mut block[filled..])?;
                if n == 0 {
                    break;
                }
                filled += n;
            }
            if filled < size {
                return Ok(()); // the end; a partial block is dropped
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
    let stderr = err.join().map_err(|_| anyhow::anyhow!("Video error thread failed"))??;
    result?;
    if !status.success() {
        bail!("Video tool failed: {}", String::from_utf8_lossy(&stderr))
    }
    Ok(count)
}

pub fn preview(source: &Path, target: &Path, prefs: &Preferences, job: &Job) -> Result<()> {
    let (ffmpeg, ffprobe) = video_exes(prefs)?;
    let mut a = args(&["-v", "error", "-show_streams", "-show_format", "-of", "json"]);
    a.push(source.to_string_lossy().into_owned());
    let info: Value = serde_json::from_slice(&capture(&ffprobe, &a, 1024 * 1024, job)?)?;
    let streams = info["streams"].as_array().context("No video streams")?;
    if !streams.iter().any(|s| s["codec_type"] == "video") {
        bail!("Source has no video stream")
    }
    let sound = streams.iter().any(|s| s["codec_type"] == "audio");
    let seconds = info["format"]["duration"]
        .as_str()
        .and_then(|s| s.parse::<f64>().ok())
        .filter(|n| n.is_finite() && *n > 0.)
        .unwrap_or(60.)
        .min((STREAM_MAX_FRAMES / PREVIEW_FPS) as f64);
    let reserve = ((seconds * PREVIEW_FPS as f64).ceil() as usize + 2).min(STREAM_MAX_FRAMES);
    let _slot = EncodeSlot::take(job)?;
    // SAFETY: plain arithmetic in the encoder.
    let bytes = unsafe { oh3do_preview7_workspace(reserve as u32) } as usize;
    let mut workspace = vec![0u8; bytes];
    // SAFETY: workspace holds bytes writable bytes.
    if unsafe { oh3do_preview7_begin(workspace.as_mut_ptr(), bytes as u64, reserve as u32) } != 0 {
        bail!("Preview encoder setup failed")
    }
    let mut a = args(&["-v", "error", "-nostdin", "-i"]);
    a.push(source.to_string_lossy().into_owned());
    let filter = format!(
        "fps={PREVIEW_FPS},scale={STREAM_WIDTH}:{STREAM_HEIGHT}:force_original_aspect_ratio=decrease:flags=area,\
         pad={STREAM_WIDTH}:{STREAM_HEIGHT}:(ow-iw)/2:(oh-ih)/2:color=0x081021"
    );
    a.extend(args(&["-map", "0:v:0", "-an", "-sn", "-t", &seconds.to_string(), "-vf", &filter,
        "-frames:v", &reserve.to_string(), "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"]));
    job.say("Reading preview frames");
    let count = stream(&ffmpeg, &a, STREAM_FRAME, reserve, job, |frame| {
        // SAFETY: frame is one 320x240 RGB24 picture; begin reserved room.
        if unsafe { oh3do_preview7_frame(workspace.as_mut_ptr(), frame.as_ptr()) } != 0 {
            bail!("Preview encoder rejected a frame")
        }
        Ok(())
    })?;
    if count == 0 {
        bail!("Incomplete video frames")
    }
    let samples = count * 11025 / PREVIEW_FPS;
    let mut audio = vec![];
    if sound {
        let seconds = count as f64 / PREVIEW_FPS as f64;
        let mut a = args(&["-v", "error", "-nostdin", "-i"]);
        a.push(source.to_string_lossy().into_owned());
        a.extend(args(&["-map", "0:a:0", "-vn", "-sn", "-t", &seconds.to_string(), "-ac", "1", "-ar", "11025",
            "-af", &format!("volume=0.8,afade=t=in:d=0.01,afade=t=out:st={}:d=0.01", (seconds - 0.01).max(0.)),
            "-f", "s8", "pipe:1"]));
        audio = capture(&ffmpeg, &a, samples + 8192, job)?;
        audio.resize(samples, 0);
    }
    job.say(format!("Encoding preview: {count} frames"));
    let out = encode_workspace(&mut workspace, count, sound.then_some(&audio[..]))?;
    drop(workspace);
    f::atomic(target, &out)?;
    job.say(format!(
        "Preview: {:.1} seconds at {PREVIEW_FPS} fps{}, {} KB",
        count as f64 / PREVIEW_FPS as f64,
        if sound { " with audio" } else { "" },
        out.len() / 1024
    ));
    Ok(())
}
fn files(root: &Path, extensions: &[&str]) -> Result<Vec<PathBuf>> {
    if !root.is_dir() {
        return Ok(vec![]);
    }
    let mut paths = vec![];
    for e in walkdir::WalkDir::new(root)
        .follow_links(false)
        .into_iter()
        .filter_entry(|e| !f::is_link(e.path()))
    {
        let e = e?;
        if e.file_type().is_file()
            && extensions.contains(
                &e.path()
                    .extension()
                    .unwrap_or_default()
                    .to_string_lossy()
                    .to_ascii_lowercase()
                    .as_str(),
            )
        {
            paths.push(e.path().to_owned())
        }
    }
    paths.sort();
    Ok(paths)
}
pub fn matches(title: &str, source: &Path, paths: &[PathBuf]) -> Vec<PathBuf> {
    media_matches(title, source, paths, false)
}
pub fn preview_matches(title: &str, source: &Path, paths: &[PathBuf]) -> Vec<PathBuf> {
    media_matches(title, source, paths, true)
}
fn media_matches(
    title: &str,
    source: &Path,
    paths: &[PathBuf],
    other_regions: bool,
) -> Vec<PathBuf> {
    for name in [
        title.to_owned(),
        source
            .file_stem()
            .unwrap_or_default()
            .to_string_lossy()
            .into_owned(),
    ] {
        let exact = paths
            .iter()
            .filter(|p| {
                p.file_stem()
                    .unwrap_or_default()
                    .to_string_lossy()
                    .eq_ignore_ascii_case(&name)
            })
            .cloned()
            .collect::<Vec<_>>();
        if !exact.is_empty() {
            return exact;
        }
    }
    let kind = |s: &str| {
        rx(r"(?i)\b(demo|demonstration|trial|sampler)\b")
            .captures(s)
            .map(|c| c[1].to_lowercase())
            .unwrap_or_else(|| "game".into())
    };
    let candidates: Vec<_> = paths
        .iter()
        .filter(|p| {
            let name = p.file_stem().unwrap_or_default().to_string_lossy();
            d::title_key(&name) == d::title_key(title)
                && kind(&name) == kind(title)
                && (d::disc_number(&name).is_none()
                    || d::disc_number(&name) == d::disc_number(title))
        })
        .cloned()
        .collect();
    let compatible: Vec<_> = candidates
        .iter()
        .filter(|p| {
            let name = p.file_stem().unwrap_or_default().to_string_lossy();
            d::regions(&name).is_empty() || !d::regions(&name).is_disjoint(&d::regions(title))
        })
        .cloned()
        .collect();
    // A regional fallback never resolves ambiguity among compatible clips.
    if compatible.is_empty() && other_regions {
        candidates
    } else {
        compatible
    }
}
fn read_sources(prefs: &Preferences, kind: &str) -> Result<Vec<PathBuf>> {
    if prefs.emumovies.is_empty() {
        return Ok(vec![]);
    }
    let mut root = PathBuf::from(&prefs.emumovies);
    if root.join("Panasonic_3DO").is_dir() {
        root.push("Panasonic_3DO")
    }
    if kind == "cover" {
        return files(
            &root.join("Box"),
            &["png", "jpg", "jpeg", "webp", "bmp", "gif", "tiff", "tif"],
        );
    }
    let mut clips = HashMap::new();
    for folder in ["Video_MP4", "Video_MP4_HI_QUAL", "Video_MP4_HD"] {
        let mut group: HashMap<String, Vec<PathBuf>> = HashMap::new();
        for p in files(&root.join(folder), &["mp4", "avi", "mov", "mkv", "webm"])? {
            group
                .entry(p.file_stem().unwrap().to_string_lossy().to_lowercase())
                .or_default()
                .push(p);
        }
        clips.extend(group)
    }
    Ok(clips.into_values().flatten().collect())
}
fn sources(prefs: &Preferences, kind: &str, job: &Job) -> Result<Vec<PathBuf>> {
    let key = format!("{}:{kind}", prefs.emumovies);
    if let Some(paths) = job.media_sources.lock().unwrap().get(&key) {
        return Ok(paths.clone());
    }
    let paths = read_sources(prefs, kind)?;
    job.media_sources.lock().unwrap().insert(key, paths.clone());
    Ok(paths)
}
fn download_cover(title: &str, cache: &Path, job: &Job) -> Result<Option<(PathBuf, String)>> {
    let list = cache.join("cover-list.html");
    catalog::download(catalog::ART, &list, 16 * 1024 * 1024, job)?;
    let html = fs::read_to_string(list)?;
    let names = rx(r#"href="([^"]+\.png)""#)
        .captures_iter(&html)
        .filter_map(|c| urlencoding::decode(&c[1]).ok().map(|s| s.into_owned()))
        .filter(|s| !s.contains(['/', '\\']) && !s.starts_with('.'))
        .collect::<Vec<_>>();
    let exact = names
        .iter()
        .filter(|s| {
            Path::new(s)
                .file_stem()
                .unwrap()
                .to_string_lossy()
                .eq_ignore_ascii_case(title)
        })
        .cloned()
        .collect::<Vec<_>>();
    let candidates = if !exact.is_empty() {
        exact
    } else {
        names
            .into_iter()
            .filter(|s| {
                let stem = Path::new(s).file_stem().unwrap().to_string_lossy();
                d::title_key(&stem) == d::title_key(title)
                    && !d::regions(&stem).is_disjoint(&d::regions(title))
            })
            .collect()
    };
    let first = candidates
        .iter()
        .filter(|s| d::disc_number(s) == Some(1))
        .cloned()
        .collect::<Vec<_>>();
    let chosen = if candidates.len() == 1 {
        candidates.first()
    } else if first.len() == 1 {
        first.first()
    } else {
        None
    };
    if let Some(name) = chosen {
        let url = format!("{}{}", catalog::ART, urlencoding::encode(name));
        let key = format!("{:x}", Sha256::digest(url.as_bytes()));
        let p = cache.join("artwork").join(format!("{key}.png"));
        catalog::download(&url, &p, 32 * 1024 * 1024, job)?;
        Ok(Some((p, url)))
    } else {
        Ok(None)
    }
}
pub fn prepare_entry(
    entry: &mut Entry,
    prefs: &Preferences,
    cache: &Path,
    out: &mut Outputs,
    job: &Job,
) -> Result<()> {
    prepare_missing(entry, prefs, cache, out, true, true, job)
}
pub fn original_source(
    kind: &str,
    origin: &str,
    prefs: &Preferences,
    cache: &Path,
    job: &Job,
) -> Result<Option<PathBuf>> {
    if kind == "cover" && origin.starts_with("https://") {
        let key = format!("{:x}", Sha256::digest(origin.as_bytes()));
        let path = cache.join("artwork").join(format!("{key}.png"));
        if !path.is_file() && !prefs.offline {
            catalog::download(origin, &path, 32 * 1024 * 1024, job)?;
        }
        return Ok(path.is_file().then_some(path));
    }
    let path = PathBuf::from(origin);
    if path.is_absolute() {
        return Ok((path.is_file() && !f::is_link(&path)).then_some(path));
    }
    if !prefs.emumovies.is_empty() {
        // Older tools saved paths relative to the selected media folder.
        let root = PathBuf::from(&prefs.emumovies);
        for base in [&root, &root.join("Panasonic_3DO")] {
            for path in [
                base.join(origin),
                base.join(if kind == "cover" {
                    "Box"
                } else {
                    "Video_MP4_HI_QUAL"
                })
                .join(origin),
            ] {
                if path.is_file() && !f::is_link(&path) {
                    return Ok(Some(path));
                }
            }
        }
    }
    Ok(None)
}
pub fn prepare_missing(
    entry: &mut Entry,
    prefs: &Preferences,
    cache: &Path,
    out: &mut Outputs,
    want_cover: bool,
    want_preview: bool,
    job: &Job,
) -> Result<()> {
    for (kind, want) in [("cover", want_cover), ("preview", want_preview)] {
        if !want {
            continue;
        }
        job.check()?;
        let provided = if kind == "cover" {
            entry.cover.clone()
        } else {
            entry.preview.clone()
        };
        let source = if let Some(p) = provided {
            Some((p.clone(), p.to_string_lossy().into_owned()))
        } else {
            let paths = sources(prefs, kind, job)?;
            let candidates = if kind == "preview" {
                preview_matches(&entry.title, &entry.source, &paths)
            } else {
                matches(&entry.title, &entry.source, &paths)
            };
            if candidates.len() == 1 {
                Some((
                    candidates[0].clone(),
                    candidates[0].to_string_lossy().into_owned(),
                ))
            } else if kind == "cover" && prefs.download_covers && !prefs.offline {
                match download_cover(&entry.title, cache, job) {
                    Ok(v) => v,
                    Err(e) => {
                        job.say(format!("Cover download skipped: {e:#}"));
                        None
                    }
                }
            } else {
                None
            }
        };
        if let Some((source, origin)) = source {
            let result = (|| -> Result<Prepared> {
                if f::is_link(&source) || !source.is_file() {
                    bail!("Choose a real media file")
                };
                job.say(format!("Preparing {kind}: {}", source.display()));
                let key = format!(
                    "{:x}",
                    Sha256::digest(
                        format!(
                            "{}:{kind}:{}",
                            f::sha256(&source, job)?,
                            if kind == "cover" {
                                "native-v3-160x120-fast96-cap15-512k"
                            } else {
                                PREVIEW_KEY
                            }
                        )
                        .as_bytes()
                    )
                );
                let ext = if kind == "cover" { "3dc" } else { "3dv" };
                let target = cache.join("converted-native").join(format!("{key}.{ext}"));
                if !target.exists() {
                    if kind == "cover" {
                        fast_cover(&source, &target)?
                    } else {
                        preview(&source, &target, prefs, job)?
                    }
                }
                validate(&target)?;
                let large = if kind == "cover" {
                    let large = cache.join("converted-native").join(format!("{key}-large.3dc"));
                    if !large.exists() {
                        large_cover(&source, &large)?
                    }
                    validate(&large)?;
                    Some(large)
                } else {
                    None
                };
                let name = format!("{kind}-{}.{ext}", &f::sha256(&target, job)?[..24]);
                Ok(Prepared {
                    name,
                    path: target,
                    source: origin,
                    large,
                })
            })();
            match result {
                Ok(asset) => {
                    out.insert((entry.target.clone(), kind.into()), asset);
                }
                Err(e) => {
                    job.check()?;
                    job.say(format!("{kind} skipped: {e:#}"));
                }
            }
        }
    }
    Ok(())
}
/// Preview payload check, as in the menu's src/usb_media.c: over big-endian
/// words, a += w; b += a; the result is a ^ (b rotated by 16). It costs the
/// console about a tenth of CRC32.
pub fn word_sum(data: &[u8]) -> u32 {
    let (mut a, mut b) = (0u32, 0u32);
    for word in data.chunks(4) {
        let mut bytes = [0u8; 4];
        bytes[..word.len()].copy_from_slice(word);
        a = a.wrapping_add(u32::from_be_bytes(bytes));
        b = b.wrapping_add(a);
    }
    a ^ b.rotate_left(16)
}

fn be(data: &[u8], pos: usize) -> Result<u32> {
    Ok(u32::from_be_bytes(
        data.get(pos..pos + 4)
            .context("Truncated media")?
            .try_into()?,
    ))
}
fn inflate(data: &[u8], expected: usize) -> Result<()> {
    let mut z = flate2::Decompress::new(true);
    let mut out = vec![0; expected + 1];
    let status = z.decompress(data, &mut out, flate2::FlushDecompress::Finish)?;
    if status != flate2::Status::StreamEnd
        || z.total_out() != expected as u64
        || z.total_in() != data.len() as u64
    {
        bail!("Invalid compressed frame")
    };
    Ok(())
}
pub fn validate(path: &Path) -> Result<()> {
    let preview = path
        .extension()
        .unwrap_or_default()
        .to_string_lossy()
        .eq_ignore_ascii_case("3dv");
    if fs::metadata(path)?.len() > if preview { 16384 } else { 1024 } * 1024 + 48 {
        bail!("Oversized media")
    };
    let data = fs::read(path)?;
    if path
        .extension()
        .unwrap_or_default()
        .to_string_lossy()
        .eq_ignore_ascii_case("png")
    {
        if data.len() > 512 * 1024 || !data.starts_with(b"\x89PNG\r\n\x1a\n") {
            bail!("Invalid PNG")
        };
        let mut pos = 8;
        let mut ended = false;
        while pos + 12 <= data.len() {
            let n = be(&data, pos)? as usize;
            if pos + 12 + n > data.len() {
                bail!("Truncated PNG")
            };
            if crc32fast::hash(&data[pos + 4..pos + 8 + n]) != be(&data, pos + 8 + n)? {
                bail!("PNG checksum mismatch")
            };
            if &data[pos + 4..pos + 8] == b"IEND" {
                if n != 0 || pos + 12 != data.len() {
                    bail!("Invalid PNG end")
                };
                ended = true;
                break;
            }
            pos += n + 12;
        }
        let image = image::load_from_memory_with_format(&data, image::ImageFormat::Png)?;
        if !ended || (image.width(), image.height()) != (96, 120) {
            bail!("Resize covers to 96x120 PNG")
        };
        return Ok(());
    }
    if data.len() < 24 {
        bail!("Invalid media dimensions")
    };
    let width = u16::from_be_bytes(data[8..10].try_into()?) as usize;
    let height = u16::from_be_bytes(data[10..12].try_into()?) as usize;
    if data.starts_with(b"OM3DCOV2") {
        let bytes = be(&data, 12)? as usize;
        let large = path.to_string_lossy().to_ascii_lowercase().ends_with("-large.3dc");
        let (size, limit) = if large { ((192, 240), 192 * 240 * 2 + 1024) } else { ((96, 120), 32768) };
        if (width, height) != size
            || bytes == 0
            || bytes > limit
            || data.len() != 32 + bytes
            || be(&data, 20)? as usize != width * height * 2
            || &data[24 + bytes..] != b"END3DC02"
            || crc32fast::hash(&data[24..24 + bytes]) != be(&data, 16)?
        {
            bail!("Invalid prepared cover")
        }
        return inflate(&data[24..24 + bytes], width * height * 2);
    }
    if data.starts_with(b"OM3DVID7") {
        return validate_stream(&data);
    }
    if !data.starts_with(b"OM3DVID5") || data.len() < 48 {
        bail!("Invalid preview signature")
    };
    let bytes = be(&data, 12)? as usize;
    let crc = be(&data, 16)?;
    let frames = u16::from_be_bytes(data[20..22].try_into()?) as usize;
    let fps = u16::from_be_bytes(data[22..24].try_into()?) as usize;
    if data.len() != 40 + bytes + 8
        || data[data.len() - 8..] != *b"END3DV05"
        || bytes % 4 != 0
        || word_sum(&data[40..data.len() - 8]) != crc
    {
        bail!("Invalid preview bounds/checksum")
    }
    if (width, height, fps) != (PREVIEW_WIDTH, PREVIEW_HEIGHT, PREVIEW_FPS) || frames == 0 || frames > PREVIEW_FRAMES {
        bail!("Invalid preview size, rate or length")
    }
    let video = be(&data, 24)? as usize;
    let audio = be(&data, 28)? as usize;
    let rate = be(&data, 32)?;
    let samples = be(&data, 36)? as usize;
    let expected = ((frames * 11025).div_ceil(fps) + 3) & !3;
    let pad = video.next_multiple_of(4) - video;
    if bytes > 4096 * 1024
        || video + pad + audio != bytes
        || data[40 + bytes - pad..40 + bytes].iter().any(|&b| b != 0)
        || video <= (frames + 1) * 4
        || ((audio > 0) && (rate != 11025 || samples != audio || samples != expected))
        || (audio == 0 && (samples != 0 || rate != 0))
    {
        bail!("Invalid preview audio or RAM bounds")
    }
    // The frame table comes first; packets are checked by the menu's decoder as they play.
    let table = &data[40..40 + (frames + 1) * 4];
    if be(table, 0)? as usize != (frames + 1) * 4 || be(table, frames * 4)? as usize != video {
        bail!("Invalid frame table")
    };
    for frame in 0..frames {
        if be(table, frame * 4)? >= be(table, (frame + 1) * 4)? {
            bail!("Invalid frame offset")
        };
    }
    Ok(())
}
/// OM3DVID7, as the menu's preview7_header and preview7_chunk check it: the
/// header block and its sum, the chunk table against the file, and every
/// chunk's sum, numbering, entries and frame table. Packets are checked by
/// the decoder as they play.
fn validate_stream(data: &[u8]) -> Result<()> {
    let h16 = |p: usize| -> usize { u16::from_be_bytes([data[p], data[p + 1]]) as usize };
    if data.len() < 2048 {
        bail!("Truncated streamed preview")
    }
    let (chunk_frames, frames, chunks, preroll) = (h16(14), be(data, 16)? as usize, h16(20), h16(22));
    let (entries4, entries2, rate, max_chunk, slots) = (h16(24), h16(26), be(data, 28)?, be(data, 32)? as usize, h16(44));
    if (h16(8), h16(10), h16(12)) != (STREAM_WIDTH, STREAM_HEIGHT, PREVIEW_FPS)
        || !(1..=30).contains(&chunk_frames)
        || !(1..=STREAM_MAX_FRAMES).contains(&frames)
        || chunks != frames.div_ceil(chunk_frames)
        || chunks > (2048 - 64) / 2
        || preroll == 0
        || preroll > chunks
        || slots < preroll
        || entries4 > 8192
        || entries2 > 4096
        || (rate != 0 && rate != 11025)
        || max_chunk == 0
        || max_chunk % 2048 != 0
        || data[46..64].iter().any(|&b| b != 0)
        || data[64 + 2 * chunks..2048].iter().any(|&b| b != 0)
    {
        bail!("Invalid streamed preview header")
    }
    let mut block = data[..2048].to_vec();
    block[40..44].fill(0);
    if word_sum(&block) != be(data, 40)? {
        bail!("Streamed preview header checksum mismatch")
    }
    let samples_to = |f: usize| f * 11025 / PREVIEW_FPS;
    let (mut at, mut first4, mut first2) = (2048usize, 0usize, 0usize);
    for c in 0..chunks {
        let size = h16(64 + 2 * c) * 2048;
        if size == 0 || size > max_chunk || at + size > data.len() {
            bail!("Invalid streamed preview chunk table")
        }
        let chunk = &data[at..at + size];
        let c16 = |p: usize| u16::from_be_bytes([chunk[p], chunk[p + 1]]) as usize;
        let first = c * chunk_frames;
        let count = (frames - first).min(chunk_frames);
        let audio = be(chunk, 16)? as usize;
        let video = be(chunk, 20)? as usize;
        let (n4, n2) = (c16(10), c16(14));
        let entries = 24 + audio.next_multiple_of(4) + n4 * 40 + n2 * 12;
        if word_sum(&chunk[4..]) != be(chunk, 0)?
            || c16(4) != c
            || c16(6) != count
            || c16(8) != first4
            || c16(12) != first2
            || first4 + n4 > entries4
            || first2 + n2 > entries2
            || audio != if rate != 0 { samples_to(first + count) - samples_to(first) } else { 0 }
            || entries + video > size
            || video < (count + 1) * 4
            || be(chunk, entries)? as usize != (count + 1) * 4
            || be(chunk, entries + count * 4)? as usize != video
        {
            bail!("Invalid streamed preview chunk {c}")
        }
        first4 += n4;
        first2 += n2;
        at += size;
    }
    if at != data.len() {
        bail!("Streamed preview has trailing data")
    }
    Ok(())
}
pub fn install_video_tools(cache: &Path, job: &Job) -> Result<PathBuf> {
    let platform=match(std::env::consts::OS,std::env::consts::ARCH){("windows","x86_64")=>"win64-lgpl-shared-9.0.zip",("linux","x86_64")=>"linux64-lgpl-9.0.tar.xz",_=>bail!("Automatic download supports Windows/Linux x64. Choose an existing FFmpeg/ffprobe folder on this platform.")};
    if !platform.ends_with(".zip") {
        bail!("Choose the FFmpeg/ffprobe folder supplied by the Linux distribution; automatic archive installation is currently Windows-only")
    }
    let response = reqwest::blocking::Client::new()
        .get("https://api.github.com/repos/BtbN/FFmpeg-Builds/releases/latest")
        .header("User-Agent", "OhMy3DO-Library")
        .send()?
        .error_for_status()?;
    let release: Value = response.json()?;
    let asset = release["assets"]
        .as_array()
        .context("No video tool assets")?
        .iter()
        .find(|a| {
            a["name"]
                .as_str()
                .is_some_and(|s| s.starts_with("ffmpeg-n9.0-latest-") && s.ends_with(platform))
        })
        .context("Compatible video tool download missing")?;
    let digest = asset["digest"]
        .as_str()
        .and_then(|s| s.strip_prefix("sha256:"))
        .context("Provider does not supply a SHA256 digest")?;
    if digest.len() != 64 {
        bail!("Invalid provider digest")
    }
    let url = asset["browser_download_url"]
        .as_str()
        .context("No video tool URL")?;
    if !url.starts_with("https://github.com/BtbN/FFmpeg-Builds/releases/download/") {
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
    fs::create_dir_all(&destination)?;
    let mut zip = zip::ZipArchive::new(fs::File::open(&archive)?)?;
    let mut size = 0u64;
    let mut bin = None;
    for n in 0..zip.len() {
        job.check()?;
        let mut entry = zip.by_index(n)?;
        let rel = entry.enclosed_name().context("Unsafe video archive path")?;
        if entry.unix_mode().is_some_and(|m| m & 0o170000 == 0o120000) {
            bail!("Links in video tool archive")
        };
        size += entry.size();
        if size > 1024 * 1024 * 1024 {
            bail!("Video tools archive too large")
        };
        let path = destination.join(&rel);
        if entry.is_dir() {
            fs::create_dir_all(&path)?;
            continue;
        }
        fs::create_dir_all(path.parent().unwrap())?;
        let mut tmp = tempfile::NamedTempFile::new_in(path.parent().unwrap())?;
        std::io::copy(&mut entry, &mut tmp)?;
        tmp.persist(&path).map_err(|e| e.error)?;
        if path.file_name().is_some_and(|s| s == "ffmpeg.exe") {
            bin = path.parent().map(Path::to_owned);
        }
    }
    let bin = bin.context("FFmpeg not found in downloaded tools")?;
    if !bin.join("ffprobe.exe").is_file() {
        bail!("ffprobe is missing")
    };
    job.say("Video tools installed and checksum verified");
    Ok(bin)
}
