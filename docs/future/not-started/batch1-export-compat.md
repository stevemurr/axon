# Make the plugin safe for batch-1 auto-EQ exports (activate-time check + adapter)

Status: not-started
Opened: 2026-07-06
Issue: #24

Crash bug found by the training-coupling investigation: the documented
retrain -> export -> install flow currently ships a plugin that blows up on
the audio thread.

## Why / evidence

- Fresh `nablafx-export` auto-EQ controllers are **batch-1** (audio_in
  [1,1,128], state [2,1,64]). The shipped bundles are **batch-2** after the
  2026-07-05 in-place surgery (commit 84e135b), and
  `OrtMiniSession::run_controller` hard-codes the [2,1,T] feed and batch-2
  param/state offsets (axon_plugin.cpp ~517-580).
- There is NO activate-time batch check: a batch-1 bundle enumerates and
  activates cleanly, then ORT rejects the first Run — an uncaught exception
  on the audio thread (plugin_process has no try/catch by design; see the
  cleanup-pass deferred list).
- The adaptivity probe is already shape-driven and handles both; the plugin
  handles only batch-2. The asymmetry is documented in the README Training
  section but should be fixed, not documented.

## Plan (either, or both)

1. **Activate-time guard** (minimal): read the session's audio input batch
   dim at activate; if != 2, fail activation with a clear message naming the
   fix (run the batch-2 surgery script on the export). Cheap, no behavior
   change for good bundles.
2. **Proper fix**: make run_controller shape-driven like the probe (batch
   from the session, per-batch offsets derived), so batch-1 exports simply
   work (mono-feed duplication becomes unnecessary for them). Alternatively
   fold the batch-2 surgery into `axon autoeq export` so exports are always
   batch-2 before they reach weights/.

## Acceptance

- A synthetic batch-1 bundle either activates and processes correctly
  (option 2) or fails activation with an actionable error (option 1) — unit
  test with a surgically-downgraded model.
- Shipped batch-2 path unchanged: suite green, eval null byte-identical.
