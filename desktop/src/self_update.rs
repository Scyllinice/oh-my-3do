//! Updating the portable desktop app in place.
//!
//! An update happens in three stages, so the app is never left broken:
//!
//! 1. [`stage`]: the downloaded release ZIP (already checked against the
//!    release's published SHA-256) is unpacked. The new executable is
//!    written beside the running one under a temporary name, and the
//!    program's documentation and licenses go into the update cache. An
//!    install plan ([`Install`]) is saved as JSON.
//! 2. [`start`]: a copy of the running program is started as the installer,
//!    and the app exits.
//! 3. [`apply`] (run by that installer): once the app has exited, the old
//!    executable is renamed aside, the new one moved into place, and the new
//!    app started. If it does not report that its window is ready within 45
//!    seconds, everything is put back as it was.
//!
//! Only the executable and the allowed documentation files are ever
//! replaced; settings, libraries and game files are never touched.

use crate::{fsutil as f, library::Journal, Job};
use anyhow::{bail, ensure, Context, Result};
use serde::{Deserialize, Serialize};
use std::{
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
    process::{Child, Command},
    time::{Duration, Instant},
};

/// The folder every file of a release ZIP sits in.
const RELEASE_FOLDER: &str = "OhMy3DO-Library/";

/// The desktop executable's path inside a release ZIP.
const RELEASE_EXECUTABLE: &str = if cfg!(windows) {
    "OhMy3DO-Library/OhMy3DOLibrary.exe"
} else {
    "OhMy3DO-Library/OhMy3DOLibrary"
};

/// The extension executables need on this system ("" outside Windows).
const EXECUTABLE_EXTENSION: &str = if cfg!(windows) { ".exe" } else { "" };

/// The release files, besides the executable and anything in `licenses/`,
/// that an update copies into the app folder.
const SUPPORT_FILES: [&str; 5] = [
    "LICENSE",
    "README.md",
    "THIRD_PARTY.md",
    "BUILD.json",
    "OhMy3DOLibrary.png",
];

/// The largest executable an update will accept.
const MAX_EXECUTABLE_BYTES: u64 = 256 * 1024 * 1024;

/// The most the support files of a release may add up to.
const MAX_SUPPORT_BYTES: u64 = 32 * 1024 * 1024;

/// The most files a release ZIP may contain.
const MAX_RELEASE_FILES: usize = 8192;

/// The largest result file [`last_report`] will read.
const MAX_REPORT_BYTES: u64 = 64 * 1024;

/// How long to wait for the old app to exit, and for the new one to start.
const WAIT_LIMIT: Duration = Duration::from_secs(45);

/// How often to look again while waiting.
const POLL_INTERVAL: Duration = Duration::from_millis(100);

/// A support file (documentation or license) staged for the update.
#[derive(Clone, Serialize, Deserialize)]
pub struct Asset {
    /// Where it goes, relative to the app folder.
    pub relative: String,
    /// Where it waits in the update cache.
    pub staged: PathBuf,
    /// Its SHA-256 when staged, so a changed file is caught.
    pub sha256: String,
}

/// An install plan, written by [`stage`] and carried out by [`apply`].
#[derive(Clone, Serialize, Deserialize)]
pub struct Install {
    /// The app being replaced.
    pub executable: PathBuf,
    /// The new executable, beside the old one under a temporary name.
    pub staged: PathBuf,
    /// Where the old executable is moved and kept.
    pub previous: PathBuf,
    /// A file the new app creates once its window is ready.
    pub ready: PathBuf,
    /// A short text log of how the update went.
    pub log: PathBuf,
    /// SHA-256 of the old and new executables, checked before each step.
    pub original_sha256: String,
    pub new_sha256: String,
    /// The process id of the app that started the update.
    pub parent: u32,
    /// False for an update on exit: the new app checks it starts, then
    /// closes again.
    #[serde(default = "restart_by_default")]
    pub restart: bool,
    #[serde(default)]
    pub assets: Vec<Asset>,
}

/// Plans saved before `restart` existed always restarted the app.
fn restart_by_default() -> bool {
    true
}

/// How the last update went, saved for the app to show when it next starts.
#[derive(Clone, Serialize, Deserialize)]
pub struct Report {
    pub executable: PathBuf,
    pub success: bool,
    /// Whether the old executable is still in place, unchanged.
    pub previous_intact: bool,
    pub detail: String,
    pub log: PathBuf,
}

/// Saves the outcome of an update (to its log, and as `last-result.json`
/// in the update cache). Called before any attempt to reopen the old app,
/// so a failure is recorded even for an update on exit, which leaves the
/// app closed.
pub fn record_result(plan: &Install, result: &Result<()>) -> Result<()> {
    let detail = match result {
        Ok(()) => "Updated and startup verified successfully.".into(),
        Err(error) => format!("{error:#}"),
    };
    fs::write(&plan.log, &detail)?;

    let current_sha256 = f::sha256(&plan.executable, &Job::default()).ok();
    let report = Report {
        executable: plan.executable.clone(),
        success: result.is_ok(),
        previous_intact: current_sha256.as_deref() == Some(&plan.original_sha256),
        detail,
        log: plan.log.clone(),
    };

    // The log is in self-update/<id>/, and the result goes in self-update/.
    let directory = plan
        .log
        .parent()
        .and_then(Path::parent)
        .context("Missing update cache directory")?;
    f::atomic(
        &directory.join("last-result.json"),
        &serde_json::to_vec_pretty(&report)?,
    )
}

/// The saved outcome of the last update of this executable, if there is one.
pub fn last_report(cache: &Path, executable: &Path) -> Option<Report> {
    let path = cache.join("self-update/last-result.json");
    if fs::metadata(&path).ok()?.len() > MAX_REPORT_BYTES {
        return None;
    }
    let bytes = fs::read(path).ok()?;
    let report: Report = serde_json::from_slice(&bytes).ok()?;

    let is_this_app = report.executable == executable.canonicalize().ok()?;
    is_this_app.then_some(report)
}

/// Throws away a staged update: the new executable, and the plan at `path`.
/// Refuses if the staged executable has changed, so a damaged or tampered
/// file is kept for inspection.
pub fn discard(plan: &Install, path: &Path) -> Result<()> {
    ensure!(
        f::sha256(&plan.staged, &Job::default())? == plan.new_sha256,
        "Staged update changed; keep its recovery files and check updates again"
    );
    fs::remove_file(&plan.staged)?;
    let _ = fs::remove_file(path);
    Ok(())
}

/// Unpacks a verified release ZIP and writes an install plan for
/// `executable` (the running app). Work files go in `cache`/self-update/.
///
/// Returns the plan and where it was saved.
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
    let app_folder = executable
        .parent()
        .context("Missing executable directory")?;

    // Each update gets its own work folder in the cache.
    let id = uuid::Uuid::new_v4().to_string();
    let work = cache.join("self-update").join(&id);
    fs::create_dir_all(&work)?;

    let mut zip = zip::ZipArchive::new(fs::File::open(zip_path)?)?;
    let new_program = read_release_executable(&mut zip)?;
    job.check()?;
    let assets = extract_support_files(&mut zip, &work.join("assets"), job)?;

    let staged = app_folder.join(format!(".ohmy3do-update-{id}{EXECUTABLE_EXTENSION}"));
    write_new_executable(&staged, &new_program)?;

    let plan = Install {
        executable: executable.clone(),
        staged: staged.clone(),
        previous: app_folder.join(format!(".ohmy3do-previous-{id}{EXECUTABLE_EXTENSION}")),
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

/// Reads the desktop executable out of a release ZIP. Checks there is
/// exactly one, its size is sensible, and it is a program for this system
/// (by the first bytes of the file).
fn read_release_executable(zip: &mut zip::ZipArchive<fs::File>) -> Result<Vec<u8>> {
    let copies = zip
        .file_names()
        .filter(|name| *name == RELEASE_EXECUTABLE)
        .count();
    ensure!(
        copies == 1,
        "Release must contain exactly one desktop executable"
    );

    let file = zip.by_name(RELEASE_EXECUTABLE)?;
    ensure!(
        file.size() > 64 && file.size() <= MAX_EXECUTABLE_BYTES,
        "Invalid executable size"
    );

    // The size in the ZIP could lie, so read at most one byte past the limit.
    let mut bytes = Vec::new();
    file.take(MAX_EXECUTABLE_BYTES + 1)
        .read_to_end(&mut bytes)?;
    ensure!(
        bytes.len() as u64 <= MAX_EXECUTABLE_BYTES,
        "Oversized executable"
    );

    let for_this_system = if cfg!(windows) {
        bytes.starts_with(b"MZ")
    } else if cfg!(target_os = "macos") {
        bytes.starts_with(&[0xcf, 0xfa, 0xed, 0xfe])
    } else {
        bytes.starts_with(b"\x7fELF")
    };
    ensure!(
        for_this_system,
        "Release does not contain an executable for this platform"
    );

    Ok(bytes)
}

/// Whether a file in the release (relative to its folder) is one an update
/// copies into the app folder: the licenses and [`SUPPORT_FILES`].
fn is_support_file(relative: &str) -> bool {
    relative.starts_with("licenses/") || SUPPORT_FILES.contains(&relative)
}

/// Copies the release's support files (documentation and licenses) from
/// the ZIP into `assets_folder`, and returns them with their checksums.
/// Every other file in the release is ignored.
fn extract_support_files(
    zip: &mut zip::ZipArchive<fs::File>,
    assets_folder: &Path,
    job: &Job,
) -> Result<Vec<Asset>> {
    let names: Vec<String> = zip.file_names().map(str::to_owned).collect();
    ensure!(
        names.len() <= MAX_RELEASE_FILES,
        "Release contains too many files"
    );

    let mut assets: Vec<Asset> = vec![];
    let mut total_bytes = 0u64;

    for name in names {
        let Some(relative) = name.strip_prefix(RELEASE_FOLDER) else {
            continue;
        };
        let is_folder = name.ends_with('/');
        if is_folder || !is_support_file(relative) {
            continue;
        }

        // `f::safe` refuses names that would land outside the folder.
        let target = f::safe(assets_folder, relative)?;
        let duplicate = assets
            .iter()
            .any(|asset| asset.relative.eq_ignore_ascii_case(relative));
        ensure!(!duplicate, "Duplicate release asset");

        let file = zip.by_name(&name)?;
        const FILE_TYPE_MASK: u32 = 0o170000;
        const SYMBOLIC_LINK: u32 = 0o120000;
        let is_link = file
            .unix_mode()
            .is_some_and(|mode| mode & FILE_TYPE_MASK == SYMBOLIC_LINK);
        ensure!(!is_link, "Release contains a symbolic link");

        total_bytes = total_bytes
            .checked_add(file.size())
            .context("Asset size overflow")?;
        ensure!(
            total_bytes <= MAX_SUPPORT_BYTES,
            "Release support files exceed 32 MB"
        );

        // The size in the ZIP could lie, so read at most one byte past the limit.
        let mut data = vec![];
        file.take(MAX_SUPPORT_BYTES + 1).read_to_end(&mut data)?;
        ensure!(
            data.len() as u64 <= MAX_SUPPORT_BYTES,
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

    Ok(assets)
}

/// Writes the new executable to `path` (a new file beside the running app),
/// flushes it to disk and marks it runnable. A half-written file is removed.
fn write_new_executable(path: &Path, program: &[u8]) -> Result<()> {
    let mut output = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .context(
            "The app folder is not writable. \
             Move the portable app to a writable folder before updating",
        )?;

    let written = (|| -> Result<()> {
        output.write_all(program)?;
        output.sync_all()?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(path, fs::Permissions::from_mode(0o755))?;
        }
        Ok(())
    })();

    // Close the file before removing it (Windows cannot remove an open file).
    drop(output);
    if written.is_err() {
        let _ = fs::remove_file(path);
    }
    written
}

/// Starts the installer for the plan saved at `path`: a copy of the running
/// app, run with `--apply-desktop-update`. The app should exit next, so the
/// installer can replace it.
pub fn start(plan: &Install, path: &Path) -> Result<Child> {
    let installer_name = format!("installer{EXECUTABLE_EXTENSION}");
    let installer = path
        .parent()
        .context("Missing update directory")?
        .join(installer_name);
    fs::copy(&plan.executable, &installer)?;

    Command::new(installer)
        .arg("--apply-desktop-update")
        .arg(path)
        .spawn()
        .context("Could not start the update installer; current app is unchanged")
}

/// Whether the process with id `pid` is still running.
fn process_running(pid: u32) -> bool {
    #[cfg(unix)]
    {
        unsafe extern "C" {
            fn kill(pid: i32, signal: i32) -> i32;
        }
        // Signal 0 sends nothing; it only checks that the process exists.
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
        const PROCESS_QUERY_LIMITED_INFORMATION: u32 = 0x1000;
        // The "exit code" of a process that has not exited.
        const STILL_ACTIVE: u32 = 259;

        unsafe {
            let handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
            if handle.is_null() {
                return false;
            }
            let mut exit_code = 0;
            let answered = GetExitCodeProcess(handle, &mut exit_code);
            CloseHandle(handle);
            answered != 0 && exit_code == STILL_ACTIVE
        }
    }
}

/// Carries out an install plan (run by the installer [`start`] launched).
///
/// Waits for the old app to exit, checks nothing changed since staging,
/// swaps the executables, copies the support files and starts the new app.
/// If the new app fails to start or does not report its window ready in
/// time, the old executable and files are put back.
pub fn apply(plan: &Install) -> Result<()> {
    wait_for_app_to_exit(plan.parent)?;
    verify_plan(plan)?;

    // The journal records each support file it replaces, so they can be put
    // back if the new app fails.
    let app_folder = plan.executable.parent().unwrap();
    let work_folder = plan.log.parent().context("Missing update work directory")?;
    let mut journal = Journal::new(app_folder, &work_folder.join("recovery"))?;

    // Swap the executables. If the new one cannot be moved in, move the old
    // one straight back.
    fs::rename(&plan.executable, &plan.previous)
        .context("Cannot replace running app; close other copies first")?;
    if let Err(error) = fs::rename(&plan.staged, &plan.executable) {
        fs::rename(&plan.previous, &plan.executable)?;
        return Err(error.into());
    }

    let mut new_app = match install_files_and_launch(plan, &mut journal) {
        Ok(child) => child,
        Err(error) => {
            restore_previous(plan, &mut journal)?;
            return Err(error);
        }
    };

    // The new app creates the `ready` file once its window is up.
    let deadline = Instant::now() + WAIT_LIMIT;
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

        let exited = new_app.try_wait()?.is_some();
        if exited || Instant::now() > deadline {
            let _ = new_app.kill();
            let _ = new_app.wait();
            restore_previous(plan, &mut journal)?;
            bail!("Updated app did not start correctly; restored the previous executable");
        }

        std::thread::sleep(POLL_INTERVAL);
    }
}

/// Waits up to [`WAIT_LIMIT`] for the app that started the update to exit.
/// If it does not, the update is abandoned before anything is changed.
fn wait_for_app_to_exit(pid: u32) -> Result<()> {
    let deadline = Instant::now() + WAIT_LIMIT;
    while process_running(pid) {
        ensure!(
            Instant::now() < deadline,
            "App did not exit; current executable was preserved"
        );
        std::thread::sleep(POLL_INTERVAL);
    }
    Ok(())
}

/// Checks an install plan before anything is changed: all executables are
/// in the app folder, no earlier attempt left files behind, and the old
/// executable, the new one and every support file are exactly as staged.
fn verify_plan(plan: &Install) -> Result<()> {
    let job = Job::default();

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

    let app_folder = plan.executable.parent().unwrap();
    for asset in &plan.assets {
        ensure!(
            is_support_file(&asset.relative),
            "Unexpected application support file"
        );
        // `f::safe` refuses a path that would land outside the app folder.
        f::safe(app_folder, &asset.relative)?;
        ensure!(
            f::sha256(&asset.staged, &job)? == asset.sha256,
            "Release support file changed since verification"
        );
    }
    Ok(())
}

/// After the executables are swapped: copies the support files into the app
/// folder, checks the new executable reads back correctly, and starts it.
fn install_files_and_launch(plan: &Install, journal: &mut Journal) -> Result<Child> {
    for asset in &plan.assets {
        journal.write(&asset.relative, &fs::read(&asset.staged)?)?;
    }

    ensure!(
        f::sha256(&plan.executable, &Job::default())? == plan.new_sha256,
        "Installed executable readback failed"
    );

    let mut command = Command::new(&plan.executable);
    command.arg("--update-ready").arg(&plan.ready);
    if !plan.restart {
        command.arg("--update-exit");
    }
    Ok(command.spawn()?)
}

/// Undoes a failed install: puts back the replaced support files and the
/// old executable.
fn restore_previous(plan: &Install, journal: &mut Journal) -> Result<()> {
    journal.undo()?;
    fs::remove_file(&plan.executable)?;
    fs::rename(&plan.previous, &plan.executable)?;
    Ok(())
}

/// Reopens the old app after a failed install, so the person is not left
/// with nothing. Does nothing for an update on exit, or if the old app is
/// somehow still running. Never launches an executable that is not exactly
/// the original.
pub fn restart_previous(plan: &Install) -> Result<()> {
    if !plan.restart || process_running(plan.parent) {
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
