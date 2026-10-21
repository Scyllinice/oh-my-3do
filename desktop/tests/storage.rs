use oh_my_3do_library::usb::{CopyItem,CopyPlan,estimate_with_space};
use std::collections::BTreeMap;
fn plan()->CopyPlan{CopyPlan{source:"/source".into(),destination:"/dest".into(),moves:vec![],files:vec![item("Games/USA/Game/Game.iso",1000,"unchanged"),item("Games/USA/Game/Track.bin",200,"copy"),item("Games/USA/Game/.ohmy3do/cover.3dc",30,"copy"),item("Games/USA/Game/.ohmy3do/preview.3dv",50,"update"),item("boot.iso",100,"unchanged"),item(".ohmy3do/ohmy3do.ini",20,"preserve")],games:1,bytes:280,selection:None,generated:BTreeMap::new()}}
fn item(path:&str,size:u64,status:&str)->CopyItem{CopyItem{path:path.into(),source_path:None,status:status.into(),size,source_stamp:0,destination_stamp:None,verified:false}}
#[test]
fn accounts_for_multitrack_games_media_and_existing_files_without_double_charging(){let p=plan();let s=estimate_with_space(&p,100_000_000);assert_eq!((s.games,s.covers,s.previews,s.support),(1200,30,50,120));assert_eq!(s.total,1400);assert_eq!(s.to_copy,280);assert!(s.fits);assert_eq!(s.remaining,s.available-s.required);}
#[test]
fn reserve_boundaries_and_insufficient_space_saturate_instead_of_underflowing(){let p=plan();let required=estimate_with_space(&p,0).required;assert!(required>p.bytes);let fits=estimate_with_space(&p,required);assert!(fits.fits);assert_eq!(fits.remaining,0);let short=estimate_with_space(&p,required-1);assert!(!short.fits);assert_eq!((short.shortfall,short.remaining),(1,0));}
#[test]
fn large_copies_reserve_one_percent_in_addition_to_file_bytes(){let mut p=plan();p.files=vec![item("Games/Large.iso",100_000_000_000,"copy")];let s=estimate_with_space(&p,u64::MAX);assert_eq!(s.reserve,1_000_000_000);assert_eq!(s.required,101_000_000_000);}
