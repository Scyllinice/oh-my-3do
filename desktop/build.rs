// The preview encoder is shared with tools/prepare-preview.py; compile the same C source in.
fn main() {
    // A precompiled resource keeps native and cargo-xwin builds independent
    // of the host's resource compiler. Regenerate it with prepare-branding.py.
    println!("cargo:rerun-if-changed=../assets/app-icon.res");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows")
        && std::env::var_os("CARGO_FEATURE_LEGACY_UI").is_some()
    {
        let icon = std::fs::canonicalize("../assets/app-icon.res").expect(
            "Missing Windows icon resource; run tools/prepare-branding.py --windows-resource",
        );
        println!(
            "cargo:rustc-link-arg-bin=oh-my-3do-library={}",
            icon.display()
        );
    }
    let source = "../tools/preview-encoder.c";
    println!("cargo:rerun-if-changed={source}");
    // Media rebuilds encode on several threads; give each its own encoder state.
    let msvc = std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc");
    cc::Build::new()
        .file(source)
        .define(
            "OH3DO_THREAD_LOCAL",
            if msvc {
                "__declspec(thread)"
            } else {
                "__thread"
            },
        )
        .opt_level(2)
        .warnings(true)
        .compile("preview_encoder");
    // The streamed-preview encoder keeps no static state and includes the
    // menu's decoder, which checks every file it writes.
    let streamed = "../tools/preview7-encoder.c";
    println!("cargo:rerun-if-changed={streamed}");
    println!("cargo:rerun-if-changed=../src/preview_v7.c");
    println!("cargo:rerun-if-changed=../src/preview_v7.h");
    let mut build = cc::Build::new();
    build
        .file(streamed)
        .include("../src")
        .opt_level(2)
        .warnings(true);
    if !msvc {
        build.flag("-std=c99");
    }
    build.compile("preview7_encoder");
}
