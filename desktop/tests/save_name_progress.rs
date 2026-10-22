use oh_my_3do_library::{saves,Job};
use std::{fs,collections::HashMap,sync::{Arc,Mutex,atomic::Ordering}};
#[test]fn scanning_reports_each_disc_and_honors_cancellation(){
 let root=tempfile::tempdir().unwrap();for name in ["A.iso","B.iso"]{fs::write(root.path().join(name),[0;2048]).unwrap();}
 let reports=Arc::new(Mutex::new(vec![]));let received=reports.clone();let job=Job{progress:Arc::new(move|p|received.lock().unwrap().push((p.completed,p.total,p.phase))),..Default::default()};
 saves::save_names_text(root.path(),&["A.iso".into(),"B.iso".into()],&HashMap::new(),&job).unwrap();
 assert_eq!(reports.lock().unwrap().iter().map(|r|(r.0,r.1)).collect::<Vec<_>>(),vec![(0,2),(1,2),(2,2)]);
 let mut job=Job::default();let cancel=job.cancel.clone();job.log=Arc::new(move |_|cancel.store(true,Ordering::Relaxed));
 assert!(saves::save_names_text(root.path(),&["A.iso".into()],&HashMap::new(),&job).err().unwrap().to_string().contains("cancel"));
}
