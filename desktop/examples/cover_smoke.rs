use oh_my_3do_library::media;
use std::path::PathBuf;
fn main() -> anyhow::Result<()> {
    let dir = PathBuf::from(std::env::args().nth(1).expect("output directory"));
    std::fs::create_dir_all(&dir)?;
    let source = dir.join("native-cover-source.png");
    image::RgbImage::from_fn(200, 300, |x, y| {
        image::Rgb([(x % 256) as u8, (y % 256) as u8, 90])
    })
    .save(&source)?;
    let target = dir.join("native-cover.png");
    media::cover(&source, &target)?;
    media::validate(&target)?;
    let image = image::open(&target)?.to_rgb8();
    let mut expected = vec![];
    for p in image.pixels() {
        let word = ((p[0] as u16 >> 3) << 10) | ((p[1] as u16 >> 3) << 5) | (p[2] as u16 >> 3);
        expected.extend_from_slice(&word.to_be_bytes());
    }
    std::fs::write(dir.join("native-cover.rgb555"), expected)?;
    Ok(())
}
