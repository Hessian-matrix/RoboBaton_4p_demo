# License scope

Unless otherwise stated, the source code, scripts, configuration files, and documentation authored in this repository are licensed under the Apache License 2.0 in `LICENSE`.

Precompiled vendor shared libraries under `lib/` and copies of those libraries in packaged runtime directories are excluded from the Apache License 2.0. They are distributed only for use with RoboBaton 4P hardware and its supported system software. They may not be redistributed separately, reverse engineered, or ported to unauthorized hardware, except where applicable law requires otherwise.

The embedded `sensor_web` service includes the Rust standard library, musl, and third-party crates under their own licenses, not the repository's Apache-2.0 grant. `web/dependencies.lock.json` identifies Rust 1.95.0, the complete Cargo.lock graph and registry checksums, and license-file hashes. `web/THIRD_PARTY_NOTICES.txt` retains the corresponding upstream license and notice texts, including str0m and its cryptographic dependencies.

Crate notices come from the exact registry archives authenticated by Cargo.lock checksums. Rust and musl license texts are retained with source attribution. No third-party source or license text is modified.

Runtime packages and CMake installs include `LICENSE`, this scope statement, and `licenses/sensor_web/{THIRD_PARTY_NOTICES.txt,dependencies.lock.json}`. The static executable embeds its browser assets; no external asset tree, MediaMTX, compiler, or dependency installer is delivered. Builds require pre-provisioned Rust 1.95.0, cargo-zigbuild 0.23.0, Zig 0.16.0, the ARM64 musl target, and the locked crate cache. Actual builds use Cargo's frozen/offline mode.

The package verifier reads the allocated `.sensor_web` identity section from the static AArch64 ELF and checks the application version, compiler, dependency lock, and source digest. The digest covers `VERSION`, the Web build script, and recursive Web source/assets/lock/notices, excluding generated binaries/caches. Do not remove this section or merely update sidecar hashes: old binaries must not be paired with new source or browser assets.
