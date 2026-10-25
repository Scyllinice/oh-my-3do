//! The support report: a plain-text file the person can export and send
//! with a bug report. It holds the drive's readiness check, the console's
//! own diagnostics.txt and the app's recent activity log, with file paths
//! but never any game or save data, and is limited in size.

use crate::{fsutil as f, readiness, Job};
use anyhow::{ensure, Context, Result};
use std::{
    fs,
    io::Read,
    path::Path,
    time::{SystemTime, UNIX_EPOCH},
};
/// Every report starts with this line; exporting only replaces files that do.
const HEADER: &str = "Oh My 3DO support report\n";

/// The largest report allowed.
const LIMIT: usize = 2 * 1024 * 1024;

/// Writes the support report for the drive at `root`, with the last 500
/// lines of `activity` (the app's activity log).
pub fn collect(root: &Path, activity: &[String], job: &Job) -> Result<String> {
    let root = f::root(root)?;

    // About the app, the computer and the drive.
    let mut report = format!(
        "{HEADER}App version: {}\n\
         Platform: {} / {}\n\
         Generated (Unix UTC seconds): {}\n\
         Drive: {}\n\n\
         Includes file paths and diagnostic text; no game or save payloads.\n\n",
        env!("CARGO_PKG_VERSION"),
        std::env::consts::OS,
        std::env::consts::ARCH,
        SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs(),
        root.display()
    );

    // The drive's readiness check.
    let readiness = readiness::check(&root, job)?;
    report.push_str(&format!(
        "READINESS\n{} errors, {} warnings\n",
        readiness.errors, readiness.warnings
    ));
    for c in readiness.checks {
        report.push_str(&format!("\n[{}] {}\n{}\n", c.status, c.name, c.detail));
        if !c.fix.is_empty() {
            report.push_str(&format!("Suggested action: {}\n", c.fix));
        }
    }
    job.check()?;

    // What the console menu exported (its first 4 KiB, without the padding
    // the menu adds to fill the file).
    report.push_str("\nCONSOLE DIAGNOSTICS\n");
    let diagnostics = f::safe(&root, ".ohmy3do/diagnostics.txt")?;
    if diagnostics.is_file() {
        let mut data = vec![];
        fs::File::open(diagnostics)?
            .take(4097)
            .read_to_end(&mut data)?;
        let truncated = data.len() > 4096;
        data.truncate(4096);
        let text = String::from_utf8_lossy(&data);
        report.push_str(text.trim_end_matches(['\0', ' ', '\r', '\n']));
        report.push('\n');
        if truncated {
            report.push_str("[Console diagnostics truncated at 4 KiB.]\n");
        }
    } else {
        report.push_str("No console diagnostics found. Export them from the console's Diagnostics tool if needed.\n");
    }

    // The app's recent activity, each line cut to 2048 characters.
    report.push_str("\nDESKTOP ACTIVITY (most recent 500 messages)\n");
    for line in activity
        .iter()
        .rev()
        .take(500)
        .collect::<Vec<_>>()
        .into_iter()
        .rev()
    {
        report.extend(line.chars().take(2048));
        report.push('\n');
    }
    ensure!(report.len() <= LIMIT, "Support report exceeds size limit");
    Ok(report)
}
/// Writes the support report to `target`, a .txt file. An existing file is
/// replaced only if it is an earlier support report that does not change
/// while the report is being made; the console's own diagnostics.txt is
/// never replaced. The written file is read back to check it.
pub fn export(root: &Path, target: &Path, activity: &[String], job: &Job) -> Result<()> {
    ensure!(
        target
            .extension()
            .is_some_and(|e| e.eq_ignore_ascii_case("txt")),
        "Choose a .txt support report filename"
    );
    let parent = f::root(target.parent().context("Choose a report folder")?)?;
    let target = f::safe(
        &parent,
        target
            .file_name()
            .context("Choose a report filename")?
            .to_str()
            .context("Invalid report filename")?,
    )?;
    let root = f::root(root)?;
    ensure!(
        target != root.join(".ohmy3do/diagnostics.txt"),
        "Keep the original console diagnostics; choose a different report filename"
    );
    let previous = if target.exists() {
        ensure!(
            fs::metadata(&target)?.len() <= LIMIT as u64,
            "Existing report is oversized"
        );
        let data = fs::read(&target)?;
        ensure!(
            data.starts_with(HEADER.as_bytes()),
            "This file is not a support report; choose another filename"
        );
        Some(data)
    } else {
        None
    };
    let report = collect(&root, activity, job)?;
    job.check()?;
    let current = if target.exists() {
        Some(fs::read(&target)?)
    } else {
        None
    };
    ensure!(
        previous == current,
        "Report changed during export; choose another filename"
    );
    f::atomic(&target, report.as_bytes())?;
    ensure!(
        fs::read(&target)? == report.as_bytes(),
        "Support report readback failed"
    );
    Ok(())
}
