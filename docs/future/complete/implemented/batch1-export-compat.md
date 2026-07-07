# Make the plugin safe for batch-1 auto-EQ exports (activate-time check + adapter)

Status: complete/implemented
Opened: 2026-07-06
Issue: #24
Concluded: 2026-07-06
Outcome: Shipped Option 1 (activate-time batch guard) — a batch-1 auto-EQ
export now fails activation with an actionable message instead of crashing the
host on the first process() call. Batch-2 path unchanged.

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

## Outcome

Shipped 2026-07-06 (branch `fix/batch1-export-crash`), **Option 1 — the
activate-time guard** (the minimal, low-risk fix; Option 2's shape-driven
run_controller is deferred and can still be done later without re-opening the
crash).

What changed:
- New shared helper `native/clap/src/ort_shape.hpp` — `ort_input_batch(session,
  "audio_in")` reads the model's declared batch dim via the ORT C++ API.
- `OrtMiniSession::audio_in_batch()` wraps it; `plugin_activate_impl` calls it
  right after each auto-EQ controller session is constructed and throws
  `std::runtime_error` if the batch dim != 2. The existing activate try/catch
  turns that throw into a clean activation failure the host reports — instead
  of the uncaught audio-thread exception (`std::terminate`) a batch-1 bundle
  used to cause at first process().

Verification:
- New `test_batch_probe_rejects_batch1` (in `test_ort_session.cpp`) builds the
  embedded batch-1 Model A and asserts `ort_input_batch` reports batch=1 (→ the
  guard rejects) and -1 for a missing input. `test_ort_session`: ALL 12 passed.
- `uv run axon test`: 30/30 green — the shipped batch-2 bundle activates and
  processes exactly as before (guard is a no-op when batch==2).
- No render change to the batch-2 path (the guard only runs at activate and
  only throws on a non-conforming bundle).
