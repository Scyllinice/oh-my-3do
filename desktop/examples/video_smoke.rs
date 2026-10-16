use oh_my_3do_library::{media, model::Preferences, Job};
use std::{path::PathBuf, process::Command, sync::Arc};
fn main() -> anyhow::Result<()> {
    let args = std::env::args().collect::<Vec<_>>();
    let cache = PathBuf::from(&args[1]);
    let output = PathBuf::from(&args[2]);
    let job = Job {
        log: Arc::new(|s| println!("{s}")),
        ..Default::default()
    };
    let bin = media::install_video_tools(&cache, &job)?;
    let prefs = Preferences {
        video_tools: bin.to_string_lossy().into_owned(),
        ..Default::default()
    };
    let (ffmpeg, _) = media::video_exes(&prefs)?;
    let temp = tempfile::tempdir()?;
    let source = temp.path().join("source.mkv");
    let status = Command::new(ffmpeg)
        .args([
            "-v",
            "error",
            "-nostdin",
            "-f",
            "lavfi",
            "-i",
            "color=c=red:s=64x80:r=30:d=1",
            "-f",
            "lavfi",
            "-i",
            "sine=frequency=220:sample_rate=44100:duration=1",
            "-c:v",
            "ffv1",
            "-c:a",
            "pcm_s16le",
            "-shortest",
        ])
        .arg(&source)
        .status()?;
    anyhow::ensure!(status.success(), "Synthetic source generation failed");
    media::preview(&source, &output, &prefs, &job)?;
    media::validate(&output)?;
    println!(
        "Native preview generated and validated: {}",
        output.display()
    );
    Ok(())
}
