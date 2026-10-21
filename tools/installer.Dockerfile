FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends nsis python3 ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
