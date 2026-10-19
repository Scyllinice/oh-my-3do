# Cross toolchain for tools/build-desktop.py --windows: MSVC target via clang/lld and cargo-xwin.
FROM rust:1.94-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends clang lld llvm && rm -rf /var/lib/apt/lists/*
RUN rustup target add x86_64-pc-windows-msvc && cargo install --locked cargo-xwin@0.23.1
RUN chmod -R a+rX /usr/local/cargo /usr/local/rustup
