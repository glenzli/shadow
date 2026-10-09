# External image edit stage

Shadow supplies a material editing stage through its independent editor. Shape owns its input
Artifact Revision, Candidate and explicit Accept/CAS. Shadow never opens a Shape database or
advances an accepted Shape head. Human and machine edits share `EditController` and the existing
Recipe CAS; [stdio tools](agent-edit-stdio.md) remain the adjustment API.

## Launch and request

```text
Shadow --pipeline-edit --request /absolute/request.json --result /absolute/result.json
Shadow --pipeline-edit --request /absolute/request.json --result /absolute/result.json --agent-stdio
```

The optional POSIX stdio transport belongs to the same human-visible window. Existing pipeline
schemas and `--agent-stdio --isolate` retain their contracts. The additive single-image request is:

```json
{
  "schema": "shadow.external-edit/1",
  "requestId": "caller-session-id",
  "input": {
    "path": "/absolute/materialized.png",
    "revision": "opaque-caller-input-revision",
    "sha256": "64 lowercase hexadecimal characters"
  },
  "output": {"path": "/absolute/new-edited.png"}
}
```

Fields are exact; unknown fields, malformed identities and conflicting paths are rejected.
`requestId` and `revision` are nonempty strings of at most 256 UTF-16 units. Input/output paths are
absolute, at most 4096 units, without NUL. Input must exist; output and result must be new paths.
Output/result/request/input cannot alias. Paths echoed under `input` preserve the request spelling.

Input is RGB8 or **fully opaque** RGBA8 PNG: at most 64 MiB encoded, 16,777,216 pixels, 16,384 pixels
per dimension and 16,384 chunks. Untagged RGB assumes sRGB; a valid sRGB chunk is accepted.
Transparency, ICC/gamma/chromaticity/EXIF/HDR overrides, animation, compressed text, grayscale and
higher bit depths fail explicitly. A richer representation needs an explicit, disclosed caller
materialization. Output is full-resolution sRGB RGB8 PNG, including the editor's spatial edits and
its existing standard sRGB ICC tag. For another editing stage, materialize those known sRGB pixels
as untagged/sRGB-chunk PNG; never strip an arbitrary profile without color conversion. This is a
flattened derived artifact, not an editable-original or portable Recipe round trip.

Admission copies and hashes the same bounded bytes on a worker into a private read-only source.
Catalog, editor and tools all use that copy. The caller materialization is hashed again before and
after export; a mismatch fails the return even when size/mtime are unchanged. Originals are never
written. Shape must independently verify its own input revision and returned bytes at import/Accept.

## Completion and terminal result

The human **Return edited image** action and stdio `complete` share save/export/return.
`complete` takes `{"expected": <whole snapshot identity>}`. It rejects unsaved gestures/drafts,
busy state and stale identities. Its immediate `accepted` response only acknowledges admission;
read the result file for completion. Discovery includes `externalStage` with the request/input,
output/result paths and termination rules. The bound output/result paths are reserved for completion; an ordinary tool `export` cannot consume
them early. A neutral image can be returned: the existing durable
export queue creates its neutral Recipe commit. This does not create another draft/revision protocol.

A successful result has this shape (all digest strings are lowercase hexadecimal):

```json
{
  "schema": "shadow.external-edit-result/1",
  "requestId": "caller-session-id",
  "input": {"path": "/absolute/materialized.png", "revision": "opaque-caller-input-revision", "sha256": "..."},
  "outcome": "completed",
  "errors": [],
  "artifact": {
    "path": "/absolute/new-edited.png", "sha256": "...", "byteLength": "12345",
    "width": 640, "height": 426, "format": "png", "colorSpace": "sRGB", "bitDepth": 8
  },
  "render": {
    "sessionId": "...", "photoId": "...", "representationId": "...",
    "recipeCommitId": "...", "recipeSnapshotDigest": "..."
  }
}
```

`byteLength` is a decimal string; dimensions/bitDepth are integers. Artifact SHA-256 is taken from
the encoder's same staging file before exclusive publication, not by reopening a mutable output
path. Recipe/representation/digest come from the frozen export item, not a later mutable head.
The session ID agrees with stdio snapshots. The private Catalog/Recipe is removed on normal exit;
render IDs are receipts, not a later continuation handle.

`outcome` is `completed`, `cancelled` or `failed`; only completed contains `artifact` and `render`.
Exit codes are respectively 0, 2 and 1. Result publication failure also exits 1. Invalid launch
requests fail before admission and may have no result. A crash/no terminal result is incomplete.
Output and result publish as new files only. On failure/cancellation a completed output may remain;
it is not an eligible Candidate without a completed verified receipt. Another owner's output or
result is preserved. Retrying uses new paths/session; IDs are correlation, not durable replay keys.

Clean idle stdio `shutdown` cancels the bound session and drains its acknowledgement after terminal
publication. Pipe EOF detaches AI and leaves the human window alive. Preview cancellation remains
supported before publication; an executing apply/export is not interruptible. The UI's stop/cancel
is best effort around the existing export queue, not a new renderer cancellation promise.

Shape imports only a completed verified receipt as a Candidate, deduplicates by session/artifact,
and performs its own exact input-head CAS on explicit Accept. Return, export and shutdown never
implicitly Accept a new Shape head.

## Rendering and tests

PNG enters the native raster provider's common linear RGB16 contract. RGB8 samples use Shadow's
existing sRGB ICC transfer table; alpha is verified before removal. Cold preparation decodes the
whole bounded PNG; gestures reuse the existing warm source and recompute their normal node suffix.
No new GPU transfer, cache protocol or renderer is introduced. Provider identity includes the PNG
implementation. Preview/detail/export use the same Recipe at their existing resolutions.

`shadow-image-png-decoder-contract`, `shadow-desktop-pipeline-launch` and
`shadow-desktop-external-edit-stage` cover the native input, strict binding and real process boundary.
The integration uses synthetic PNGs, real stdin/stdout and the packaged human-owner smoke route;
its measured timings are smoke observations, not a comparative performance benchmark.
