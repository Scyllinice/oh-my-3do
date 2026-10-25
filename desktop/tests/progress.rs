use oh_my_3do_library::{fsutil, Job, Progress};
use std::sync::{Arc, Mutex, atomic::{AtomicBool,Ordering}};
fn observed() -> (Job,Arc<Mutex<Vec<Progress>>>) {
    let events=Arc::new(Mutex::new(Vec::new()));let out=events.clone();
    (Job {progress:Arc::new(move |p|out.lock().unwrap().push(p)),..Default::default()},events)
}
#[test]
fn nested_progress_ranges_preserve_fraction_and_share_cancellation() {
    let (job,events)=observed();
    let child=job.progress_range(200,600,1000,"All files");
    child.report(25,100,"Current file");
    let grandchild=child.progress_range(100,100,200,"Readback");
    grandchild.report(50,100,"Readback bytes");
    let p=events.lock().unwrap();assert_eq!(p[0].completed,350);assert_eq!(p[1].completed,650);
    assert!(p.iter().all(|p|p.total==1000&&p.phase=="All files"));drop(p);
    job.cancel.store(true,Ordering::Relaxed);assert!(child.check().is_err());
    let quiet=job.quiet_progress();quiet.report(1,2,"Quiet worker");assert_eq!(events.lock().unwrap().len(),2);assert!(quiet.check().is_err());
}
#[test]
fn copy_reports_incremental_transfer_and_readback_without_changing_bytes() {
    let dir=tempfile::tempdir().unwrap();let source=dir.path().join("source.iso");let target=dir.path().join("target.iso");
    let bytes=vec![37;3*1024*1024+17];std::fs::write(&source,&bytes).unwrap();
    let (job,events)=observed();fsutil::copy_new(&source,&target,&job,None).unwrap();
    assert_eq!(std::fs::read(target).unwrap(),bytes);
    let events=events.lock().unwrap();assert!(events.len()>5);
    let total=bytes.len() as u64*2;assert_eq!(events.first().unwrap().completed,0);assert_eq!(events.last().unwrap().completed,total);
    assert!(events.iter().all(|p|p.total==total&&p.completed<=total));
    assert!(events.windows(2).all(|pair|pair[0].completed<=pair[1].completed));
    assert!(events.iter().any(|p|p.completed>0&&p.completed<bytes.len() as u64));
    assert!(events.iter().any(|p|p.completed>bytes.len() as u64&&p.completed<total));
}
#[test]
fn copy_keeps_the_source_date_and_verified_files_read_back_from_disk() {
    let dir=tempfile::tempdir().unwrap();let source=dir.path().join("source.iso");let target=dir.path().join("target.iso");
    let bytes=vec![91;2*1024*1024+5];std::fs::write(&source,&bytes).unwrap();
    filetime::set_file_mtime(&source,filetime::FileTime::from_unix_time(1_402_444_800,123_456_700)).unwrap();
    fsutil::copy_new(&source,&target,&Job::default(),None).unwrap();
    assert!(fsutil::same_time(fsutil::stamp(&target).unwrap().1,fsutil::stamp(&source).unwrap().1));
    assert_eq!(fsutil::read_from_disk(&target).unwrap(),bytes);
    assert_eq!(fsutil::sha256_from_disk(&target,&Job::default()).unwrap(),fsutil::sha256(&source,&Job::default()).unwrap());
    assert!(fsutil::same_time(10_000_000_000,11_999_999_999));assert!(!fsutil::same_time(10_000_000_000,12_000_000_001));
}
#[test]
fn cancel_during_copy_progress_never_publishes_a_partial_file() {
    let dir=tempfile::tempdir().unwrap();let source=dir.path().join("source.iso");let target=dir.path().join("target.iso");
    std::fs::write(&source,vec![19;3*1024*1024]).unwrap();
    let cancel=Arc::new(AtomicBool::new(false));let stop=cancel.clone();
    let job=Job {cancel,progress:Arc::new(move |p|if p.completed>0 {stop.store(true,Ordering::Relaxed)}),..Default::default()};
    assert!(fsutil::copy_new(&source,&target,&job,None).is_err());assert!(!target.exists());
    assert_eq!(std::fs::read_dir(dir.path()).unwrap().count(),1);
}

#[test]
fn journal_copy_includes_the_initial_hash_without_restarting_progress() {
    let dir=tempfile::tempdir().unwrap();let source=dir.path().join("source.iso");let target=dir.path().join("drive");let cache=dir.path().join("cache");
    std::fs::create_dir_all(&target).unwrap();std::fs::create_dir_all(&cache).unwrap();
    let bytes=vec![71;3*1024*1024+11];std::fs::write(&source,&bytes).unwrap();
    let (job,events)=observed();let mut journal=oh_my_3do_library::library::Journal::new(&target,&cache).unwrap();
    journal.copy(&source,"Game.iso",None,&job).unwrap();journal.finish().unwrap();
    assert_eq!(std::fs::read(target.join("Game.iso")).unwrap(),bytes);
    let events=events.lock().unwrap();let total=bytes.len() as u64*3;
    assert_eq!(events.last().unwrap().completed,total);assert!(events.iter().all(|p|p.total==total));
    assert!(events.windows(2).all(|p|p[0].completed<=p[1].completed));
}
