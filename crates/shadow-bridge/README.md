# shadow-bridge

`shadow-bridge` is the coarse-grained CXX boundary between Rust application state and the C++20 image kernel.

The current bridge performs one read-only operation:

```text
Rust path
→ CXX open_libraw_utf8
→ C++ DecoderProvider / DecodeSession
→ owned metadata + capability + preview snapshots / selected preview payload
→ pure shadow-domain types
```

No LibRaw or CXX type escapes the crate's public API. `inspect_libraw` returns a serializable descriptor snapshot; `extract_best_libraw_preview` returns the kernel-selected embedded preview or `None`. C++ exceptions become `BridgeError`; Rust panics and C++ exceptions never cross the language boundary directly.

Compressed embedded previews are small enough to cross as owned bytes. Large mosaic and RGB buffers intentionally remain in C++; their future bridge will use opaque handles and tile requests, not `Vec<u16>` copies across FFI.

## Path contract

The first implementation is Mac-first and accepts a UTF-8 path. This limitation is explicit: `inspect_libraw` rejects a non-UTF-8 `Path` instead of using a lossy display string. The Windows adapter will add a native UTF-16 entry point while preserving the existing provider and snapshot schemas.

## Build

Cargo uses CXX 1.0.198 and compiles the same `shadow-image` sources used by CMake. LibRaw 0.22+ is discovered through `pkg-config`; on macOS the build script filters Homebrew's obsolete `-lstdc++` entry because CXX already links libc++.

```sh
cargo test --package shadow-bridge
cargo run --package shadow-cli -- inspect-raw /absolute/path/to/input.dng
```

The ignored `real_dng_snapshot_crosses_the_bridge` test can be enabled with an absolute local fixture path in `SHADOW_TEST_DNG`.
