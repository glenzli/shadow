# Concurrent workspace coordination

Before planning or changing code, read `DEV_SKELETON.md`; when reviewing or making a substantive
change, also read `REVIEW_SKELETON.md`. These files contain durable orientation only. Verify all
implementation facts through the source-first navigation rules below.

When multiple agents or threads edit this workspace, use the personal
`$coordinate-shared-workspace` skill before making new writes.

- Keep active claims and cross-thread messages under the local, ignored
  `.agent-coordination/` directory.
- Claim a coarse semantic scope and the likely write paths; do not claim the whole repository.
- Treat existing dirty files as another agent's work until ownership is known.
- Read active claims before crossing module boundaries. If write scopes overlap, communicate and
  choose one owner for the shared contract before continuing.
- Never clean, revert, stage, reformat, or commit changes owned by another claim.
- Release or hand off the claim after focused validation.

The coordination protocol is advisory and should stay lightweight. Agents may write clear
Markdown or JSON directly when the helper script is unnecessary.

## Repository as the navigation index

Treat the source tree, module declarations, crate READMEs, and module-level documentation as
Shadow's canonical architecture index. Do not create a separate exhaustive knowledge base that
duplicates them.

- Start at [`docs/README.md`](docs/README.md), use the
  [repository map](docs/architecture/README.md) to choose a subsystem, then use the nearest crate
  or application README and its entry module to find the semantic owner. The root
  [`README.md`](README.md) remains the product-facing entry.
- Keep `lib.rs`, `main.rs`, CXX/Qt facades, and top-level QML controllers readable as indexes and
  composition boundaries. Put new state machines, persistence policies, protocols, and feature
  behavior in responsibility-named child modules.
- When adding, extracting, or renaming a responsibility, update the nearest code-owned index in the
  same change. Describe ownership and the next navigation step; do not duplicate implementation
  narratives across several documents.
- Before a substantial edit to a growing hub, apply `$maintain-source-cohesion` and record the
  keep-or-extract decision. Known review targets include both bridge `lib.rs` files, desktop
  controllers, and large Precision QML surfaces.
- Keep private-invariant tests adjacent to their implementation but outside production source
  files. Move multi-domain entry-point tests into responsibility-named contract modules; reserve
  top-level `tests/` for public cross-module behavior. Test fixtures must also obey the local
  payload boundary below.
- Refactor only the responsibility exposed by the current change. A legacy hotspot is a review
  trigger, not permission for unrelated repository-wide cleanup.

### Canonical test topology

Shadow deliberately does not keep executable test bodies inline in production source. This removes
the subjective “still small enough” threshold that allowed large hidden test suites to accumulate.
API doctests and compile-time assertions may remain with the contract they document; ordinary test
functions may not.

Rust tests use one owner-controlled topology:

- A production owner may end with `#[cfg(test)] mod tests;`, but its test implementations start in
  `<owner>/tests.rs`, never inside the production `.rs` file and never in a sibling
  `<owner>_tests.rs`. Do not use `#[path]` to redirect `mod tests` into a shared or distant test
  directory; the filesystem location must identify the production owner directly.
- When one owner needs several test responsibilities, replace that single file with
  `<owner>/tests/mod.rs` and responsibility-named children. The directory already says “tests”, so
  children do not repeat `_test` or `_tests` and may not use generic names such as `test.rs`,
  `misc.rs`, or numbered parts.
- `src/tests/` is reserved for private contracts owned by the crate facade and intentionally
  spanning multiple sibling modules. Its children use `<responsibility>_contract.rs`; owner-local
  tests must not drift into this central tree merely for convenient private access. The facade
  registers children but does not inject a wildcard import or cross-domain prelude: every child
  imports its production contracts and named fixtures directly.
- A crate-level `tests/` directory is reserved for black-box contracts that consume the real public
  crate API. These tests may not use `#[path]`, `include!`, or an equivalent source inclusion to
  compile a private production file a second time.
- The production owner declares its own private test module. A distant facade must not register
  sibling `<owner>_tests.rs` files on the owner's behalf.
- Fixtures and support code live with the narrowest test owner. Promote them only when multiple
  semantic owners actually reuse them, and name the promoted module by the fixture or harness
  contract rather than `support` alone.

Native tests follow their build-system idioms while preserving the same semantic levels:

- C++ and Qt test sources live under the subsystem `tests/` tree. Image-kernel cross-translation-
  unit contracts use `<responsibility>_contract_test.cpp`; focused desktop component tests use
  `<responsibility>_test.cpp`.
- A multi-file native suite may use a responsibility-named directory with a thin runner, but may
  not hide behavior in a generic `test.cpp`. Fixtures, plugins, and reusable support are named and
  grouped separately from runnable sources.
- Every runnable native test source maps to exactly one build target and one test-runner
  registration. Moving a test also moves its compile definitions, dependencies, environment,
  labels, timeouts, and fixture contract. When a test property is assigned conditionally, inspect
  the generated CTest registry so a later assignment cannot silently overwrite the earlier gate.
- A CTest invocation does not establish source freshness. After changing native sources, headers,
  manifests, or test contracts, build the exact affected targets first and then run those freshly
  linked artifacts or their CTest registrations; an old passing executable is invalid evidence.
- A QML source-path load is a focused component test, not proof that the shipped module can reach
  that component. Each newly registered component family needs at least one test or startup path
  that resolves it through the packaged module/resource graph; real-window acceptance remains the
  evidence for geometry, overlap, spacing, focus, pointer input, and runtime language switching.

`cargo xtask test-layout` enforces this Rust topology as an absolute zero-debt gate and runs first
in `cargo xtask check`. Shadow has no remaining inline executable tests, sibling
`*_test.rs`/`*_tests.rs` owners, private production source inclusions, permanently disabled test
code, or implementation-bearing `tests/mod.rs` facades. Do not reintroduce a baseline or numerical
allowance for those categories; a new finding is a structural error, not newly budgeted legacy.

### Desktop localization contract

English `tr()`/`qsTr()` source text is the canonical desktop message identity. Simplified Chinese
must remain a complete product presentation, not a best-effort catalog that silently falls back to
English:

- Route every user-visible desktop message through Qt translation. Technical tokens such as
  `RGB`, `LUT`, camera formats, numeric patterns, and schema identities may intentionally remain
  language-neutral; ordinary labels, status, errors, actions, and accessibility text may not.
- A production message has exactly one finished, non-empty Simplified Chinese entry. Do not commit
  `unfinished`, stale, duplicate, or placeholder-mismatched messages, even when the XML contains
  text: `lrelease -nounfinished` would discard them and create a mixed-language UI.
- Run `cargo xtask desktop-i18n-check` whenever production QML/C++ text or the Chinese catalog
  changes. The canonical desktop build and release commands run the same exact extraction,
  message-set, placeholder, and QM-compilation gate before compiling the application.

## Risk-scaled freedom

Use the least ceremony that keeps the current work recoverable. The protocol supplies defaults,
not a project-management state machine:

- For read-only exploration and isolated source modules, agents may choose their own task shape,
  validation depth, negotiation mode, and handoff timing.
- For shared contracts, agents should negotiate an owner and checkpoint, but may deviate from the
  default flow when they record the reason, affected scopes, bounded validation, and next decision
  point. An explained exception is preferable to an artificial claim that no longer matches the
  work.
- Only three boundaries are hard guards: never take over another owner's paths without owner/user
  authorization; never overwrite, clean, stage, or commit another owner's work; and serialize
  shared Git index/HEAD or canonical build-directory mutations through one temporary steward.
- An active claim does not freeze independent work or an unrelated scoped commit. Stop only when
  paths, semantic contracts, or shared mutable resources actually overlap.

Prefer the helper for concurrent claim, acknowledgement, and release mutations because it makes
those writes atomic. Direct records remain valid for recovery, interoperability, or a genuinely
simpler one-off note; preserve the documented fields so other agents can still understand them.

## Local shared-workspace transport boundary

This repository currently uses the **`local-shared-workspace`** topology: all
participating agents work on the same machine and the same live worktree.
Claims, handoffs, and messages coordinate authority over that worktree; they
are not a request to copy it to another agent. Source edits become visible
through the shared filesystem, and a handoff carries only intent, paths,
contracts, and validation evidence.

- Treat the repository root as sync-sensitive. Never create, attach, archive,
  or hand off a worktree snapshot, build artifact, RAW/DNG, rendered analysis
  image, cache, catalog, or other binary payload merely to collaborate. Pass a
  path, stable identity, digest, dimensions, or bounded diagnostic summary
  instead.
- A source image enters model context only through an explicit, bounded action
  such as rendering one display-sized preview. Do not read, attach, encode, or
  paste RAW files or bulk fixtures into a conversation. Use metadata and local
  paths by default; a visual inspection must name one file and its purpose.
- Build output, Cargo targets, generated previews, downloaded models, caches,
  and fixture corpora must live **outside** this worktree. The guarded payload
  paths are `target/`, `build/`, `local-reference/sample-assets/`, and
  `local-reference/experiment-output/`. Small local coordination records and
  the checked local skill references are not binary handoff channels.
- Before a write-heavy validation command, run
  `sh scripts/local_shared_workspace_guard.sh`. A failure is a transport
  blocker: do not begin another build in the source root. Report the remaining
  local payload to the user or release integrator and use an external build
  directory instead.
- Use an absolute task-private `CARGO_TARGET_DIR` and `SHADOW_BUILD_DIR` outside
  the worktree for concurrent checks. The tracked defaults deliberately resolve
  to sibling `.shadow-local-*` directories, not paths beneath this repository.
  A shared external default is suitable only for one local developer; concurrent
  agents must choose separate directories.
- A change to distributed, cross-machine collaboration requires an explicit
  user-approved topology change. Define a small transfer manifest first; it
  may contain source patches and named, size-bounded artifacts, never an
  implicit workspace snapshot.

## Identity and continuity

- Use a stable task/scope id and an owner id that distinguishes the current task/thread and agent;
  keep a friendly display label separate. Generic owners such as `codex-root` are insufficient
  when several agents can use the same role name.
- Claims describe current write authority, not the whole history of a long task. Preserve durable
  intent in checkpoints/handoffs: objective, base revision, exact paths, accepted contract,
  validation evidence, remaining work, and next safe owner.
- Classify messages as `conflict`, `decision`, `handoff`, `takeover`, `validation`, or ordinary
  `info`. Mark conflicts, decisions, takeovers, and required handoffs for acknowledgement; the
  recipient checks action-required messages at natural boundaries and acknowledges them before
  crossing the affected contract.
- Treat unclaimed staged or modified paths as an ownership gap to investigate, not as free work.
  Task continuity and source ownership are related but separate: releasing a claim must not erase
  its handoff evidence, and a waiting record never grants write authority.

## Negotiation before locking

Claims protect an agreed execution boundary; they are **not** the way agents
decide how work should be divided. Before treating an overlap as a blocker,
agents should briefly reason together about the shape of the work and choose
the least expensive collaboration mode:

- **Independent slices:** each agent owns a semantic module and publishes a
  stable contract for the other to consume.
- **Sequenced handoff:** one task naturally creates the API, data, or test
  fixture the next task needs; use a short lease and a concrete handoff.
- **Shared transaction:** when several requested outcomes genuinely require
  one façade, schema, or interaction surface, nominate a temporary transaction
  steward. The steward owns the final edits and validation of that shared
  boundary; other agents may supply investigation, a proposed patch shape,
  tests, or a small adjacent implementation through the same conversation.
- **Exploration first:** when the dependency shape is uncertain, one agent may
  investigate read-only and return a recommendation before anyone reserves the
  contested path.

Use a compact message or handoff record to propose the mode, the desired
outcome, the durable contract, and the next decision point. A steward may
accept a contributor's coherent subtask, ask for a narrower contribution, or
defer it in favor of the main milestone. Prefer a useful, reasoned response to
mechanical queueing. If discussion does not make the work cheaper or the
participants are unavailable, fall back to ordinary short claims and waiting
records; never turn a negotiation into an unbounded hold on a shared file.

For a shared transaction, do not give multiple agents silent concurrent write
authority over the same code. Share context freely, but leave one explicit
integrator responsible for the final combined diff, validation, and handoff.
The integrator may deliberately absorb a nearby task when that is cheaper than
another lease, provided it announces the expanded outcome and still releases
the contested path at a real checkpoint.

## Deferred dependencies

An active claim that blocks a dependent change is a **waiting state**, not an
active claim of its own:

- Do not reserve the blocked paths or poll their owner. Create a compact record
  in `.agent-coordination/waiting/<scope>.md` with the owner/claim awaited,
  exact unblock condition, needed handoff contract, resume validation, and a
  non-conflicting fallback task.
- Notify the owner once that a handoff is awaited. Then continue with the
  fallback task (or finish the turn); do not invent work merely to stay busy.
- Recheck waiting records only at natural checkpoints: an owner message, the
  end of a focused task, a new user request, or a normal coordination health
  check. A waiting record never reserves files.
- Once unblocked, read the latest claim and handoff, claim only the paths still
  needed, validate the boundary, and mark the waiting record resolved. Treat a
  stale or changed contract as a fresh dependency check.

## Key blockers: notify or monitor

A blocked key task needs an explicit user-facing decision: either stop and
surface the decision now, or monitor a bounded, mechanically observable
condition and resume safely. Do not silently turn either case into indefinite
waiting.

- **Stop and notify the user** when progress needs a product, priority, scope,
  budget, authority, security, data-loss, external-coordination, or conflicting
  contract decision. Also notify when the awaited owner is unavailable or the
  unblock condition cannot be stated and verified precisely.
- **Monitor and resume** only when the blocked boundary, owner, exact unblock
  signal, and resume validation are all known; no code must be changed while
  waiting; and the user has not asked for a decision at the boundary. Record a
  waiting state, use a bounded recurring check rather than busy polling, and
  preserve the user's notification preference.
- **Escalate a monitor** when its condition changes, its checks fail, the owner
  becomes stale, or waiting would materially delay the agreed outcome. Report
  the evidence, the safe alternatives, and the next decision required; never
  continue monitoring merely to avoid asking.
- On a successful monitored handoff, re-read the current claim and contract,
  take only the newly available narrow scope, run the recorded validation, and
  report that work has resumed.

## High-contention paths: short leases

Public facades and interaction hubs — for example bridge `lib.rs` files,
desktop backends/controllers, shared QML dialogs, and catalog entry points —
are **high-contention paths**. Treat them as short leases:

- First move substantive work into an owned service/module. Claim the shared
  facade only for the narrow wiring and validation boundary; do not keep it
  while exploring or performing bulk implementation.
- Every high-contention claim must state its first releasable milestone and
  handoff contract. Release it immediately after that milestone, even when
  follow-up work remains in private modules.
- If another task is known to wait on the path, send the handoff at that first
  boundary and either transfer the narrow lease or explicitly say why it must
  continue. Heartbeats should report the next release boundary, not merely
  that work is ongoing.
- Do not repeatedly reclaim a hub without material progress or a changed
  contract. Downstream work should use a waiting record and an independent
  task until the next real handoff point.

## Path stewards and micro-handoffs

When a high-contention path is already correctly leased, its owner is the
temporary **path steward**. A waiting task may ask that steward to perform a
small adjacent integration instead of waiting for a full release:

- Put the request in `.agent-coordination/handoffs/<scope>.md` and message the
  steward. State the exact files/symbols, durable contract, acceptance test,
  stop condition, and the work that is explicitly out of scope. The request
  reserves nothing.
- The steward must explicitly accept, defer, or decline at a natural
  checkpoint. Acceptance incorporates the narrow paths into its own claim and
  produces a handoff message on completion; it does not silently transfer task
  ownership.
- Keep a micro-handoff cohesive: one data-flow or interaction slice that can
  be tested independently. If it opens unrelated dependencies, changes the
  steward's main milestone, or cannot be explained in one compact record,
  decline it and let the requester wait or take a separate lease.
- Prefer this over a file-level "merge": shared workspaces already share the
  files. What must move safely is intent, contract, validation, and the right
  to edit a narrow boundary.

## Lease liveness, pause, and recovery

Claims must make liveness and recovery visible; a timestamp is evidence of
activity, not permission to overwrite an unavailable owner's work.

- A claim should carry `heartbeat_at`, a concrete `first_release` milestone,
  and, when it depends on another active scope, a `depends_on` list. Refresh a
  heartbeat only after material progress or a checked validation boundary.
- A **pause** means the owner stops editing. Record `status: "paused"`, a
  checkpoint (base revision, owned paths, current validation, and known
  failure), and a precise `resume_condition`. Release high-contention paths at
  pause unless one small, already-running integration transaction cannot be
  split safely.
- A claim whose heartbeat exceeds the configured health threshold is a **stale
  candidate**, never an automatically abandoned claim. Send one takeover
  request, record a waiting state, and surface the evidence to the user if the
  owner cannot confirm the handoff. Only an explicit user decision or an owner
  release authorizes takeover of the paths.
- Do not break a wait cycle by claiming both sides. Declare the dependency,
  then replace the cycle with either a sequenced handoff or one named shared
  transaction steward.

Use `cargo xtask coordination-health` for a non-destructive audit. It reports
stale/malformed claims, missing first-release boundaries, oversized hub leases,
declared dependency cycles, and overlapping path claims. It never deletes,
steals, stages, or takes over a claim.

Use `cargo xtask coordination-health --commit-gate` after staging an explicit
commit scope. It rejects unreadable ownership, malformed cached diffs, and
staged paths that overlap an active claim, while allowing independent active
work to continue. Use `--bulk-stage-gate` before a full-worktree staging
operation; that stricter gate requires an empty index and no active or
unreadable claims.

## Checkpoints, staging, and shared transactions

An interrupted agent should leave a recoverable checkpoint, not a hidden
half-commit or a worktree-wide stash.

- A checkpoint records the base revision, exact owned paths, validation command
  and result, incomplete contract, and the next safe owner. It belongs under
  `.agent-coordination/handoffs/` or the paused claim, and does **not** reserve
  extra paths.
- Never use `git stash` as cross-agent coordination and never stage the whole
  worktree while another claim is active. The agent performing a commit becomes
  the temporary release integrator: it acquires a short exclusive lease for
  `.git/index` and `.git/HEAD`, stages only exact owned or handed-off paths,
  inspects the cached diff, runs `coordination-health --commit-gate`, and
  creates one coherent semantic commit. Release the Git lease immediately
  afterward.
- Contributors may stage or commit their own complete independent slice instead
  of waiting for a permanent central integrator, provided they own every staged
  path and hold the Git lease. If a file mixes multiple owners or contracts,
  nominate one transaction steward or split its hunks with an explicit
  manifest; do not guess ownership from the filename.
- An atomic full-worktree commit is exceptional. Run
  `coordination-health --bulk-stage-gate` before staging, then run
  `--commit-gate` on the resulting index. Never use `git add .` or `git add -A`
  as a shortcut around an unresolved ownership inventory.
- When several modules truly need one shared façade, name a **transaction
  steward** in the claim. Contributors keep their private implementation
  slices; the steward owns only final wiring, one combined validation, and the
  release/commit handoff. A steward must publish a first checkpoint before
  accepting any adjacent follow-up.
- If a task must be stopped intentionally, the owner publishes the checkpoint,
  releases the claim, and lets the next agent take a fresh semantic lease. This
  is cheaper and safer than keeping a special agent permanently parked on UI or
  bridge paths.

## Build outputs are shared mutable state

The source worktree is not the only concurrent resource. A CMake/Ninja build
directory and a Cargo target directory can race while they regenerate files or
link artifacts, producing misleading compile errors or a corrupted incremental
state.

- The CMake presets and Cargo configuration default to sibling
  `.shadow-local-build/` and `.shadow-local-target/` directories outside the
  repository. `target/` and `build/` beneath the worktree are quarantined
  legacy payloads, not canonical integration resources; do not recreate them.
- While a source lease is active, use a lease-specific external CMake build
  directory and `CARGO_TARGET_DIR` for focused checks. Do not concurrently
  reconfigure or link another task's external directory just to test a private
  module.
- A full build has a short, explicit **validation lease**: record the source
  scopes it validates, the command, and the result. It owns no source file and
  ends immediately after the check, so it cannot block productive work.
- Keep every first-release boundary buildable. Develop a broad change in a
  private module first; take the shared facade only for a small additive
  mapping, compile it, and release. Never retain a facade solely because an
  intermediate cross-layer change is currently broken.

### Canonical runnable debug build

Task-private build directories are validation workspaces, not user-facing application locations.
The only stable debug entry is `../.shadow-local-build/current-debug/Shadow.app`, launched from the
repository with `scripts/run_debug.sh`.

- A task may report its private build path as validation evidence, but must not present that path
  as the application's normal launch command.
- Only a temporary **canonical debug build steward** may mutate `current-debug`. The steward claims
  the exact pseudo-path `.agent-coordination/resources/canonical-debug` with semantic write
  `release:canonical-debug`. Reuse those identities for every promotion; aliases such as
  `canonical-debug-current`, `canonical-debug-build`, or `canonical-debug-promotion` do not name
  separate resources. The steward waits for all source contracts included by the build to reach
  buildable handoffs and validates the complete current-source application rather than assembling
  a shell and helper from different checkpoints.
- Before promotion, run the local workspace guard, the canonical desktop build and localization
  gate, startup/edit smoke coverage, and any installed private-provider smoke relevant to the
  changed decoder boundary. Record the exact validation label and source scope in the handoff.
- Promote with `scripts/promote_debug_build.sh /absolute/path/to/Shadow.app <validation-label>`.
  Promotion copies the candidate into an immutable revision/timestamp directory and atomically
  advances `current-debug`; it never mutates an app that the user may already be running.
- The promotion script's physical directory lock is a final race guard, not the logical claim.
  Only an atomic already-exists result means another steward may hold it. Permission, sandbox,
  missing-parent, read-only-filesystem, and malformed-lock failures are environment blockers and
  must preserve their real error category instead of being reported as contention.
- If promotion needs user authorization or an environment change, pause the canonical debug claim
  with the candidate checkpoint, exact operation, `release:canonical-debug`, stable error kind,
  and resume condition. Do not release a claim while promising to resume the same promotion. After
  authorization, recheck the physical lock, `current-debug`, candidate identity, and source base;
  resume with that evidence before retrying. If a prior task released the claim, reacquire the
  exact canonical resource first.
- The owner of a task-private build may delete it only after its source handoff and any required
  promotion are complete. The promotion steward does not delete another task's directory. Retain
  the current promoted release and at least one previous release for rollback; prune older promoted
  releases only while holding the canonical build lease, never by following or deleting the
  `current-debug` symlink target indirectly.
