//! Guided installation of the menu release matching this desktop application.
use crate::{console_config, fsutil as f, library, Job};
use anyhow::{ensure, Context, Result};
use reqwest::blocking::Client;
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::Read,
    path::{Path, PathBuf},
    time::Duration,
};
const LIMIT: usize = 16 * 1024 * 1024;
pub(crate) fn client() -> Result<Client> {
    Ok(Client::builder()
        .user_agent("OhMy3DO-Library")
        .connect_timeout(Duration::from_secs(15))
        .timeout(Duration::from_secs(120))
        .build()?)
}

pub(crate) fn download(client: &Client, url: &str, limit: usize, job: &Job) -> Result<Vec<u8>> {
    job.check()?;
    let response = client.get(url).send()?.error_for_status()?;
    ensure!(
        response.content_length().is_none_or(|n| n <= limit as u64),
        "Download exceeds size limit"
    );
    let expected = response.content_length().unwrap_or(0);job.report(0,expected,"Downloading");
    let mut bytes = Vec::new();
    let mut stream = response.take(limit as u64 + 1);
    let mut buffer = [0u8; 32768];
    loop {
        job.check()?;
        let n = stream.read(&mut buffer)?;
        if n == 0 {
            break;
        }
        bytes.extend_from_slice(&buffer[..n]);
        ensure!(bytes.len() <= limit, "Download exceeds size limit");
        job.report(bytes.len() as u64,expected,"Downloading");
    }
    Ok(bytes)
}
pub fn verify_image(image: &[u8], checksum: &str) -> Result<()> {
    let fields: Vec<_> = checksum.split_whitespace().collect();
    ensure!(
        fields.len() == 2
            && fields[0].len() == 64
            && fields[0].bytes().all(|b| b.is_ascii_hexdigit())
            && fields[1].trim_start_matches('*') == "boot.iso",
        "Invalid published boot.iso checksum"
    );
    ensure!(
        format!("{:x}", Sha256::digest(image)).eq_ignore_ascii_case(fields[0]),
        "Menu download checksum mismatch"
    );
    ensure!(
        (84..=LIMIT).contains(&image.len()) && image[..7] == [1, 90, 90, 90, 90, 90, 1],
        "Not a 3DO menu boot image"
    );
    Ok(())
}
/// Network work completes and verifies before the library is changed.
pub fn install_release(
    root: &Path,
    cache: &Path,
    settings: &console_config::Settings,
    job: &Job,
) -> Result<PathBuf> {
    let version = env!("CARGO_PKG_VERSION");
    job.say(format!(
        "Downloading menu v{version} from Scyllinice/oh-my-3do"
    ));
    let client = client()?;
    let release: Value = serde_json::from_slice(&download(
        &client,
        &format!("https://api.github.com/repos/Scyllinice/oh-my-3do/releases/tags/v{version}"),
        1024 * 1024,
        job,
    )?)?;
    ensure!(
        release["tag_name"] == format!("v{version}") && release["draft"] == false,
        "Menu release does not match this application"
    );
    let assets = release["assets"]
        .as_array()
        .context("Release has no assets")?;
    let asset = |name: &str| -> Result<String> {
        let url = assets
            .iter()
            .find(|a| a["name"] == name)
            .and_then(|a| a["browser_download_url"].as_str())
            .context("Matching menu release asset is missing")?;
        ensure!(
            url.starts_with(&format!(
                "https://github.com/Scyllinice/oh-my-3do/releases/download/v{version}/"
            )),
            "Unexpected release asset URL"
        );
        Ok(url.to_owned())
    };
    let checksum = download(&client, &asset("boot.iso.sha256")?, 1024, job)?;
    let image = download(&client, &asset("boot.iso")?, LIMIT, job)?;
    verify_image(&image, std::str::from_utf8(&checksum)?)?;
    install_verified(root, cache, &image, settings, job)
}
/// Install an already verified menu image with rollback and readback.
pub fn install_verified(
    root: &Path,
    cache: &Path,
    image: &[u8],
    settings: &console_config::Settings,
    job: &Job,
) -> Result<PathBuf> {
    ensure!(
        (84..=LIMIT).contains(&image.len()) && image[..7] == [1, 90, 90, 90, 90, 90, 1],
        "Not a 3DO menu boot image"
    );
    let root = f::root(root)?;
    let _lock = library::lock_library(&root, cache)?;
    let mut tx = library::Journal::new(&root, cache)?;
    let result = (|| -> Result<()> {
        job.check()?;
        let refresh_catalog = f::safe(&root, ".ohmy3do/saves/slot-catalog.3di")?
            .metadata()
            .is_ok_and(|m| m.len() > 0);
        ensure_support(&root, &mut tx, settings, job)?;
        if refresh_catalog {
            let slots = crate::save_catalog::scan(&root.join(".ohmy3do/saves"), job)?;
            crate::save_catalog::write(&slots, &mut tx, ".ohmy3do/saves/", job)?;
        }
        job.check()?;
        tx.write("boot.iso", image)?;
        ensure!(
            fs::read(f::safe(&root, "boot.iso")?)? == image,
            "Menu image readback failed"
        );
        tx.finish()
    })();
    if let Err(error) = result {
        tx.undo().context(format!("{error:#}; rollback failed"))?;
        return Err(error);
    }
    job.say("Drive ready: menu, settings and save support installed. No game library was copied.");
    Ok(tx.path)
}

/// Required runtime layout shared by setup, boot updates and USB preparation.
/// Existing settings, save archives, media and indexes are never replaced here.
pub fn ensure_support(
    root: &Path,
    tx: &mut library::Journal,
    settings: &console_config::Settings,
    job: &Job,
) -> Result<()> {
    job.check()?;
    ensure_navigation(root, tx, job)?;
    for name in [".ohmy3do/saves", ".ohmy3do/covers", ".ohmy3do/previews"] {
        tx.ensure_directory(name)?;
    }
    crate::save_cheats::ensure_database(root, tx)?;
    let ini = f::safe(root, console_config::PATH)?;
    ensure!(
        !ini.exists() || ini.is_file(),
        "Console settings path is occupied by a directory"
    );
    if !ini.exists() || ini.metadata()?.len() == 0 {
        tx.write(console_config::PATH, &settings.encode()?)?;
    }
    if !f::safe(root, crate::categories::PATH)?.exists() { tx.write(crate::categories::PATH,&crate::categories::load(root)?.encode()?)?; }
    library::build_missing_indexes(root, tx, job)?;
    let catalog = f::safe(root, ".ohmy3do/saves/slot-catalog.3di")?;
    ensure!(
        !catalog.exists() || catalog.is_file(),
        "Save catalog path is occupied by a directory"
    );
    if !catalog.exists() || catalog.metadata()?.len() == 0 {
        let slots = crate::save_catalog::scan(&root.join(".ohmy3do/saves"), job)?;
        crate::save_catalog::write(&slots, tx, ".ohmy3do/saves/", job)?;
    }
    job.check()?;
    Ok(())
}

/// A firmware-visible, one-sector pivot for directory ID collisions. Dotfiles
/// and FAT hidden files are excluded by the ODE, so the menu hides this name.
/// Only ancestors of actual discs need pivots; game folders already have discs.
pub fn ensure_navigation(root: &Path, tx: &mut library::Journal, job: &Job) -> Result<()> {
    let data = navigation_helper_data();
    for relative in navigation_helpers(root, job)? {
        job.check()?;
        let path = f::safe(root, &relative)?;
        if path.exists() {
            ensure!(path.is_file() && fs::read(&path)? == data, "Navigation helper path is occupied: {}", path.display());
        } else {
            tx.write(&relative, &data)?;
            ensure!(fs::read(path)? == data, "Navigation helper readback failed");
        }
    }
    Ok(())
}

pub const NAVIGATION_HELPER: &str = "Oh My 3DO - Folder Navigation Helper (Keep).iso";
pub(crate) fn navigation_helper_data() -> [u8; 2048] {
    let mut data = [0u8; 2048];
    data[..7].copy_from_slice(&[1, 90, 90, 90, 90, 90, 1]);
    data[76..80].copy_from_slice(&2048u32.to_be_bytes());
    data[80..84].copy_from_slice(&1u32.to_be_bytes());
    let label = b"Oh My 3DO navigation helper. Not a game. Never mount.";
    data[128..128 + label.len()].copy_from_slice(label);
    data
}

/// Expected helper paths, without creating files or validating disc payloads.
pub(crate) fn navigation_helpers(root: &Path, job: &Job) -> Result<Vec<String>> {
    let mut folders = std::collections::BTreeSet::new();
    for entry in walkdir::WalkDir::new(root).follow_links(false).into_iter().filter_entry(|e| {
        !f::is_link(e.path()) && (e.depth() == 0 || !e.file_type().is_dir() ||
            !e.file_name().to_string_lossy().starts_with('.') &&
            !["$boot", "system", "system volume information"].contains(&e.file_name().to_string_lossy().to_ascii_lowercase().as_str()))
    }) {
        job.check()?;
        let entry = entry?;
        if !entry.file_type().is_file() || entry.file_name().to_string_lossy().eq_ignore_ascii_case(NAVIGATION_HELPER) {
            continue;
        }
        let extension = entry.path().extension().unwrap_or_default().to_string_lossy().to_ascii_lowercase();
        if !["iso", "cue"].contains(&extension.as_str()) { continue; }
        if entry.path().parent() == Some(root) { continue; }
        let mut parent = entry.path().parent().and_then(Path::parent);
        while let Some(folder) = parent {
            if folder == root { break; }
            ensure!(folder.starts_with(root), "Disc is outside the drive");
            folders.insert(folder.to_owned());
            parent = folder.parent();
        }
    }
    folders.into_iter().map(|folder| Ok(folder.strip_prefix(root)?.join(NAVIGATION_HELPER).to_string_lossy().replace('\\', "/"))).collect()
}
/// Offline setup/repair from a local menu, with no staging library required.
pub fn install_local(
    root: &Path,
    cache: &Path,
    source: &Path,
    settings: &console_config::Settings,
    job: &Job,
) -> Result<PathBuf> {
    job.check()?;
    let mut image = Vec::new();
    fs::File::open(source)?
        .take((LIMIT + 1) as u64)
        .read_to_end(&mut image)?;
    install_verified(root, cache, &image, settings, job)
}
