// Compiles the om3dvid library's PC code: the encoder, the decoder (which the
// encoder checks every file with) and the reader. None keeps static state,
// so separate encoders and readers can run on separate threads.
fn main() {
    let library = "..";
    let sources = [
        "src/om3dvid7_encoder.c",
        "src/om3dvid7_decoder.c",
        "src/om3dvid7_reader.c",
    ];
    for file in sources.iter().chain(&[
        "include/om3dvid7.h",
        "include/om3dvid7_encoder.h",
        "include/om3dvid7_reader.h",
    ]) {
        println!("cargo:rerun-if-changed={library}/{file}");
    }

    let mut build = cc::Build::new();
    for source in sources {
        build.file(format!("{library}/{source}"));
    }
    build
        .include(format!("{library}/include"))
        .opt_level(2)
        .warnings(true);
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() != Ok("msvc") {
        build.flag("-std=c99");
    }
    build.compile("om3dvid7");
}
