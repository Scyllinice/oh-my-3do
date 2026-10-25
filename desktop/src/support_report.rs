//! User-exported, bounded plain-text diagnostics; excludes save/game payloads.
use crate::{fsutil as f, readiness, Job};
use anyhow::{ensure, Context, Result};
use std::{
    fs,
    io::Read,
    path::Path,
    time::{SystemTime, UNIX_EPOCH},
};
const HEADER: &str = "Oh My 3DO support report\n";
const LIMIT: usize = 2 * 1024 * 1024;
pub fn collect(root: &Path, activity: &[String], job: &Job) -> Result<String> {
    let root = f::root(root)?;
    let mut report=format!("{HEADER}App version: {}\nPlatform: {} / {}\nGenerated (Unix UTC seconds): {}\nDrive: {}\n\nIncludes file paths and diagnostic text; no game or save payloads.\n\n",env!("CARGO_PKG_VERSION"),std::env::consts::OS,std::env::consts::ARCH,SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs(),root.display());
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
