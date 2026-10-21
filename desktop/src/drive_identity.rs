//! A persistent identity belongs to a prepared drive, never to its mount letter.
use crate::{fsutil as f,model::Preferences};
use anyhow::{ensure,Context,Result};
use serde::{Serialize,Deserialize};
use std::{path::Path,fs};
pub const MARKER:&str=".ohmy3do/drive.json";
#[derive(Clone,Debug,Serialize,Deserialize,PartialEq,Eq)]
pub struct Identity {pub id:String,pub name:String}
#[derive(Serialize)]
pub struct Inspection {pub root:String,pub identity:Option<Identity>,pub expected:Option<Identity>,pub mismatch:bool,pub token:String}
pub fn inspect(root:&Path,prefs:&Preferences)->Result<Inspection>{
 let root=f::root(root)?;let key=root.to_string_lossy().into_owned();let path=f::safe(&root,MARKER)?;
 let identity=if path.exists(){ensure!(fs::metadata(&path)?.len()<=1024,"Oversized drive identity");let value:Identity=serde_json::from_slice(&fs::read(&path)?)?;validate(&value)?;Some(value)}else{None};
 let expected=prefs.drives.get(&key).cloned();
 let mismatch=expected.as_ref().is_some_and(|old|identity.as_ref().is_none_or(|now|now.id!=old.id));
 let token=identity.as_ref().map(|v|v.id.clone()).unwrap_or_else(||"unnamed".into());
 Ok(Inspection{root:key,identity,expected,mismatch,token})
}
fn validate(value:&Identity)->Result<()>{ensure!(uuid::Uuid::parse_str(&value.id).is_ok(),"Invalid drive ID");ensure!(!value.name.trim().is_empty()&&value.name.chars().count()<=64&&!value.name.chars().any(char::is_control),"Use a drive name of 1–64 characters");Ok(())}
pub fn remember(root:&Path,name:&str,prefs:&mut Preferences,expected:&str)->Result<Identity>{
 let view=inspect(root,prefs)?;ensure!(view.token==expected,"Drive changed; inspect it again");
 let value=Identity{id:view.identity.map(|i|i.id).unwrap_or_else(||uuid::Uuid::new_v4().to_string()),name:name.trim().into()};validate(&value)?;
 let path=f::safe(&f::root(root)?,MARKER)?;fs::create_dir_all(path.parent().context("Missing support directory")?)?;f::atomic(&path,&serde_json::to_vec_pretty(&value)?)?;
 prefs.drives.insert(view.root,value.clone());Ok(value)
}
pub fn check(root:&Path,prefs:&Preferences,token:&str)->Result<()>{let view=inspect(root,prefs)?;ensure!(view.token==token,"Destination drive changed since review. Review it again before writing.");ensure!(!view.mismatch,"A different drive is mounted here. Check and remember this drive in Drive setup first.");Ok(())}
