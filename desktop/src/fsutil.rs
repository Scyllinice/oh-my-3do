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
pub fn fold(s: &str) -> String {
    s.to_ascii_lowercase()
}
pub fn stamp(p: &Path) -> Result<(u64, u128)> {
    let m = fs::metadata(p)?;
    Ok((
        m.len(),
        m.modified()?.duration_since(UNIX_EPOCH)?.as_nanos(),
    ))
}
pub fn is_link(p: &Path) -> bool {
    let Ok(m) = fs::symlink_metadata(p) else {
        return false;
    };
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        m.file_attributes() & 0x400 != 0
    }
    #[cfg(not(windows))]
    {
        m.file_type().is_symlink()
    }
}
pub fn root(p: &Path) -> Result<PathBuf> {
    if is_link(p) || !p.is_dir() {
        bail!("Choose a real, existing directory: {}", p.display())
    }
    Ok(p.canonicalize()?)
}
fn safe_with(root: &Path, name: &str, mut check: impl FnMut(&Path, &str, &OsStr) -> Result<()>) -> Result<PathBuf> {
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
        if p.is_dir() { check(&p, s, raw)?; }
        p.push(raw);
        if is_link(&p) {
            bail!("Links and junctions are unsupported: {}", p.display())
        }
    }
    Ok(p)
}
/// Fresh validation for writes and standalone lookups.
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
/// A directory-name snapshot for one read-only inventory operation. Construct
/// a fresh instance for every refresh; never use cached names to authorize writes.
/// Link/junction checks still run on every component of every lookup.
pub struct ReadPaths {
    root: PathBuf,
    directories: HashMap<PathBuf, HashMap<String, Vec<OsString>>>,
}
impl ReadPaths {
    pub fn new(directory: &Path) -> Result<Self> {
        Ok(Self { root: root(directory)?, directories: HashMap::new() })
    }
    pub fn root(&self) -> &Path { &self.root }
    pub fn path(&mut self, name: &str) -> Result<PathBuf> {
        let directories = &mut self.directories;
        safe_with(&self.root, name, |directory, text, raw| {
            let names = match directories.entry(directory.to_owned()) {
                std::collections::hash_map::Entry::Occupied(entry) => entry.into_mut(),
                std::collections::hash_map::Entry::Vacant(entry) => {
                    let mut names: HashMap<String, Vec<OsString>> = HashMap::new();
                    for child in fs::read_dir(directory)? {
                        let name = child?.file_name();
                        names.entry(name.to_string_lossy().to_ascii_lowercase()).or_default().push(name);
                    }
                    entry.insert(names)
                }
            };
            if names.get(&text.to_ascii_lowercase()).is_some_and(|names| names.iter().any(|name| name != raw)) {
                bail!("Case-colliding path: {name}");
            }
            Ok(())
        })
    }
}
/// Modification times within the coarsest drive resolution (FAT's two seconds).
pub fn same_time(a: u128, b: u128) -> bool {
    a.abs_diff(b) <= 2_000_000_000
}
pub fn sha256(p: &Path, job: &Job) -> Result<String> {
    hash_file(File::open(p)?, job)
}
/// Like `sha256`, but read from the drive itself rather than the system's
/// cache, so a readback after writing checks what actually reached the card.
/// The file must already be flushed.
pub fn sha256_from_disk(p: &Path, job: &Job) -> Result<String> {
    hash_file(open_from_disk(p)?, job)
}
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
const BLOCK: usize = 1024 * 1024;
/// Unbuffered Windows reads need sector-aligned memory.
fn aligned_buffer() -> (Vec<u8>, usize) {
    let b = vec![0; BLOCK + 4096];
    let start = b.as_ptr().align_offset(4096);
    (b, start)
}
fn hash_file(mut f: File, job: &Job) -> Result<String> {
    let total = f.metadata()?.len();let mut completed = 0;
    job.report(0,total,"Verifying file");
    let mut h = Sha256::new();
    let (mut b, start) = aligned_buffer();
    loop {
        job.check()?;
        let n = f.read(&mut b[start..start + BLOCK])?;
        if n == 0 {
            break;
        }
        h.update(&b[start..start + n]);completed += n as u64;job.report(completed,total,"Verifying file");
    }
    Ok(format!("{:x}", h.finalize()))
}
#[cfg(windows)]
fn open_from_disk(p: &Path) -> Result<File> {
    use std::os::windows::fs::OpenOptionsExt;
    const FILE_FLAG_NO_BUFFERING: u32 = 0x2000_0000;
    Ok(fs::OpenOptions::new().read(true).custom_flags(FILE_FLAG_NO_BUFFERING).open(p)?)
}
#[cfg(target_os = "linux")]
fn open_from_disk(p: &Path) -> Result<File> {
    use std::os::fd::AsRawFd;
    extern "C" {
        fn posix_fadvise(fd: i32, offset: i64, len: i64, advice: i32) -> i32;
    }
    const POSIX_FADV_DONTNEED: i32 = 4;
    let f = File::open(p)?;
    let error = unsafe { posix_fadvise(f.as_raw_fd(), 0, 0, POSIX_FADV_DONTNEED) };
    anyhow::ensure!(error == 0, "Could not bypass the read cache for {} (error {error})", p.display());
    Ok(f)
}
#[cfg(target_os = "macos")]
fn open_from_disk(p: &Path) -> Result<File> {
    use std::os::fd::AsRawFd;
    extern "C" {
        fn fcntl(fd: i32, cmd: i32, ...) -> i32;
    }
    const F_NOCACHE: i32 = 48;
    let f = File::open(p)?;
    anyhow::ensure!(unsafe { fcntl(f.as_raw_fd(), F_NOCACHE, 1) } != -1, "Could not bypass the read cache for {}", p.display());
    Ok(f)
}
#[cfg(not(any(windows, target_os = "linux", target_os = "macos")))]
fn open_from_disk(p: &Path) -> Result<File> {
    Ok(File::open(p)?)
}
pub fn atomic(p: &Path, data: &[u8]) -> Result<()> {
    let parent = p.parent().context("Missing parent")?;
    fs::create_dir_all(parent)?;
    let mut t = tempfile::NamedTempFile::new_in(parent)?;
    t.write_all(data)?;
    t.as_file().sync_all()?;
    t.persist(p).map_err(|e| e.error)?;
    Ok(())
}
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
    let total = before.0.saturating_mul(2).max(1);let mut completed = 0;
    job.report(0,total,"Copying and verifying file");
    let mut hash = Sha256::new();
    let mut b = vec![0; 1024 * 1024];
    loop {
        job.check()?;
        let n = src.read(&mut b)?;
        if n == 0 {
            break;
        }
        t.write_all(&b[..n])?;
        hash.update(&b[..n]);completed += n as u64;job.report(completed,total,"Copying and verifying file");
    }
    t.as_file().sync_all()?;
    let actual_hash = format!("{:x}", hash.finalize());
    if expected.is_some_and(|h| h != actual_hash) {
        bail!("Source contents changed since scan: {}", source.display())
    }
    let verify_job = job.progress_range(before.0,before.0,total,"Copying and verifying file");
    if stamp(source)? != before || sha256_from_disk(t.path(), &verify_job)? != actual_hash {
        bail!(
            "Source changed or copy verification failed: {}",
            source.display()
        )
    }
    // Set through the writing handle: Windows restamps a written file when
    // that handle closes, overriding a date set through any other handle.
    t.as_file().set_modified(UNIX_EPOCH + std::time::Duration::from_nanos(before.1 as u64))?;
    job.check()?;
    drop(t.persist_noclobber(target).map_err(|e| e.error)?);
    if !same_time(stamp(target)?.1, before.1) {
        job.say(format!("Copied and verified, but the drive did not keep the original date: {}", target.display()));
    }
    job.report(total,total,"Copying and verifying file");
    Ok(actual_hash)
}
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
