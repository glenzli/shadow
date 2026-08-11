# Shadow

Shadow is a local-first, AI-native photo catalog and non-destructive editor. It is currently
building its Mac-first foundation while keeping persistent photo, Recipe, and image-processing
contracts portable to Windows.

## Why Shadow

- Keep original photos read-only while edits, review decisions, and organization remain durable.
- Browse and develop camera RAW files through a provider-neutral image pipeline.
- Use local AI for semantic discovery and assistance without making model output authoritative.
- Treat local folders and authenticated remote Libraries as sources of the same logical photos.

Shadow is under active development. Persistent formats and application workflows may still evolve
before the first stable release.

## Run the development build

The canonical local entry points are:

```sh
cargo xtask desktop-build-promote
cargo xtask desktop-run-debug
```

The POSIX and PowerShell launchers are intentionally thin wrappers around the same commands:

```sh
./scripts/build_and_promote_debug.sh
./scripts/run_debug.sh
```

The first command builds and promotes a complete debug application; the second launches the
current promoted build. Machine setup, required local assets, focused validation, and alternative
developer commands are documented in the [development guide](docs/development/README.md).

## Documentation

- [Developer documentation](docs/README.md) — documentation map and contribution entry points
- [Architecture and repository map](docs/architecture/README.md) — subsystem ownership and source navigation
- [Development guide](docs/development/README.md) — build, test, formatting, and debug workflows
- [Contract versioning](docs/contracts/versioning.md) — compatibility and dated revision policy
- [Library Server operations](docs/operations/library-server.md) — local server and remote Library workflows
- [Third-party provenance](THIRD_PARTY.md) — imported code, data, profiles, and licensing records

Component READMEs remain code-owned navigation indexes. They identify responsibility boundaries and
point to the source owner; current behavior is defined by source, schemas, and adjacent contract
tests.

## License

Shadow is free software under the GNU General Public License, version 3 or later
(`GPL-3.0-or-later`). See [LICENSE](LICENSE) for the license notice and canonical GPLv3 text.

The public distribution contains no vendor SDK, vendor profile, or private decoder-provider
implementation. An imported source file, profile, model, or dataset must retain its upstream
provenance and applicable license as described in [THIRD_PARTY.md](THIRD_PARTY.md).
