# Replace ORT with a hand-rolled C++ cell for the auto-EQ LSTM controller

Status: not-started
Opened: 2026-07-06
Issue: #16

The biggest remaining sound-preserving CPU win in the chain: take the neural
controller from ~26% of process CPU to ~1–2%.

## Why / evidence (measured 2026-07-05)

- The per-class controller model is a **single-timestep** LSTM (the whole
  128-sample block is one input vector; `receptive_field: 1` in each
  `weights/axon_bundle/auto_eq_*/plugin_meta.json`): frontend Reshape/Transpose
  → 2 LSTM cells (hidden 64) → small MatMul head → Tile ×128. Total ≈ 100k–2M
  MACs per call — single-digit microseconds of arithmetic.
- Yet the batched ORT call measures **~92 µs/block** in-chain (AutoEqOrtCtrl
  sub-timer). The cost is ONNX Runtime per-node dispatch across ~20 graph
  nodes, not math: alloc-elimination measured only ×1.03, and batch-2 (fewer
  Run calls, same node count per call) only ×1.20 (87.6 vs 105.4 µs in C++).
- Therefore a native implementation should land 10–25× faster: controller
  ~26% of process CPU → ~1–2%. It also removes all audio-thread ORT
  allocations for this path (see `ort_audio_thread_allocations.md`).
- The batch-2 rewiring (2026-07-05) already hoisted the controller to ONE
  call per block at plugin level — the exact seam a native cell drops into
  (`OrtMiniSession::run_controller` caller in `axon_plugin.cpp`).

## Plan

1. Weight extraction at export/build time: a small script reads each class
   ONNX (LSTM W/R/B tensors, head MatMul weights, frontend constants) and
   generates a header or binary blob (follow the `adaptive_eq_targets.hpp`
   generated-header precedent). 5 classes ≈ 5 × ~10k params ≈ small.
2. `src/lstm_controller.hpp`: two LSTM cell steps (sigmoid/tanh gates, vDSP
   GEMV) + head + the exact frontend normalization order from the graph.
   Match ONNX LSTM activation semantics exactly (default f=Sigmoid,
   g=h=Tanh; check for clip attributes).
3. Validate per class against ORT block-for-block on random state-carried
   sequences: target ≤ 1e-6 param deviation (the batch-2 change was accepted
   at ~1e-7 with a −131 dBFS render null — same bar family).
4. Wire behind a compile-time or runtime fallback to ORT initially; remove
   ORT for this path once trusted.

## Acceptance

- Per-class output match vs ORT ≤ 1e-6 over ≥2000 state-carried blocks.
- `uv run axon eval null` vs pre-change bundle: IDENTICAL-class result
  (ORT-flake retry protocol applies).
- `uv run axon test` green; new `test_lstm_controller` KAT vs recorded ORT
  outputs.
- `uv run axon bench` instrumented: AutoEqOrtCtrl mean ≤ 10 µs/block
  (from ~92), total process CPU −20% or better on the neural path.
