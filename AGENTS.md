# Concurrent workspace coordination

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

- Treat the canonical `build/desktop-dev` and `build/desktop-release`
  directories as integration resources. Only the release integrator runs a
  full configure/build there, after the relevant source claims are released.
- While a source lease is active, use a lease-specific CMake build directory or
  `CARGO_TARGET_DIR` for focused checks. Do not concurrently reconfigure or
  link the canonical desktop build just to test a private module.
- A full build has a short, explicit **validation lease**: record the source
  scopes it validates, the command, and the result. It owns no source file and
  ends immediately after the check, so it cannot block productive work.
- Keep every first-release boundary buildable. Develop a broad change in a
  private module first; take the shared facade only for a small additive
  mapping, compile it, and release. Never retain a facade solely because an
  intermediate cross-layer change is currently broken.
