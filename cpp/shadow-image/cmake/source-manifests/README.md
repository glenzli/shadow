# Native source manifests

These line-based manifests are the single compiled-source index for `shadow-image`.
Both the native CMake target and the direct Cargo build in `shadow-bridge` read them.

- `portable.txt` contains sources compiled on every supported platform.
- `metal.txt` and `metal-stubs.txt` select the Metal implementation or the non-Metal fallback.

Keep paths relative to `cpp/shadow-image`, one source per line. Headers and bridge-only
translation units remain owned by their consuming build graph; do not list them here.
