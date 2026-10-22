//! Verified portable self-updates. A copy of the running program installs after exit.
use crate::{fsutil as f, library::Journal, Job};
use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use std::{
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
    process::{Child, Command},
    time::{Duration, Instant},
};

#[derive(Clone, Serialize, Deserialize)]
pub struct Asset {
    pub relative: String,
    pub staged: PathBuf,
    pub sha256: String,
}
#[derive(Clone, Serialize, Deserialize)]
pub struct Install {
    pub executable: PathBuf,
    pub staged: PathBuf,
    pub previous: PathBuf,
    pub ready: PathBuf,
    pub log: PathBuf,
    pub original_sha256: String,
    pub new_sha256: String,
    pub parent: u32,
    #[serde(default = "restart_default")]
    pub restart: bool,
    #[serde(default)]
    pub assets: Vec<Asset>,
}
fn restart_default() -> bool { true }

#[derive(Clone, Serialize, Deserialize)]
pub struct Report {
    pub executable: PathBuf,
    pub success: bool,
    pub previous_intact: bool,
    pub detail: String,
    pub log: PathBuf,
}
/// Persist failures before attempting to reopen the old app, including failures
/// during an update on exit (which must leave the application closed).
pub fn record_result(plan: &Install, result: &Result<()>) -> Result<()> {
    let detail = match result { Ok(()) => "Updated and startup verified successfully.".into(), Err(e) => format!("{e:#}") };
    fs::write(&plan.log, &detail)?;
    let report = Report { executable: plan.executable.clone(), success: result.is_ok(),
        previous_intact: f::sha256(&plan.executable, &Job::default()).ok().as_deref() == Some(&plan.original_sha256),
        detail, log: plan.log.clone() };
    let directory = plan.log.parent().and_then(Path::parent).context("Missing update cache directory")?;
    f::atomic(&directory.join("last-result.json"), &serde_json::to_vec_pretty(&report)?)
}
pub fn last_report(cache: &Path, executable: &Path) -> Option<Report> {
    let bytes = fs::read(cache.join("self-update/last-result.json")).ok()?;
    if bytes.len() > 64 * 1024 { return None; }
    let report: Report = serde_json::from_slice(&bytes).ok()?;
    (report.executable == executable.canonicalize().ok()?).then_some(report)
}
pub fn discard(plan: &Install, path: &Path) -> Result<()> {
    ensure!(f::sha256(&plan.staged, &Job::default())? == plan.new_sha256,
        "Staged update changed; keep its recovery files and check updates again");
    fs::remove_file(&plan.staged)?;
    let _ = fs::remove_file(path);
    Ok(())
}
/// The ZIP has already passed the release's published SHA-256 verification.
/// Extract the executable and allowlisted program documentation/licenses, never settings or game files.
pub fn stage(
    zip_path: &Path,
    executable: &Path,
    cache: &Path,
    job: &Job,
) -> Result<(Install, PathBuf)> {
    ensure!(
        !f::is_link(executable),
        "Cannot update a symbolic-link executable"
    );
    let executable = executable.canonicalize()?;
    let directory = executable
        .parent()
        .context("Missing executable directory")?;
    let id = uuid::Uuid::new_v4().to_string();
    let work = cache.join("self-update").join(&id);
    fs::create_dir_all(&work)?;
    let member = if cfg!(windows) {
        "OhMy3DO-Library/OhMy3DOLibrary.exe"
    } else {
        "OhMy3DO-Library/OhMy3DOLibrary"
    };
    let mut zip = zip::ZipArchive::new(fs::File::open(zip_path)?)?;
    ensure!(
        zip.file_names().filter(|n| *n == member).count() == 1,
        "Release must contain exactly one desktop executable"
    );
    let file = zip.by_name(member)?;
    ensure!(
        file.size() > 64 && file.size() <= 256 * 1024 * 1024,
        "Invalid executable size"
    );
    let mut bytes = Vec::new();
    file.take(256 * 1024 * 1024 + 1).read_to_end(&mut bytes)?;
    ensure!(bytes.len() <= 256 * 1024 * 1024, "Oversized executable");
    ensure!(
        if cfg!(windows) {
            bytes.starts_with(b"MZ")
        } else if cfg!(target_os = "macos") {
            bytes.starts_with(&[0xcf, 0xfa, 0xed, 0xfe])
        } else {
            bytes.starts_with(b"\x7fELF")
        },
        "Release does not contain an executable for this platform"
    );
    job.check()?;
    let mut assets = vec![];
    let mut asset_bytes = 0u64;
    let names = zip.file_names().map(str::to_owned).collect::<Vec<_>>();
    ensure!(names.len() <= 8192, "Release contains too many files");
    for name in names {
        let Some(relative) = name.strip_prefix("OhMy3DO-Library/") else {
            continue;
        };
        if name.ends_with('/')
            || !(relative.starts_with("licenses/")
                || [
                    "LICENSE",
                    "README.md",
                    "THIRD_PARTY.md",
                    "BUILD.json",
                    "OhMy3DOLibrary.png",
                ]
                .contains(&relative))
        {
            continue;
        }
        let target = f::safe(&work.join("assets"), relative)?;
        ensure!(
            !assets
                .iter()
                .any(|a: &Asset| a.relative.eq_ignore_ascii_case(relative)),
            "Duplicate release asset"
        );
        let file = zip.by_name(&name)?;
        ensure!(
            file.unix_mode().is_none_or(|m| m & 0o170000 != 0o120000),
            "Release contains a symbolic link"
        );
        asset_bytes = asset_bytes
            .checked_add(file.size())
            .context("Asset size overflow")?;
        ensure!(
            asset_bytes <= 32 * 1024 * 1024,
            "Release support files exceed 32 MB"
        );
        let mut data = vec![];
        file.take(32 * 1024 * 1024 + 1).read_to_end(&mut data)?;
        ensure!(
            data.len() <= 32 * 1024 * 1024,
            "Oversized release support file"
        );
        job.check()?;
        fs::create_dir_all(target.parent().unwrap())?;
        fs::write(&target, data)?;
        assets.push(Asset {
            relative: relative.into(),
            sha256: f::sha256(&target, job)?,
            staged: target,
        });
    }
    let staged = directory.join(format!(
        ".ohmy3do-update-{id}{}",
        if cfg!(windows) { ".exe" } else { "" }
    ));
    let mut output = fs::OpenOptions::new().write(true).create_new(true).open(&staged)
        .context("The app folder is not writable. Move the portable app to a writable folder before updating")?;
    let written = (|| -> Result<()> {
        output.write_all(&bytes)?;
        output.sync_all()?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&staged, fs::Permissions::from_mode(0o755))?;
        }
        Ok(())
    })();
    if let Err(error) = written {
        drop(output);
        let _ = fs::remove_file(&staged);
        return Err(error);
    }
    drop(output);
    let plan = Install {
        executable: executable.clone(),
        staged: staged.clone(),
        previous: directory.join(format!(
            ".ohmy3do-previous-{id}{}",
            if cfg!(windows) { ".exe" } else { "" }
        )),
        ready: work.join("ready"),
        log: work.join("install.log"),
        original_sha256: f::sha256(&executable, job)?,
        new_sha256: f::sha256(&staged, job)?,
        parent: std::process::id(),
        restart: true,
        assets,
    };
    let path = work.join("install.json");
    f::atomic(&path, &serde_json::to_vec_pretty(&plan)?)?;
    Ok((plan, path))
}
pub fn start(plan: &Install, path: &Path) -> Result<Child> {
    let helper = path
        .parent()
        .context("Missing update directory")?
        .join(if cfg!(windows) {
            "installer.exe"
        } else {
            "installer"
        });
    fs::copy(&plan.executable, &helper)?;
    Command::new(helper)
        .arg("--apply-desktop-update")
        .arg(path)
        .spawn()
        .context("Could not start the update installer; current app is unchanged")
}
fn alive(pid: u32) -> bool {
    #[cfg(unix)]
    {
        unsafe extern "C" {
            fn kill(pid: i32, signal: i32) -> i32;
        }
        unsafe { kill(pid as i32, 0) == 0 }
    }
    #[cfg(windows)]
    {
        #[link(name = "kernel32")]
        unsafe extern "system" {
            fn OpenProcess(access: u32, inherit: i32, pid: u32) -> *mut std::ffi::c_void;
            fn GetExitCodeProcess(handle: *mut std::ffi::c_void, code: *mut u32) -> i32;
            fn CloseHandle(handle: *mut std::ffi::c_void) -> i32;
        }
        unsafe {
            let handle = OpenProcess(0x1000, 0, pid);
            if handle.is_null() {
                return false;
            }
            let mut code = 0;
            let ok = GetExitCodeProcess(handle, &mut code);
            CloseHandle(handle);
            ok != 0 && code == 259
        }
    }
}
/// Install atomically and restore the original on a failed launch or missing UI-ready signal.
pub fn apply(plan: &Install) -> Result<()> {
    let job = Job::default();
    let deadline = Instant::now() + Duration::from_secs(45);
    while alive(plan.parent) {
        ensure!(
            Instant::now() < deadline,
            "App did not exit; current executable was preserved"
        );
        std::thread::sleep(Duration::from_millis(100));
    }
    ensure!(
        plan.executable.parent() == plan.staged.parent()
            && plan.previous.parent() == plan.executable.parent(),
        "Update files must share the app directory"
    );
    ensure!(
        !plan.previous.exists() && !plan.ready.exists(),
        "Stale update plan"
    );
    ensure!(
        f::sha256(&plan.executable, &job)? == plan.original_sha256,
        "Installed executable changed since verification"
    );
    ensure!(
        f::sha256(&plan.staged, &job)? == plan.new_sha256,
        "Downloaded executable changed since verification"
    );
    for asset in &plan.assets {
        ensure!(
            asset.relative.starts_with("licenses/")
                || [
                    "LICENSE",
                    "README.md",
                    "THIRD_PARTY.md",
                    "BUILD.json",
                    "OhMy3DOLibrary.png"
                ]
                .contains(&asset.relative.as_str()),
            "Unexpected application support file"
        );
        f::safe(plan.executable.parent().unwrap(), &asset.relative)?;
        ensure!(
            f::sha256(&asset.staged, &job)? == asset.sha256,
            "Release support file changed since verification"
        );
    }
    let mut journal = Journal::new(
        plan.executable.parent().unwrap(),
        &plan
            .log
            .parent()
            .context("Missing update work directory")?
            .join("recovery"),
    )?;
    fs::rename(&plan.executable, &plan.previous)
        .context("Cannot replace running app; close other copies first")?;
    if let Err(e) = fs::rename(&plan.staged, &plan.executable) {
        fs::rename(&plan.previous, &plan.executable)?;
        return Err(e.into());
    }
    let launch = (|| -> Result<Child> {
        for asset in &plan.assets {
            journal.write(&asset.relative, &fs::read(&asset.staged)?)?;
        }
        ensure!(
            f::sha256(&plan.executable, &job)? == plan.new_sha256,
            "Installed executable readback failed"
        );
        let mut command = Command::new(&plan.executable);
        command.arg("--update-ready").arg(&plan.ready);
        if !plan.restart { command.arg("--update-exit"); }
        Ok(command.spawn()?)
    })();
    let mut child = match launch {
        Ok(child) => child,
        Err(e) => {
            journal.undo()?;
            fs::remove_file(&plan.executable)?;
            fs::rename(&plan.previous, &plan.executable)?;
            return Err(e);
        }
    };
    let deadline = Instant::now() + Duration::from_secs(45);
    loop {
        if plan.ready.is_file() {
            journal.finish()?;
            fs::write(
                &plan.log,
                format!(
                    "Installed and started successfully. Previous executable retained at {}\n",
                    plan.previous.display()
                ),
            )?;
            return Ok(());
        }
        if child.try_wait()?.is_some() || Instant::now() > deadline {
            let _ = child.kill();
            let _ = child.wait();
            journal.undo()?;
            fs::remove_file(&plan.executable)?;
            fs::rename(&plan.previous, &plan.executable)?;
            anyhow::bail!("Updated app did not start correctly; restored the previous executable");
        }
        std::thread::sleep(Duration::from_millis(100));
    }
}

/// Recover the UI after a failed installation, but never launch a changed executable
/// or start a duplicate when the original process is still alive.
pub fn restart_previous(plan: &Install) -> Result<()> {
    if !plan.restart || alive(plan.parent) {
        return Ok(());
    }
    ensure!(
        f::sha256(&plan.executable, &Job::default())? == plan.original_sha256,
        "Original app could not be verified for restart; inspect the update log"
    );
    Command::new(&plan.executable)
        .arg("--update-failed")
        .arg(&plan.log)
        .spawn()
        .context("Could not restart the previous application")?;
    Ok(())
}
