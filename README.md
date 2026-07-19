# Shadow

Shadow is a local-first, AI-native photo catalog and non-destructive editor. The project is currently in its Mac-first foundation phase; the persistent core and image interfaces are designed to remain portable to Windows.

The first executable slices are intentionally small:

```text
folder scan → transactional catalog registration → stable reopen → statistics
RAW/DNG → metadata → embedded preview → sensor mosaic → reference RGB
```

## Developer commands

```sh
cargo xtask check
cargo xtask test
cargo xtask doctor
cargo xtask native-check
cargo run --package shadow-cli -- init ./catalogs/demo.sqlite
cargo run --package shadow-cli -- scan ./catalogs/demo.sqlite /path/to/photos
./build/native-dev/cpp/shadow-image/shadow-raw-probe /path/to/input.dng ./bench-results/raw-probe
```

The current C++ probe intentionally uses LibRaw's reference RGB processing only as a decoder boundary and correctness baseline. Shadow's own scene-linear color and adjustment pipeline will replace that stage. Qt is not required for this probe. The local product and research material lives under `local-reference/` and is intentionally ignored by Git.
