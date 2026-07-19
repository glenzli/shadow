# Shadow

Shadow is a local-first, AI-native photo catalog and non-destructive editor. The project is currently in its Mac-first foundation phase; the persistent core and image interfaces are designed to remain portable to Windows.

The first executable slice is intentionally small:

```text
folder scan → transactional catalog registration → stable reopen → statistics
```

## Developer commands

```sh
cargo xtask check
cargo xtask test
cargo xtask doctor
cargo run --package shadow-cli -- init ./catalogs/demo.sqlite
cargo run --package shadow-cli -- scan ./catalogs/demo.sqlite /path/to/photos
```

Qt/C++ image and desktop modules will be connected after the catalog slice is stable. The local product and research material lives under `local-reference/` and is intentionally ignored by Git.

