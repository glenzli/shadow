# Single-photo edit tools over process pipes

Launch the desktop executable directly with connected stdin/stdout pipes:

```text
Shadow --agent-stdio --isolate /absolute/path/photo.jpg
```

This opt-in POSIX entry runs the ordinary independent editor and its existing EditController.
It accepts exactly one explicit source supported by the native photo router (JPEG and available RAW providers).
PNG export support does not imply PNG original admission. It does not attach to an already running Library, listen
on a socket, expose arbitrary Recipe JSON, execute models, or introduce a second draft owner.
Windows rejects the flag. Use a subprocess with both streams set to PIPE; diagnostics go to stderr.

The independent editor has a **temporary Catalog**. Commits and Undo are real within this session;
export a new file before shutdown. Restart creates a different session and rejects all old tokens.
A flattened PNG is an output artifact, not an editable-original or portable Recipe round trip.

## Wire contract

Each request and response is one UTF-8 JSON object followed by a newline. Requests have exactly
`schema`, `id`, `op`, and `params`. Schema is `shadow.edit-tools/1`; IDs are nonempty bounded strings.
Unknown fields, wrong types, non-finite/out-of-range exposure, and requests over 64 KiB are rejected.
IDs correlate responses and do not provide durable replay or cross-restart idempotency. Reusing one
of the last 256 IDs is rejected. Responses can arrive out of order when cancelling a pending preview.

```json
{"schema":"shadow.edit-tools/1","id":"1","op":"discover","params":{}}
{"schema":"shadow.edit-tools/1","id":"2","op":"snapshot","params":{}}
```

Success returns `ok: true` and `result`; failure returns `ok: false` and `error.code/message`.
`discover` is the current supported operation/format/cancellation contract. While the selected photo
opens, snapshot returns not_ready; unsupported or corrupt input returns source_unavailable with the
admission diagnostic. An asynchronous history-open failure uses the same code and owner status. The client can still discover capabilities and shut down after that failure.
`snapshot` returns:

- `identity`: sessionId, snapshotId, photoId, representationId, baseCommitId, workingCommitId,
  activeVariantId, and the existing draftRevision encoded as a decimal string. Empty commit strings
  mean no commit; null does not. The opaque snapshot token also binds the full server-side stack,
  source size/mtime, photo generation, and owner invalidation epoch.
- `nodes`: IDs, labels, exposureStops, and whether each local enabled exposure node is editable.
- dirty, autosavePending, autosaveFailed, gestureActive, busy, canUndo, and sourceFingerprint.

Snapshot copies the current owner state. It does not finish a gesture, autosave, close the photo,
project a portable Recipe, or add history. Each fresh snapshot replaces the previous token and
candidate. Source fingerprint is the existing size/mtime contract, **not a content digest**.

Copy the entire returned identity into `expected` below; never reconstruct it from a filename or
replace only its working head to force a stale request through.

| Operation | Exact params | Result / limit |
| --- | --- | --- |
| preview | expected, operation `{type: "set_exposure", nodeId, stops}`, outputPath | New JPEG artifact and proposalId; exposure is absolute stops in [-16,16], preview max edge 1024. No draft/ref/Undo mutation. |
| apply | expected, proposalId | Exact photo/representation/Variant/commit/recipe/digest receipt, one Undo step. Requires the reviewed current proposal and a clean, idle human draft. |
| export | expected, outputPath | New full-resolution sRGB RGB8 PNG pinned to that committed Recipe. A neutral or historical draft must first be applied. |
| cancel | requestId | Preview only, accepted until artifact publication begins. Apply and export return cancel_unsupported. |
| shutdown | empty object | Requires a clean idle owner, reserves it, drains the final response, then exits. |

Output paths must be absolute, have an existing directory, and use `.jpg`/`.jpeg` for previews or
`.png` for exports. Existing files and dangling symlinks are rejected; the common exporter also
publishes exclusively if another writer creates the destination after admission. Originals remain
read-only. A preview can finish after a human change: its artifact is preserved, but `current` is
false and no applicable proposal is returned. Cancellation acknowledged before publication leaves
no artifact. Rejected cancellation after publication began does not retract a completed file.

The server retains one snapshot, one candidate, and one in-flight tool operation. A stale identity
returns stale_snapshot; a current but unsaved/gesturing draft returns draft_pending. It never saves,
rebases, or discards that human draft on behalf of a read/preview/apply request. Tool commits share
the existing Catalog working-head + active-Variant CAS. A publication receipt comes directly from
the committed immutable record; subsequent movable-head refresh is not its source of truth.

Human editing may continue during a frozen export. The independent editor serializes tool/GUI
export consumers and session close, preventing a second runner from stealing the first runner's
receipt. Export publication stays successful if optional output metadata readback fails; the reply
then contains published=true, path, commitId, and metadataUnavailable=true. Output metadata and
SHA-256, when available, are explicitly post-publication readback observations. External writers
can subsequently change a path, so this is not an immutable storage capability.

Input EOF or output backpressure failure detaches the client and preserves the actual editor window
and human work. Send shutdown and keep draining stdout for an orderly process exit. A crashed or
forcibly terminated client has no durable idempotent retry guarantee. The output buffer is bounded
at 256 KiB; an unresponsive peer cannot block the GUI indefinitely. No credentials or extra OS
permissions are required by the tools.

## Rendering and validation boundary

Exposure changes only the selected Grade node and downstream rendering. The complete captured
stack and immutable base preserve upstream Foundation, source development, masks, and private
Recipe meaning. Preview reuses the existing transient exact renderer and warm-source identities;
no durable preview blob is published. JPEG is an explicit host-byte consumer: GPU results, where
used, materialize at the existing bounded preview boundary and encode once for the external file.
There is no new host readback/upload loop. Full-resolution export uses the same exact Recipe
compiler through the established durable queue. Resolution/encoding differ; authored semantics do
not. Cancellation uses the existing render token plus a bounded publication-stage transition.

Owner/UI arbitration and the actual external pipe are separate tests:

```sh
ctest --test-dir "$SHADOW_BUILD_DIR" -R 'shadow-desktop-edit-tool-' --output-on-failure
```

The integration test launches the real stdio entry with synthetic JPEG pixels and no injected test
harness. The owner/UI test separately drives the same controller, holds workers to test close/apply
races, and sends real window pointer events to packaged Undo/Redo controls. It runs offscreen by
default; its script takes the application and shadow-edit-tool-fixture executables, and accepts --visible and --evidence-root for an explicit visible-window acceptance.
No new quiet-machine latency baseline is claimed by these correctness tests.
