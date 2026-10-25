//! Checking for, downloading and installing updates to the console menu
//! (`boot.iso` on each drive) and the desktop app.
//!
//! Releases are read from the project's GitHub releases. The menu and the
//! desktop app are released separately ("menu-v1.2.3", "desktop-v1.2.3");
//! older combined releases ("v1.2.3") count for both until each has a
//! release of its own. Only stable releases are offered, and every download
//! must match the SHA-256 published beside it.
//!
//! A developer can instead point the app at a local build folder holding
//! update manifests (`menu-update.json`, `desktop-update-<platform>.json`)
//! to test an update before publishing it ([`check_local`]).

use crate::{fsutil as f, library, setup, Job};
use anyhow::{bail, ensure, Context, Result};
use reqwest::blocking::Client;
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
};

/// The project's GitHub repository.
pub const REPO: &str = "https://github.com/Scyllinice/oh-my-3do";

/// GitHub's list of the project's releases, 100 per page.
const RELEASES_API: &str = "https://api.github.com/repos/Scyllinice/oh-my-3do/releases";

/// The desktop platforms releases are built for.
const DESKTOP_PLATFORMS: [&str; 4] = ["Windows-x64", "Linux-x64", "macOS-arm64", "macOS-x64"];

/// The largest menu image accepted.
const MAX_BOOT_ISO_BYTES: u64 = 16 * 1024 * 1024;

/// The largest desktop ZIP accepted.
const MAX_DESKTOP_ZIP_BYTES: usize = 256 * 1024 * 1024;

/// The largest page of the release list accepted.
const MAX_RELEASE_PAGE_BYTES: usize = 8 * 1024 * 1024;

/// The most release notes kept, in characters.
const MAX_NOTES_CHARACTERS: usize = 12000;

/// The largest local update manifest accepted.
const MAX_MANIFEST_BYTES: usize = 16 * 1024;

/// The largest `menu-versions.json` read.
const MAX_MENU_VERSIONS_BYTES: usize = 1024 * 1024;

/// The two things that are updated separately.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Component {
    Menu,
    Desktop,
}

impl Component {
    /// The start of this component's release tags.
    fn tag_prefix(self) -> &'static str {
        match self {
            Component::Menu => "menu-v",
            Component::Desktop => "desktop-v",
        }
    }
}

/// A release that can be installed: published on GitHub, or a local build.
#[derive(Clone, Debug)]
pub struct Release {
    /// None for an older combined "vX.Y.Z" release (both menu and desktop).
    pub component: Option<Component>,
    pub tag: String,
    /// "X.Y.Z".
    pub version: String,
    pub notes: String,
    /// Each downloadable file, as (name, download URL).
    assets: Vec<(String, String)>,
    /// Set for a local developer build instead of a published release.
    local: Option<LocalBuild>,
}

/// Where a local developer build is, and the checksums its manifest gives.
#[derive(Clone, Debug)]
struct LocalBuild {
    root: PathBuf,
    /// The update file's name: `boot.iso`, or the desktop ZIP.
    artifact: String,
    /// SHA-256 of the update file.
    digest: String,
    /// For the desktop app: SHA-256 of the executable inside the ZIP, to
    /// tell whether it differs from the running one.
    binary_digest: Option<String>,
}

/// The updates on offer, as shown to the person.
#[derive(Clone, Debug)]
pub struct Offers {
    pub menu: Option<Release>,
    pub desktop: Option<Release>,
    /// SHA-256 of the offered menu image.
    pub boot_digest: Option<String>,
    /// Each drive whose menu could be updated, with its current menu.
    pub targets: Vec<BootState>,
    /// Drives that could not be checked.
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

/// A drive and the SHA-256 of its `boot.iso` (None if it has none).
#[derive(Clone, Debug)]
pub struct BootState {
    pub root: PathBuf,
    pub digest: Option<String>,
}

/// The result of checking GitHub for updates.
#[derive(Clone, Debug)]
pub struct Check {
    pub menu: Release,
    pub desktop: Release,
    pub boot_digest: String,
    pub targets: Vec<BootState>,
    pub warnings: Vec<String>,
}

// ---- Versions ----

/// A version "X.Y.Z" as three numbers. Refuses anything else, including
/// numbers with leading zeros.
fn version(value: &str) -> Result<[u64; 3]> {
    let parts: Vec<&str> = value.split('.').collect();
    ensure!(parts.len() == 3, "Unsupported release version");

    let mut numbers = [0; 3];
    for (index, part) in parts.iter().enumerate() {
        let is_plain_number = !part.is_empty()
            && part.bytes().all(|byte| byte.is_ascii_digit())
            && (part.len() == 1 || !part.starts_with('0'));
        ensure!(is_plain_number, "Unsupported release version");
        numbers[index] = part.parse()?;
    }
    Ok(numbers)
}

/// Whether version `latest` is newer than `installed`.
pub fn is_newer(latest: &str, installed: &str) -> Result<bool> {
    Ok(version(latest)? > version(installed)?)
}

/// Whether text is a SHA-256 in hex.
fn is_sha256(text: &str) -> bool {
    text.len() == 64 && text.bytes().all(|byte| byte.is_ascii_hexdigit())
}

// ---- Releases ----

impl Release {
    /// Reads one release from GitHub's JSON. Refuses drafts, prereleases,
    /// unknown tag forms, and any file not served from this repository's
    /// own release download address.
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
            let number = tag.strip_prefix('v').context("Unsupported release tag")?;
            (None, number)
        };
        let number = number.to_owned();
        version(&number)?;

        let mut assets: Vec<(String, String)> = Vec::new();
        for asset in value["assets"]
            .as_array()
            .context("Release assets missing")?
        {
            let name = asset["name"].as_str().context("Asset name missing")?;
            let url = asset["browser_download_url"]
                .as_str()
                .context("Asset URL missing")?;
            let duplicate = assets.iter().any(|(existing, _)| existing == name);
            ensure!(!duplicate, "Duplicate release asset");

            // Require this repository, release tag and file name exactly,
            // not just a shared prefix.
            let plain_name = !name.is_empty()
                && name
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || b".-_".contains(&byte));
            let expected_url = format!("{REPO}/releases/download/{tag}/{name}");
            ensure!(
                plain_name && url == expected_url,
                "Unexpected release asset URL"
            );
            assets.push((name.to_owned(), url.to_owned()));
        }

        let notes = value["body"]
            .as_str()
            .unwrap_or("")
            .chars()
            .take(MAX_NOTES_CHARACTERS)
            .collect();

        Ok(Self {
            component,
            tag,
            version: number,
            notes,
            assets,
            local: None,
        })
    }

    /// Whether this is a local developer build.
    pub fn is_local(&self) -> bool {
        self.local.is_some()
    }

    /// For a local desktop build: the SHA-256 of its executable.
    pub fn binary_digest(&self) -> Option<&str> {
        self.local
            .as_ref()
            .and_then(|build| build.binary_digest.as_deref())
    }

    /// Whether this desktop release should be offered over the running app
    /// (`executable`, version `installed`): a local build if its executable
    /// differs, a published release if it is newer.
    pub fn desktop_available(&self, executable: &Path, installed: &str, job: &Job) -> Result<bool> {
        if let Some(digest) = self.binary_digest() {
            return Ok(f::sha256(executable, job)? != digest);
        }
        is_newer(&self.version, installed)
    }

    /// Where to read about this release: its GitHub page, or the local
    /// build's folder.
    pub fn page(&self) -> String {
        if let Some(build) = &self.local {
            return build.root.display().to_string();
        }
        format!("{REPO}/releases/tag/{}", self.tag)
    }

    /// The download URL of the release file `name`.
    fn asset(&self, name: &str) -> Result<&str> {
        self.assets
            .iter()
            .find(|(asset_name, _)| asset_name == name)
            .map(|(_, url)| url.as_str())
            .with_context(|| format!("Release is missing {name}"))
    }

    /// The desktop ZIP's file name for `platform` (such as "Linux-x64"),
    /// checking the release has it and its checksum file.
    pub fn desktop_name(&self, platform: &str) -> Result<String> {
        ensure!(
            self.component != Some(Component::Menu),
            "Not a desktop release"
        );
        ensure!(
            DESKTOP_PLATFORMS.contains(&platform),
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

    /// The published SHA-256 of release file `name` (from its `.sha256`
    /// file, or the local build's manifest).
    pub(crate) fn digest(&self, client: &Client, name: &str, job: &Job) -> Result<String> {
        if let Some(build) = &self.local {
            job.check()?;
            ensure!(name == build.artifact, "Unexpected local build artifact");
            return Ok(build.digest.clone());
        }

        let checksum_url = self.asset(&format!("{name}.sha256"))?;
        let bytes = setup::download(client, checksum_url, 1024, job)?;
        checksum(std::str::from_utf8(&bytes)?, name)
    }

    /// Downloads (or reads, for a local build) release file `name`, at most
    /// `limit` bytes, and checks it against its published SHA-256.
    pub(crate) fn bytes(
        &self,
        name: &str,
        limit: usize,
        client: &Client,
        job: &Job,
    ) -> Result<Vec<u8>> {
        let expected = self.digest(client, name, job)?;
        let bytes = match &self.local {
            Some(build) => read_local(&build.root, name, limit, job)?,
            None => setup::download(client, self.asset(name)?, limit, job)?,
        };
        ensure!(
            format!("{:x}", Sha256::digest(&bytes)) == expected,
            "Update checksum mismatch for {name}; check for updates again if the build changed"
        );
        Ok(bytes)
    }
}

/// Reads a file from a local build folder, at most `limit` bytes, reporting
/// progress.
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
        // The file could grow while it is read.
        ensure!(
            bytes.len() + count <= limit,
            "Local update exceeds size limit: {name}"
        );
        bytes.extend_from_slice(&buffer[..count]);
        job.report(bytes.len() as u64, size, "Reading local update");
    }
    Ok(bytes)
}

// ---- Local developer builds ----

/// Reads a local build's update manifest for `component` from `directory`.
/// None if there is no manifest for it.
///
/// A manifest is JSON like:
///
/// ```json
/// {"schema": 1, "component": "desktop", "version": "1.2.3",
///  "platform": "Linux-x64", "artifact": "OhMy3DO-Library-Linux-x64-v1.2.3.zip",
///  "sha256": "...", "binary_sha256": "..."}
/// ```
///
/// (The menu's has no platform or binary_sha256, and its artifact is
/// `boot.iso`.) The checksums are fixed when the offer is made, so a file
/// rebuilt while the person is deciding is caught and needs a new check.
pub fn local_release(directory: &Path, component: Component, job: &Job) -> Result<Option<Release>> {
    let root = f::root(directory).context("Local update directory is unavailable")?;
    let platform = platform().context("No desktop package for this platform")?;
    let manifest_name = match component {
        Component::Menu => "menu-update.json".into(),
        Component::Desktop => format!("desktop-update-{platform}.json"),
    };
    if !f::safe(&root, &manifest_name)?.exists() {
        return Ok(None);
    }

    let manifest: Value =
        serde_json::from_slice(&read_local(&root, &manifest_name, MAX_MANIFEST_BYTES, job)?)?;
    let kind = match component {
        Component::Menu => "menu",
        Component::Desktop => "desktop",
    };
    ensure!(
        manifest["schema"] == 1 && manifest["component"] == kind,
        "Invalid local update manifest: {manifest_name}"
    );

    let number = manifest["version"]
        .as_str()
        .context("Local build version missing")?;
    version(number)?;

    let artifact = manifest["artifact"]
        .as_str()
        .context("Local artifact missing")?;
    let expected_artifact = match component {
        Component::Menu => "boot.iso".to_owned(),
        Component::Desktop => {
            ensure!(
                manifest["platform"] == platform,
                "Local desktop platform mismatch"
            );
            format!("OhMy3DO-Library-{platform}-v{number}.zip")
        }
    };
    ensure!(
        artifact == expected_artifact,
        "Unexpected local artifact filename"
    );

    let digest = manifest["sha256"]
        .as_str()
        .context("Local checksum missing")?;
    ensure!(is_sha256(digest), "Invalid local artifact checksum");

    let binary_digest = if component == Component::Desktop {
        let hash = manifest["binary_sha256"]
            .as_str()
            .context("Local executable checksum missing")?;
        ensure!(is_sha256(hash), "Invalid local executable checksum");
        Some(hash.to_ascii_lowercase())
    } else {
        None
    };

    ensure!(
        f::safe(&root, artifact)?.is_file(),
        "Local update artifact is missing: {artifact}"
    );

    Ok(Some(Release {
        component: Some(component),
        tag: format!("{kind}-v{number}"),
        version: number.into(),
        notes: format!(
            "Local developer build\nDirectory: {}\nArtifact: {artifact}\nSHA-256: {digest}",
            root.display()
        ),
        assets: vec![],
        local: Some(LocalBuild {
            root,
            artifact: artifact.into(),
            digest: digest.to_ascii_lowercase(),
            binary_digest,
        }),
    }))
}

/// The updates a local build folder offers, and (if it has a menu) the
/// state of each drive in `roots`.
pub fn check_local(directory: &Path, roots: &[PathBuf], job: &Job) -> Result<Offers> {
    let menu = local_release(directory, Component::Menu, job)?;
    let desktop = local_release(directory, Component::Desktop, job)?;
    ensure!(
        menu.is_some() || desktop.is_some(),
        "No local update manifests found. Choose the build output directory \
         containing menu-update.json or desktop-update-{}.json",
        platform().unwrap_or("PLATFORM")
    );

    let boot_digest = menu
        .as_ref()
        .and_then(|release| release.local.as_ref())
        .map(|build| build.digest.clone());
    let (targets, warnings) = if menu.is_some() {
        scan_targets(roots, job)?
    } else {
        (vec![], vec![])
    };

    Ok(Offers {
        menu,
        desktop,
        boot_digest,
        targets,
        warnings,
    })
}

/// Reads a published checksum file: "<sha256> <name>" (or "<sha256>
/// *<name>", as `sha256sum` writes for binary files). Returns the checksum
/// in lowercase, refusing a file for another name.
pub fn checksum(text: &str, name: &str) -> Result<String> {
    let fields: Vec<&str> = text.split_whitespace().collect();
    let valid = fields.len() == 2
        && is_sha256(fields[0])
        && fields[1].strip_prefix('*').unwrap_or(fields[1]) == name;
    ensure!(valid, "Invalid published checksum for {name}");
    Ok(fields[0].to_ascii_lowercase())
}

/// This computer's desktop platform name, as used in release file names.
/// None for a system without a desktop release.
pub fn platform() -> Option<&'static str> {
    match (std::env::consts::OS, std::env::consts::ARCH) {
        ("windows", "x86_64") => Some("Windows-x64"),
        ("linux", "x86_64") => Some("Linux-x64"),
        ("macos", "aarch64") => Some("macOS-arm64"),
        ("macos", "x86_64") => Some("macOS-x64"),
        _ => None,
    }
}

// ---- The menu on each drive ----

/// The SHA-256 of a drive's `boot.iso`, or None if it has none.
fn boot_digest(root: &Path, job: &Job) -> Result<Option<String>> {
    let path = f::safe(root, "boot.iso")?;
    if !path.exists() {
        return Ok(None);
    }
    ensure!(
        path.is_file() && fs::metadata(&path)?.len() <= MAX_BOOT_ISO_BYTES,
        "Existing boot.iso is not a menu image under 16 MiB"
    );
    Ok(Some(f::sha256(&path, job)?))
}

/// The menu versions the app has installed, by image SHA-256, from
/// `menu-versions.json` in the cache. Empty if it is missing or unreadable.
fn known_menu_versions(cache: &Path) -> BTreeMap<String, String> {
    fs::read(cache.join("menu-versions.json"))
        .ok()
        .filter(|bytes| bytes.len() <= MAX_MENU_VERSIONS_BYTES)
        .and_then(|bytes| serde_json::from_slice(&bytes).ok())
        .unwrap_or_default()
}

/// Records that the menu image with SHA-256 `digest` is version `number`.
///
/// A drive's menu version is only ever known this way, from the image's
/// contents: never guessed from a file name or from another drive. An image
/// the app did not install stays "unknown".
pub fn remember_menu(cache: &Path, digest: &str, number: &str) -> Result<()> {
    ensure!(is_sha256(digest), "Invalid menu digest");
    version(number)?;

    let mut known = known_menu_versions(cache);
    known.insert(digest.to_ascii_lowercase(), number.into());
    f::atomic(
        &cache.join("menu-versions.json"),
        &serde_json::to_vec_pretty(&known)?,
    )
}

/// Whether a drive has a menu, and its version if the app knows it, as
/// `{"root", "installed", "version"}`.
pub fn installed_menu(root: &Path, cache: &Path, job: &Job) -> Result<Value> {
    let root = f::root(root)?;
    let digest = boot_digest(&root, job)?;
    let known = known_menu_versions(cache);
    let number = digest
        .as_ref()
        .and_then(|digest| known.get(digest))
        .filter(|number| version(number).is_ok());
    Ok(serde_json::json!({"root": root, "installed": digest.is_some(), "version": number}))
}

/// The state of each drive in `roots`, without duplicates. A drive that
/// cannot be checked is reported in the warnings and skipped.
fn scan_targets(roots: &[PathBuf], job: &Job) -> Result<(Vec<BootState>, Vec<String>)> {
    let mut targets: Vec<BootState> = Vec::new();
    let mut warnings = Vec::new();

    for path in roots {
        job.check()?;
        if path.as_os_str().is_empty() {
            continue;
        }

        let state = f::root(path).and_then(|root| {
            Ok(BootState {
                digest: boot_digest(&root, job)?,
                root,
            })
        });
        match state {
            Ok(state) => {
                let already_listed = targets.iter().any(|target| target.root == state.root);
                if !already_listed {
                    targets.push(state);
                }
            }
            Err(error) => {
                let warning = format!("Cannot check {}: {error:#}", path.display());
                job.say(&warning);
                warnings.push(warning);
            }
        }
    }

    job.check()?;
    Ok((targets, warnings))
}

// ---- Finding the latest release ----

/// The newest stable release for `component` among GitHub's releases.
///
/// Versions are compared as numbers, so publication order does not matter.
/// A combined "vX.Y.Z" release is only used until the component has a stable
/// release of its own. Tags in other forms are ignored, but a recognized
/// stable release with bad assets is an error rather than being skipped (so
/// an older release is never installed by mistake).
pub fn select_release(values: &[Value], component: Component) -> Result<Release> {
    let prefix = component.tag_prefix();
    let mut newest_own: Option<Release> = None;
    let mut newest_combined: Option<Release> = None;

    for value in values {
        if value["draft"] != false || value["prerelease"] != false {
            continue;
        }

        let tag = value["tag_name"].as_str().unwrap_or("");
        let (newest, number) = if let Some(number) = tag.strip_prefix(prefix) {
            (&mut newest_own, number)
        } else if let Some(number) = tag.strip_prefix('v') {
            (&mut newest_combined, number)
        } else {
            continue;
        };
        if version(number).is_err() {
            continue;
        }

        let release = Release::parse(&serde_json::to_vec(value)?)?;
        let is_newest = newest
            .as_ref()
            .is_none_or(|old| is_newer(&release.version, &old.version).unwrap_or(false));
        if is_newest {
            *newest = Some(release);
        }
    }

    newest_own
        .or(newest_combined)
        .context("No stable release found for this component")
}

/// Reads every page of the release list through `fetch` (given the page
/// number, from 1). GitHub returns 100 releases a page; a shorter page is
/// the last.
fn release_pages(mut fetch: impl FnMut(usize) -> Result<Vec<Value>>) -> Result<Vec<Value>> {
    const PER_PAGE: usize = 100;
    const MAX_PAGES: usize = 100;

    let mut releases = Vec::new();
    for page in 1..=MAX_PAGES {
        let batch = fetch(page)?;
        let last_page = batch.len() < PER_PAGE;
        releases.extend(batch);
        if last_page {
            return Ok(releases);
        }
    }
    bail!("Too many release pages; cannot complete the update check")
}

/// Every published release of the project, from GitHub.
///
/// GitHub marks only one release as the repository's "latest", so every
/// page is read: frequent desktop releases must not hide the latest menu
/// release.
fn published_releases(client: &Client, job: &Job) -> Result<Vec<Value>> {
    release_pages(|page| {
        let url = format!("{RELEASES_API}?per_page=100&page={page}");
        let bytes = setup::download(client, &url, MAX_RELEASE_PAGE_BYTES, job)?;
        Ok(serde_json::from_slice(&bytes)?)
    })
}

/// The newest stable release for `component`.
pub fn latest(component: Component, job: &Job) -> Result<Release> {
    let client = setup::client()?;
    select_release(&published_releases(&client, job)?, component)
}

/// Checks GitHub for the newest menu and desktop releases, and the state of
/// each drive in `roots`.
pub fn check(roots: &[PathBuf], job: &Job) -> Result<Check> {
    let client = setup::client()?;
    let releases = published_releases(&client, job)?;
    let menu = select_release(&releases, Component::Menu)?;
    let desktop = select_release(&releases, Component::Desktop)?;
    let boot_digest = menu.digest(&client, "boot.iso", job)?;
    let (targets, warnings) = scan_targets(roots, job)?;

    Ok(Check {
        menu,
        desktop,
        boot_digest,
        targets,
        warnings,
    })
}

// ---- Downloading and installing ----

/// Downloads a desktop release's ZIP for `platform` to a new file
/// `destination`, after checking its checksum and that it is a ZIP.
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

    let bytes = release.bytes(&name, MAX_DESKTOP_ZIP_BYTES, &setup::client()?, job)?;
    let zip = zip::ZipArchive::new(std::io::Cursor::new(&bytes)).context("Invalid desktop ZIP")?;
    ensure!(!zip.is_empty(), "Empty desktop ZIP");
    job.check()?;
    write_download(destination, &bytes, job)
}

/// Saves a download as the new file `destination`. It is written to a
/// temporary file first and only then given its name, so a canceled or
/// failed save leaves nothing behind, and an existing file is never
/// replaced.
fn write_download(destination: &Path, bytes: &[u8], job: &Job) -> Result<()> {
    let folder = destination
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    let mut file = tempfile::NamedTempFile::new_in(folder)?;
    file.write_all(bytes)?;
    file.as_file().sync_all()?;
    job.check()?;

    file.persist_noclobber(destination)
        .map_err(|error| error.error)
        .context("Could not save download; existing files are preserved")?;
    Ok(())
}

/// Downloads a release's menu image and checks it is a valid menu with the
/// SHA-256 `digest` that was offered.
fn menu_bytes(release: &Release, digest: &str, job: &Job) -> Result<Vec<u8>> {
    ensure!(
        release.component != Some(Component::Desktop),
        "Not a menu release"
    );
    let bytes = release.bytes(
        "boot.iso",
        MAX_BOOT_ISO_BYTES as usize,
        &setup::client()?,
        job,
    )?;
    setup::verify_image(&bytes, &format!("{digest}  boot.iso"))?;
    Ok(bytes)
}

/// Downloads a release's menu image to a new file `destination`.
pub fn download_boot(release: &Release, digest: &str, destination: &Path, job: &Job) -> Result<()> {
    ensure!(
        !destination.exists(),
        "Choose a new download filename; existing files are preserved"
    );
    job.say(format!("Downloading {} boot.iso", release.tag));
    write_download(destination, &menu_bytes(release, digest, job)?, job)
}

/// Installs a release's menu on the drive `expected`. The image must be the
/// one offered (SHA-256 `digest`), even if the release's files were replaced
/// since. Returns the journal's path.
pub fn install_boot(
    release: &Release,
    expected: &BootState,
    digest: &str,
    cache: &Path,
    job: &Job,
) -> Result<PathBuf> {
    let bytes = menu_bytes(release, digest, job)?;
    install_boot_bytes(expected, &bytes, cache, job)
}

/// Writes a menu image to the drive `expected`, and adds any support files
/// the drive is missing (existing files are kept), all through a journal.
/// Refuses if the drive's current `boot.iso` differs from when the update
/// was offered. Returns the journal's path.
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

    let mut journal = library::Journal::new(&root, cache)?;
    let result = (|| -> Result<()> {
        job.check()?;
        setup::ensure_support(&root, &mut journal, &Default::default(), job)?;
        journal.write("boot.iso", bytes)?;
        ensure!(
            fs::read(f::safe(&root, "boot.iso")?)? == bytes,
            "Menu update readback failed"
        );
        job.check()?;
        journal.finish()
    })();

    if let Err(error) = result {
        journal.undo().context(format!(
            "{error:#}; rollback failed; keep {}",
            journal.path.display()
        ))?;
        return Err(error);
    }
    Ok(journal.path)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::Ordering;

    #[test]
    fn release_listing_reads_past_frequent_desktop_releases_and_fails_on_incomplete_pages() {
        let value = |tag: &str| serde_json::json!({"tag_name": tag, "draft": false, "prerelease": false, "assets": []});
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
        assert!(write_download(&dir.path().join("canceled.zip"), b"partial", &job).is_err());
        assert_eq!(fs::read_dir(dir.path()).unwrap().count(), 1);
    }
}
