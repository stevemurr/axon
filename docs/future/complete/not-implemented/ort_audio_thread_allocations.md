# ORT audio-thread allocations (RT-safety debt)

Status: complete/not-implemented
Opened: 2026-07-06
Issue: #18
Concluded: 2026-07-06
Outcome: Measured dead end — IoBinding/prealloc removes ~0 of the ~84 per-call
allocations (they are ORT-internal, not I/O-boundary). Real remediation
(off-thread inference) tracked in a new doc.

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

## Results (2026-07-06)

**The debt is real and confirmed — but the proposed fix cannot retire it.**

Method: a `malloc`/`free` interposer (`DYLD_INSERT_LIBRARIES`) over `axon_bench`
renders (differencing scenarios to isolate each ORT path), plus a standalone
probe that loads the real `ssl_comp/model.onnx` and compares the returning
`Session::Run` against an `IoBinding` path, all under the same hook.

**How many allocations, and where (measured):**

- `OrtMiniSession::run()` (ssl_comp TCN): **84 malloc/free pairs per forward**
  (bus_comp_only vs bypass = +144,670 mallocs over ~1,722 forwards; the
  standalone probe reproduces 84.0/call exactly). ssl_comp is stateless
  (`state_tensors: []`, `num_controls: 0`), so its I/O boundary is just one
  input tensor + one output — ~5 objects. **The other ~79 are ORT-internal.**
- `run_controller()` (neural-EQ LSTM): **~198 malloc/free pairs per call**
  (neural vs deterministic, both with the IIR renderer fixed, = +1,364,271
  over ~6,890 calls). This path is OPT-IN: the DEFAULT auto-EQ engine is the
  deterministic C++ cascade (`adaptive_eq`), which uses no ORT and allocates
  nothing on the audio thread. So in the shipped default, ssl_comp is the only
  on-thread ORT allocator.
- All counts are malloc≈free (churn, not leak): the debt is transient per-call
  allocation, exactly the RT-safety pattern the doc describes — but tiny in
  time/bytes (matches the prior pass's ~0.23%).

**Why the fix doesn't work (the decisive probe):** on the real ssl_comp model,
2,000 inferences minus a load+warmup baseline:

| path / ORT config | mallocs per call |
|---|---|
| returning `Session::Run` (current code) | **84.0** |
| `IoBinding` + input/output pre-bound to fixed buffers | **85.0** |
| mem-pattern OFF | 108.0 |
| CPU mem-arena OFF | 67.9 |

`IoBinding` — the doc's core mechanism — removes **zero** of the 84 allocations
(it adds one). No SessionOptions lever gets near zero either; the floor is
~68–108/call. The allocations are ORT's per-node intermediate-tensor
allocations inside the sequential CPU executor, NOT the I/O-boundary objects
(input `Ort::Value` wrappers, name arrays, the returning output vector) that
prealloc + `IoBinding` were meant to eliminate. Those I/O objects are only ~5
of the 84; caching them is real-time cosmetics, not the "zero allocations"
acceptance bar.

**Verdict: do NOT implement (as scoped).** The proposed prealloc/`IoBinding`
remediation is measured-ineffective — it cannot reach the acceptance criterion
because the allocations are internal to ORT's kernels. The only ways to
actually reach zero on-thread allocation are disproportionate to a debt with no
throughput cost and only a theoretical priority-inversion risk:
1. Replace the ORT runtime with a hand-rolled inference cell — for the LSTM
   controller that is already a separate, larger idea ([[custom-lstm-controller-cell]],
   #16); there is no equivalent plan for the ssl_comp TCN, and hand-rolling a
   dilated-conv TCN is a major fidelity-risk effort.
2. Move inference off the audio thread (worker + lock-free handoff). Viable in
   principle for the controller (emits params, not audio) but not for ssl_comp
   (in the signal path); a different, larger design — not this doc.

Recommendation: conclude not-implemented, keeping this as the record that
`IoBinding` was measured and does not help, so nobody re-attempts it. The
residual debt is inherent to running ORT inference on the audio thread and is
only retired by moving inference off ORT (#16) or off-thread.

## Outcome

Concluded **not-implemented** 2026-07-06 (measured dead end).

The prealloc + `IoBinding` remediation this doc proposed was measured against
the real ssl_comp model and **removes ~0 of the ~84 per-call allocations**
(84.0 → 85.0/call; full table in Results). They are ORT-internal
node-execution allocations in the sequential CPU executor, not the I/O-boundary
objects `IoBinding` addresses, and no SessionOptions lever reaches zero either
(floor ~68–108/call). For a debt with no throughput cost (~0.23%) and only a
theoretical priority-inversion risk, no proportionate on-thread fix exists.

Nothing shipped. The only viable remediation — moving inference off the audio
thread onto a worker with a lock-free handoff — is spun out as a new idea:
[[offload-inference-to-worker-thread]] (issue #25). Hand-rolling the runtime to
kill the internal allocations is separately tracked for the LSTM controller by
[[custom-lstm-controller-cell]] (#16). This doc stands as the record that
`IoBinding`/prealloc was tried and measured ineffective — do not re-attempt it.
