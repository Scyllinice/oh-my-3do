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
        tx.ensure_directory(".ohmy3do/saves")?;
        crate::save_cheats::ensure_database(&root, &mut tx)?;
        if !f::safe(&root, console_config::PATH)?.exists() {
            tx.write(console_config::PATH, &settings.encode()?)?;
        }
        library::build_indexes(&root, &mut tx, job)?;
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
    job.say("Menu installed. Add your discs, then review the USB copy.");
    Ok(tx.path)
}
