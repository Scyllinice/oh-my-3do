//! OS-assisted removal of a selected removable drive. Never forces an unmount.
use crate::fsutil as f;
use anyhow::{bail,ensure,Context,Result};
use serde::Serialize;
use std::path::{Path,PathBuf};
#[derive(Debug,Serialize)]
pub struct Ejected {pub root:PathBuf,pub message:String}
pub fn eject(root:&Path,protected:&[PathBuf])->Result<Ejected>{
 let root=f::root(root)?;
 for path in protected {
  if let Ok(path)=path.canonicalize(){ensure!(!path.starts_with(&root),"Close files and move the app/library/cache off this drive before ejecting");}
 }
 platform(&root)?;
 Ok(Ejected{root,message:"Drive safely ejected. You can remove it now.".into()})
}
#[cfg(target_os="linux")]
#[derive(Debug,PartialEq,Eq)]
pub struct LinuxTarget {pub device:String,pub disk:String}
#[cfg(target_os="linux")]
pub fn linux_target(root:&Path,listing:&serde_json::Value)->Result<LinuxTarget>{
 use serde_json::Value;
 fn flag(v:&Value)->bool{v==true||v==1||v=="1"}
 fn mounts(node:&Value)->Vec<&str>{node["mountpoints"].as_array().map(|m|m.iter().filter_map(Value::as_str).collect()).unwrap_or_default()}
 fn walk<'a>(node:&'a Value,all:&mut Vec<&'a Value>){all.push(node);if let Some(children)=node["children"].as_array(){for child in children{walk(child,all);}}}
 ensure!(root!=Path::new("/"),"The system drive cannot be ejected");
 let disks=listing["blockdevices"].as_array().context("Invalid block-device response")?;
 for disk in disks{
  let mut nodes=vec![];walk(disk,&mut nodes);
  if let Some(node)=nodes.iter().find(|n|mounts(n).iter().any(|p|Path::new(p)==root)){
   ensure!(disk["type"]=="disk"&&(flag(&disk["rm"])||flag(&disk["hotplug"])),"This is not a removable/hot-plug drive; use your OS eject controls");
   ensure!(["disk","part"].contains(&node["type"].as_str().unwrap_or("")),"Use your OS eject controls for this drive type");
   ensure!(nodes.iter().all(|n|mounts(n).iter().all(|p|Path::new(p)==root)),"Other volumes on this drive are mounted; use your OS eject controls");
   let device=node["path"].as_str().context("Device path missing")?;
   let disk=disk["path"].as_str().context("Disk path missing")?;
   ensure!(device.starts_with("/dev/")&&disk.starts_with("/dev/"),"Invalid block device paths");
   return Ok(LinuxTarget{device:device.into(),disk:disk.into()});
  }
 }
 bail!("Choose the removable drive's mount root, not a subfolder. If this is a WSL/network mount, eject it using Windows.")
}
#[cfg(target_os="linux")]
fn platform(root:&Path)->Result<()>{
 let output=std::process::Command::new("lsblk").args(["--json","--output","PATH,TYPE,RM,HOTPLUG,MOUNTPOINTS"]).output().context("Cannot inspect drives: install util-linux or use OS eject")?;
 ensure!(output.status.success(),"Cannot inspect block devices");
 let target=linux_target(root,&serde_json::from_slice(&output.stdout)?)?;
 linux_remove(&target,|args|{
  let result=std::process::Command::new("udisksctl").args(args).output().context("Install udisks2 or use your OS eject controls")?;
  ensure!(result.status.success(),"{}",String::from_utf8_lossy(&result.stderr).trim());Ok(())
 })
}
#[cfg(target_os="linux")]
pub fn linux_remove(target:&LinuxTarget,mut run:impl FnMut(&[&str])->Result<()>)->Result<()>{
 run(&["unmount","--block-device",&target.device,"--no-user-interaction"]).context("Drive is still mounted; close files and try again or use OS eject")?;
 run(&["power-off","--block-device",&target.disk,"--no-user-interaction"]).context("Volume unmounted, but the OS did not confirm safe removal; use OS eject")
}
#[cfg(windows)]
fn platform(root:&Path)->Result<()>{
 use std::{os::windows::{ffi::OsStrExt,process::CommandExt},ptr,time::{Duration,Instant}};
 #[link(name="kernel32")]
 extern "system" {
  fn GetDriveTypeW(root:*const u16)->u32;
  fn GetVolumeInformationW(root:*const u16,name:*mut u16,size:u32,serial:*mut u32,max:*mut u32,flags:*mut u32,fs:*mut u16,fs_size:u32)->i32;
  fn SetThreadErrorMode(mode:u32,old:*mut u32)->i32;
 }
 let plain=root.to_string_lossy().trim_start_matches("\\\\?\\").to_owned();
 let b=plain.as_bytes();
 ensure!(b.len()==3&&b[0].is_ascii_alphabetic()&&b[1]==b':'&&b[2]==b'\\',"Choose a removable drive root such as K:\\");
 let wide:Vec<u16>=std::ffi::OsStr::new(&plain).encode_wide().chain(Some(0)).collect();
 ensure!(matches!(unsafe{GetDriveTypeW(wide.as_ptr())},2|3),"Use Windows Safely Remove Hardware for network or unsupported drives");
 // Avoid no-media dialogs while checking whether Shell actually removed it.
 let mut old=0;
 ensure!(unsafe{SetThreadErrorMode(0x8001,&mut old)}!=0,"Cannot configure safe drive detection");
 struct Mode(u32);impl Drop for Mode{fn drop(&mut self){unsafe{SetThreadErrorMode(self.0,ptr::null_mut());}}}
 let _mode=Mode(old);
 let present=||->Result<bool>{
  if unsafe{GetVolumeInformationW(wide.as_ptr(),ptr::null_mut(),0,ptr::null_mut(),ptr::null_mut(),ptr::null_mut(),ptr::null_mut(),0)}!=0{return Ok(true);}
  let error=std::io::Error::last_os_error();
  if matches!(error.raw_os_error(),Some(3|15|21|1167)){Ok(false)}else{Err(error.into())}
 };
 ensure!(present()?,"Drive is already unavailable");
 // Remove only the selected card media. Never request PnP removal of its
 // reader, which could take other slots offline. Lock before dismount;
 // standard-user Explorer fallback is also verified below.
 let script = include_str!("eject-windows.ps1");
 // Catch at the outer boundary to return the message without PowerShell's
 // source excerpt, CategoryInfo and stack-formatting noise.
 let script = format!("try {{ & {{ {script} }} -Letter '{}' }} catch {{ [Console]::Error.WriteLine($_.Exception.GetBaseException().Message); exit 1 }}", b[0] as char);
 let output=std::process::Command::new("powershell.exe").args(["-NoProfile","-NonInteractive","-Command",&script]).creation_flags(0x08000000).output().context("Cannot request Windows eject; use Safely Remove Hardware")?;
 ensure!(output.status.success(),"Windows eject failed: {}",String::from_utf8_lossy(&output.stderr).trim());
 // Confirm the selected volume is unavailable after the removal request.
 let started=Instant::now();
 while started.elapsed()<Duration::from_secs(15){
  if !present()?{return Ok(());}
  std::thread::sleep(Duration::from_millis(250));
 }
 bail!("Windows did not confirm removal. Close files and use Safely Remove Hardware before unplugging the drive.")
}
#[cfg(not(any(windows,target_os="linux")))]
fn platform(_: &Path)->Result<()>{bail!("Use your operating system's eject controls on this platform")}
