//! Optional descriptions are loaded only when the console details view opens.
use crate::{fsutil as f,library,Job};
use anyhow::{ensure,Context,Result};
use serde_json::Value;
use std::collections::{BTreeMap,HashSet};
use unicode_normalization::UnicodeNormalization;
pub const MAX:usize=2048;
fn matched_information<'a>(record:&Value,info:&'a std::collections::HashMap<String,Vec<Value>>)->Option<&'a Value>{
 if let Some(id)=record["information_match"]["id"].as_str(){
  return info.values().flatten().find(|m|m["launchbox_id"].as_str()==Some(id));
 }
 let title=record["catalog_name"].as_str().filter(|s|!s.trim().is_empty()).or_else(||record["title"].as_str()).unwrap_or("");
 let values=info.get(&crate::disc::title_key(title))?;
 (values.len()==1).then(||&values[0])
}
fn apply_downloaded(record:&mut Value,meta:&Value)->Result<()>{
 if !record["metadata"].is_object(){record["metadata"]=serde_json::json!({});}
 for(key,value)in meta.as_object().context("Invalid downloaded information")?{
  if record["metadata_overrides"].get(key).is_none()&&!value.is_null()&&value.as_str()!=Some(""){record["metadata"][key]=value.clone();}
 }
 if record["metadata_overrides"].get("description").is_none()&&!description(meta).trim().is_empty(){
  record["metadata"].as_object_mut().unwrap().remove("overview");
  record["metadata"]["description_source"]=meta["source"].clone();
 }
 Ok(())
}
pub fn description(meta:&Value)->&str{meta["description"].as_str().filter(|s|!s.trim().is_empty()).or_else(||meta["overview"].as_str()).unwrap_or("")}
pub fn console_text(text:&str)->String{
 let text:String=text.nfkd().map(|c|match c{'\u{2018}'|'\u{2019}'=>'\'','\u{201c}'|'\u{201d}'=>'"','\u{2013}'|'\u{2014}'=>'-',c if c.is_ascii()&&!c.is_control()=>c,_=>' '}).collect();
 let mut text=text.split_whitespace().collect::<Vec<_>>().join(" ");if text.len()>MAX{let end=text[..MAX-3].rfind(' ').unwrap_or(MAX-3);text.truncate(end);text.push_str("...");}text
}
pub fn filename(path:&str)->String{format!(".ohmy3do/info/{:08x}.txt",crc32fast::hash(f::fold(path).as_bytes()))}
/// A drive keeps each description in the .ohmy3do folder beside its disc, like
/// covers and previews, so the menu reads it without a trip to the drive root.
pub fn drive_filename(path:&str)->String{
 let name=format!("{:08x}.txt",crc32fast::hash(f::fold(path).as_bytes()));
 match path.rsplit_once('/'){Some((home,_))=>format!("{home}/.ohmy3do/{name}"),None=>format!(".ohmy3do/{name}")}
}
/// A description left in a drive's root `.ohmy3do/info/` by an older desktop,
/// or the empty file firmware creates when the menu looked for a missing one.
pub fn old_drive_file(name:&str,file:&std::path::Path)->Result<bool>{
 let Some(stem)=name.strip_suffix(".txt") else {return Ok(false)};
 if stem.len()!=8||!stem.bytes().all(|b|b.is_ascii_digit()||(b'a'..=b'f').contains(&b))||!file.is_file(){return Ok(false);}
 let size=std::fs::metadata(file)?.len();
 Ok(size==0||(size<=16384&&std::fs::read(file)?.starts_with(b"OM3DINFO1\n")))
}
pub fn encode(path:&str,description:&str)->Result<Vec<u8>>{
 ensure!(!path.is_empty()&&path.len()<=8192&&!path.contains(['\n','\r','\0']),"Invalid game information path");
 let body=format!("{path}\n{}\n",console_text(description));
 Ok(format!("OM3DINFO1\n{body}{:08x}\n",crc32fast::hash(body.as_bytes())).into_bytes())
}
pub fn files(db:&Value)->Result<BTreeMap<String,Vec<u8>>>{files_for(db,None)}
/// Generate only the chosen discs, including descriptions already in the database.
pub fn files_for(db:&Value,selected:Option<&HashSet<String>>)->Result<BTreeMap<String,Vec<u8>>>{
 let mut files=BTreeMap::new();
 for r in db["games"].as_array().context("Invalid library")?{
  let path=r["path"].as_str().context("Missing disc path")?;
  if selected.is_some_and(|set|!set.contains(&f::fold(path))){continue;}
  let text=description(&r["metadata"]);if text.trim().is_empty()&&r["metadata_overrides"].get("description").is_none(){continue;}
  let name=filename(path);let bytes=encode(path,text)?;
  if let Some(old)=files.insert(name,bytes.clone()){ensure!(old==bytes,"Game information filename collision");}
 }
 Ok(files)
}
/// Optional metadata must not prevent artwork preparation when offline or unavailable.
/// Existing descriptions and nonempty metadata fields are never replaced here.
pub fn fill_missing(db:&mut Value,prefs:&crate::model::Preferences,cache:&std::path::Path,selected:Option<&HashSet<String>>,job:&Job)->Result<usize>{
 let wanted=|r:&Value|r["path"].as_str().is_some_and(|p|selected.is_none_or(|set|set.contains(&f::fold(p))))&&description(&r["metadata"]).trim().is_empty()&&r["metadata_overrides"].get("description").is_none();
 if !db["games"].as_array().context("Invalid library")?.iter().any(wanted){return Ok(0);}
 job.check()?;job.say("Finding missing game descriptions");
 let info=match crate::catalog::information_candidates(cache,prefs.offline,job){
  Ok(info)=>info,
  Err(error)=>{job.check()?;job.say(format!("Game descriptions unavailable; artwork preparation will continue: {error:#}"));return Ok(0);}
 };
 let mut changed=0;
 for record in db["games"].as_array_mut().context("Invalid library")?{
  job.check()?;if !wanted(record){continue;}
  let Some(meta)=matched_information(record,&info) else {continue;};
  if description(meta).trim().is_empty(){continue;}
  if !record["metadata"].is_object(){record["metadata"]=serde_json::json!({});}
  for (key,value) in meta.as_object().context("Invalid game information")?{
   let old=&record["metadata"][key];
   if record["metadata_overrides"].get(key).is_none()&&(old.is_null()||old.as_str().is_some_and(|s|s.trim().is_empty()))&&!value.is_null()&&value.as_str()!=Some(""){
    record["metadata"][key]=value.clone();
   }
  }
  record["metadata"]["description_source"]=meta["source"].clone();
  changed+=1;
 }
 job.say(format!("Found missing descriptions for {changed} disc entries"));Ok(changed)
}
pub fn refresh(root:&std::path::Path,prefs:&crate::model::Preferences,cache:&std::path::Path,job:&Job)->Result<std::path::PathBuf>{
 let info=crate::catalog::information_candidates(cache,prefs.offline,job)?;
 let _lock=library::lock_library(root,cache)?;let mut db=library::database(root)?;let mut changed=0;
 for record in db["games"].as_array_mut().context("Invalid library")?{
  job.check()?;
  if let Some(meta)=matched_information(record,&info){
   apply_downloaded(record,meta)?;changed+=1;
  }
 }
 let mut tx=library::Journal::new(root,cache)?;
 let result=(||->Result<()>{library::write_database(&mut tx,db,vec![],&[])?;library::build_indexes(root,&mut tx,job)?;job.check()?;tx.finish()})();
 if let Err(error)=result{tx.undo()?;return Err(error)}job.say(format!("Game information refreshed for {changed} disc entries; unmatched titles retained"));Ok(tx.path)
}

pub const EDIT_FIELDS:&[&str]=&["description","genre","developer","publisher","release_date","year","players"];
#[derive(Clone,Debug,serde::Serialize,serde::Deserialize,PartialEq)]
pub struct EditRecord {pub path:String,pub metadata:Value,pub overrides:Value}
#[derive(serde::Serialize)]
pub struct EditView {pub records:Vec<EditRecord>,pub fields:Value}
fn snapshot(db:&Value,discs:&[String])->Result<Vec<EditRecord>>{
 let selected:HashSet<_>=discs.iter().map(|p|f::fold(p)).collect();
 ensure!(!selected.is_empty(),"Choose a game to edit");
 let records=db["games"].as_array().context("Invalid library")?.iter().filter(|r|r["path"].as_str().is_some_and(|p|selected.contains(&f::fold(p)))).map(|r|EditRecord{path:r["path"].as_str().unwrap().into(),metadata:r["metadata"].clone(),overrides:r["metadata_overrides"].clone()}).collect::<Vec<_>>();
 ensure!(records.len()==selected.len(),"Game selection changed; reload the library");Ok(records)
}
pub fn edit_view(root:&std::path::Path,discs:&[String])->Result<EditView>{
 let records=snapshot(&library::database(root)?,discs)?;let meta=&records[0].metadata;let mut fields=serde_json::Map::new();
 for key in EDIT_FIELDS{
  let text=if *key=="description"{description(meta).into()}else if let Some(text)=meta[key].as_str(){text.into()}else if let Some(n)=meta[key].as_u64(){n.to_string()}else{String::new()};
  fields.insert((*key).into(),Value::String(text));
 }
 Ok(EditView{records,fields:Value::Object(fields)})
}
fn validate_patch(patch:&Value)->Result<serde_json::Map<String,Value>>{
 let fields=patch.as_object().context("Invalid game information edits")?;
 let mut out=serde_json::Map::new();
 for (key,value) in fields{
  ensure!(EDIT_FIELDS.contains(&key.as_str()),"Unknown information field: {key}");
  let text=value.as_str().context("Information fields must be text")?.trim();
  let value=if key=="year"||key=="players"{
   if text.is_empty(){Value::Null}else{let number=text.parse::<u64>().context("Year and player count must be whole numbers")?;ensure!((1..=if key=="year"{9999}else{64}).contains(&number),"Year or player count is out of range");serde_json::json!(number)}
  }else{
   let description=key=="description";
   ensure!(text.len()<=if description{64*1024}else{512},"Information field is too long: {key}");
   ensure!(!text.chars().any(|c|c.is_control()&&!(description&&matches!(c,'\r'|'\n'|'\t'))),"Invalid characters in {key}");Value::String(text.into())
  };
  out.insert(key.clone(),value);
 }
 Ok(out)
}
/// Render updated resident metadata without scanning or changing unrelated games.
pub fn console_metadata(root:&std::path::Path,db:&Value)->Result<Vec<u8>>{
 let find=f::safe(root,".ohmy3do/library.3di")?;
 let names=if find.is_file(){crate::usb::find(&find)?.into_iter().collect::<Vec<_>>()}else{
  db["games"].as_array().context("Invalid library")?.iter().filter_map(|r|r["path"].as_str()).filter(|p|f::safe(root,p).is_ok_and(|p|p.is_file())).map(str::to_owned).collect()
 };
 library::metadata_bytes(db,&names)
}
pub fn save_edits(root:&std::path::Path,expected:&[EditRecord],patch:&Value,cache:&std::path::Path,job:&Job)->Result<std::path::PathBuf>{
 save_edits_impl(root,expected,patch,None,cache,job)
}
pub fn save_edits_restoring(root:&std::path::Path,expected:&[EditRecord],patch:&Value,plan:&RestorePlan,fields:&[String],cache:&std::path::Path,job:&Job)->Result<std::path::PathBuf>{
 save_edits_impl(root,expected,patch,Some((plan,fields)),cache,job)
}
fn save_edits_impl(root:&std::path::Path,expected:&[EditRecord],patch:&Value,restore:Option<(&RestorePlan,&[String])>,cache:&std::path::Path,job:&Job)->Result<std::path::PathBuf>{
 let root=f::root(root)?;let patch=validate_patch(patch)?;
 let restored:HashSet<String>=restore.map(|(_,fields)|fields.iter().cloned().collect()).unwrap_or_default();
 ensure!(!patch.is_empty()||!restored.is_empty(),"No information changes to save");
 if let Some((plan,fields))=restore{
  ensure!(plan.root==root&&plan.original==expected,"Restore preview belongs to another game; review it again");
  ensure!(restored.len()==fields.len()&&fields.iter().all(|k|plan.available.contains(k)),"Choose fields supplied in the downloaded preview");
  ensure!(fields.iter().all(|key|!patch.contains_key(key)),"A field cannot be edited and restored at the same time");
 }
 let _lock=library::lock_library(&root,cache)?;
 let mut db=library::database(&root)?;let discs=expected.iter().map(|r|r.path.clone()).collect::<Vec<_>>();
 ensure!(snapshot(&db,&discs)?==expected,"Game information changed; reopen the editor before saving");
 let selected:HashSet<_>=discs.iter().map(|p|f::fold(p)).collect();let mut descriptions=BTreeMap::new();
 for r in db["games"].as_array_mut().context("Invalid library")?{
  job.check()?;let path=r["path"].as_str().context("Missing disc path")?.to_owned();if !selected.contains(&f::fold(&path)){continue;}
  ensure!(f::safe(&root,&path)?.is_file(),"Game file disappeared; reload the library");
  if !r["metadata"].is_object(){r["metadata"]=serde_json::json!({});}
  if !r["metadata_overrides"].is_object(){r["metadata_overrides"]=serde_json::json!({});}
  for (key,value) in &patch{r["metadata"][key]=value.clone();r["metadata_overrides"][key]=value.clone();}
  if patch.contains_key("description"){
   r["metadata"].as_object_mut().unwrap().remove("overview");
   r["metadata"]["description_source"]=serde_json::json!("Edited in Oh My 3DO");
  }
  if let Some((plan,_))=restore{
   let source=plan.records.iter().find(|v|v.path==path).context("Disc missing from restore preview")?;
   for key in &restored{
    r["metadata"][key]=source.fields[key].clone();
    r["metadata_overrides"].as_object_mut().unwrap().remove(key);
   }
   if restored.contains("description"){
    r["metadata"].as_object_mut().unwrap().remove("overview");
    r["metadata"]["description_source"]=Value::String(source.source.clone());
   }
  }
  let name=filename(&path);let bytes=encode(&path,description(&r["metadata"]))?;
  if let Some(old)=descriptions.insert(name,bytes.clone()){ensure!(old==bytes,"Game information filename collision");}
 }
 commit_information(&root,db,descriptions,cache,job)
}
fn commit_information(root:&std::path::Path,db:Value,descriptions:BTreeMap<String,Vec<u8>>,cache:&std::path::Path,job:&Job)->Result<std::path::PathBuf>{
 let count=descriptions.len();let metadata=console_metadata(root,&db)?;let mut tx=library::Journal::new(root,cache)?;
 let result=(||->Result<()>{library::write_database(&mut tx,db,vec![],&[])?;tx.write(".ohmy3do/metadata.3di",&metadata)?;for(name,bytes)in descriptions{job.check()?;tx.write(&name,&bytes)?;}job.check()?;tx.finish()})();
 if let Err(error)=result{tx.undo()?;return Err(error)}job.say(format!("Saved information for {count} disc entries"));Ok(tx.path)
}

#[derive(serde::Serialize)]
pub struct RestoreRecord {pub path:String,pub fields:Value,pub source:String}
#[derive(serde::Serialize)]
pub struct RestorePlan {
 pub root:std::path::PathBuf,
 pub original:Vec<EditRecord>,
 pub records:Vec<RestoreRecord>,
 pub available:Vec<String>,
}
/// A read-only preview retains the exact downloaded values reviewed by the user.
/// Unavailable fields are excluded instead of clearing existing information.
pub fn preview_restore(root:&std::path::Path,expected:&[EditRecord],prefs:&crate::model::Preferences,cache:&std::path::Path,job:&Job)->Result<RestorePlan>{
 let root=f::root(root)?;let db=library::database(&root)?;let discs=expected.iter().map(|r|r.path.clone()).collect::<Vec<_>>();
 ensure!(snapshot(&db,&discs)?==expected,"Game information changed; reopen the editor before restoring");
 let info=crate::catalog::information_candidates(cache,prefs.offline,job)?;let mut records=Vec::new();
 for original in expected{
  job.check()?;
  let record=db["games"].as_array().context("Invalid library")?.iter().find(|r|r["path"]==original.path).context("Game selection changed")?;
  let title=record["catalog_name"].as_str().filter(|s|!s.trim().is_empty()).or_else(||record["title"].as_str()).unwrap_or("");
  let meta=matched_information(record,&info).with_context(||format!("No unambiguous downloaded information for {title}. Existing edits were kept."))?;
  let mut fields=serde_json::Map::new();
  for key in EDIT_FIELDS{
   let text=if *key=="description"{description(meta).into()}else if let Some(text)=meta[key].as_str(){text.into()}else if let Some(n)=meta[key].as_u64(){n.to_string()}else{String::new()};
   if text.trim().is_empty(){continue;}
   if let Ok(mut value)=validate_patch(&serde_json::json!({*key:text})){fields.insert((*key).into(),value.remove(*key).unwrap());}
  }
  records.push(RestoreRecord{path:original.path.clone(),fields:Value::Object(fields),source:meta["source"].as_str().unwrap_or(crate::catalog::LAUNCHBOX).into()});
 }
 let available=EDIT_FIELDS.iter().filter(|key|records.iter().all(|r|r.fields.get(**key).is_some())).map(|s|(*s).to_owned()).collect();
 Ok(RestorePlan{root,original:expected.to_vec(),records,available})
}

#[derive(serde::Serialize,serde::Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CustomEntry {pub path:String,pub title:String,pub region:String,pub disc:Value,pub fields:Value}
#[derive(serde::Serialize,serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct CustomArchive {format:String,version:u32,games:Vec<CustomEntry>}
const TRANSFER_LIMIT:u64=16*1024*1024;
fn field_text(value:&Value)->String{match value{Value::String(s)=>s.clone(),Value::Number(n)=>n.to_string(),_=>String::new()}}
fn override_fields(record:&Value)->Result<Value>{
 let mut fields=serde_json::Map::new();
 for key in EDIT_FIELDS{if let Some(value)=record["metadata_overrides"].get(key){fields.insert((*key).into(),Value::String(field_text(value)));}}
 Ok(Value::Object(validate_patch(&Value::Object(fields))?))
}
/// Portable backups contain manual overrides only, including intentional blanks.
pub fn export_custom(root:&std::path::Path,target:&std::path::Path,discs:&[String],job:&Job)->Result<usize>{
 let root=f::root(root)?;let db=library::database(&root)?;
 let selected:HashSet<_>=discs.iter().map(|p|f::fold(p)).collect();
 if !discs.is_empty(){snapshot(&db,discs)?;}
 let mut games=Vec::new();
 for record in db["games"].as_array().context("Invalid library")?{
  job.check()?;let path=record["path"].as_str().context("Missing disc path")?;
  if !selected.is_empty()&&!selected.contains(&f::fold(path)){continue;}
  let fields=override_fields(record)?;if fields.as_object().unwrap().is_empty(){continue;}
  games.push(CustomEntry{path:path.into(),title:record["title"].as_str().unwrap_or("").into(),region:record["region"].as_str().unwrap_or("").into(),disc:record["disc"].clone(),fields});
 }
 ensure!(!games.is_empty(),"No custom metadata to export in this selection");
 ensure!(target.extension().is_some_and(|s|s.eq_ignore_ascii_case("json")),"Choose a JSON export file");
 ensure!(!f::is_link(target),"Choose a regular export file");
 let parent=f::root(target.parent().context("Choose an export folder")?)?;
 ensure!(!parent.starts_with(root.join(".ohmy3do")),"Keep exports outside the library's internal .ohmy3do folder");
 let count=games.len();let data=serde_json::to_vec_pretty(&CustomArchive{format:"ohmy3do-custom-metadata".into(),version:1,games})?;
 ensure!(data.len() as u64<=TRANSFER_LIMIT,"Metadata export exceeds 16 MB; export a smaller selection");job.check()?;
 f::atomic(&parent.join(target.file_name().context("Choose an export filename")?),&data)?;Ok(count)
}
#[derive(serde::Serialize)]
pub struct ImportRecord {pub path:String,pub source_path:String,pub title:String,pub original:EditRecord,pub fields:Value,pub conflicts:Vec<String>,pub matching:String}
#[derive(serde::Serialize)]
pub struct ImportPlan {pub root:std::path::PathBuf,pub records:Vec<ImportRecord>,pub skipped:Vec<String>}
fn same_identity(a:&Value,b:&CustomEntry)->bool{
 // Keep edition/region designators: title_key intentionally strips too much for imports.
 a["title"].as_str().is_some_and(|t|!t.trim().is_empty()&&t.trim().eq_ignore_ascii_case(b.title.trim()))
 &&a["region"].as_str().unwrap_or("").eq_ignore_ascii_case(&b.region)&&a["disc"]==b.disc
}
pub fn preview_import(root:&std::path::Path,file:&std::path::Path,job:&Job)->Result<ImportPlan>{
 use std::io::Read;
 ensure!(file.is_file()&&!f::is_link(file),"Choose a regular metadata export file");
 let mut bytes=Vec::new();std::fs::File::open(file)?.take(TRANSFER_LIMIT+1).read_to_end(&mut bytes)?;
 ensure!(bytes.len() as u64<=TRANSFER_LIMIT,"Metadata import exceeds 16 MB");
 let archive:CustomArchive=serde_json::from_slice(&bytes).context("Invalid custom metadata export")?;
 ensure!(archive.format=="ohmy3do-custom-metadata"&&archive.version==1,"Unsupported metadata export format or version");
 ensure!(archive.games.len()<=50000,"Too many metadata entries");
 let root=f::root(root)?;let db=library::database(&root)?;let games=db["games"].as_array().context("Invalid library")?;
 let mut records=Vec::new();let mut skipped=Vec::new();let mut paths=HashSet::new();let mut targets=HashSet::new();
 for entry in archive.games{
  job.check()?;ensure!(!entry.path.is_empty()&&entry.path.len()<=8192&&paths.insert(f::fold(&entry.path)),"Invalid or repeated metadata source path");
  let raw=entry.fields.as_object().context("Invalid custom metadata fields")?;let mut text=serde_json::Map::new();
  for (key,value) in raw{
   ensure!(value.is_string()||((key=="year"||key=="players")&&(value.is_null()||value.is_u64())),"Invalid value for {key}");
   text.insert(key.clone(),Value::String(field_text(value)));
  }
  let fields=validate_patch(&Value::Object(text))?;
  if fields.is_empty(){skipped.push(format!("{}: no custom fields",entry.path));continue;}
  let exact=games.iter().filter(|r|r["path"].as_str().is_some_and(|p|p.eq_ignore_ascii_case(&entry.path))).collect::<Vec<_>>();
  let candidates=if exact.is_empty(){games.iter().filter(|r|same_identity(r,&entry)).collect::<Vec<_>>()}else{exact};
  if candidates.len()!=1{skipped.push(format!("{}: {}",entry.path,if candidates.is_empty(){"no matching disc"}else{"ambiguous match"}));continue;}
  let r=candidates[0];
  // A reused relative path must never silently receive a different game's edits.
  if !same_identity(r,&entry){skipped.push(format!("{}: title, region or disc differs",entry.path));continue;}
  let path=r["path"].as_str().context("Missing disc path")?;
  ensure!(targets.insert(f::fold(path)),"Multiple exported entries match the same disc; import canceled");
  let fields=fields.into_iter().filter(|(key,value)|r["metadata_overrides"].get(key)!=Some(value)||r["metadata"].get(key)!=Some(value)).collect::<serde_json::Map<_,_>>();
  if fields.is_empty(){skipped.push(format!("{}: already matches",entry.path));continue;}
  let conflicts=fields.iter().filter(|(key,value)|r["metadata_overrides"].get(*key).is_some_and(|v|v!=*value)).map(|(key,_)|key.clone()).collect();
  let original=EditRecord{path:path.into(),metadata:r["metadata"].clone(),overrides:r["metadata_overrides"].clone()};
  records.push(ImportRecord{path:path.into(),source_path:entry.path.clone(),title:entry.title,original,fields:Value::Object(fields),conflicts,matching:if path.eq_ignore_ascii_case(&entry.path){"Relative path"}else{"Unique title, region and disc"}.into()});
 }
 Ok(ImportPlan{root,records,skipped})
}
pub fn import_custom(root:&std::path::Path,plan:&ImportPlan,selected:&[String],cache:&std::path::Path,job:&Job)->Result<std::path::PathBuf>{
 let root=f::root(root)?;ensure!(root==plan.root,"Library changed; review the import again");
 let chosen:HashSet<_>=selected.iter().map(|p|f::fold(p)).collect();
 ensure!(!chosen.is_empty()&&chosen.len()==selected.len(),"Choose distinct entries to import");
 let rows=plan.records.iter().filter(|r|chosen.contains(&f::fold(&r.path))).collect::<Vec<_>>();
 ensure!(rows.len()==chosen.len(),"Import selection changed; review it again");
 let _lock=library::lock_library(&root,cache)?;let mut db=library::database(&root)?;
 let originals=rows.iter().map(|r|(f::fold(&r.path),&r.original)).collect::<BTreeMap<_,_>>();
 ensure!(snapshot(&db,selected)?.iter().all(|r|originals.get(&f::fold(&r.path))==Some(&r)),"Metadata changed; review the import again");
 let patches=rows.into_iter().map(|r|(f::fold(&r.path),&r.fields)).collect::<BTreeMap<_,_>>();let mut descriptions=BTreeMap::new();
 for r in db["games"].as_array_mut().context("Invalid library")?{
  job.check()?;let path=r["path"].as_str().context("Missing disc path")?.to_owned();let Some(fields)=patches.get(&f::fold(&path))else{continue;};
  ensure!(f::safe(&root,&path)?.is_file(),"Game file disappeared; review the import again");
  if !r["metadata"].is_object(){r["metadata"]=serde_json::json!({});}if !r["metadata_overrides"].is_object(){r["metadata_overrides"]=serde_json::json!({});}
  for(key,value)in fields.as_object().context("Invalid import fields")?{r["metadata"][key]=value.clone();r["metadata_overrides"][key]=value.clone();}
  if fields.get("description").is_some(){r["metadata"].as_object_mut().unwrap().remove("overview");r["metadata"]["description_source"]=Value::String("Edited in Oh My 3DO".into());}
  let name=filename(&path);let bytes=encode(&path,description(&r["metadata"]))?;
  if let Some(old)=descriptions.insert(name,bytes.clone()){ensure!(old==bytes,"Game information filename collision");}
 }
 commit_information(&root,db,descriptions,cache,job)
}

#[derive(serde::Serialize)]
pub struct MatchReview {
 pub root:std::path::PathBuf,pub records:Vec<Value>,pub choices:Vec<Value>,pub status:BTreeMap<String,String>,
}
pub fn review_matches(root:&std::path::Path,prefs:&crate::model::Preferences,cache:&std::path::Path,job:&Job)->Result<MatchReview>{
 let root=f::root(root)?;let db=library::database(&root)?;let info=crate::catalog::information_candidates(cache,prefs.offline,job)?;
 let mut choices=BTreeMap::new();
 for value in info.values().flatten(){
  if let Some(id)=value["launchbox_id"].as_str().filter(|id|!id.is_empty()){
   if !value["title"].as_str().unwrap_or("").trim().is_empty(){choices.entry(id.to_owned()).or_insert_with(||value.clone());}
  }
 }
 let records=db["games"].as_array().context("Invalid library")?.clone();let mut status=BTreeMap::new();
 for r in &records{
  job.check()?;let path=r["path"].as_str().context("Missing disc path")?;
  let matched=matched_information(r,&info);
  let reason=if matched.is_some(){if r["information_match"]["id"].is_string(){"Chosen match"}else{"Automatic match"}}
   else if r["information_match"]["id"].is_string(){"Chosen entry unavailable in cached data"}
   else{let title=r["catalog_name"].as_str().filter(|s|!s.is_empty()).or_else(||r["title"].as_str()).unwrap_or("");if info.get(&crate::disc::title_key(title)).is_some_and(|v|v.len()>1){"Ambiguous title"}else{"No matching title"}};
  status.insert(path.into(),reason.into());
 }
 let mut choices=choices.into_values().collect::<Vec<_>>();choices.sort_by(|a,b|a["title"].as_str().cmp(&b["title"].as_str()).then_with(||a["launchbox_id"].as_str().cmp(&b["launchbox_id"].as_str())));
 Ok(MatchReview{root,records,choices,status})
}
pub fn choose_match(root:&std::path::Path,review:&MatchReview,discs:&[String],id:&str,cache:&std::path::Path,job:&Job)->Result<std::path::PathBuf>{
 let root=f::root(root)?;ensure!(root==review.root,"Library changed; review information matches again");
 let meta=review.choices.iter().find(|m|m["launchbox_id"].as_str()==Some(id)).context("Choose a LaunchBox entry from the review")?;
 let selected:HashSet<_>=discs.iter().map(|p|f::fold(p)).collect();ensure!(!selected.is_empty()&&selected.len()==discs.len(),"Choose distinct disc entries");
 let originals=review.records.iter().filter_map(|r|r["path"].as_str().filter(|p|selected.contains(&f::fold(p))).map(|p|(f::fold(p),r))).collect::<BTreeMap<_,_>>();
 ensure!(originals.len()==selected.len(),"Game selection changed; review it again");
 let _lock=library::lock_library(&root,cache)?;let mut db=library::database(&root)?;let mut descriptions=BTreeMap::new();let mut count=0;
 for r in db["games"].as_array_mut().context("Invalid library")?{
  job.check()?;let path=r["path"].as_str().context("Missing disc path")?.to_owned();let Some(old)=originals.get(&f::fold(&path))else{continue;};
  ensure!(*old==&*r,"Game information changed; review the match again");ensure!(f::safe(&root,&path)?.is_file(),"Game file disappeared; reload the library");
  apply_downloaded(r,meta)?;r["information_match"]=serde_json::json!({"id":id,"title":meta["title"]});
  let name=filename(&path);let bytes=encode(&path,description(&r["metadata"]))?;
  if let Some(old)=descriptions.insert(name,bytes.clone()){ensure!(old==bytes,"Game information filename collision");}count+=1;
 }
 ensure!(count==selected.len(),"Game selection changed; review it again");
 commit_information(&root,db,descriptions,cache,job)
}

fn backup_directory(root:&std::path::Path,cache:&std::path::Path)->Result<std::path::PathBuf>{
 use sha2::{Digest,Sha256};
 let root=f::root(root)?;let key=format!("{:x}",Sha256::digest(root.to_string_lossy().as_bytes()));
 Ok(cache.join("custom-metadata-backups").join(key))
}
fn backup_name(name:&str)->bool{
 let Some(rest)=name.strip_prefix("metadata-").and_then(|s|s.strip_suffix(".json"))else{return false;};
 rest.len()==57&&rest.as_bytes()[20]==b'-'&&rest[..20].bytes().all(|b|b.is_ascii_digit())&&uuid::Uuid::parse_str(&rest[21..]).is_ok()
}
fn backup_paths(directory:&std::path::Path)->Result<Vec<std::path::PathBuf>>{
 let mut files=Vec::new();if !directory.exists(){return Ok(files);}
 ensure!(!f::is_link(directory),"Metadata backup folder must be a regular folder");
 for entry in std::fs::read_dir(directory)?{let entry=entry?;if entry.file_type()?.is_file()&&backup_name(&entry.file_name().to_string_lossy()){files.push(entry.path());}}
 files.sort();files.reverse();Ok(files)
}
/// Independent of Undo journals and stored on the desktop, never on the console drive.
pub fn backup_custom(root:&std::path::Path,cache:&std::path::Path)->Result<()>{
 if !f::safe(root,".ohmy3do/library.json")?.is_file(){return Ok(());}
 let db=library::database(root)?;let mut games=Vec::new();
 for r in db["games"].as_array().context("Invalid library")?{
  let fields=override_fields(r)?;if fields.as_object().unwrap().is_empty(){continue;}
  games.push(CustomEntry{path:r["path"].as_str().context("Missing disc path")?.into(),title:r["title"].as_str().unwrap_or("").into(),region:r["region"].as_str().unwrap_or("").into(),disc:r["disc"].clone(),fields});
 }
 if games.is_empty(){return Ok(());}
 games.sort_by(|a,b|a.path.cmp(&b.path));
 let data=serde_json::to_vec_pretty(&CustomArchive{format:"ohmy3do-custom-metadata".into(),version:1,games})?;
 ensure!(data.len() as u64<=TRANSFER_LIMIT,"Custom metadata backup exceeds 16 MB; export smaller selections before continuing");
 let directory=backup_directory(root,cache)?;let files=backup_paths(&directory)?;
 let mut duplicate=None;
 for (index,path) in files.iter().enumerate(){
  if std::fs::metadata(path)?.len()==data.len() as u64&&std::fs::read(path)?==data{
   if index==0{return Ok(());}duplicate=Some(path);break;
  }
 }
 let stamp=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)?.as_millis();
 let name=format!("metadata-{stamp:020}-{}.json",uuid::Uuid::new_v4());
 f::atomic(&directory.join(name),&data)?;
 if let Some(old)=duplicate{std::fs::remove_file(old)?;}
 // Delete only our oldest recognized snapshots, after the new copy is durable.
 for old in backup_paths(&directory)?.into_iter().skip(5){std::fs::remove_file(old)?;}
 Ok(())
}
#[derive(serde::Serialize)]
pub struct MetadataBackup {pub name:String,pub modified:u64,pub bytes:u64,pub entries:usize}
#[derive(serde::Serialize)]
pub struct MetadataBackups {pub directory:std::path::PathBuf,pub backups:Vec<MetadataBackup>}
pub fn backups(root:&std::path::Path,cache:&std::path::Path)->Result<MetadataBackups>{
 let directory=backup_directory(root,cache)?;let mut backups=Vec::new();
 for path in backup_paths(&directory)?{
  let metadata=std::fs::metadata(&path)?;ensure!(metadata.len()<=TRANSFER_LIMIT,"Oversized metadata backup");
  let archive:CustomArchive=serde_json::from_slice(&std::fs::read(&path)?)?;
  backups.push(MetadataBackup{name:path.file_name().unwrap().to_string_lossy().into(),modified:metadata.modified()?.duration_since(std::time::UNIX_EPOCH)?.as_secs(),bytes:metadata.len(),entries:archive.games.len()});
 }
 Ok(MetadataBackups{directory,backups})
}
pub fn preview_backup(root:&std::path::Path,cache:&std::path::Path,name:&str,job:&Job)->Result<ImportPlan>{
 ensure!(backup_name(name),"Choose a metadata snapshot from this library");
 preview_import(root,&backup_directory(root,cache)?.join(name),job)
}
