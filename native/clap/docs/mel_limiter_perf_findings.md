# Mel Limiter — performance findings

Module 7 of the DSP perf/quality pass. **No source change shipped** — the one
candidate is algebraically bit-identical but cannot be nulled below the strict
~-120 dBFS PERF bar because the module's output is build/instance-variant at a
higher level than that (see below). The win was tiny anyway, so it is reverted
and documented.

## Baseline (Release -O3, Apple Silicon, 48 kHz, stereo, drive 2x, ceiling 0.5)
- **~143 ns/sample (~0.69% of a 128-blk budget).** Cost is the per-hop STFT
  (1024-pt FFT, hop 256: fwd+inv per channel) plus per-sample brickwall +
  drain + blend.

## Candidate (NOT shipped): per-sample modulo -> branch-wrap
`process()` does runtime `% out_ring.size()` (1280) and `% dry_sz` (1536) per
output sample — true integer divisions (the moduli are `std::vector::size()`,
not compile-time constants; the constant moduli kFFTSize/kBrickLA/kDqCap the
compiler already handles). The read/write pointers only advance by 1, so
compare-and-reset is exactly equivalent. Index equivalence was proven
exhaustively over the full range (0 mismatches).

## Why it can't be classified PERF here
The MelLimiter's **water-filling gain solver is discontinuous** (`std::sort` +
a branch-selected cutoff), and the analysis/synthesis use vDSP FFTs whose exact
rounding is **buffer-alignment sensitive**. Together these amplify
codegen/alignment ULP differences:

- **Identical source, two instances in one binary** (renamed namespace) diverge
  by **1.1e-4 (-79 dBFS) in the warmup transient** (first ~40 ms) and only
  **2.7e-7 (-131 dBFS) in steady state**.
- new-vs-old (the candidate) shows the SAME profile: warmup **-75.6 dBFS**,
  steady **-118.7 dBFS** — i.e. it adds nothing beyond the inherent
  instance-to-instance variance.

So the two-binary null test can't validate even a provably-index-identical
refactor: the warmup-transient variance (~-76 dBFS, present with NO change)
exceeds the bar. Per the workflow's rule ("if you can't null it, revert"), the
change is reverted. The win was ~14% of 0.69% ≈ **0.1% of block budget** — not
worth shipping unprovable.

## Notes (not bugs)
- The variance is a **benign low-level warmup transient**; steady-state is
  ~-119 dBFS between instances. It does NOT affect the **hard-clip ceiling
  guarantee** (`std::clamp(o, -ceiling, ceiling)` makes `|out| <= ceiling`
  unconditionally) — verified by `test_mel_limiter`.
- The brickwall is a **sample-peak** limiter (no oversampled true-peak
  detection) despite the "true-peak" comment; inter-sample peaks are handled by
  the downstream TruePeakCeiling stage, so this is correct by architecture.
- Two stages in this chain (this one and the Auto-EQ SpectralMaskEq) are
  numerically ill-conditioned enough that the strict -120 dBFS null bar can't
  validate algebraically-identical refactors. If micro-optimizing these is ever
  desired, the pragmatic policy is to null-test **steady-state only** with an
  inaudible tolerance (e.g. -110 dBFS) rather than full-signal bit-identity.

Deliverable: `bench/bench_mel_limiter.cpp`.
