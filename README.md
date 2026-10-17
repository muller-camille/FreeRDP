# Windows client builds

This branch only holds the build of the FreeRDP Windows client (`wfreerdp.exe`) from the `patches-3.32.2` branch of
this fork, and the workflow that publishes it as a release:

- `build.sh`: FreeRDP at a pinned commit and OpenSSL at a pinned version (archive checked by its SHA-256 sum),
  cross-compiled from Linux with mingw-w64 and statically linked: a single executable that only uses Windows DLLs.
- `.github/workflows/release.yml`: runs `build.sh` when it changes on this branch and publishes release
  `wfreerdp-<version>-<release>` with the executable, the licenses, `BUILD.txt`, `SHA256SUMS.txt` and a build
  provenance attestation (`gh attestation verify wfreerdp.exe --repo muller-camille/FreeRDP`).

To publish a new build, change the pinned commit or the build in `build.sh` and bump `release`.
