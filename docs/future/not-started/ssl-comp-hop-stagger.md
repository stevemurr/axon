# Stagger the ssl_comp L/R hop phases to halve the per-block spike

Status: not-started
Opened: 2026-07-06
Issue: #22

Jitter/p95 win, zero throughput or latency change: both channels' TCN
forwards currently land in the SAME 128-sample block.

## Why / evidence (measured 2026-07-05)

- Both channels' `ssl_comp_in_fill` start at 0 and advance identically, so at
  every 1024-sample hop boundary the channel loop runs BOTH ~0.82 ms forwards
  back-to-back in one block (~1.65 ms after the trace_len=1655 resize) — the
  dominant per-block spike: block p95 ≈ 2.3 ms vs the 2.9 ms deadline at
  44.1 kHz / buf 128; at buf 512 it is the visible comb in DAW load graphs.
- Offsetting one channel's hop phase by kSslHop/2 (4 flushes) puts exactly one
  forward in each hop block: spike ≈ halves (block p95 est. ~1.3–1.5 ms).
- Output timing is UNCHANGED at any hop phase: the wet always trails input by
  kSslHop − kBlockSize = 896 samples (queue pop is phase-independent), and
  each consumed output depends only on its trailing 1654 inputs — empirically
  verified byte-exact by `scripts/verify_ssl_comp_model.py` causality checks.
  L/R stay sample-aligned; no realtime/monitoring impact.

## Plan

1. In activate, initialize channel 1's accumulator phase to kSslHop/2 with a
   first-partial-hop pass-through gate mirroring the existing first-hop
   semantics (dry until the first full window exists).
2. Verify mono (n_ch==1) unaffected; check the dry-delay alignment math is
   untouched (it is phase-independent by construction — confirm in code).

## Acceptance

- `uv run axon eval null`: steady-state null vs pre-change (warmup blocks may
  differ during the offset channel's first partial hop — document exact
  window). Byte-identity is NOT promised: same math, different tensor
  positions can differ in last-ULP SIMD tails; hold to the steady-state bar
  and listen.
- Instrumented `uv run axon bench`: SslComp per-flush p95 ~1900 → ~1000 µs;
  per-block p95 at buf 128 from ~2.3 ms to ≤ ~1.5 ms; zero deadline misses.
- `uv run axon test` green.
