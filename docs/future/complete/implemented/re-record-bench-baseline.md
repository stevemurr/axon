# Re-record bench/baseline.json from a clean Release build

Status: complete/implemented
Opened: 2026-07-06
Issue: #20
Concluded: 2026-07-06
Outcome: baseline re-recorded by the owner from the clean Release build the same day the idea was filed

Small task, real payoff: regression diffs are currently meaningless.

## Why / evidence

- The committed `native/clap/bench/baseline.json` was measured (pre
  2026-07-05) on a build whose CMake cache had an EMPTY `CMAKE_BUILD_TYPE` —
  no optimization. Every properly built plugin since is ~2–3× faster (bypass
  RTF 4.8 baseline vs ~15 now), so `uv run axon bench`'s diff column reads as
  huge "improvements" noise and the exit-code-2 regression gate can't fire
  meaningfully.
- The build path now guards against non-Release caches (build.sh), so a
  re-recorded baseline stays honest.
- The chain also changed since (ssl_comp resize, mel vDSP, batch-2 LSTM,
  display decimation) — the baseline should capture today's shipped truth.

## Plan

1. Clean non-instrumented build: `uv run axon build` (guards enforce
   Release; verify no `axon.stage-timing` marker).
2. `uv run axon bench -- --update-baseline` on a quiet machine (the runner
   refuses instrumented data by design; use default iters for stable p99).
3. Commit baseline.json with the environment noted in the commit message
   (machine, macOS, sample rates covered).

## Acceptance

- Immediately after: `uv run axon bench` reports ~0% deltas and exit 0.
- Introduce a deliberate slowdown locally (e.g. run with AXON_STAGE_TIMING
  instrumented build via --bench-bin override) and confirm the regression
  gate fires (exit 2) — proves the gate is live again.

## Outcome (2026-07-06)

Done by the owner directly (`uv run axon bench -- --update-baseline`) from the
clean Release build, same day this idea was filed. New baseline: bundle =
build/Axon.clap (repo path, previously /tmp from the pre-rename era), 30 cells,
non-instrumented by construction (the runner refuses per-stage data).
Regression diffs are meaningful again; the acceptance's deliberate-slowdown
gate check remains a good habit for the next bench change.
