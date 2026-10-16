use oh_my_3do_library::{catalog, library, usb, Job};
use std::path::PathBuf;
fn main() -> anyhow::Result<()> {
    let args = std::env::args().collect::<Vec<_>>();
    let root = PathBuf::from(args.get(1).expect("staging root"));
    let cache = PathBuf::from(args.get(2).expect("cache root"));
    let root = root.canonicalize()?;
    let job = Job {
        log: std::sync::Arc::new(|s| println!("{s}")),
        ..Default::default()
    };
    let cat = catalog::load(&cache, true, &job)?;
    println!("Catalog entries: {}", cat.games.len());
    let (files, games) = usb::inventory(&root, &job)?;
    let db = library::database(&root)?;
    println!(
        "Validated {} runtime files; {} disc entries; {} database records",
        files.len(),
        games,
        db["games"].as_array().unwrap().len()
    );
    Ok(())
}
