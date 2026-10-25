//! File helpers shared by the whole engine, written for the drives the
//! console reads: FAT-formatted, case-insensitive, and often removable.
//! Library paths are checked before use, files are replaced atomically, and
//! copies are verified by reading them back from the drive itself.

use crate::Job;
use anyhow::{bail, Context, Result};
use sha2::{Digest, Sha256};
use std::{
    collections::HashMap,
    ffi::{OsStr, OsString},
    fs::{self, File},
    io::{Read, Write},
    path::{Component, Path, PathBuf},
    time::UNIX_EPOCH,
};

/// A name's case-insensitive form, for comparing names as FAT does.
pub fn fold(s: &str) -> String {
    s.to_ascii_lowercase()
}

/// A file's size and modification time (in nanoseconds since 1970), to
/// tell later whether it changed.
pub fn stamp(p: &Path) -> Result<(u64, u128)> {
    let m = fs::metadata(p)?;
    Ok((
        m.len(),
        m.modified()?.duration_since(UNIX_EPOCH)?.as_nanos(),
    ))
}

/// Whether `p` is a symbolic link, or on Windows any reparse point
/// (junctions included). The engine never follows them.
pub fn is_link(p: &Path) -> bool {
    let Ok(m) = fs::symlink_metadata(p) else {
        return false;
    };
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;
        m.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0
    }
    #[cfg(not(windows))]
    {
        m.file_type().is_symlink()
    }
}

/// Checks that `p` is an existing folder (not a link) and returns its full
/// path, for use as the root of a library or drive.
pub fn root(p: &Path) -> Result<PathBuf> {
    if is_link(p) || !p.is_dir() {
        bail!("Choose a real, existing directory: {}", p.display())
    }
    Ok(p.canonicalize()?)
}

/// Joins `name`, a relative path with '/' separators, onto `root`, refusing
/// anything that could escape the root or that a FAT drive (or Windows)
/// cannot store: "..", absolute paths, backslashes, control characters,
/// reserved device names like CON, and names ending in a space or dot. Each
/// existing folder along the way is passed to `check` with the next name, so
/// the caller can refuse names differing only in case from one already
/// there. Links along the way are refused.
fn safe_with(
    root: &Path,
    name: &str,
    mut check: impl FnMut(&Path, &str, &OsStr) -> Result<()>,
) -> Result<PathBuf> {
    if name.is_empty()
        || name.contains(['\\', ':'])
        || name.chars().any(|c| c < ' ' || c == '\u{7f}')
    {
        bail!("Invalid library path: {name}")
    }
    let mut p = root.to_path_buf();
    for part in Path::new(name).components() {
        let Component::Normal(raw) = part else {
            bail!("Unsafe library path: {name}")
        };
        let s = raw.to_str().context("Non-UTF8 filenames are unsupported")?;
        if s.ends_with([' ', '.']) || s.contains(['<', '>', '"', '|', '?', '*']) || s.len() >= 1024
        {
            bail!("Unsupported FAT filename: {s}")
        }
        let base = s.split('.').next().unwrap_or("").to_ascii_uppercase();
        if ["CON", "PRN", "AUX", "NUL"].contains(&base.as_str())
            || ((base.starts_with("COM") || base.starts_with("LPT"))
                && base.len() == 4
                && matches!(base.as_bytes()[3], b'1'..=b'9'))
        {
            bail!("Reserved FAT filename: {s}")
        }
        if p.is_dir() {
            check(&p, s, raw)?;
        }
        p.push(raw);
        if is_link(&p) {
            bail!("Links and junctions are unsupported: {}", p.display())
        }
    }
    Ok(p)
}

/// The full path of library path `name` below `root`, checked as
/// `safe_with` describes, reading each folder afresh to refuse names that
/// differ only in case. Use this for every write.
pub fn safe(root: &Path, name: &str) -> Result<PathBuf> {
    safe_with(root, name, |directory, text, raw| {
        for entry in fs::read_dir(directory)? {
            let entry_name = entry?.file_name();
            if entry_name.to_string_lossy().eq_ignore_ascii_case(text) && entry_name != raw {
                bail!("Case-colliding path: {name}");
            }
        }
        Ok(())
    })
}

/// Checked lookups for one read-only pass over a library or drive, which
/// remembers each folder's names instead of reading it again for every
/// path. Make a new one for every pass, and never use one to approve a
/// write: names it remembers may be out of date. Links are still checked on
/// every lookup.
pub struct ReadPaths {
    root: PathBuf,
    /// Each folder read so far: its names by their case-insensitive form.
    directories: HashMap<PathBuf, HashMap<String, Vec<OsString>>>,
}

impl ReadPaths {
    pub fn new(directory: &Path) -> Result<Self> {
        Ok(Self {
            root: root(directory)?,
            directories: HashMap::new(),
        })
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    /// As [`safe`], but using the remembered folder listings.
    pub fn path(&mut self, name: &str) -> Result<PathBuf> {
        let directories = &mut self.directories;
        safe_with(&self.root, name, |directory, text, raw| {
            let names = match directories.entry(directory.to_owned()) {
                std::collections::hash_map::Entry::Occupied(entry) => entry.into_mut(),
                std::collections::hash_map::Entry::Vacant(entry) => {
                    let mut names: HashMap<String, Vec<OsString>> = HashMap::new();
                    for child in fs::read_dir(directory)? {
                        let name = child?.file_name();
                        names
                            .entry(name.to_string_lossy().to_ascii_lowercase())
                            .or_default()
                            .push(name);
                    }
                    entry.insert(names)
                }
            };
            if names
                .get(&text.to_ascii_lowercase())
                .is_some_and(|names| names.iter().any(|name| name != raw))
            {
                bail!("Case-colliding path: {name}");
            }
            Ok(())
        })
    }
}

/// Whether two modification times (in nanoseconds) are the same to within
/// the coarsest drive's resolution (FAT's two seconds).
pub fn same_time(a: u128, b: u128) -> bool {
    a.abs_diff(b) <= 2_000_000_000
}

/// The SHA-256 of a file, as lowercase hex, reporting progress.
pub fn sha256(p: &Path, job: &Job) -> Result<String> {
    hash_file(File::open(p)?, job)
}

/// Like `sha256`, but read from the drive itself rather than the system's
/// cache, so a readback after writing checks what actually reached the card.
/// The file must already be flushed.
pub fn sha256_from_disk(p: &Path, job: &Job) -> Result<String> {
    hash_file(open_from_disk(p)?, job)
}

/// Reads a whole file from the drive itself, bypassing the system's cache
/// (see `sha256_from_disk`).
pub fn read_from_disk(p: &Path) -> Result<Vec<u8>> {
    let mut f = open_from_disk(p)?;
    let (mut b, start) = aligned_buffer();
    let mut out = vec![];
    loop {
        let n = f.read(&mut b[start..start + BLOCK])?;
        if n == 0 {
            return Ok(out);
        }
        out.extend_from_slice(&b[start..start + n]);
    }
}

/// How much is read at a time when hashing or reading from the drive.
const BLOCK: usize = 1024 * 1024;

/// A buffer of BLOCK bytes starting at a 4096-byte boundary (returned as the
/// vector and the offset of the boundary in it): unbuffered Windows reads
/// need sector-aligned memory.
fn aligned_buffer() -> (Vec<u8>, usize) {
    let b = vec![0; BLOCK + 4096];
    let start = b.as_ptr().align_offset(4096);
    (b, start)
}

/// The SHA-256 of everything read from `f`, reporting progress and
/// stopping if the job is canceled.
fn hash_file(mut f: File, job: &Job) -> Result<String> {
    let total = f.metadata()?.len();
    let mut completed = 0;
    job.report(0, total, "Verifying file");
    let mut h = Sha256::new();
    let (mut b, start) = aligned_buffer();
    loop {
        job.check()?;
        let n = f.read(&mut b[start..start + BLOCK])?;
        if n == 0 {
            break;
        }
        h.update(&b[start..start + n]);
        completed += n as u64;
        job.report(completed, total, "Verifying file");
    }
    Ok(format!("{:x}", h.finalize()))
}

/// Opens a file for reading without the system's cache (Windows).
#[cfg(windows)]
fn open_from_disk(p: &Path) -> Result<File> {
    use std::os::windows::fs::OpenOptionsExt;
    const FILE_FLAG_NO_BUFFERING: u32 = 0x2000_0000;
    Ok(fs::OpenOptions::new()
        .read(true)
        .custom_flags(FILE_FLAG_NO_BUFFERING)
        .open(p)?)
}

/// Opens a file for reading after dropping it from the system's cache
/// (Linux), so the reads go to the drive.
#[cfg(target_os = "linux")]
fn open_from_disk(p: &Path) -> Result<File> {
    use std::os::fd::AsRawFd;
    extern "C" {
        fn posix_fadvise(fd: i32, offset: i64, len: i64, advice: i32) -> i32;
    }
    const POSIX_FADV_DONTNEED: i32 = 4;
    let f = File::open(p)?;
    let error = unsafe { posix_fadvise(f.as_raw_fd(), 0, 0, POSIX_FADV_DONTNEED) };
    anyhow::ensure!(
        error == 0,
        "Could not bypass the read cache for {} (error {error})",
        p.display()
    );
    Ok(f)
}

/// Opens a file for reading with caching turned off (macOS).
#[cfg(target_os = "macos")]
fn open_from_disk(p: &Path) -> Result<File> {
    use std::os::fd::AsRawFd;
    extern "C" {
        fn fcntl(fd: i32, cmd: i32, ...) -> i32;
    }
    const F_NOCACHE: i32 = 48;
    let f = File::open(p)?;
    anyhow::ensure!(
        unsafe { fcntl(f.as_raw_fd(), F_NOCACHE, 1) } != -1,
        "Could not bypass the read cache for {}",
        p.display()
    );
    Ok(f)
}

/// Elsewhere, an ordinary open: the cache cannot be bypassed.
#[cfg(not(any(windows, target_os = "linux", target_os = "macos")))]
fn open_from_disk(p: &Path) -> Result<File> {
    Ok(File::open(p)?)
}

/// Replaces (or creates) the file `p` with `data` all at once: written to a
/// temporary file beside it, flushed to the drive, then renamed over it, so
/// the file is never left half-written.
pub fn atomic(p: &Path, data: &[u8]) -> Result<()> {
    let parent = p.parent().context("Missing parent")?;
    fs::create_dir_all(parent)?;
    let mut t = tempfile::NamedTempFile::new_in(parent)?;
    t.write_all(data)?;
    t.as_file().sync_all()?;
    t.persist(p).map_err(|e| e.error)?;
    Ok(())
}

/// Copies `source` to `target`, which must not exist yet, and returns the
/// copy's SHA-256. The copy is written under a temporary name, flushed, read
/// back from the drive and compared, and only then given its name, with the
/// source's modification time. Fails if the source changes during the copy,
/// or if `expected` is given and the source's SHA-256 differs from it.
/// Progress counts the copy and the check (twice the file's size).
pub fn copy_new(source: &Path, target: &Path, job: &Job, expected: Option<&str>) -> Result<String> {
    if target.exists() {
        bail!("Refusing to replace {}", target.display())
    }
    let before = stamp(source)?;
    if before.0 >= 4 * 1024u64.pow(3) {
        bail!("File exceeds FAT32 limit")
    }
    let parent = target.parent().context("Missing destination parent")?;
    fs::create_dir_all(parent)?;
    let mut t = tempfile::NamedTempFile::new_in(parent)?;
    let mut src = File::open(source)?;
    let total = before.0.saturating_mul(2).max(1);
    let mut completed = 0;

    // Copy, hashing what was read.
    job.report(0, total, "Copying and verifying file");
    let mut hash = Sha256::new();
    let mut b = vec![0; 1024 * 1024];
    loop {
        job.check()?;
        let n = src.read(&mut b)?;
        if n == 0 {
            break;
        }
        t.write_all(&b[..n])?;
        hash.update(&b[..n]);
        completed += n as u64;
        job.report(completed, total, "Copying and verifying file");
    }
    t.as_file().sync_all()?;
    let actual_hash = format!("{:x}", hash.finalize());
    if expected.is_some_and(|h| h != actual_hash) {
        bail!("Source contents changed since scan: {}", source.display())
    }

    // Read the copy back from the drive and compare.
    let verify_job = job.progress_range(before.0, before.0, total, "Copying and verifying file");
    if stamp(source)? != before || sha256_from_disk(t.path(), &verify_job)? != actual_hash {
        bail!(
            "Source changed or copy verification failed: {}",
            source.display()
        )
    }
    // Set through the writing handle: Windows restamps a written file when
    // that handle closes, overriding a date set through any other handle.
    t.as_file()
        .set_modified(UNIX_EPOCH + std::time::Duration::from_nanos(before.1 as u64))?;
    job.check()?;
    drop(t.persist_noclobber(target).map_err(|e| e.error)?);
    if !same_time(stamp(target)?.1, before.1) {
        job.say(format!(
            "Copied and verified, but the drive did not keep the original date: {}",
            target.display()
        ));
    }
    job.report(total, total, "Copying and verifying file");
    Ok(actual_hash)
}

/// Fails if `p` (which need not exist yet) is inside `root`, so reports and
/// backups are never written into the library they describe.
pub fn outside(p: &Path, root: &Path) -> Result<()> {
    let absolute = if p.exists() {
        p.canonicalize()?
    } else {
        let parent = p.parent().context("No parent")?;
        parent
            .canonicalize()?
            .join(p.file_name().context("No filename")?)
    };
    if absolute.starts_with(root) {
        bail!("Keep reports and backups outside the library")
    };
    Ok(())
}
