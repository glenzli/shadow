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
