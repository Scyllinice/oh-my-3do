//! Published stable releases and verified portable downloads.
use crate::{fsutil as f, library, setup, Job};
use anyhow::{ensure, Context, Result};
use reqwest::blocking::Client;
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
};
const REPO: &str = "https://github.com/Scyllinice/oh-my-3do";
#[derive(Clone, Debug)]
pub struct Release {
    pub tag: String,
    pub version: String,
    pub notes: String,
    assets: Vec<(String, String)>,
}
#[derive(Clone, Debug)]
pub struct BootState {
    pub root: PathBuf,
    pub digest: Option<String>,
}
#[derive(Clone, Debug)]
pub struct Check {
    pub release: Release,
    pub boot_digest: String,
    pub targets: Vec<BootState>,
    pub warnings: Vec<String>,
}
fn version(value: &str) -> Result<[u64; 3]> {
    let parts: Vec<_> = value.split('.').collect();
    ensure!(parts.len() == 3, "Unsupported release version");
    let mut numbers = [0; 3];
    for (i, p) in parts.iter().enumerate() {
        ensure!(
            !p.is_empty()
                && p.bytes().all(|b| b.is_ascii_digit())
                && (p.len() == 1 || !p.starts_with('0')),
            "Unsupported release version"
        );
        numbers[i] = p.parse()?;
    }
    Ok(numbers)
}
pub fn is_newer(latest: &str, installed: &str) -> Result<bool> {
    Ok(version(latest)? > version(installed)?)
}
impl Release {
    pub fn parse(data: &[u8]) -> Result<Self> {
        let value: Value = serde_json::from_slice(data)?;
        ensure!(
            value["draft"] == false && value["prerelease"] == false,
            "Release is not a stable public release"
        );
        let tag = value["tag_name"]
            .as_str()
            .context("Release tag missing")?
            .to_owned();
        let number = tag
            .strip_prefix('v')
            .context("Unsupported release tag")?
            .to_owned();
        version(&number)?;
        let mut assets = Vec::new();
        for asset in value["assets"]
            .as_array()
            .context("Release assets missing")?
        {
            let name = asset["name"].as_str().context("Asset name missing")?;
            let url = asset["browser_download_url"]
                .as_str()
                .context("Asset URL missing")?;
            ensure!(
                !assets.iter().any(|(n, _)| n == name),
                "Duplicate release asset"
            );
            // Require this repository, release tag and filename, not a shared prefix.
            ensure!(
                !name.is_empty()
                    && name
                        .bytes()
                        .all(|b| b.is_ascii_alphanumeric() || b".-_".contains(&b))
                    && url == format!("{REPO}/releases/download/{tag}/{name}"),
                "Unexpected release asset URL"
            );
            assets.push((name.to_owned(), url.to_owned()));
        }
        Ok(Self {
            tag,
            version: number,
            notes: value["body"]
                .as_str()
                .unwrap_or("")
                .chars()
                .take(12000)
                .collect(),
            assets,
        })
    }
    pub fn page(&self) -> String {
        format!("{REPO}/releases/tag/{}", self.tag)
    }
    fn asset(&self, name: &str) -> Result<&str> {
        self.assets
            .iter()
            .find(|(n, _)| n == name)
            .map(|(_, u)| u.as_str())
            .with_context(|| format!("Release is missing {name}"))
    }
    pub fn desktop_name(&self, platform: &str) -> Result<String> {
        ensure!(
            ["Windows-x64", "Linux-x64", "macOS-arm64", "macOS-x64"].contains(&platform),
            "Unsupported desktop platform"
        );
        let name = format!("OhMy3DO-Library-{platform}-v{}.zip", self.version);
        self.asset(&name)?;
        self.asset(&format!("{name}.sha256"))?;
        Ok(name)
    }
    fn digest(&self, client: &Client, name: &str, job: &Job) -> Result<String> {
        let bytes = setup::download(client, self.asset(&format!("{name}.sha256"))?, 1024, job)?;
        checksum(std::str::from_utf8(&bytes)?, name)
    }
    fn bytes(&self, name: &str, limit: usize, client: &Client, job: &Job) -> Result<Vec<u8>> {
        let expected = self.digest(client, name, job)?;
        let bytes = setup::download(client, self.asset(name)?, limit, job)?;
        ensure!(
            format!("{:x}", Sha256::digest(&bytes)) == expected,
            "Download checksum mismatch for {name}"
        );
        Ok(bytes)
    }
}
pub fn checksum(text: &str, name: &str) -> Result<String> {
    let fields: Vec<_> = text.split_whitespace().collect();
    ensure!(
        fields.len() == 2
            && fields[0].len() == 64
            && fields[0].bytes().all(|b| b.is_ascii_hexdigit())
            && fields[1].strip_prefix('*').unwrap_or(fields[1]) == name,
        "Invalid published checksum for {name}"
    );
    Ok(fields[0].to_ascii_lowercase())
}
pub fn platform() -> Option<&'static str> {
    match (std::env::consts::OS, std::env::consts::ARCH) {
        ("windows", "x86_64") => Some("Windows-x64"),
        ("linux", "x86_64") => Some("Linux-x64"),
        ("macos", "aarch64") => Some("macOS-arm64"),
        ("macos", "x86_64") => Some("macOS-x64"),
        _ => None,
    }
}
fn boot_digest(root: &Path, job: &Job) -> Result<Option<String>> {
    let path = f::safe(root, "boot.iso")?;
    if !path.exists() {
        return Ok(None);
    }
    ensure!(
        path.is_file() && fs::metadata(&path)?.len() <= 16 * 1024 * 1024,
        "Existing boot.iso is not a menu image under 16 MiB"
    );
    Ok(Some(f::sha256(&path, job)?))
}
pub fn check(roots: &[PathBuf], job: &Job) -> Result<Check> {
    let client = setup::client()?;
    let release = Release::parse(&setup::download(
        &client,
        "https://api.github.com/repos/Scyllinice/oh-my-3do/releases/latest",
        1024 * 1024,
        job,
    )?)?;
    let digest = release.digest(&client, "boot.iso", job)?;
    let mut targets: Vec<BootState> = Vec::new();
    let mut warnings = Vec::new();
    for path in roots {
        job.check()?;
        if path.as_os_str().is_empty() {
            continue;
        }
        match f::root(path).and_then(|root| {
            Ok(BootState {
                digest: boot_digest(&root, job)?,
                root,
            })
        }) {
            Ok(state) if !targets.iter().any(|s| s.root == state.root) => targets.push(state),
            Ok(_) => (),
            Err(e) => {
                let warning = format!("Cannot check {}: {e:#}", path.display());
                job.say(&warning);
                warnings.push(warning);
            }
        }
    }
    job.check()?;
    Ok(Check {
        release,
        boot_digest: digest,
        targets,
        warnings,
    })
}
pub fn download_desktop(
    release: &Release,
    platform: &str,
    destination: &Path,
    job: &Job,
) -> Result<()> {
    ensure!(
        !destination.exists(),
        "Choose a new download filename; existing files are preserved"
    );
    let name = release.desktop_name(platform)?;
    job.say(format!("Downloading {name}"));
    let bytes = release.bytes(&name, 256 * 1024 * 1024, &setup::client()?, job)?;
    let zip = zip::ZipArchive::new(std::io::Cursor::new(&bytes)).context("Invalid desktop ZIP")?;
    ensure!(!zip.is_empty(), "Empty desktop ZIP");
    job.check()?;
    write_download(destination, &bytes, job)
}
fn write_download(destination: &Path, bytes: &[u8], job: &Job) -> Result<()> {
    let mut file = tempfile::NamedTempFile::new_in(
        destination
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .unwrap_or(Path::new(".")),
    )?;
    file.write_all(bytes)?;
    file.as_file().sync_all()?;
    job.check()?;
    file.persist_noclobber(destination)
        .map_err(|e| e.error)
        .context("Could not save download; existing files are preserved")?;
    Ok(())
}
fn menu_bytes(release: &Release, digest: &str, job: &Job) -> Result<Vec<u8>> {
    let bytes = release.bytes("boot.iso", 16 * 1024 * 1024, &setup::client()?, job)?;
    setup::verify_image(&bytes, &format!("{digest}  boot.iso"))?;
    Ok(bytes)
}
pub fn download_boot(release: &Release, digest: &str, destination: &Path, job: &Job) -> Result<()> {
    ensure!(
        !destination.exists(),
        "Choose a new download filename; existing files are preserved"
    );
    job.say(format!("Downloading {} boot.iso", release.tag));
    write_download(destination, &menu_bytes(release, digest, job)?, job)
}
pub fn install_boot(
    release: &Release,
    expected: &BootState,
    digest: &str,
    cache: &Path,
    job: &Job,
) -> Result<PathBuf> {
    // Pin to the release shown in the offer, even if its assets were replaced.
    let bytes = menu_bytes(release, digest, job)?;
    install_boot_bytes(expected, &bytes, cache, job)
}
/// Update the menu and create missing runtime support, preserving existing user files.
pub fn install_boot_bytes(
    expected: &BootState,
    bytes: &[u8],
    cache: &Path,
    job: &Job,
) -> Result<PathBuf> {
    let digest = format!("{:x}", Sha256::digest(bytes));
    setup::verify_image(bytes, &format!("{digest}  boot.iso"))?;
    let root = f::root(&expected.root)?;
    ensure!(
        root == expected.root,
        "Update target changed; check for updates again"
    );
    let _lock = library::lock_library(&root, cache)?;
    ensure!(
        boot_digest(&root, job)? == expected.digest,
        "boot.iso changed since the update check; check again"
    );
    let mut tx = library::Journal::new(&root, cache)?;
    let result = (|| -> Result<()> {
        job.check()?;
        setup::ensure_support(&root, &mut tx, &Default::default(), job)?;
        tx.write("boot.iso", bytes)?;
        ensure!(
            fs::read(f::safe(&root, "boot.iso")?)? == bytes,
            "Menu update readback failed"
        );
        job.check()?;
        tx.finish()
    })();
    if let Err(e) = result {
        tx.undo().context(format!(
            "{e:#}; rollback failed; keep {}",
            tx.path.display()
        ))?;
        return Err(e);
    }
    Ok(tx.path)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::Ordering;
    #[test]
    fn verified_download_publication_is_atomic_and_never_clobbers() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("download.zip");
        write_download(&path, b"original", &Job::default()).unwrap();
        assert!(write_download(&path, b"replacement", &Job::default()).is_err());
        assert_eq!(fs::read(&path).unwrap(), b"original");
        let job = Job::default();
        job.cancel.store(true, Ordering::Relaxed);
        assert!(write_download(&dir.path().join("cancelled.zip"), b"partial", &job).is_err());
        assert_eq!(fs::read_dir(dir.path()).unwrap().count(), 1);
    }
}
