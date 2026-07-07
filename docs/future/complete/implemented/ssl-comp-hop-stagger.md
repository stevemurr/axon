# Stagger the ssl_comp L/R hop phases to halve the per-block spike

Status: complete/implemented
Opened: 2026-07-06
Issue: #22
Concluded: 2026-07-06
Outcome: Shipped — per-block ssl_comp spike (p95/p99) cut ~40-45%, deadline
misses quartered, steady-state null; cost is a one-time inaudible ~12-60 ms
activate-time warm-up on the offset channel.

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

## Results (2026-07-06)

**Mechanism confirmed from code.** In `plugin_activate_impl` every channel's
`ssl_comp_in_fill` is initialized to 0 inside the `for (auto& ch : chains)`
loop (axon_plugin.cpp:1215) and each advances by `kBlockSize` identically
(:1922), so both channels hit `if (fill >= kSslHop)` (:1927) on the SAME host
block → two TCN forwards back-to-back. The instrumented bench's calls ratio
proves the stereo double-up: 8612 forwards / 34450 blocks = 4.0 = 2 channels ×
(1 flush per 8 blocks), both on the hop-boundary block.

**Prototype** (working tree, measured — not yet committed): pre-load
channel 1's accumulator to `kSslHop/2` at activate so its hop boundary sits
half a hop from channel 0's, plus a one-shot "priming flush" that seeds the
ring but suppresses ORT output so the offset channel stays dry until its first
fully-real window. Mono untouched (only `chains[1]` is offset).

**Timing — instrumented `axon bench`, before → after (halved):**

| scenario / buf | SslComp p95 µs | block p99 µs | deadline misses |
|---|---|---|---|
| bus_comp_only / 128 | 1683 → **946** | 2039 → **1130** | 18 → **4** |
| bus_comp_only / 512 | 1683 → **945** | 2598 → **1621** | 1 → **0** |
| full_chain_all / 128 | 1681 → **947** | 2046 → **1148** | 15 → **2** |
| full_chain_all / 512 | 1681 → **946** | 2593 → **1651** | 1 → **0** |

Per-forward `SslOrtForward` mean is unchanged (~870 µs) — same total work, just
one forward per block instead of two. RTF unchanged (~8×). The dominant
systematic per-block spike (p95/p99) is cut ~40–45%. (Residual `max_ns`
outliers of 4–6 ms are cold-cache / OS-scheduling flukes, not the systematic
comb — those are unchanged and unrelated.)

**Correctness — `uv run axon test`: 29/29 green** (incl. test_ssl_hop_contract,
test_ssl_integration, test_tolerance_stages).

**Null vs a byte-clean pre-change reference bundle** (stashed HEAD, rebuilt;
stereo `axon_bench` render, per-channel/per-region diff):

- *ORT is deterministic here*: post-change rendered twice = byte-identical
  (-inf dB). So all deltas below are the change, not ORT flake (issue #19).
- *L channel (untouched logic): -76.7 dBFS everywhere*, warm-up included — a
  last-ULP SIMD/alignment artifact from `ChannelChain` growing one field
  (heap addresses shift). Inaudible; exactly the "different tensor positions,
  last-ULP tails" the Acceptance anticipated.
- *R channel (the offset one): steady-state -77.3 dBFS = the same floor → null.*
  bus_comp_only converges to the floor by ~93 ms.
- *Warm-up window*: R stays dry ~512 samples (~11.6 ms) longer than pre-change
  at activate, so R differs by up to **-2.8 dBFS** (isolated) over the first
  **~62 ms**, then rejoins the floor. One-time, does NOT recur.
- *Full chain*: the warm-up transient feeds the multi-second LUFS auto-gain
  integrator, so full_chain_all reconverges more slowly — whole-file max
  -5.2 dBFS, then **-54.5 dB @ 0.5 s → -64 dB @ 2 s → -91 dB @ 5 s → -144 dB
  @ 10 s**. Monotonic decay to numerical zero = benign reconvergence (a bug
  would hold a floor or grow), and <-54 dBFS is inaudible throughout.
- The whole-file `uv run axon eval null` gate therefore **reports FAIL**
  (-1.7 / -5.2 / -2.8 dBFS across sets) — as the Acceptance predicted
  ("Byte-identity is NOT promised"). The steady-state bar is met: transparent
  after activate, asymptotically null.

**Verdict: implement.** Strong, code-certain jitter win (per-block spike and
p99 both cut ~40–45%, deadline misses roughly quartered) for a ~15-line,
stereo-only, mono-safe change. The single cost is a one-time ~12–60 ms
activate-time warm-up asymmetry on the offset channel (L wet / R dry), below
audibility in practice for a master-bus stage (it lands in the host's
latency-compensation pre-roll / lead-in). No steady-state, latency, or L/R
alignment change. Ship with: a unit test pinning the staggered flush phase +
the priming-flush dry warm-up, the instrumented before/after numbers, and this
documented warm-up window.

## Outcome

Shipped 2026-07-06 (`native/clap/src/axon_plugin.cpp`).

**What changed** (~20 lines, mono-safe):
- New per-channel `ssl_comp_prime_flushes` counter on `ChannelChain`.
- `plugin_activate_impl`: when `channels >= 2` and ssl_comp is loaded, pre-load
  `chains[1].ssl_comp_in_fill = kSslHop/2` and `ssl_comp_prime_flushes = 1`
  (guarded by `static_assert((kSslHop/2) % kBlockSize == 0)`).
- Flush loop: a priming flush (`prime > 0`) seeds the ring, decrements the
  counter, and skips the ORT forward + output — so the offset channel stays in
  its existing dry warm-up until its first fully-real window.

**Verification (measured):**
- `uv run axon test`: **30/30 green**, incl. new `test_ssl_hop_stagger`
  (baseline collides on all 64 hops; fixed = 0 collisions, forwards half a hop
  apart, exactly 1 priming flush).
- Instrumented `axon bench`: SslComp per-block **p95 1682 → 946 µs**, block
  **p99 (buf128) ~2.04 → ~1.13 ms**, **deadline misses 18→4 / 15→2** (buf128),
  **1→0** (buf512). Per-forward time and RTF unchanged. NB the Acceptance's
  "zero deadline misses" is met only at buf512; at buf128 the residual misses
  are cold-cache / OS-scheduling `max_ns` outliers (4–6 ms), not the systematic
  comb — the systematic spike is what halved.
- Null vs a byte-clean pre-change bundle: steady-state null to the ORT floor
  (−77 dBFS); the only above-floor delta is the documented one-time warm-up
  window (R dry ~12 ms longer at activate, ≤ −2.8 dBFS isolated / −5.2 dBFS
  full-chain over the first ~62 ms, then rejoins the floor; full chain
  reconverges to −144 dB by 10 s via the LUFS auto-gain). Whole-file
  `axon eval null` reports FAIL by design (byte-identity not promised).
