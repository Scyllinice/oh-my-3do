use oh_my_3do_library::{drive_identity as drive,game_info,slot_notes,save_catalog,save_archive::{Archive,Entry},library,model::Preferences,Job};
use serde_json::json;
use std::{fs,path::Path};
fn put(root:&Path,name:&str,bytes:&[u8]){let p=root.join(name);fs::create_dir_all(p.parent().unwrap()).unwrap();fs::write(p,bytes).unwrap();}
#[test]fn named_drive_detects_swaps_and_retains_id_across_rename(){
 let t=tempfile::tempdir().unwrap();let mut prefs=Preferences::default();
 let first=drive::inspect(t.path(),&prefs).unwrap();assert_eq!(first.token,"unnamed");
 let named=drive::remember(t.path(),"Console SD",&mut prefs,&first.token).unwrap();
 assert!(drive::check(t.path(),&prefs,"unnamed").is_err());drive::check(t.path(),&prefs,&named.id).unwrap();
 let renamed=drive::remember(t.path(),"My 3DO",&mut prefs,&named.id).unwrap();assert_eq!(renamed.id,named.id);
 fs::remove_file(t.path().join(drive::MARKER)).unwrap();assert!(drive::inspect(t.path(),&prefs).unwrap().mismatch);assert!(drive::check(t.path(),&prefs,"unnamed").is_err());
 assert!(drive::remember(t.path(),"wrong",&mut prefs,&named.id).is_err());
 let replacement=drive::remember(t.path(),"Replacement",&mut prefs,"unnamed").unwrap();assert_ne!(replacement.id,named.id);
 drive::check(t.path(),&prefs,&replacement.id).unwrap();
 let b=tempfile::tempdir().unwrap();put(b.path(),drive::MARKER,&fs::read(t.path().join(drive::MARKER)).unwrap());assert_eq!(drive::inspect(b.path(),&prefs).unwrap().identity.unwrap().id,replacement.id);
}
#[test]fn notes_are_searchable_conflict_checked_and_do_not_modify_archive(){
 let t=tempfile::tempdir().unwrap();let path=t.path().join("slot01.3ds");
 let archive=Archive{entries:vec![Entry{name:b"DoomPrefs".to_vec(),file_type:0,data:vec![1,2,3]}]}.encode().unwrap();fs::write(&path,&archive).unwrap();
 assert_eq!(slot_notes::read(&path).unwrap(),None);slot_notes::save(&path,"Before final boss\nWeapons unlocked",None).unwrap();
 assert_eq!(fs::read(&path).unwrap(),archive);assert!(save_catalog::scan(t.path(),&Job::default()).unwrap()[0].matches("boss unlocked"));
 assert!(slot_notes::save(&path,"stale",None).is_err());slot_notes::save(&path,"Original",Some("Before final boss\nWeapons unlocked")).unwrap();
 assert_eq!(fs::read(&path).unwrap(),archive);assert!(slot_notes::save(&path,&"x".repeat(4097),Some("Original")).is_err());assert!(slot_notes::path(&t.path().join("recovery.3ds")).is_err());
}
#[test]fn descriptions_are_bounded_checksummed_and_include_full_path(){
 let path="Games/USA/Example/disc.iso";let encoded=game_info::encode(path,"Café — an adventure.\nPlay & explore!").unwrap();
 assert_eq!(game_info::console_text("Café — an adventure.\nPlay & explore!"),"Cafe - an adventure. Play & explore!");
 assert_eq!(encoded,include_bytes!("../../tests/fixtures/game-info.txt"));
 assert!(encoded.starts_with(format!("OM3DINFO1\n{path}\n").as_bytes()));assert_eq!(game_info::filename(path),game_info::filename(&path.to_uppercase()));
 assert!(game_info::console_text(&"word ".repeat(1000)).len()<=2048);
 let bytes=game_info::files(&json!({"games":[{"path":path,"metadata":{"description":"Test"}}]})).unwrap();assert_eq!(bytes[&game_info::filename(path)],game_info::encode(path,"Test").unwrap());
}
#[test]fn selected_media_does_not_touch_unselected_missing_discs_and_is_undoable(){
 let t=tempfile::tempdir().unwrap();let root=t.path().join("library");let cache=t.path().join("cache");let source=t.path().join("art");fs::create_dir_all(source.join("Box")).unwrap();
 let disc="Games/USA/Example/disc.iso";put(&root,disc,b"fixture");
 let other="Games/USA/Other/missing.iso";
 put(&root,".ohmy3do/library.json",&serde_json::to_vec(&json!({"schema":1,"sources":[],"games":[{"path":disc,"title":"Example (USA)","metadata":{},"category":"Games","region":"USA"},{"path":other,"title":"Other","metadata":{}}]})).unwrap());
 image::RgbImage::from_pixel(96,120,image::Rgb([50,100,20])).save(source.join("Box/Example (USA).png")).unwrap();
 let before=fs::read(root.join(".ohmy3do/library.json")).unwrap();let prefs=Preferences{emumovies:source.to_string_lossy().into_owned(),offline:true,..Default::default()};
 let journal=library::update_selected_media(&root,&prefs,&cache,&[disc.into()],&Job::default()).unwrap();let db=library::database(&root).unwrap();assert!(db["games"].as_array().unwrap().iter().find(|r|r["path"]==disc).unwrap()["cover"].is_string());assert!(db["games"].as_array().unwrap().iter().find(|r|r["path"]==other).unwrap()["cover"].is_null());
 library::undo(&journal).unwrap();assert_eq!(fs::read(root.join(".ohmy3do/library.json")).unwrap(),before);
 assert!(library::update_selected_media(&root,&prefs,&cache,&[],&Job::default()).is_err());
}
#[test]fn missing_media_prepares_descriptions_offline_and_transfers_selected_without_changing_drive_identity(){
 use std::io::Write;
 use oh_my_3do_library::{usb,console_config};
 let t=tempfile::tempdir().unwrap();let root=t.path().join("library");let dest=t.path().join("usb");let cache=t.path().join("cache");fs::create_dir_all(&cache).unwrap();fs::create_dir_all(&dest).unwrap();
 let path="Games/USA/Example/disc.iso";put(&root,path,b"fixture");put(&root,"Games/USA/Other/disc.iso",b"other");
 let mut boot=vec![0;2048];boot[..7].copy_from_slice(&[1,90,90,90,90,90,1]);boot[76..80].copy_from_slice(&2048u32.to_be_bytes());boot[80..84].copy_from_slice(&1u32.to_be_bytes());put(&root,"boot.iso",&boot);put(&root,".ohmy3do/ohmy3do.ini",&console_config::Settings::default().encode().unwrap());
 put(&root,".ohmy3do/library.json",&serde_json::to_vec(&json!({"schema":1,"sources":[],"games":[{"path":path,"title":"Example (USA)","metadata":{}},{"path":"Games/USA/Other/disc.iso","title":"Other","metadata":{"description":"Keep me"}}]})).unwrap());
 let file=fs::File::create(cache.join("launchbox.zip")).unwrap();let mut zip=zip::ZipWriter::new(file);zip.start_file("Metadata.xml",zip::write::SimpleFileOptions::default()).unwrap();zip.write_all(b"<LaunchBox><Game><Name>Example</Name><Platform>3DO</Platform><Overview>Explore &amp; discover.</Overview><Publisher>Example Publisher</Publisher></Game></LaunchBox>").unwrap();zip.finish().unwrap();
 let prefs=Preferences{offline:true,..Default::default()};let mut tx=library::Journal::new(&root,&cache).unwrap();library::build_indexes(&root,&mut tx,&Job::default()).unwrap();tx.finish().unwrap();library::update_selected_media(&root,&prefs,&cache,&[path.into()],&Job::default()).unwrap();
 let db=library::database(&root).unwrap();assert_eq!(db["games"][0]["metadata"]["description"],"Explore & discover.");
 let mut remembered=Preferences::default();drive::remember(&dest,"Console SD",&mut remembered,"unnamed").unwrap();let marker=fs::read(dest.join(drive::MARKER)).unwrap();
 let plan=usb::plan_selected(&root,&dest,&cache,false,&[path.into()],&Job::default()).unwrap();assert!(plan.files.iter().any(|f|f.path==game_info::filename(path)));assert!(!plan.files.iter().any(|f|f.path==game_info::filename("Games/USA/Other/disc.iso")));
 usb::apply(&plan,&cache,&Job::default()).unwrap();assert_eq!(fs::read(dest.join(game_info::filename(path))).unwrap(),game_info::encode(path,"Explore & discover.").unwrap());assert_eq!(fs::read(dest.join(drive::MARKER)).unwrap(),marker);
}
#[test]fn missing_media_repairs_existing_descriptions_without_downloads_or_changing_other_games(){
 let t=tempfile::tempdir().unwrap();let root=t.path().join("library");let cache=t.path().join("cache");
 let path="Games/USA/Example/disc.iso";put(&root,path,b"fixture");let other="Games/USA/Other/disc.iso";put(&root,other,b"fixture");
 put(&root,".ohmy3do/library.json",&serde_json::to_vec(&json!({"schema":1,"sources":[],"games":[{"path":path,"title":"Example","metadata":{"description":"My description","genre":"Custom"}},{"path":other,"title":"Other","metadata":{}}]})).unwrap());
 put(&cache,"launchbox.zip",b"Deliberately invalid: existing selected descriptions must not load this");
 let before=fs::read(root.join(".ohmy3do/library.json")).unwrap();let prefs=Preferences{offline:true,..Default::default()};
 let journal=library::update_selected_media(&root,&prefs,&cache,&[path.into()],&Job::default()).unwrap();
 let file=root.join(game_info::filename(path));assert_eq!(fs::read(&file).unwrap(),game_info::encode(path,"My description").unwrap());assert_eq!(fs::read(root.join(".ohmy3do/library.json")).unwrap(),before);
 assert!(!root.join(game_info::filename(other)).exists());library::undo(&journal).unwrap();assert!(!file.exists());
 put(&root,&game_info::filename(path),b"broken");library::update_selected_media(&root,&prefs,&cache,&[path.into()],&Job::default()).unwrap();assert_eq!(fs::read(file).unwrap(),game_info::encode(path,"My description").unwrap());
}
#[test]fn information_editor_preserves_disc_fields_supports_undo_conflicts_and_protects_cleared_fields(){
 use std::io::Write;
 let t=tempfile::tempdir().unwrap();let root=t.path().join("library");let cache=t.path().join("cache");
 let first="Games/USA/Example/Disc 1.iso";let second="Games/USA/Example/Disc 2.iso";let other="Games/USA/Other/disc.iso";
 for path in [first,second,other]{put(&root,path,b"fixture");}
 put(&root,".ohmy3do/library.json",&serde_json::to_vec(&json!({"schema":1,"sources":[],"games":[{"path":first,"title":"Example (Disc 1)","cover":"keep.png","metadata":{"description":"Before","genre":"Old","year":1990}},{"path":second,"title":"Example (Disc 2)","metadata":{"description":"Before","genre":"Other disc genre","publisher":"Disc publisher"}},{"path":other,"title":"Other","metadata":{}}]})).unwrap());
 let mut tx=library::Journal::new(&root,&cache).unwrap();library::build_indexes(&root,&mut tx,&Job::default()).unwrap();tx.finish().unwrap();
 let original=fs::read(root.join(".ohmy3do/library.json")).unwrap();let discs=[first.into(),second.into()];let edit=game_info::edit_view(&root,&discs).unwrap();
 let patch=json!({"description":"My adventure notes","genre":"Custom","year":"2001","players":"2"});
 let journal=game_info::save_edits(&root,&edit.records,&patch,&cache,&Job::default()).unwrap();
 let db=library::database(&root).unwrap();assert_eq!(db["games"][0]["cover"],"keep.png");assert_eq!(db["games"][1]["metadata"]["publisher"],"Disc publisher");assert_eq!(db["games"][2]["metadata"],json!({}));
 for record in &db["games"].as_array().unwrap()[..2]{assert_eq!(record["metadata"]["description"],"My adventure notes");assert_eq!(record["metadata"]["year"],2001);assert_eq!(record["metadata"]["players"],2);assert_eq!(fs::read(root.join(game_info::filename(record["path"].as_str().unwrap()))).unwrap(),game_info::encode(record["path"].as_str().unwrap(),"My adventure notes").unwrap());}
 assert!(game_info::save_edits(&root,&edit.records,&patch,&cache,&Job::default()).is_err());
 library::undo(&journal).unwrap();assert_eq!(fs::read(root.join(".ohmy3do/library.json")).unwrap(),original);
 game_info::save_edits(&root,&edit.records,&patch,&cache,&Job::default()).unwrap();
 let file=fs::File::create(cache.join("launchbox.zip")).unwrap();let mut zip=zip::ZipWriter::new(file);zip.start_file("Metadata.xml",zip::write::SimpleFileOptions::default()).unwrap();zip.write_all(b"<LaunchBox><Game><Name>Example</Name><Platform>3DO</Platform><Overview>Downloaded description</Overview><Genres>Downloaded genre</Genres><Publisher>Downloaded publisher</Publisher><ReleaseYear>1994</ReleaseYear></Game></LaunchBox>").unwrap();zip.finish().unwrap();
 let prefs=Preferences{offline:true,..Default::default()};game_info::refresh(&root,&prefs,&cache,&Job::default()).unwrap();
 let current=game_info::edit_view(&root,&discs).unwrap();assert_eq!(current.fields["description"],"My adventure notes");assert_eq!(current.fields["genre"],"Custom");assert_eq!(current.fields["year"],"2001");assert_eq!(current.fields["publisher"],"Downloaded publisher");
 for bad in [json!({"year":"abc"}),json!({"year":"10000"}),json!({"players":"0"}),json!({"source":"forged"}),json!({"description":"bad\u{0000}text"})]{assert!(game_info::save_edits(&root,&current.records,&bad,&cache,&Job::default()).is_err());}
 game_info::save_edits(&root,&current.records,&json!({"description":"","genre":"","year":""}),&cache,&Job::default()).unwrap();game_info::refresh(&root,&prefs,&cache,&Job::default()).unwrap();
 let cleared=game_info::edit_view(&root,&discs).unwrap();assert_eq!(cleared.fields["description"],"");assert_eq!(cleared.fields["genre"],"");assert_eq!(cleared.fields["year"],"");
 fs::remove_file(root.join(game_info::filename(first))).unwrap();library::update_selected_media(&root,&prefs,&cache,&discs,&Job::default()).unwrap();assert_eq!(fs::read(root.join(game_info::filename(first))).unwrap(),game_info::encode(first,"").unwrap());
}
