use crate::Job;
use anyhow::{bail, Context, Result};
use sha2::{Digest, Sha256};
use std::{
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
pub fn safe(root: &Path, name: &str) -> Result<PathBuf> {
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
            for entry in fs::read_dir(&p)? {
                let n = entry?.file_name();
                if n.to_string_lossy().eq_ignore_ascii_case(s) && n != raw {
                    bail!("Case-colliding path: {name}")
                }
            }
        }
        p.push(raw);
        if is_link(&p) {
            bail!("Links and junctions are unsupported: {}", p.display())
        }
    }
    Ok(p)
}
pub fn sha256(p: &Path, job: &Job) -> Result<String> {
    let mut f = File::open(p)?;
    let mut h = Sha256::new();
    let mut b = vec![0; 1024 * 1024];
    loop {
        job.check()?;
        let n = f.read(&mut b)?;
        if n == 0 {
            break;
        }
        h.update(&b[..n]);
    }
    Ok(format!("{:x}", h.finalize()))
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
    }
    t.as_file().sync_all()?;
    let actual_hash = format!("{:x}", hash.finalize());
    if expected.is_some_and(|h| h != actual_hash) {
        bail!("Source contents changed since scan: {}", source.display())
    }
    if stamp(source)? != before || sha256(t.path(), job)? != actual_hash {
        bail!(
            "Source changed or copy verification failed: {}",
            source.display()
        )
    }
    filetime::set_file_mtime(
        t.path(),
        filetime::FileTime::from_unix_time(
            (before.1 / 1_000_000_000) as i64,
            (before.1 % 1_000_000_000) as u32,
        ),
    )?;
    job.check()?;
    t.persist_noclobber(target).map_err(|e| e.error)?;
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
