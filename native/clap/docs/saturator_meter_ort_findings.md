# Saturator (parked) + meter + ORT infrastructure — findings

Module 9 (final) of the DSP perf/quality pass. Covers the parked Saturator, the
shared LoudnessMeter, and the ORT session wrappers.

## Saturator (RationalA) — QUALITY finding: aliasing (no oversampling)

The Saturator is a static rational nonlinearity (`RationalA`, degree-6
numerator / degree-5 denominator, coefficients from
`weights/axon_bundle/saturator/plugin_meta.json`). It is **parked** (StageID 2,
intentionally not in the default chain) and runs the nonlinearity **at base rate
with no oversampling** (`axon_plugin.cpp`, `chain.saturator.eval(...)` per
sample). A degree-6 polynomial generates harmonics up to 6× the input, which
fold back across Nyquist.

Measured alias energy (9 kHz sine @ 0.6, alias products in the inharmonic
0.8–2.5 kHz band, relative to the fundamental):

| | alias / fund |
|---|---|
| base rate (current) | **3.86e-2  (~-28 dB)** |
| 8× oversampled | 1.11e-4  (~-79 dB) |

So the current saturator dumps ~**350× more** alias energy than an oversampled
version. The Exciter already solves exactly this with a 4× polyphase
windowed-sinc up/down FIR — the same treatment would fix the saturator.

**Why not shipped:** adding an oversampling FIR **adds latency** (the saturator
bundle declares `latency_samples: 0`, `receptive_field: 1`), which changes the
stage's host-reported latency and the chain's delay compensation. Per the
workflow's stop conditions, a latency change is a product decision, not a
refactor. It is also a QUALITY change (intentionally changes the sound), so it
needs an A/B sign-off. **Recommendation:** if the saturator is ever un-parked
for the default chain, add 4× oversampling around the `eval()` call (mirror the
Exciter), accept ~Exciter-class latency, and re-validate THD/alias A/B.

`RationalA` itself is otherwise efficient: ~4.7 ns/sample/ch, stateless, no
alloc/modulo/branches in `process()`. Deliverable: `bench/bench_saturator.cpp`.

## LoudnessMeter — NO CHANGE (already good)

BS.1770-4 K-weighting (pre + RLB biquads) + sub-block accumulation for
short/momentary LUFS, plus decaying sample-peak and a 300 ms RMS.

- Per-sample hot path is 4 biquads (stereo) + peak + RMS accumulate — necessary
  for LUFS; no allocation; the only modulos (`% ring_n_`, `% rms_n_`) fire once
  per ~100 ms sub-block, not per sample, so they are immaterial.
- It is a metering tap (runs once on the master bus, not per chain stage).
- Coefficients are standard BS.1770; accuracy was already cross-checked against
  LufsLeveler. RT-safe. Nothing worth changing.

## ORT session infrastructure — RT-safety smell (documented, not perf)

Two wrappers exist:

- **`OrtMiniSession`** (Axon composite; used by auto-EQ, saturator-ORT path, and
  bus comp). `run()` / `run_controller()` allocate small `std::vector`s per call
  (`inputs`, `in_names`, `out_names`) and use the *returning* form of
  `Ort::Session::Run` (which allocates the output `std::vector<Ort::Value>`);
  stateful models also do `unordered_map<string,…>` lookups and an O(n²)
  output-name search. These run once per hop and are **microseconds against a
  millisecond-scale model forward** (see `bus_comp_perf_findings.md`), so they
  don't move measured performance — but they are heap allocations on the audio
  thread (an RT-safety smell).
- **`OrtSession`** (the *other*, single-model plugin): its header documents an
  `Ort::IoBinding` + pre-allocated-tensor design ("`Run()` never allocates") and
  declares `rebind_()`, but the `.cpp` implements neither — `run()` allocates
  per call exactly like `OrtMiniSession`. The documented design is the right one.

**Recommendation (hardening, not speed):** implement the `Ort::IoBinding`
path (pre-bind input/output tensors to owned buffers, reuse across calls) so the
audio-thread inference is allocation-free. This is correctness/robustness work
with bit-identical output; it won't measurably change CPU because the model
forward dominates.

## Summary
- Saturator: real **aliasing** quality gap (~-28 dB vs ~-79 dB) — fix = 4×
  oversampling, but it alters latency → your call (and it's parked).
- Meter: no change.
- ORT: per-call audio-thread allocations — implement the already-sketched
  IoBinding as a separate hardening task.
