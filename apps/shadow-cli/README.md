# shadow-cli

`shadow-cli` exposes focused operator and diagnostic commands over Shadow's public Rust services.
It is not the normal desktop launch path and does not replace the graphical Library Server.

Run a command from the repository root with:

```sh
cargo run --package shadow-cli -- <command> [arguments]
```

## Commands

| Command | Purpose |
| --- | --- |
| `init <catalog.sqlite>` | Initialize a development Catalog |
| `scan <catalog.sqlite> <folder>` | Register a folder without cache preparation |
| `scan-cache <catalog.sqlite> <cache-root> <folder>` | Scan and prepare LibRaw-backed RAW visuals |
| `cache-read <catalog.sqlite> <cache-root> <path>` | Exercise verified cache recovery for one RAW source |
| `resume <catalog.sqlite> <session-id>` | Resume a recoverable scan session |
| `recoverable <catalog.sqlite>` | List recoverable scan sessions |
| `stats <catalog.sqlite>` | Print bounded Catalog statistics |
| `backup <catalog.sqlite> <backup.sqlite>` | Create and verify a non-overwriting Catalog backup |
| `verify-backup <backup.sqlite>` | Run a non-mutating restore drill |
| `restore-backup <backup.sqlite> <offline-catalog.sqlite>` | Verify and restore an offline Catalog while retaining the previous database and SQLite sidecars for rollback |
| `inspect-raw <path>` | Inspect provider-neutral RAW capabilities |
| `inspect-store <catalog.sqlite> <cache-root> <path>` | Inspect and persist one RAW source snapshot and visual |
| `people-cluster <catalog.sqlite> <cache-root> <infer-base-url> <token-file>` | Run local anonymous-person clustering diagnostics |
| `semantic-search <catalog.sqlite> <cache-root> <infer-base-url> <token-file> <query> [language]` | Run a bounded local semantic-search diagnostic |
| `library-serve <catalog.sqlite> <preview-cache> <folder> <server-state> <bind-address> <token-file> <display-name>` | Start the low-level authenticated Library service |
| `library-sync <server-address> <token-file> <mirror-root> <preview-cache>` | Mirror a remote manifest and previews |
| `library-materialize <server-address> <token-file> <mirror-root> <original-cache> <local-catalog.sqlite> <remote-photo-id> <remote-representation-id>` | Verify and admit one remote original locally |

The parser and usage string in [`src/commands.rs`](src/commands.rs) are authoritative. Update this
table in the same change when adding, renaming, or removing a command.

## Safety and credentials

- Keep Catalogs, caches, server state, downloaded originals, and photo fixtures outside Git.
- Token arguments name owner-only files; do not place raw tokens on the command line or in logs.
- `scan` and `scan-cache` reject missing or non-directory inputs before opening a Catalog or cache.
  A valid empty directory succeeds; individual traversal issues remain visible in the scan report.
- The CLI cache inspector is LibRaw-specific: scanning JPEG files registers them but does not
  prepare JPEG visuals. The desktop image pipeline has a separate decoded-image path.
- Backup creation never overwrites an existing destination.
- Restore requires the desktop app and Library Server to be stopped. It verifies the source and
  restored Catalog, and keeps the displaced database, WAL, and shared-memory files in a reported
  rollback directory.
- Remote originals are verified before local Catalog admission.

For normal application development, use the repository [development guide](../../docs/development/README.md).
For server lifecycle and graphical operation, use the [Library Server guide](../../docs/operations/library-server.md).
