FROM rust:1.94-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends clang lld llvm pkg-config libwebkit2gtk-4.1-dev libgtk-3-dev librsvg2-dev libxdo-dev xvfb xauth dbus-x11 python3 python3-selenium python3-pil webkit2gtk-driver fonts-dejavu && rm -rf /var/lib/apt/lists/*
RUN useradd -u 1000 -m builder
RUN chmod -R a+rX /usr/local/cargo /usr/local/rustup
