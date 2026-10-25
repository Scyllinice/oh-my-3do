//! Safely ejecting a removable drive (the USB stick or SD card the console
//! uses) from inside the app.
//!
//! The app asks the operating system to unmount and power off the drive, the
//! same way the OS's own eject button would. It never forces an unmount: if
//! files are still open, or the drive is not a removable one, it refuses and
//! points the person at the OS's eject controls instead.
//!
//! - Linux: finds the drive with `lsblk`, then asks `udisksctl` to unmount
//!   it and power it off.
//! - Windows: runs `eject-windows.ps1`, then waits until the drive letter
//!   really stops answering.
//! - Other systems: not supported; the person uses the OS's controls.

use crate::fsutil as f;
use anyhow::{bail, ensure, Context, Result};
use serde::Serialize;
use std::path::{Path, PathBuf};

/// The result of a successful eject, shown to the person.
#[derive(Debug, Serialize)]
pub struct Ejected {
    pub root: PathBuf,
    pub message: String,
}

/// Ejects the drive whose root folder is `root`.
///
/// `protected` lists folders the app is using (its own folder, the library,
/// the cache). If any of them is on this drive, ejecting would pull files out
/// from under the app, so it refuses.
pub fn eject(root: &Path, protected: &[PathBuf]) -> Result<Ejected> {
    let root = f::root(root)?;

    for path in protected {
        // A protected folder that no longer exists cannot be on the drive.
        if let Ok(path) = path.canonicalize() {
            ensure!(
                !path.starts_with(&root),
                "Close files and move the app/library/cache off this drive before ejecting"
            );
        }
    }

    eject_with_os(&root)?;

    Ok(Ejected {
        root,
        message: "Drive safely ejected. You can remove it now.".into(),
    })
}

/// A saved library or drive root as the app lists it: whether it is there,
/// and whether the app may offer to eject it.
#[derive(Debug, Serialize)]
pub struct Status {
    pub root: PathBuf,
    pub connected: bool,
    pub removable: bool,
}

/// Looks at a saved root without changing anything: is it plugged in, and is
/// it a drive that [`eject`] would accept?
pub fn status(root: &Path) -> Status {
    let connected = without_no_disk_dialog(|| !f::is_link(root) && root.is_dir());
    let removable = connected && f::root(root).is_ok_and(|root| can_eject(&root));
    Status {
        root: root.to_owned(),
        connected,
        removable,
    }
}

// ---- Linux ----

/// The columns asked of `lsblk`: each device's path, kind ("disk", "part",
/// ...), whether it is removable or hot-pluggable, and where it is mounted.
#[cfg(target_os = "linux")]
const LSBLK_COLUMNS: &str = "PATH,TYPE,RM,HOTPLUG,MOUNTPOINTS";

/// What to tell `udisksctl` to eject on Linux.
#[cfg(target_os = "linux")]
#[derive(Debug, PartialEq, Eq)]
pub struct LinuxTarget {
    /// The mounted volume to unmount, such as `/dev/sdb1`.
    pub device: String,
    /// The whole disk to power off, such as `/dev/sdb`.
    pub disk: String,
}

/// Lists the system's block devices as JSON from `lsblk`.
#[cfg(target_os = "linux")]
fn list_block_devices() -> Result<serde_json::Value> {
    let output = std::process::Command::new("lsblk")
        .args(["--json", "--output", LSBLK_COLUMNS])
        .output()
        .context("Cannot inspect drives: install util-linux or use OS eject")?;
    ensure!(output.status.success(), "Cannot inspect block devices");
    Ok(serde_json::from_slice(&output.stdout)?)
}

/// Whether an `lsblk` yes/no column is set. Different versions write it as
/// `true`, `1` or `"1"`.
#[cfg(target_os = "linux")]
fn lsblk_flag(value: &serde_json::Value) -> bool {
    value == true || value == 1 || value == "1"
}

/// The places an `lsblk` device is mounted (none for an unmounted one).
#[cfg(target_os = "linux")]
fn mount_points(device: &serde_json::Value) -> Vec<&str> {
    match device["mountpoints"].as_array() {
        Some(mounts) => mounts.iter().filter_map(|m| m.as_str()).collect(),
        None => Vec::new(),
    }
}

/// A disk and everything under it (its partitions, and theirs), in order.
#[cfg(target_os = "linux")]
fn device_and_children<'a>(device: &'a serde_json::Value, all: &mut Vec<&'a serde_json::Value>) {
    all.push(device);
    if let Some(children) = device["children"].as_array() {
        for child in children {
            device_and_children(child, all);
        }
    }
}

/// Finds which device to unmount and which disk to power off for the drive
/// mounted at `root`, from an `lsblk` listing.
///
/// Refuses unless `root` is exactly where a removable or hot-plug disk (or
/// one of its partitions) is mounted, and nothing else on that disk is
/// mounted somewhere else: powering off the disk would cut those off too.
#[cfg(target_os = "linux")]
pub fn linux_target(root: &Path, listing: &serde_json::Value) -> Result<LinuxTarget> {
    ensure!(root != Path::new("/"), "The system drive cannot be ejected");
    let disks = listing["blockdevices"]
        .as_array()
        .context("Invalid block-device response")?;

    for disk in disks {
        let mut devices = vec![];
        device_and_children(disk, &mut devices);

        // Which of this disk's devices, if any, is mounted at `root`?
        let mounted_here = devices.iter().find(|device| {
            mount_points(device)
                .iter()
                .any(|mount| Path::new(mount) == root)
        });
        let Some(device) = mounted_here else {
            continue;
        };

        let disk_is_removable =
            disk["type"] == "disk" && (lsblk_flag(&disk["rm"]) || lsblk_flag(&disk["hotplug"]));
        ensure!(
            disk_is_removable,
            "This is not a removable/hot-plug drive; use your OS eject controls"
        );

        let device_type = device["type"].as_str().unwrap_or("");
        ensure!(
            device_type == "disk" || device_type == "part",
            "Use your OS eject controls for this drive type"
        );

        let only_mounted_here = devices.iter().all(|device| {
            mount_points(device)
                .iter()
                .all(|mount| Path::new(mount) == root)
        });
        ensure!(
            only_mounted_here,
            "Other volumes on this drive are mounted; use your OS eject controls"
        );

        let device_path = device["path"].as_str().context("Device path missing")?;
        let disk_path = disk["path"].as_str().context("Disk path missing")?;
        ensure!(
            device_path.starts_with("/dev/") && disk_path.starts_with("/dev/"),
            "Invalid block device paths"
        );

        return Ok(LinuxTarget {
            device: device_path.into(),
            disk: disk_path.into(),
        });
    }

    bail!(
        "Choose the removable drive's mount root, not a subfolder. \
         If this is a WSL/network mount, eject it using Windows."
    )
}

/// Ejects the drive at `root` with `udisksctl`.
#[cfg(target_os = "linux")]
fn eject_with_os(root: &Path) -> Result<()> {
    let target = linux_target(root, &list_block_devices()?)?;

    linux_remove(&target, |args| {
        let result = std::process::Command::new("udisksctl")
            .args(args)
            .output()
            .context("Install udisks2 or use your OS eject controls")?;
        ensure!(
            result.status.success(),
            "{}",
            String::from_utf8_lossy(&result.stderr).trim()
        );
        Ok(())
    })
}

/// Unmounts the target's volume, then powers off its disk, running each
/// `udisksctl` command through `run` (tests pass a stand-in). Never asks
/// the person for a password: it fails instead.
#[cfg(target_os = "linux")]
pub fn linux_remove(
    target: &LinuxTarget,
    mut run: impl FnMut(&[&str]) -> Result<()>,
) -> Result<()> {
    run(&[
        "unmount",
        "--block-device",
        &target.device,
        "--no-user-interaction",
    ])
    .context("Drive is still mounted; close files and try again or use OS eject")?;

    run(&[
        "power-off",
        "--block-device",
        &target.disk,
        "--no-user-interaction",
    ])
    .context("Volume unmounted, but the OS did not confirm safe removal; use OS eject")
}

/// Only a removable drive's own root, which [`eject`] would accept: never
/// the system drive, a fixed disk or a network share.
#[cfg(target_os = "linux")]
fn can_eject(root: &Path) -> bool {
    list_block_devices().is_ok_and(|listing| linux_target(root, &listing).is_ok())
}

// ---- Windows ----

/// The Windows functions this file calls, and the values they use.
#[cfg(windows)]
mod windows_api {
    #[link(name = "kernel32")]
    extern "system" {
        pub fn GetDriveTypeW(root: *const u16) -> u32;
        pub fn GetVolumeInformationW(
            root: *const u16,
            name: *mut u16,
            name_size: u32,
            serial: *mut u32,
            max_component_length: *mut u32,
            flags: *mut u32,
            file_system_name: *mut u16,
            file_system_name_size: u32,
        ) -> i32;
        pub fn SetThreadErrorMode(mode: u32, old_mode: *mut u32) -> i32;
    }

    /// `GetDriveTypeW`'s answer for removable media such as an SD card.
    pub const DRIVE_REMOVABLE: u32 = 2;

    /// Error mode that stops Windows showing "There is no disk in the
    /// drive" and similar dialogs (SEM_FAILCRITICALERRORS |
    /// SEM_NOOPENFILEERRORBOX).
    pub const QUIET_ERROR_MODE: u32 = 0x8001;

    /// Process flag that starts PowerShell without a console window.
    pub const CREATE_NO_WINDOW: u32 = 0x0800_0000;

    /// Errors that mean a drive letter has no volume behind it any more:
    /// path not found, invalid drive, not ready, device not connected.
    pub const DRIVE_GONE_ERRORS: [i32; 4] = [3, 15, 21, 1167];
}

/// A drive root as plain text such as `K:\`, without the `\\?\` prefix a
/// canonical path has. None if `root` is not the root of a lettered drive.
#[cfg(windows)]
fn drive_letter_root(root: &Path) -> Option<String> {
    let plain = root
        .to_string_lossy()
        .trim_start_matches("\\\\?\\")
        .to_owned();
    let bytes = plain.as_bytes();
    let is_drive_root =
        bytes.len() == 3 && bytes[0].is_ascii_alphabetic() && bytes[1] == b':' && bytes[2] == b'\\';
    is_drive_root.then_some(plain)
}

/// Text as a nul-terminated UTF-16 string, as Windows functions take it.
#[cfg(windows)]
fn wide_string(text: &str) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    std::ffi::OsStr::new(text)
        .encode_wide()
        .chain(Some(0))
        .collect()
}

/// Whether Windows reports the drive (`K:\` as UTF-16) as removable media.
#[cfg(windows)]
fn is_removable_drive(wide_root: &[u16]) -> bool {
    let drive_type = unsafe { windows_api::GetDriveTypeW(wide_root.as_ptr()) };
    drive_type == windows_api::DRIVE_REMOVABLE
}

/// Turns off Windows' "no disk" dialogs for this thread while it exists,
/// and puts the old setting back when dropped.
#[cfg(windows)]
struct QuietErrorMode {
    old_mode: u32,
}

#[cfg(windows)]
impl QuietErrorMode {
    /// Turns the dialogs off. None if Windows refused.
    fn start() -> Option<Self> {
        let mut old_mode = 0;
        let changed = unsafe {
            windows_api::SetThreadErrorMode(windows_api::QUIET_ERROR_MODE, &mut old_mode)
        };
        (changed != 0).then_some(QuietErrorMode { old_mode })
    }
}

#[cfg(windows)]
impl Drop for QuietErrorMode {
    fn drop(&mut self) {
        unsafe {
            windows_api::SetThreadErrorMode(self.old_mode, std::ptr::null_mut());
        }
    }
}

/// Whether the drive (`K:\` as UTF-16) still has a volume Windows can read.
/// An error other than "the drive is gone" is passed on.
#[cfg(windows)]
fn volume_present(wide_root: &[u16]) -> Result<bool> {
    use std::ptr::null_mut;
    let answered = unsafe {
        windows_api::GetVolumeInformationW(
            wide_root.as_ptr(),
            null_mut(),
            0,
            null_mut(),
            null_mut(),
            null_mut(),
            null_mut(),
            0,
        )
    };
    if answered != 0 {
        return Ok(true);
    }

    let error = std::io::Error::last_os_error();
    let drive_gone = error
        .raw_os_error()
        .is_some_and(|code| windows_api::DRIVE_GONE_ERRORS.contains(&code));
    if drive_gone {
        Ok(false)
    } else {
        Err(error.into())
    }
}

/// Ejects the drive at `root` with `eject-windows.ps1`, then waits up to 15
/// seconds for the drive letter to stop answering, which is the only sure
/// sign Windows really let it go.
#[cfg(windows)]
fn eject_with_os(root: &Path) -> Result<()> {
    use std::{
        os::windows::process::CommandExt,
        time::{Duration, Instant},
    };

    let drive_root =
        drive_letter_root(root).context("Choose a removable drive root such as K:\\")?;
    let drive_letter = &drive_root[..1];
    let wide_root = wide_string(&drive_root);

    // Only removable media, as the removal script also requires: an internal
    // or external fixed disk is the system's to remove.
    ensure!(
        is_removable_drive(&wide_root),
        "This is not a removable drive; use Windows Safely Remove Hardware"
    );

    // Avoid no-media dialogs while checking whether Windows actually removed
    // it. The setting is restored when `_quiet` goes out of scope.
    let _quiet = QuietErrorMode::start().context("Cannot configure safe drive detection")?;
    ensure!(volume_present(&wide_root)?, "Drive is already unavailable");

    // The script removes only the selected card media. It never requests
    // PnP removal of the card reader, which could take other slots offline.
    // It locks the volume before dismounting, and falls back to Explorer's
    // eject for standard users; either way the result is checked below.
    //
    // Errors are caught around the whole script so only the message comes
    // back, without PowerShell's source excerpt, category and stack noise.
    let script = include_str!("eject-windows.ps1");
    let command = format!(
        "try {{ & {{ {script} }} -Letter '{drive_letter}' }} \
         catch {{ [Console]::Error.WriteLine($_.Exception.GetBaseException().Message); exit 1 }}"
    );
    let output = std::process::Command::new("powershell.exe")
        .args(["-NoProfile", "-NonInteractive", "-Command", &command])
        .creation_flags(windows_api::CREATE_NO_WINDOW)
        .output()
        .context("Cannot request Windows eject; use Safely Remove Hardware")?;
    ensure!(
        output.status.success(),
        "Windows eject failed: {}",
        String::from_utf8_lossy(&output.stderr).trim()
    );

    // Confirm the selected volume is unavailable after the removal request.
    let started = Instant::now();
    while started.elapsed() < Duration::from_secs(15) {
        if !volume_present(&wide_root)? {
            return Ok(());
        }
        std::thread::sleep(Duration::from_millis(250));
    }

    bail!(
        "Windows did not confirm removal. \
         Close files and use Safely Remove Hardware before unplugging the drive."
    )
}

/// Only a removable drive's own root, which [`eject`] would accept: never
/// the system drive (usually C:), a fixed disk or a network share.
#[cfg(windows)]
fn can_eject(root: &Path) -> bool {
    let Some(drive_root) = drive_letter_root(root) else {
        return false;
    };

    let system_drive = std::env::var("SystemDrive").unwrap_or_else(|_| "C:".into());
    if drive_root[..2].eq_ignore_ascii_case(system_drive.trim_end_matches('\\')) {
        return false;
    }

    is_removable_drive(&wide_string(&drive_root))
}

/// Runs `check` without Windows' "no disk" dialog popping up for an empty
/// card slot. If the dialogs cannot be turned off, runs it anyway.
#[cfg(windows)]
fn without_no_disk_dialog<T>(check: impl FnOnce() -> T) -> T {
    let _quiet = QuietErrorMode::start();
    check()
}

// ---- Other systems ----

#[cfg(not(any(windows, target_os = "linux")))]
fn eject_with_os(_root: &Path) -> Result<()> {
    bail!("Use your operating system's eject controls on this platform")
}

#[cfg(not(any(windows, target_os = "linux")))]
fn can_eject(_root: &Path) -> bool {
    false
}

/// Only Windows shows "no disk" dialogs, so elsewhere this just runs `check`.
#[cfg(not(windows))]
fn without_no_disk_dialog<T>(check: impl FnOnce() -> T) -> T {
    check()
}
