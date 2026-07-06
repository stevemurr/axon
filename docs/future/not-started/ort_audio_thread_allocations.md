# ORT audio-thread allocations (RT-safety debt)

Status: not-started
Opened: 2026-07-06
Issue: #18

Severity: Severity: no measured throughput cost (~0.2%); this is
real-time-safety hygiene — heap alloc/free on the audio thread risks priority
inversion under memory pressure, not slowness.

## The debt

`OrtMiniSession` (native/clap/src/axon_plugin.cpp) heap-allocates on the audio
thread on **every inference call**:

- `run()` (ssl_comp TCN, once per 1024-sample hop per... now once per hop,
  both channels batched): per-call `std::vector<Ort::Value> inputs` +
  `std::vector<const char*>` name arrays, an `Ort::Value` wrapper per tensor,
  and the *returning* form of `Ort::Session::Run` which allocates the output
  vector AND the output tensors themselves, followed by a copy-out.
- `run_controller()` (auto-EQ LSTM, once per 128-sample block): same pattern
  (~345 calls/sec at 44.1 kHz), plus `unordered_map` state-buffer lookups.
- One-time lazy `ctrl_stack_` allocation on the first block (batch-2 stacking
  buffer) — first-callback alloc, benign but same category.

Separately, `OrtSession` (`ort_session.hpp/.cpp`, used by the single-model
`nablafx_clap` plugin, not Axon) *documents* an `Ort::IoBinding`
pre-bound-buffer design in its header comment but never implemented it — the
.cpp still allocates per call.

## Measured (2026-07-05 perf pass — why this was deprioritized)

Scratch benches with the real models: eliminating the allocations (prealloc +
IoBinding) measured **2.3 µs per ~1000 µs ssl_comp forward (0.23%)** and
**×1.033 on the LSTM controller call** — negligible throughput. The value is
purely RT-safety: no malloc/free in the callback, and possibly trimming the
rare multi-hundred-µs max-latency outliers seen on the controller sub-timer
(unproven — could be scheduling).

## Fix sketch

Pre-build at activate time, per session: cached input `Ort::Value` wrappers
over caller-owned (64-byte-aligned) buffers, cached name arrays, precomputed
state-out index mapping, and `Ort::IoBinding` with outputs bound to
pre-allocated tensors (kills the returning-Run allocation and the copy-out).
Care points: the auto-EQ state A/B swap must rebind (or write-through) the
bound state tensors each call; output shapes are fixed per model, so binding
is straightforward. Use 64-byte-aligned buffers — this dovetails with (and is
the cheapest test of) `ort_render_nondeterminism.md`'s alignment hypothesis,
so do the two investigations together.

## Acceptance

- Zero heap allocations inside `process()` after the first block: verify with
  a malloc-hook/Instruments allocation trace over a bench render.
- Byte-identical renders vs pre-change goldens (retry protocol per the
  nondeterminism doc) — binding must not change any tensor math.
- Full suite + contract tests green.
