# Dependencies and asset provenance

The root MIT license covers this project's original source and documentation.
Dependencies retain their own licenses and notices; the MIT license does not
relicense the 3DO SDK, Portfolio OS, firmware, games, or third-party artwork.

- [3do-devkit](https://github.com/trapexit/3do-devkit) supplies the legacy compiler,
  SDK headers/libraries, disc filesystem and tools inside the Docker image.
  Consult that repository and the included components for their terms.
- [Portfolio OS](https://github.com/trapexit/portfolio_os) and
  [FCare/SataTo3DO](https://github.com/FCare/SataTo3DO) are reference checkouts used
  by optional tests. They are fetched separately into ignored `third_party/`;
  this repository does not vendor their source or toolchain binaries.
- The firmware patch targets FCare's code and retains its upstream licensing.
- The logo and loading-screen images were generated for this project; their
  prompts and preparation are documented in `assets/README.md`.
- Demo cover imagery derives from the project's loading-screen artwork. The
  repository contains no commercial game images or game box scans.
- Personal cover associations, converted covers, USB backups, ISO builds and
  downloaded dependencies are excluded by `.gitignore`.

Signed ISO outputs contain SDK runtime components. Consult the dependency terms
when redistributing compiled images; this source license does not grant rights
to dependency components.

The ISO uses the explicit runtime selection in `runtime/sdk-files.txt`, rather
than the complete SDK sample filesystem. The build records the retained files
and linked libraries and their hashes in `runtime-audit.json`.
