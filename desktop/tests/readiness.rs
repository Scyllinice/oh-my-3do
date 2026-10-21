use oh_my_3do_library::{console_config::Settings, library, readiness, save_archive::{Archive,Entry}, setup, Job};
use std::{collections::BTreeMap,fs,path::Path};
fn image()->Vec<u8>{let mut b=vec![0;2048];b[..7].copy_from_slice(&[1,90,90,90,90,90,1]);b[76..80].copy_from_slice(&2048u32.to_be_bytes());b[80..84].copy_from_slice(&1u32.to_be_bytes());b}
fn prepare(root:&Path,cache:&Path){let p=root.join("Games/USA/Doom/Doom.iso");fs::create_dir_all(p.parent().unwrap()).unwrap();fs::write(p,image()).unwrap();setup::install_verified(root,cache,&image(),&Settings::default(),&Job::default()).unwrap();}
fn snapshot(root:&Path)->BTreeMap<String,Vec<u8>>{walkdir::WalkDir::new(root).into_iter().map(Result::unwrap).filter(|e|e.file_type().is_file()).map(|e|(e.path().strip_prefix(root).unwrap().to_string_lossy().into_owned(),fs::read(e.path()).unwrap())).collect()}
#[test]
fn prepared_drive_passes_with_optional_media_warning_and_check_is_read_only(){
 let root=tempfile::tempdir().unwrap();let cache=tempfile::tempdir().unwrap();prepare(root.path(),cache.path());
 let before=snapshot(root.path());let report=readiness::check(root.path(),&Job::default()).unwrap();
 assert!(report.ready,"{:?}",report);assert_eq!(report.errors,0);assert_eq!(report.warnings,1);
 assert!(report.checks.iter().any(|c|c.name=="previews"&&c.status=="ok"));assert_eq!(snapshot(root.path()),before);
}
#[test]
fn reports_independent_errors_and_does_not_repair_them(){
 let root=tempfile::tempdir().unwrap();let cache=tempfile::tempdir().unwrap();prepare(root.path(),cache.path());
 fs::write(root.path().join("boot.iso"),b"broken").unwrap();
 fs::write(root.path().join(".ohmy3do/ohmy3do.ini"),b"bad setting").unwrap();
 fs::remove_file(root.path().join("Games").join(setup::NAVIGATION_HELPER)).unwrap();
 fs::write(root.path().join(".ohmy3do/library.3di"),b"bad index").unwrap();
 let before=snapshot(root.path());let report=readiness::check(root.path(),&Job::default()).unwrap();
 assert!(!report.ready);for name in ["Boot ISO","Console settings","Folder navigation helpers","Search indexes"]{assert!(report.checks.iter().any(|c|c.name==name&&c.status=="error"&&!c.fix.is_empty()),"{name}: {report:?}");}
 assert_eq!(snapshot(root.path()),before);
}
#[test]
fn missing_beside_media_and_stale_save_catalog_are_reported(){
 let root=tempfile::tempdir().unwrap();let cache=tempfile::tempdir().unwrap();prepare(root.path(),cache.path());
 let map=BTreeMap::from([("games/usa/doom/doom.iso".into(),"cover.3dc".into())]);
 fs::write(root.path().join(".ohmy3do/covers/index.3di"),library::beside_index(&library::media_index(&map).unwrap()).unwrap()).unwrap();
 fs::write(root.path().join(".ohmy3do/saves/slot01.3ds"),Archive{entries:vec![Entry{name:b"DoomPrefs".to_vec(),file_type:0,data:vec![1,2,3]}]}.encode().unwrap()).unwrap();
 let report=readiness::check(root.path(),&Job::default()).unwrap();
 for name in ["covers","Save support"]{assert!(report.checks.iter().any(|c|c.name==name&&c.status=="error"),"{report:?}");}
 assert!(report.checks.iter().find(|c|c.name=="covers").unwrap().detail.contains("Games/USA/Doom/.ohmy3do/cover.3dc"));
}
#[test]
fn cancellation_never_changes_the_drive(){let root=tempfile::tempdir().unwrap();let job=Job::default();job.cancel.store(true,std::sync::atomic::Ordering::Relaxed);assert!(readiness::check(root.path(),&job).is_err());assert_eq!(fs::read_dir(root.path()).unwrap().count(),0);}
