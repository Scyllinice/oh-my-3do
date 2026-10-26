//! Encoding through the native library, with and without sound, then
//! checking and decoding the result. (The C tests check every decoded pixel;
//! these check the Rust side and that the pieces fit together.)

use om3dvid7::{EncoderSettings, Picture, Reader};

const FRAMES: usize = 12;

/// Bands of a sliding gradient with a moving bright bar: enough change from
/// frame to frame to fill two chunks.
fn test_frames() -> Vec<Vec<u8>> {
    (0..FRAMES)
        .map(|n| {
            let bar = (n * 7) % (om3dvid7::WIDTH - 16);
            let mut frame = Vec::with_capacity(om3dvid7::FRAME_BYTES);
            for band in 0..om3dvid7::HEIGHT / 8 {
                let mut row = Vec::with_capacity(om3dvid7::WIDTH * 3);
                for x in 0..om3dvid7::WIDTH {
                    if (bar..bar + 16).contains(&x) {
                        row.extend([255, 255, 255]);
                    } else {
                        row.extend([
                            ((x + 3 * n) & 255) as u8,
                            ((band * 8 + n) & 255) as u8,
                            ((x ^ band) & 255) as u8,
                        ]);
                    }
                }
                for _ in 0..8 {
                    frame.extend(&row);
                }
            }
            frame
        })
        .collect()
}

/// A quiet square wave, signed 8-bit.
fn test_sound() -> Vec<u8> {
    (0..om3dvid7::samples_for(FRAMES))
        .map(|i| if (i / 25) % 2 == 1 { 20 } else { 236 })
        .collect()
}

fn encode(audio: Option<&[u8]>) -> om3dvid7::EncodedClip {
    let mut encoder = om3dvid7::Encoder::new(FRAMES).unwrap();
    for frame in test_frames() {
        encoder.add_frame(&frame).unwrap();
    }
    encoder.finish(audio, &EncoderSettings::default()).unwrap()
}

#[test]
fn default_settings_are_oh_my_3dos() {
    let settings = EncoderSettings::default();
    assert_eq!(
        settings,
        EncoderSettings {
            rate: 200 * 1024,
            read_rate: 275 * 1024,
            slot_bytes: 640 * 1024,
            max_chunk: 160 * 1024,
            chunk_frames: 10,
            entries4: 8192,
            entries2: 4096,
            cycle_cap: 230_000,
        }
    );
}

#[test]
fn word_sum() {
    assert_eq!(
        om3dvid7::word_sum(&[0, 0, 0, 1, 0, 0, 0, 2, 0x80, 0, 0, 0]),
        Some(0x8007_8003)
    );
    assert_eq!(om3dvid7::word_sum(b"abc"), None);
}

#[test]
fn clip_with_sound_round_trips() {
    let sound = test_sound();
    let clip = encode(Some(&sound));
    let header = om3dvid7::check(&clip.data).unwrap();
    assert_eq!(
        (header.frames, clip.frames, header.chunks),
        (FRAMES, FRAMES, 2)
    );
    assert!(header.has_sound());
    assert_eq!(header.chunk_blocks.len(), header.chunks);
    assert_eq!(clip.statistics.chunks as usize, header.chunks);
    assert_eq!(clip.statistics.preroll as usize, header.preroll);
    assert_eq!(om3dvid7::read_header(&clip.data).unwrap(), header);

    // The one-call encoder makes the same file: encoding is deterministic.
    let joined: Vec<u8> = test_frames().concat();
    let again = om3dvid7::encode(&joined, Some(&sound), &EncoderSettings::default()).unwrap();
    assert_eq!(again.data, clip.data);

    let mut reader = Reader::new(&clip.data).unwrap();
    assert_eq!(
        (reader.frames(), reader.audio_rate()),
        (FRAMES, om3dvid7::AUDIO_RATE)
    );
    assert_eq!(reader.audio().unwrap(), sound); // stored as given
    for (n, original) in test_frames().iter().enumerate() {
        let (number, rgb) = reader.next_frame(Picture::Screen).unwrap();
        assert_eq!((number, rgb.len()), (n, om3dvid7::FRAME_BYTES));
        // Lossy, but each frame is far closer to its source than to black.
        let error: usize = original
            .iter()
            .zip(&rgb)
            .step_by(97)
            .map(|(&a, &b)| a.abs_diff(b) as usize)
            .sum();
        let brightness: usize = rgb.iter().step_by(97).map(|&v| v as usize).sum();
        assert!(error < brightness / 2, "frame {n}: error {error}");
    }
    // Looped back to the first frame, now as the TV picture.
    let (number, small) = reader.next_frame(Picture::Tv).unwrap();
    assert_eq!((number, small.len()), (0, om3dvid7::TV_FRAME_BYTES));
    assert_eq!(
        om3dvid7::decode(&clip.data, Picture::Tv).unwrap().len(),
        FRAMES
    );
}

#[test]
fn sound_is_optional() {
    let clip = encode(None);
    let header = om3dvid7::check(&clip.data).unwrap();
    assert!(!header.has_sound());
    let mut reader = Reader::new(&clip.data).unwrap();
    assert!(!reader.has_sound());
    assert!(reader.audio().unwrap().is_empty());
}

#[test]
fn bad_input_is_refused() {
    assert!(om3dvid7::Encoder::new(0).is_err());
    assert!(om3dvid7::Encoder::new(om3dvid7::MAX_FRAMES + 1).is_err());
    let mut encoder = om3dvid7::Encoder::new(1).unwrap();
    assert!(encoder.add_frame(&[0; 10]).is_err());
    assert!(encoder.finish(None, &EncoderSettings::default()).is_err()); // no frames
    let frames = test_frames();
    encoder.add_frame(&frames[0]).unwrap();
    assert!(encoder.add_frame(&frames[1]).is_err()); // full
    assert!(encoder
        .finish(Some(&[0; 5]), &EncoderSettings::default())
        .is_err());
    assert!(om3dvid7::encode(&[0; 100], None, &EncoderSettings::default()).is_err());
}

#[test]
fn damage_is_rejected() {
    let clip = encode(Some(&test_sound())).data;
    let mut damaged = clip.clone();
    damaged[om3dvid7::BLOCK_BYTES + 64] ^= 1; // inside the first chunk
    let mut header = clip.clone();
    header[20] ^= 1;
    let mut signature = clip.clone();
    signature[7] = b'6';
    let mut trailing = clip.clone();
    trailing.extend([0; om3dvid7::BLOCK_BYTES]);
    let cases: [(&str, &[u8]); 6] = [
        ("chunk", &damaged),
        ("header", &header),
        ("signature", &signature),
        ("trailing data", &trailing),
        ("truncated", &clip[..clip.len() - om3dvid7::BLOCK_BYTES]),
        ("header only", &clip[..om3dvid7::BLOCK_BYTES - 1]),
    ];
    for (name, bad) in cases {
        assert!(om3dvid7::check(bad).is_err(), "{name} was accepted");
    }

    // The reader finds the damage when it reaches the chunk.
    let mut late = clip.clone();
    let at = late.len() - 100; // in the last chunk
    late[at] ^= 1;
    let mut reader = Reader::new(&late).unwrap();
    let results: Vec<bool> = (0..FRAMES)
        .map(|_| reader.next_frame(Picture::Screen).is_ok())
        .collect();
    assert!(results[0] && !results[FRAMES - 1]);
    assert!(reader.audio().is_err());
    assert!(Reader::new(&clip[..clip.len() - om3dvid7::BLOCK_BYTES]).is_err());
}
