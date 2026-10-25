//! Published stable releases and verified portable downloads.
use crate::{fsutil as f, library, setup, Job};
use anyhow::{ensure, Context, Result};
use reqwest::blocking::Client;
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
};
pub const REPO: &str = "https://github.com/Scyllinice/oh-my-3do";
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Component {
    Menu,
    Desktop,
}
#[derive(Clone, Debug)]
pub struct Release {
    /// None identifies a historical combined vX.Y.Z release.
    pub component: Option<Component>,
    pub tag: String,
    pub version: String,
    pub notes: String,
    assets: Vec<(String, String)>,
    local: Option<LocalBuild>,
}
#[derive(Clone, Debug)]
struct LocalBuild {
    root: PathBuf,
    artifact: String,
    digest: String,
    binary_digest: Option<String>,
}
#[derive(Clone, Debug)]
pub struct Offers {
    pub menu: Option<Release>,
    pub desktop: Option<Release>,
    pub boot_digest: Option<String>,
    pub targets: Vec<BootState>,
    pub warnings: Vec<String>,
}
impl From<Check> for Offers {
    fn from(check: Check) -> Self {
        Self {
            menu: Some(check.menu),
            desktop: Some(check.desktop),
            boot_digest: Some(check.boot_digest),
            targets: check.targets,
            warnings: check.warnings,
        }
    }
}
#[derive(Clone, Debug)]
pub struct BootState {
    pub root: PathBuf,
    pub digest: Option<String>,
}
#[derive(Clone, Debug)]
pub struct Check {
    pub menu: Release,
    pub desktop: Release,
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
        let (component, number) = if let Some(number) = tag.strip_prefix("menu-v") {
            (Some(Component::Menu), number)
        } else if let Some(number) = tag.strip_prefix("desktop-v") {
            (Some(Component::Desktop), number)
        } else {
            (
                None,
                tag.strip_prefix('v').context("Unsupported release tag")?,
            )
        };
        let number = number.to_owned();
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
            component,
            tag,
            version: number,
            notes: value["body"]
                .as_str()
                .unwrap_or("")
                .chars()
                .take(12000)
                .collect(),
            assets,
            local: None,
        })
    }
    pub fn is_local(&self) -> bool {
        self.local.is_some()
    }
    pub fn binary_digest(&self) -> Option<&str> {
        self.local.as_ref().and_then(|b| b.binary_digest.as_deref())
    }
    pub fn desktop_available(&self, executable: &Path, installed: &str, job: &Job) -> Result<bool> {
        if let Some(digest) = self.binary_digest() {
            return Ok(f::sha256(executable, job)? != digest);
        }
        is_newer(&self.version, installed)
    }
    pub fn page(&self) -> String {
        if let Some(build) = &self.local {
            return build.root.display().to_string();
        }
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
            self.component != Some(Component::Menu),
            "Not a desktop release"
        );
        ensure!(
            ["Windows-x64", "Linux-x64", "macOS-arm64", "macOS-x64"].contains(&platform),
            "Unsupported desktop platform"
        );
        let name = format!("OhMy3DO-Library-{platform}-v{}.zip", self.version);
        if let Some(build) = &self.local {
            ensure!(
                name == build.artifact,
                "Local build does not match this platform/version"
            );
            return Ok(name);
        }
        self.asset(&name)?;
        self.asset(&format!("{name}.sha256"))?;
        Ok(name)
    }
    pub(crate) fn digest(&self, client: &Client, name: &str, job: &Job) -> Result<String> {
        if let Some(build) = &self.local {
            job.check()?;
            ensure!(name == build.artifact, "Unexpected local build artifact");
            return Ok(build.digest.clone());
        }
        let bytes = setup::download(client, self.asset(&format!("{name}.sha256"))?, 1024, job)?;
        checksum(std::str::from_utf8(&bytes)?, name)
    }
    pub(crate) fn bytes(
        &self,
        name: &str,
        limit: usize,
        client: &Client,
        job: &Job,
    ) -> Result<Vec<u8>> {
        let expected = self.digest(client, name, job)?;
        let bytes = if let Some(build) = &self.local {
            read_local(&build.root, name, limit, job)?
        } else {
            setup::download(client, self.asset(name)?, limit, job)?
        };
        ensure!(
            format!("{:x}", Sha256::digest(&bytes)) == expected,
            "Update checksum mismatch for {name}; check for updates again if the build changed"
        );
        Ok(bytes)
    }
}
fn read_local(root: &Path, name: &str, limit: usize, job: &Job) -> Result<Vec<u8>> {
    let path = f::safe(root, name)?;
    let mut file = fs::File::open(&path)
        .with_context(|| format!("Cannot read local update: {}", path.display()))?;
    let size = file.metadata()?.len();
    ensure!(
        size <= limit as u64,
        "Local update exceeds size limit: {name}"
    );
    let mut bytes = Vec::new();
    let mut buffer = [0u8; 32768];
    loop {
        job.check()?;
        let count = file.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        ensure!(
            bytes.len() + count <= limit,
            "Local update exceeds size limit: {name}"
        );
        bytes.extend_from_slice(&buffer[..count]);
        job.report(bytes.len() as u64, size, "Reading local update");
    }
    Ok(bytes)
}
/// Local manifests are explicit developer inputs. Artifact hashes stay pinned
/// to the offer, so rebuilding a file during confirmation requires a new check.
pub fn local_release(directory: &Path, component: Component, job: &Job) -> Result<Option<Release>> {
    let root = f::root(directory).context("Local update directory is unavailable")?;
    let platform = platform().context("No desktop package for this platform")?;
    let manifest = match component {
        Component::Menu => "menu-update.json".into(),
        Component::Desktop => format!("desktop-update-{platform}.json"),
    };
    if !f::safe(&root, &manifest)?.exists() {
        return Ok(None);
    }
    let value: Value = serde_json::from_slice(&read_local(&root, &manifest, 16 * 1024, job)?)?;
    let kind = if component == Component::Menu {
        "menu"
    } else {
        "desktop"
    };
    ensure!(
        value["schema"] == 1 && value["component"] == kind,
        "Invalid local update manifest: {manifest}"
    );
    let number = value["version"]
        .as_str()
        .context("Local build version missing")?;
    version(number)?;
    let name = value["artifact"]
        .as_str()
        .context("Local artifact missing")?;
    let expected = if component == Component::Menu {
        "boot.iso".into()
    } else {
        ensure!(
            value["platform"] == platform,
            "Local desktop platform mismatch"
        );
        format!("OhMy3DO-Library-{platform}-v{number}.zip")
    };
    ensure!(name == expected, "Unexpected local artifact filename");
    let digest = value["sha256"].as_str().context("Local checksum missing")?;
    let valid_hash = |s: &str| s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit());
    ensure!(valid_hash(digest), "Invalid local artifact checksum");
    let binary_digest = if component == Component::Desktop {
        let hash = value["binary_sha256"]
            .as_str()
            .context("Local executable checksum missing")?;
        ensure!(valid_hash(hash), "Invalid local executable checksum");
        Some(hash.to_ascii_lowercase())
    } else {
        None
    };
    ensure!(
        f::safe(&root, name)?.is_file(),
        "Local update artifact is missing: {name}"
    );
    Ok(Some(Release {
        component: Some(component),
        tag: format!("{kind}-v{number}"),
        version: number.into(),
        notes: format!(
            "Local developer build\nDirectory: {}\nArtifact: {name}\nSHA-256: {digest}",
            root.display()
        ),
        assets: vec![],
        local: Some(LocalBuild {
            root,
            artifact: name.into(),
            digest: digest.to_ascii_lowercase(),
            binary_digest,
        }),
    }))
}
pub fn check_local(directory: &Path, roots: &[PathBuf], job: &Job) -> Result<Offers> {
    let menu = local_release(directory, Component::Menu, job)?;
    let desktop = local_release(directory, Component::Desktop, job)?;
    ensure!(menu.is_some() || desktop.is_some(), "No local update manifests found. Choose the build output directory containing menu-update.json or desktop-update-{}.json", platform().unwrap_or("PLATFORM"));
    let digest = menu
        .as_ref()
        .and_then(|r| r.local.as_ref())
        .map(|b| b.digest.clone());
    let (targets, warnings) = if menu.is_some() {
        scan_targets(roots, job)?
    } else {
        (vec![], vec![])
    };
    Ok(Offers {
        menu,
        desktop,
        boot_digest: digest,
        targets,
        warnings,
    })
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
/// Version labels are bound to image contents, never inferred from a filename
/// or a previously selected drive. Unknown images remain explicitly unknown.
pub fn remember_menu(cache: &Path, digest: &str, number: &str) -> Result<()> {
    ensure!(
        digest.len() == 64 && digest.bytes().all(|b| b.is_ascii_hexdigit()),
        "Invalid menu digest"
    );
    version(number)?;
    let path = cache.join("menu-versions.json");
    let mut known: std::collections::BTreeMap<String, String> = fs::read(&path)
        .ok()
        .filter(|b| b.len() <= 1024 * 1024)
        .and_then(|b| serde_json::from_slice(&b).ok())
        .unwrap_or_default();
    known.insert(digest.to_ascii_lowercase(), number.into());
    f::atomic(&path, &serde_json::to_vec_pretty(&known)?)
}
pub fn installed_menu(root: &Path, cache: &Path, job: &Job) -> Result<Value> {
    let root = f::root(root)?;
    let digest = boot_digest(&root, job)?;
    let known: std::collections::BTreeMap<String, String> =
        fs::read(cache.join("menu-versions.json"))
            .ok()
            .filter(|b| b.len() <= 1024 * 1024)
            .and_then(|b| serde_json::from_slice(&b).ok())
            .unwrap_or_default();
    let number = digest
        .as_ref()
        .and_then(|d| known.get(d))
        .filter(|v| version(v).is_ok());
    Ok(serde_json::json!({"root":root,"installed":digest.is_some(),"version":number}))
}
/// Select numerically within one stream; combined releases are a fallback only
/// until that stream has its own stable release. Publication order is irrelevant.
pub fn select_release(values: &[Value], component: Component) -> Result<Release> {
    let mut split: Option<Release> = None;
    let mut combined: Option<Release> = None;
    for value in values {
        if value["draft"] != false || value["prerelease"] != false {
            continue;
        }
        let tag = value["tag_name"].as_str().unwrap_or("");
        let prefix = match component {
            Component::Menu => "menu-v",
            Component::Desktop => "desktop-v",
        };
        let slot = if tag.starts_with(prefix) {
            &mut split
        } else if tag.starts_with('v') {
            &mut combined
        } else {
            continue;
        };
        // Ignore unrelated tag schemes; fail closed for malformed assets in a
        // recognized stable release instead of silently installing an older one.
        let number = tag
            .strip_prefix(prefix)
            .or_else(|| tag.strip_prefix('v'))
            .unwrap();
        if version(number).is_err() {
            continue;
        }
        let release = Release::parse(&serde_json::to_vec(value)?)?;
        if slot
            .as_ref()
            .is_none_or(|old| is_newer(&release.version, &old.version).unwrap_or(false))
        {
            *slot = Some(release);
        }
    }
    split
        .or(combined)
        .context("No stable release found for this component")
}
fn release_pages(mut fetch: impl FnMut(usize) -> Result<Vec<Value>>) -> Result<Vec<Value>> {
    let mut releases = Vec::new();
    for page in 1..=100 {
        let batch = fetch(page)?;
        let done = batch.len() < 100;
        releases.extend(batch);
        if done {
            return Ok(releases);
        }
    }
    anyhow::bail!("Too many release pages; cannot complete the update check")
}
fn published_releases(client: &Client, job: &Job) -> Result<Vec<Value>> {
    // GitHub has one /latest marker for the repository. Read every list page
    // so frequent desktop releases cannot hide the latest console release.
    release_pages(|page| {
        let bytes = setup::download(
            client,
            &format!(
            "https://api.github.com/repos/Scyllinice/oh-my-3do/releases?per_page=100&page={page}"
        ),
            8 * 1024 * 1024,
            job,
        )?;
        Ok(serde_json::from_slice(&bytes)?)
    })
}
pub fn latest(component: Component, job: &Job) -> Result<Release> {
    let client = setup::client()?;
    select_release(&published_releases(&client, job)?, component)
}
fn scan_targets(roots: &[PathBuf], job: &Job) -> Result<(Vec<BootState>, Vec<String>)> {
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
    Ok((targets, warnings))
}
pub fn check(roots: &[PathBuf], job: &Job) -> Result<Check> {
    let client = setup::client()?;
    let releases = published_releases(&client, job)?;
    let menu = select_release(&releases, Component::Menu)?;
    let desktop = select_release(&releases, Component::Desktop)?;
    let digest = menu.digest(&client, "boot.iso", job)?;
    let (targets, warnings) = scan_targets(roots, job)?;
    Ok(Check {
        menu,
        desktop,
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
    ensure!(
        release.component != Some(Component::Desktop),
        "Not a menu release"
    );
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
    fn release_listing_reads_past_frequent_desktop_releases_and_fails_on_incomplete_pages() {
        let value = |tag: &str| serde_json::json!({"tag_name":tag,"draft":false,"prerelease":false,"assets":[]});
        let mut pages = Vec::new();
        let all = release_pages(|page| {
            pages.push(page);
            Ok(if page == 1 {
                vec![value("desktop-v9.0.0"); 100]
            } else {
                vec![value("menu-v1.2.0")]
            })
        })
        .unwrap();
        assert_eq!(pages, vec![1, 2]);
        assert_eq!(
            select_release(&all, Component::Menu).unwrap().version,
            "1.2.0"
        );
        assert!(release_pages(|page| if page == 1 {
            Ok(vec![value("desktop-v9.0.0"); 100])
        } else {
            anyhow::bail!("Connection lost")
        })
        .is_err());
    }
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
