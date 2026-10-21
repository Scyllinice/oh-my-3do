//! Read-only inspection of a prepared USB/SD drive. Never repairs or rewrites it.
use crate::{categories, console_config, disc, fsutil as f, library, media, save_catalog, save_cheats, setup, usb, Job};
use anyhow::{ensure, Context, Result};
use serde::Serialize;
use std::{collections::{BTreeMap, BTreeSet}, fs, io::Read, path::{Path, PathBuf}};
#[derive(Debug, Serialize)]
pub struct Check { pub name: String, pub status: String, pub detail: String, pub fix: String }
#[derive(Debug, Serialize)]
pub struct Report { pub root: PathBuf, pub ready: bool, pub errors: usize, pub warnings: usize, pub checks: Vec<Check> }
impl Report {
    fn add(&mut self, name: &str, status: &str, detail: String, fix: &str) {
        self.errors += usize::from(status == "error");
        self.warnings += usize::from(status == "warning");
        self.checks.push(Check {name:name.into(),status:status.into(),detail,fix:if status == "ok" {String::new()} else {fix.into()}});
    }
    fn result(&mut self, name: &str, result: Result<String>, fix: &str) {
        match result { Ok(detail) => self.add(name,"ok",detail,fix), Err(e) => self.add(name,"error",format!("{e:#}"),fix) }
    }
}
fn read(root: &Path, name: &str, limit: usize) -> Result<Vec<u8>> {
    let path = f::safe(root,name)?;
    let mut data = Vec::new();
    fs::File::open(&path).with_context(|| format!("Missing or unreadable: {name}"))?.take(limit as u64 + 1).read_to_end(&mut data)?;
    ensure!(data.len() <= limit,"Oversized file: {name}");
    Ok(data)
}
pub fn check(root: &Path, job: &Job) -> Result<Report> {
    let root = f::root(root)?;
    let mut report = Report {root:root.clone(),ready:false,errors:0,warnings:0,checks:vec![]};
    job.check()?; job.say("Checking menu and drive settings");
    report.result("Boot ISO", (|| {
        let data = read(&root,"boot.iso",16*1024*1024)?;
        ensure!(data.len() >= 84 && data[..7] == [1,90,90,90,90,90,1],"Invalid 3DO volume header");
        let word = |at| u32::from_be_bytes(data[at..at+4].try_into().unwrap()) as u64;
        ensure!(word(76)==2048 && word(80)*2048==data.len() as u64,"Boot ISO size does not match its volume header");
        Ok("3DO volume header and image size are valid.".into())
    })(), "Install a menu image from Drive setup.");
    let settings = console_config::load(&root);
    report.result("Console settings", (|| {
        let loaded = settings.as_ref().map_err(|e| anyhow::anyhow!("{e:#}"))?;
        ensure!(loaded.exists(),"ohmy3do.ini is missing");
        Ok("Console settings are valid.".into())
    })(), "Repair installed support or save console settings.");
    report.result("Categories", (|| {
        read(&root,categories::PATH,16*1024)?;
        let config = categories::load(&root)?;
        Ok(format!("{} configured categories.",config.categories.len()))
    })(), "Repair installed support or open Manage categories.");
    job.check()?; job.say("Checking game paths and search indexes");
    let discs = disc::collect(&[root.clone()],None,job);
    let mut games = BTreeMap::new();
    report.result("Game files", (|| {
        let sets = discs.as_ref().map_err(|e| anyhow::anyhow!("{e:#}"))?;
        for (disc, files) in sets {
            for file in files {
                job.check()?;
                ensure!(fs::metadata(file)?.len() < 4*1024u64.pow(3),"File exceeds FAT32 limit: {}",file.display());
                disc::issue(file)?;
            }
            let name = library::relative(&root,disc)?;
            ensure!(games.insert(f::fold(&name),name).is_none(),"Case-colliding disc paths");
        }
        Ok(format!("{} disc entries; referenced tracks exist. Game contents were not checksummed.",games.len()))
    })(), "Restore missing game/track files or copy the affected games again.");
    if discs.is_ok() {
        report.result("Search indexes", (|| {
            let index = usb::find(&f::safe(&root,".ohmy3do/library.3di")?)?;
            let expected = games.values().cloned().collect::<BTreeSet<_>>();
            ensure!(index.into_iter().collect::<BTreeSet<_>>()==expected,"Find index differs from the games on this drive");
            let metadata = library::metadata_paths(&read(&root,".ohmy3do/metadata.3di",192*1024+32)?)?;
            ensure!(metadata.into_iter().collect::<BTreeSet<_>>()==expected,"Metadata index differs from the games on this drive");
            Ok("Find and metadata indexes match the drive.".into())
        })(), "Choose this drive as the library, then rebuild search indexes in Settings & tools.");
    }
    report.result("Folder navigation helpers", (|| {
        let paths = setup::navigation_helpers(&root,job)?;
        for path in &paths {
            job.check()?;
            ensure!(read(&root,path,2048)? == setup::navigation_helper_data(),"Invalid navigation helper: {path}");
            #[cfg(windows)] {
                use std::os::windows::fs::MetadataExt;
                ensure!(fs::metadata(f::safe(&root,path)?)?.file_attributes() & 6 == 0,
                    "Navigation helper has a hidden/system attribute: {path}");
            }
        }
        Ok(format!("{} required helpers present.",paths.len()))
    })(), "Repair missing helpers in Drive setup. Remove Hidden/System attributes from helpers so the ODE can see them.");
    job.check()?; job.say("Checking save support and backup catalog");
    report.result("Save support", (|| {
        let directory = f::safe(&root,".ohmy3do/saves")?;
        let slots = save_catalog::scan(&directory,job)?;
        let invalid = slots.iter().filter(|s| !s.valid).map(|s|s.name()).collect::<Vec<_>>();
        ensure!(invalid.is_empty(),"Invalid backups: {}",invalid.join(", "));
        let mut next = 0;
        for slot in &slots {
            job.check()?;
            let node = format!(".ohmy3do/saves/{}",save_catalog::node_name(slot.id));
            ensure!(read(&root,&node,8192)? == slot.node_bytes(next),"Stale or invalid save catalog entry: {node}");
            next = slot.id;
        }
        ensure!(read(&root,".ohmy3do/saves/slot-catalog.3di",28)? == save_catalog::root_bytes(next),"Stale or invalid save catalog root");
        Ok(format!("{} occupied backups validated; slot catalog matches.",slots.len()))
    })(), "Refresh slots in Save Manager or repair installed support. Keep recovery backups.");
    report.result("Cheat database", (|| {
        let db = save_cheats::Database::parse(&read(&root,save_cheats::JSON_PATH,1024*1024)?)?;
        ensure!(db.matches_binary(&read(&root,save_cheats::BINARY_PATH,save_cheats::MAX_BINARY)?)?,"Desktop and console cheat databases differ");
        Ok("Desktop and console cheat databases match.".into())
    })(), "Install the cheat database from Settings & tools.");
    for folder in ["covers","previews"] {
        job.check()?; job.say(format!("Checking {folder}"));
        let result = (|| -> Result<(usize,usize)> {
            let index = format!(".ohmy3do/{folder}/index.3di");
            let path = f::safe(&root,&index)?;
            let beside = if path.is_file() && fs::metadata(&path)?.len() > 0 {
                let data = read(&root,&index,128*1024+24)?;
                ensure!(data.len() >= 24,"Truncated {folder} index");
                u32::from_be_bytes(data[20..24].try_into().unwrap()) == 1
            } else { false };
            let map = if beside {library::read_drive_media_index(&root,folder)?} else {library::read_media_index(&root,folder)?};
            let mut checked = BTreeSet::new();
            for (game, name) in &map {
                job.check()?;
                let extension = Path::new(name).extension().unwrap_or_default().to_string_lossy().to_ascii_lowercase();
                ensure!(if folder=="covers"{["png","3dc"].contains(&extension.as_str())}else{extension=="3dv"},"Wrong media type in {folder} index: {name}");
                let disc = games.get(game).with_context(|| format!("{folder} index refers to a missing game: {game}"))?;
                let home = disc.rsplit_once('/').map_or("",|(p,_)|p);
                let relative = if beside && !home.is_empty() {format!("{home}/.ohmy3do/{name}")} else if beside {format!(".ohmy3do/{name}")} else {format!(".ohmy3do/{folder}/{name}")};
                let path = f::safe(&root,&relative)?;
                if checked.insert(path.clone()) { media::validate(&path).with_context(||format!("Missing or invalid {folder}: {relative}"))?; }
                if folder == "covers" {
                    let large = path.with_file_name(media::large_name(name));
                    if large.exists() && checked.insert(large.clone()) {media::validate(&large)?;}
                }
            }
            Ok((map.len(),games.len().saturating_sub(map.len())))
        })();
        match result {
            Err(e) => report.add(folder,"error",format!("{e:#}"),"Prepare missing media in the library, then copy the affected games again."),
            Ok((count,missing)) => {
                let enabled = settings.as_ref().is_ok_and(|s| if folder=="covers"{s.settings.usb_covers}else{s.settings.video_previews});
                report.add(folder,if missing>0 && enabled {"warning"}else{"ok"},format!("{count} indexed disc entries validated; {missing} without {folder}. Media is optional."),"Prepare media in Artwork & previews, then copy those games.");
            }
        }
    }
    job.check()?;
    report.ready = report.errors == 0;
    Ok(report)
}
