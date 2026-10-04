# shadow-cli

`shadow-cli` is Shadow's headless application and operator entry. The `project` command opens
persistent, CLI-owned projects through the same Rust application services, immutable Recipe
compiler, Catalog CAS and native full-quality renderer as the desktop. The crate name
`shadow-desktop-bridge` also covers these Qt-free application services; the CLI does not launch Qt.
The first project protocol covers the complete import → edit → persist → reopen → export path.
It does **not yet cover every desktop capability**. Existing operator commands remain available.

Run a command from the repository root with:

```sh
cargo run --package shadow-cli -- <command> [arguments]
```

## Commands

| Command | Purpose |
| --- | --- |
| `project <project-root> <request.json\|->` | Execute one versioned JSON project request; `-` reads bounded stdin |
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

The legacy parser in [`src/commands.rs`](src/commands.rs), project transport in
[`src/project.rs`](src/project.rs), and [shared protocol](../../crates/shadow-desktop-bridge/src/headless_project/protocol.rs) are authoritative. Update this
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


## Persistent project protocol, phase 1

Build the **original Shadow workspace**, with a task-private Cargo output directory. Standalone
Cargo compiles the shared native image library; do not set `SHADOW_BRIDGE_EXTERNAL_IMAGE=true`
(that mode expects a final native desktop link). On Linux, the bridge already uses the portable
manifest and Metal stubs. No Qt SDK, Apple frameworks or Infer service is required for CPU use.
A C++20 compiler, pkg-config, LibRaw **>= 0.22** (`libraw_r`), LittleCMS, JPEG, PNG and TIFF development
packages are required; Lensfun and libheif are optional. See the [bridge build owner](../../crates/shadow-bridge/build.rs)
for the authoritative dependency probes. This build recipe is not a claim of Linux validation.

```sh
CARGO_TARGET_DIR=/absolute/private/build/shadow-cli \
SHADOW_ENABLE_METAL=0 SHADOW_BRIDGE_EXTERNAL_IMAGE=false \
  cargo build --locked --release -p shadow-cli -j 2
```

Every invocation reads exactly one UTF-8 JSON document (at most 65,536 bytes) and emits exactly one
JSON response. Unknown fields, unsupported schema versions and malformed requests are rejected.
Filesystem locations in responses use Shadow’s existing `AssetLocation` shape: `platform`,
lossless `native_path` bytes (Unix bytes or little-endian Windows UTF-16), and presentation-only
`display_path`. Do not reopen a file through lossy display text. JSON request path strings are UTF-8;
the command-line project root and returned native locations preserve non-UTF-8 Unix paths.

Exit status is `0` for success, `2` for a JSON error, and `3` if stdout could not be written. If the
response channel is lost after a commit, inspect `history`/`snapshot` before retrying; there is no
request-id deduplication protocol. Error codes are stable categories; detailed messages are diagnostics.

```json
{"schema_version":1,"command":{"op":"capabilities"}}
```

`capabilities` does not open the project, discover credentials or contact Infer. It reports CPU
commands, input limitations, output precision, and explicit unsupported features. AI generation is
currently `unsupported`, with runtime `not_probed`; this is not evidence that a configured Infer is
available or unavailable. Existing Infer diagnostics are separate explicit legacy commands.

A project is a directory containing `catalog.sqlite`, `cache/`, application state, and managed
`originals/<sha256>.<extension>` copies. `init` refuses an existing Catalog. `import` copies exactly
one selected source, verifies its bytes and admits the copy through the existing source-inspection
service. It neither scans a directory nor modifies the caller's file. Retries reuse an identical
content-addressed copy; an inspection failure may leave an unreferenced copy that can be retried.
These projects are currently local to their registered native paths; moving them to another machine
requires supported relinking, not editing Catalog rows or pretending native paths are portable.

Example (shell + `jq`, with all data outside the source checkout):

```sh
CLI=/absolute/private/build/shadow-cli/release/shadow-cli
PROJECT=/absolute/work/example.shadow
printf '%s\n' '{"schema_version":1,"command":{"op":"init"}}' | "$CLI" project "$PROJECT" -
printf '%s\n' '{"schema_version":1,"command":{"op":"import","source":"/absolute/work/input.png"}}' \
  | "$CLI" project "$PROJECT" - > imported.json
PHOTO=$(jq -r '.result.photo_id' imported.json)
jq -n --arg photo "$PHOTO" '{schema_version:1,command:{op:"snapshot",photo_id:$photo}}' \
  | "$CLI" project "$PROJECT" - > snapshot.json
jq '{schema_version:1,command:{op:"commit",guard:.result.guard,edits:[
  {op:"set_basic",node_index:0,values:{exposure_stops:0.35,saturation_factor:0.8}},
  {op:"set_geometry",values:{enabled:true,crop_left:0.1,crop_right:0.9}}
]}}' snapshot.json | "$CLI" project "$PROJECT" -
# A separate process reopens the durable committed Recipe.
jq -n --arg photo "$PHOTO" '{schema_version:1,command:{op:"snapshot",photo_id:$photo}}' \
  | "$CLI" project "$PROJECT" - > reopened.json
jq '{schema_version:1,command:{op:"export",guard:.result.guard,
  destination:"/absolute/work/output.png",bit_depth:16}}' reopened.json \
  | "$CLI" project "$PROJECT" -
```

The `guard` must be passed intact: photo, representation, original SHA-256, immutable content base,
expected durable working commit, and expected active Variant. They are the existing CAS fields,
not a parallel revision scheme. A conflict changes no authored state; capture a fresh snapshot and
reconcile explicitly. CAS does not arbitrate an unsaved desktop draft. Use a CLI-owned offline
project, and use the [desktop session-owner tool](../../docs/development/agent-edit-stdio.md) when a
human has a live editor open. Do not point this command at an active desktop Catalog.

| Project operation | Required fields and semantics |
| --- | --- |
| `init` | Creates a new project's Catalog and application stores |
| `import` | `source`: absolute or process-relative supported source path; returns durable photo/representation IDs and SHA-256 |
| `photos` | `limit` 1–100, optional `after` from `next_cursor`; optional shared Catalog `filter`; stable file-name order, no thumbnail decoding |
| `snapshot` | `photo_id`, optional immutable `commit_id`; historical reads do not move the working head |
| `commit` | Captured `guard` and `edits` (at most 64); returns the exact transaction receipt, not a mutable head readback |
| `history` | `photo_id`, `limit` 1–100, optional `after` from `next_cursor`; existing newest-first keyset history |
| `export` | Captured `guard`, `.png` `destination` outside the project, `bit_depth` 8 or 16; requires a committed content base |

`set_basic` patches only the provided exposure, contrast, grade white-balance temperature/tint and
saturation fields of `node_index`. Grade white-balance values are the native grading controls,
**not Kelvin / RAW Foundation white balance**. `set_node` patches enabled/opacity/label; `add_node`,
`remove_node` and `move_node` edit ordered local Grade Nodes. Shared-node color controls require an
explicit desktop detach. `set_geometry` patches normalized crop, quarter-turn 0–3, straighten,
perspective, flips and enabled state. The canonical Recipe validator owns every numeric range and
structural invariant; an invalid operation rejects the whole batch. Unaddressed fine color, masks,
repair, paint, completion, Foundation and other state remain in the complete immutable Recipe.

An empty `edits` list explicitly commits the captured state. This can persist a neutral photograph,
or restore an old snapshot as a **new** commit: obtain `snapshot` with that old `commit_id`, then
commit its guard with `edits:[]`. The old content parent and latest expected working head remain
independent, preserving intervening immutable history. `recipe_snapshot` in the read response is
exact source-photo state for inspection; it is not an importable replacement or a portable cross-photo
Recipe projection. Editable continuity lives in the Catalog and managed resources, not the exported PNG.

PNG output preserves the shared export renderer's full native dimensions and display-sRGB RGB8 or
RGB16 samples. PNG16 is encoded directly from 16-bit samples in network byte order, without an RGB8
intermediate. The headless file adapter writes an sRGB chunk and no source EXIF/GPS/ICC metadata.
There is no implicit resize, gamut conversion or fallback renderer. Export creates a synced temporary
file next to the destination and publishes without replacing existing files, including symlinks.
No output may be placed inside the project directory. In-progress rendering cannot be cancelled;
process interruption does not roll back an earlier committed Recipe, and a fresh export can be retried
with a new destination. Crash-orphaned staging files and durable export-queue resumption remain gaps.

Raster support is the native provider's exact admission contract, not arbitrary files with a matching
suffix. PNG input currently accepts bounded RGB8 or fully opaque RGBA8 with no profile or an sRGB
chunk; it rejects 16-bit, palette, gray, transparency, animation and unsupported metadata/profile
forms. Thus PNG16 output is a delivery artifact and cannot currently be reimported by this PNG provider.
JPEG profile/orientation handling, special linear TIFF and optional HEIF follow their shared providers.
The 2 GiB CLI source cap is an outer bound; stricter format-specific byte/pixel limits still apply.

## Desktop → CLI coverage

| Desktop capability | CLI route today | Remaining coverage |
| --- | --- | --- |
| Public RAW/raster inspection and import | `project import`; legacy `scan`, `inspect-raw` | Private decoders are not bundled; folder cache preparation remains LibRaw-specific |
| Non-destructive grading and geometry | `project snapshot/commit` over the same complete Recipe and renderer | Fine color authoring, RAW Foundation controls, new masks, repair, paint, Liquify, LUT bindings |
| Persistent photo history and recovery | Exact CAS receipts, paged `history`, historical snapshot/new restore commit, legacy backup/restore | Named version creation, Variant mutation, Library-wide history authoring |
| Library discovery and filtering | `project photos` over shared bounded Catalog query; legacy statistics/scans | Albums, keyword mutation, culling/ratings and multi-photo editing |
| Remote Library | Existing authenticated serve/sync/materialize commands | Full desktop server UI lifecycle and project-managed import from a remote selection |
| Native full-quality export | Shared exact renderer, PNG8/PNG16, immutable commit/source receipt, exclusive file publication | JPEG/TIFF output, resize/profile presets, watermarks, durable export queue runner |
| Cancellation and retry | Source/edit failures preserve earlier commits; output conflicts do not overwrite | Export cancellation and job resume are **unsupported** |
| Infer AI | Explicit existing people/semantic diagnostics; project capability reports generation unsupported | No project AI invocation; never silently substitutes a model or removes enabled AI intent |
| Human + AI live editing | Existing desktop stdio owner protocol | Project CLI is offline and does not arbitrate unsaved Qt drafts |

This is phase 1 of the original application's CLI, not a separate reduced media kernel. A successful
Mac CPU run is not Linux acceptance: build and run the same source revision on Linux independently.


## Focused validation

Use the same task-private target directory and CPU build environment above, then run:

```sh
cargo test --locked -j 2 -p shadow-desktop-bridge --lib headless_project::tests -- --test-threads=1
cargo test --locked -j 2 -p shadow-cli --test project_contract -- --test-threads=1
```

These contracts generate synthetic input and exercise separate CLI processes, full Recipe
preservation, original protection, stale/concurrent CAS, historical restoration, PNG8/16, and strict
request limits. The Unix native-path test exercises publication through non-UTF-8 paths where the
filesystem supports them. On macOS APFS it instead verifies structured rejection before project
creation; that branch is not evidence for Linux's non-UTF-8 success path. Linux must run the tests
independently. Export timing should name input/output dimensions, CPU/backend, build profile,
process/session reuse and sample count; debug cold-process timings are not Release performance.
