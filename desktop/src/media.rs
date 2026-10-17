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
fn cover_image(source: &Path) -> Result<image::RgbImage> {
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
        .resize(96, 120, image::imageops::FilterType::Lanczos3)
        .to_rgba8();
    let mut canvas = image::RgbImage::from_pixel(96, 120, image::Rgb([8, 16, 33]));
    let (left, top) = ((96 - fitted.width()) / 2, (120 - fitted.height()) / 2);
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
    image::DynamicImage::ImageRgb8(cover_image(source)?)
        .write_to(&mut bytes, image::ImageFormat::Png)?;
    f::atomic(target, bytes.get_ref())
}
pub fn fast_cover(source: &Path, target: &Path) -> Result<()> {
    let canvas = cover_image(source)?;
    let mut raw = Vec::with_capacity(23040);
    for p in canvas.pixels() {
        let pixel = ((p[0] as u16 >> 3) << 10) | ((p[1] as u16 >> 3) << 5) | (p[2] as u16 >> 3);
        raw.extend_from_slice(&pixel.to_be_bytes());
    }
    let mut z = ZlibEncoder::new(Vec::new(), Compression::best());
    z.write_all(&raw)?;
    let payload = z.finish()?;
    let mut bytes = b"OM3DCOV2".to_vec();
    bytes.extend_from_slice(&96u16.to_be_bytes());
    bytes.extend_from_slice(&120u16.to_be_bytes());
    bytes.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    bytes.extend_from_slice(&crc32fast::hash(&payload).to_be_bytes());
    bytes.extend_from_slice(&23040u32.to_be_bytes());
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
pub fn preview(source: &Path, target: &Path, prefs: &Preferences, job: &Job) -> Result<()> {
    let (ffmpeg, ffprobe) = video_exes(prefs)?;
    let mut a = args(&[
        "-v",
        "error",
        "-show_streams",
        "-show_format",
        "-of",
        "json",
    ]);
    a.push(source.to_string_lossy().into_owned());
    let info: Value = serde_json::from_slice(&capture(&ffprobe, &a, 1024 * 1024, job)?)?;
    if !info["streams"]
        .as_array()
        .context("No video streams")?
        .iter()
        .any(|s| s["codec_type"] == "video")
    {
        bail!("Source has no video stream")
    }
    let sound = info["streams"]
        .as_array()
        .unwrap()
        .iter()
        .any(|s| s["codec_type"] == "audio");
    let seconds = info["format"]["duration"]
        .as_str()
        .and_then(|s| s.parse::<f64>().ok())
        .filter(|n| n.is_finite() && *n > 0.)
        .unwrap_or(15.)
        .min(15.);
    let mut a = args(&["-v", "error", "-nostdin", "-i"]);
    a.push(source.to_string_lossy().into_owned());
    a.extend(args(&["-map","0:v:0","-an","-sn","-t",&seconds.to_string(),"-vf","fps=15,scale=160:120:force_original_aspect_ratio=decrease:flags=area,pad=160:120:(ow-iw)/2:(oh-ih)/2:color=0x081021","-frames:v","225","-pix_fmt","rgb24","-f","rawvideo","pipe:1"]));
    let raw = capture(&ffmpeg, &a, 225 * 160 * 120 * 3, job)?;
    if raw.is_empty() || raw.len() % (160 * 120 * 3) != 0 {
        bail!("Incomplete video frames")
    }
    let mut blocks = vec![];
    let mut compressed = 0;
    for frame in raw.chunks(160 * 120 * 3) {
        job.check()?;
        let mut pixels = Vec::with_capacity(38400);
        for rgb in frame.chunks_exact(3) {
            let word =
                ((rgb[0] as u16 >> 3) << 10) | ((rgb[1] as u16 >> 3) << 5) | (rgb[2] as u16 >> 3);
            pixels.extend_from_slice(&word.to_be_bytes());
        }
        let mut z = ZlibEncoder::new(Vec::new(), Compression::best());
        z.write_all(&pixels)?;
        let block = z.finish()?;
        let count = blocks.len() + 1;
        let audio = if sound {
            ((count * 11025).div_ceil(15) + 3) & !3
        } else {
            0
        };
        if (count + 1) * 4 + compressed + block.len() + audio > 512 * 1024 {
            break;
        }
        compressed += block.len();
        blocks.push(block);
    }
    if blocks.is_empty() {
        bail!("Preview cannot fit RAM budget")
    }
    let count = blocks.len();
    let mut offset = (count + 1) * 4;
    let mut video = Vec::new();
    video.extend_from_slice(&(offset as u32).to_be_bytes());
    for block in &blocks {
        offset += block.len();
        video.extend_from_slice(&(offset as u32).to_be_bytes());
    }
    for block in blocks {
        video.extend(block)
    }
    let video_bytes = video.len();
    let audio_size = if sound {
        ((count * 11025).div_ceil(15) + 3) & !3
    } else {
        0
    };
    if sound {
        let seconds = count as f64 / 15.;
        let mut a = args(&["-v", "error", "-nostdin", "-i"]);
        a.push(source.to_string_lossy().into_owned());
        a.extend(args(&[
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
            &format!(
                "volume=0.8,afade=t=in:d=0.01,afade=t=out:st={}:d=0.01",
                (seconds - 0.01).max(0.)
            ),
            "-f",
            "s8",
            "pipe:1",
        ]));
        let mut audio = capture(&ffmpeg, &a, 15 * 11025 + 8192, job)?;
        audio.resize(audio_size, 0);
        audio.truncate(audio_size);
        video.extend(audio);
    }
    let mut data = b"OM3DVID2".to_vec();
    data.extend_from_slice(&160u16.to_be_bytes());
    data.extend_from_slice(&120u16.to_be_bytes());
    data.extend_from_slice(&(video.len() as u32).to_be_bytes());
    data.extend_from_slice(&crc32fast::hash(&video).to_be_bytes());
    data.extend_from_slice(&(count as u16).to_be_bytes());
    data.extend_from_slice(&15u16.to_be_bytes());
    for n in [
        video_bytes,
        audio_size,
        if sound { 11025 } else { 0 },
        audio_size,
    ] {
        data.extend_from_slice(&(n as u32).to_be_bytes());
    }
    data.extend(video);
    data.extend_from_slice(b"END3DV02");
    f::atomic(target, &data)?;
    job.say(format!(
        "Preview: {:.1} seconds at 15 fps{}",
        count as f64 / 15.,
        if sound { " with audio" } else { "" }
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
                            "{}:{kind}:native-v3-160x120-fast96-cap15-512k",
                            f::sha256(&source, job)?
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
                let name = format!("{kind}-{}.{ext}", &f::sha256(&target, job)?[..24]);
                Ok(Prepared {
                    name,
                    path: target,
                    source: origin,
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
    if fs::metadata(path)?.len() > 1024 * 1024 + 48 {
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
        if !ended || ![(84, 105), (96, 120)].contains(&(image.width(), image.height())) {
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
        if (width, height) != (96, 120)
            || bytes == 0
            || bytes > 32768
            || data.len() != 32 + bytes
            || be(&data, 20)? != 23040
            || &data[24 + bytes..] != b"END3DC02"
            || crc32fast::hash(&data[24..24 + bytes]) != be(&data, 16)?
        {
            bail!("Invalid prepared cover")
        }
        return inflate(&data[24..24 + bytes], 23040);
    }
    if (width, height) != (64, 80)
        && !(data.starts_with(b"OM3DVID2") && (width, height) == (160, 120))
    {
        bail!("Invalid media dimensions")
    }
    let bytes = be(&data, 12)? as usize;
    let crc = be(&data, 16)?;
    if data.starts_with(b"OM3DCOV1") {
        if bytes != 10240
            || data.len() != 24 + bytes
            || be(&data, 20)? != 0
            || crc32fast::hash(&data[24..]) != crc
        {
            bail!("Invalid legacy cover")
        };
        return Ok(());
    }
    let v2 = data.starts_with(b"OM3DVID2");
    if !v2 && !data.starts_with(b"OM3DVID1") {
        bail!("Invalid preview signature")
    };
    let header = if v2 { 40 } else { 24 };
    let frames = u16::from_be_bytes(data[20..22].try_into()?) as usize;
    let fps = u16::from_be_bytes(data[22..24].try_into()?) as usize;
    if frames == 0
        || fps == 0
        || fps > if v2 { 15 } else { 12 }
        || data.len() != header + bytes + 8
        || &data[data.len() - 8..] != if v2 { b"END3DV02" } else { b"END3DV01" }
        || crc32fast::hash(&data[header..data.len() - 8]) != crc
    {
        bail!("Invalid preview bounds/checksum")
    }
    if !v2 {
        if frames > 32 || bytes != frames * 10240 {
            bail!("Invalid legacy frames")
        };
        return Ok(());
    }
    let video = be(&data, 24)? as usize;
    let audio = be(&data, 28)? as usize;
    let rate = be(&data, 32)?;
    let samples = be(&data, 36)? as usize;
    let expected = ((frames * 11025).div_ceil(fps) + 3) & !3;
    if frames > 720
        || frames > fps * 120
        || bytes > 1024 * 1024
        || video + audio != bytes
        || video <= (frames + 1) * 4
        || ((audio > 0) && (rate != 11025 || samples != audio || samples != expected))
        || (audio == 0 && (samples != 0 || rate != 0))
    {
        bail!("Invalid preview audio or RAM bounds")
    }
    let payload = &data[40..40 + video];
    if be(payload, 0)? as usize != (frames + 1) * 4 || be(payload, frames * 4)? as usize != video {
        bail!("Invalid frame table")
    };
    for frame in 0..frames {
        let a = be(payload, frame * 4)? as usize;
        let b = be(payload, (frame + 1) * 4)? as usize;
        if a >= b || b > video {
            bail!("Invalid frame offset")
        };
        inflate(&payload[a..b], width * height * 2)?;
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
