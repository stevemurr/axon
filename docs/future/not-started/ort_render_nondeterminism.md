# ORT render nondeterminism (same binary, run to run)

Status: not-started
Opened: 2026-07-06
Issue: #19

Severity: Severity: inaudible; matters only if bit-exact renders become a
requirement (regression null-testing already works around it).

## Observed (2026-07-06, cleanup-pass final gate)

Rendering the bench fixture through `build/Axon.clap` with the adaptive-EQ
golden param set (`EQ=1.0,EQ_ENGINE=1,SSC=1.0,CLS=4,...`, buffer 128,
`--iters 1 --warmup 0`) with ONE freshly built binary, three consecutive runs:
run 1 == run 3 byte-exactly, run 2 differed at **−99.3 dBFS** (max sample
diff). Earlier phases of the same workflow independently logged the same flake
at **−94.4 dBFS** (full_chain_all set) and **−86.9 dBFS** (adaptive set) —
always resolving to byte-identical on retry. Deterministic-looking sets
(defaults; full_chain_all with the neural EQ) null byte-identically far more
often, but the −94.4 dBFS full_chain_all observation shows they are not
strictly immune.

The diffs sit inside the documented Auto-EQ/MelLimiter ill-conditioned band
(−75..−90 dBFS from 1-ULP perturbations — see
`docs/*` ill-conditioning notes and `mel_limiter_perf_findings.md`), which is
why a single-ULP upstream wobble surfaces at these levels.

## What is already ruled in/out

- NOT a cleanup regression: reproduced with a single unchanged binary.
- NOT threading inside ORT sessions: all sessions run
  `SetIntraOpNumThreads(1)`, `SetInterOpNumThreads(1)`, `ORT_SEQUENTIAL`.
- The plugin's own DSP is deterministic (fixed-seed unit tests pass exactly;
  non-ORT param sets null byte-identically across many runs).
- Prime suspect: **allocation-address/alignment-dependent kernel paths** in
  ORT/MLAS (and possibly Accelerate) — SIMD kernels choose head/tail handling
  based on buffer alignment, and heap addresses vary run to run. The 1-ULP
  output differences then get amplified by the ill-conditioned stages.

## Protocol until resolved (already in force)

Null tests against goldens: a sub−85 dBFS mismatch on an ORT-involving param
set must be RETRIED (2–3 runs) before blaming a code change; only a
*reproducible* mismatch indicts the change. Byte-identical remains the bar for
non-ORT structural changes.

## Starting plan for a real investigation

1. Reproduce in isolation: loop N renders of the adaptive set, hash the data
   chunks, measure flake rate (baseline ~1/6 from the gate observations).
2. Bisect the source: log/hash each ORT call's raw output tensors
   (`OrtMiniSession::run` / `run_controller`) across runs on identical inputs —
   determine whether the TCN forward, the LSTM controller, or downstream DSP
   introduces the first differing byte.
3. Test the alignment hypothesis: pin all ORT input/output buffers to 64-byte
   alignment (posix_memalign / aligned vectors, or IoBinding with owned
   aligned tensors — dovetails with `ort_audio_thread_allocations.md`) and
   re-measure the flake rate.
4. If alignment-pinning fixes it: adopt aligned IoBinding buffers as the fix.
   If not: try `ORT_DISABLE_ALL` graph optimization (isolates fused kernels),
   then per-op bisection of the model graphs.
