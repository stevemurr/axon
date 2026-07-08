# Offload neural inference off the audio thread (lock-free handoff)

Status: complete/not-implemented
Opened: 2026-07-06
Concluded: 2026-07-08
Outcome: Not implemented — the controller (the doc's lead path) causes 0 RT misses; the real driver is ssl_comp (separate/harder). Corrected: neural LSTM is the DEFAULT engine.
Issue: #25

The only viable way to retire the ORT audio-thread allocation debt — spun out
of [[ort_audio_thread_allocations]] (#18), which proved the in-place fix
(prealloc + `IoBinding`) does NOT work.

## Why / evidence

#18 measured (real models, malloc interposer + standalone probe) that every ORT
inference on the audio thread churns heap allocations that no on-thread change
removes:

- `OrtMiniSession::run()` (ssl_comp TCN): **84 malloc/free pairs per forward**.
- `run_controller()` (neural-EQ LSTM): **~198 malloc/free pairs per call**.
- These are ORT-INTERNAL sequential-executor allocations (~79 of the 84 for
  ssl_comp, which is stateless with a 1-in/1-out boundary). `IoBinding` removes
  0 (84.0 → 85.0/call); no SessionOptions lever reaches zero (floor 68–108).

So the debt is inherent to running ORT on the audio thread. The remaining lever
is to not run it there. Moving inference to a worker thread also (bonus) pulls
the multi-hundred-µs forward spikes out of the callback entirely — related to
the hop-stagger jitter work ([[ssl-comp-hop-stagger]], #22) but a bigger hammer.

## The two paths differ — scope them separately

- **Auto-EQ controller (good candidate).** `run_controller()` emits EQ *params*
  (13 band gains), not audio. Params already change only once per block and are
  smoothed (EQS smoother, ~0.4 s). They are latency-TOLERANT: computing them on
  a worker and applying them a few blocks late is inaudible. Handoff: audio
  thread publishes the latest control frame into a lock-free slot; worker runs
  the LSTM; worker publishes params back into a double-buffer the audio thread
  reads. No allocation, no lock, no priority inversion. NOTE: the shipped
  DEFAULT EQ engine is the deterministic C++ cascade (already allocation-free),
  so this only matters for the opt-in Neural engine — weigh whether it's worth
  it, or whether the answer is simply "prefer the deterministic engine."
- **ssl_comp bus-comp (hard).** Its ORT output IS the audio signal, so it cannot
  be made latency-tolerant the same way. It already runs on a kSslHop=1024 hop
  with 896-sample latency; an off-thread design would need a pipeline where the
  worker processes hop N while the audio thread plays hop N-1, with a bounded
  queue and a guaranteed-completion deadline (a missed deadline = dropout or
  stale hop). Higher risk; may not be worth it given #18 found the throughput
  cost is ~0.

## Plan (starting point)

1. Prototype the controller handoff first (lower risk, isolated): a single
   worker thread, SPSC lock-free slots for control-in / params-out, seqlock or
   double-buffer for the params the audio thread reads. Measure on-thread
   allocations (must hit zero via the #18 malloc-hook harness) and the added
   param latency (blocks); confirm inaudible via listen + null within smoother
   tolerance.
2. Decide ssl_comp separately after the controller proves the pattern — it may
   land as its own doc if the pipeline/deadline design is substantial.

## Acceptance

- Zero heap allocations on the audio thread during `process()` for the offloaded
  path — verified with the #18 malloc-hook trace over a bench render.
- No added xruns under a stress render (worker never blocks the callback; the
  callback degrades gracefully to "reuse last params" if the worker is late).
- Output equivalent within the controller's existing smoother tolerance
  (byte-identity NOT promised — params arrive a bounded few blocks later);
  document the exact added latency and A/B listen.
- Full suite + contract tests green.

## Results (2026-07-08)

Two findings reshape this — one corrects the doc's premise, one reprioritizes the
two paths.

### Correction: the DEFAULT engine is the NEURAL LSTM, not the deterministic cascade
Both this doc and #18 assert "the shipped default auto-EQ engine is the
deterministic C++ cascade." **That is wrong.** `EQ_ENGINE` defaults to 0, and
`eq_adaptive = (eqeng >= 0.5f)` (`axon_plugin.cpp:1514`) → the dispatch's `else`
branch (`:1706`) = the per-class **LSTM `run_controller`** (ORT). The deterministic
`AdaptiveEqController` was ADDED behind `EQ_ENGINE=1` as the opt-in (commit
b948672); neural stayed the default. Confirmed empirically below: the
`AutoEqOrtCtrl` sub-timer is non-zero at default. So the controller offload matters
for the *default* config — the doc's "this only matters for the opt-in Neural
engine" hedge is inverted.

### Measured (instrumented `uv run axon bench`, defaults: EQ=1, EQ_ENGINE=0, SSC=1)
Per-stage (`full_chain`, buf=64):

| stage (sub-timer) | mean µs | p95 µs | max µs | % process |
|---|---|---|---|---|
| SslComp ↳ **SslOrtForward** (bus-comp ORT) | 832 | 1023 | **2829** | 58.4% |
| AutoEQ ↳ **AutoEqOrtCtrl** (LSTM ORT) | 88 | 128 | 830 | 24.6% |

RT deadline **misses** (process exceeded the buffer budget), by scenario × buffer:

| scenario | buf 64 | 128 | 256 | 512 | 1024 |
|---|---|---|---|---|---|
| bypass / eq_only (no ssl_comp) | **0** | 0 | 0 | 0 | 0 |
| bus_comp_only | 5 | 12 | 7 | 0 | 0 |
| full_chain / full_chain_all | 6–14 | 5–14 | 1–3 | 0 | 0 |

### The decisive reprioritization
**The RT misses come entirely from `ssl_comp`, not the controller.** `eq_only`
(controller-only, neural) has **0 misses at every buffer** — the controller's 830µs
worst case fits even a buf=64 budget (~1451µs at 44.1k). Only `ssl_comp`'s
2829µs spike blows the budget. So:

- The doc's Phase-1 "good candidate" (offload the **controller**) targets the path
  with **no measured RT-miss impact**. Its only debt is allocations (~198/call) —
  and #18 already measured those as no-throughput-cost, theoretical-priority-
  inversion-only. Offloading it is a large RT-threading effort (SPSC lock-free +
  double-buffer + graceful degradation) to retire pure allocation *hygiene* on a
  path that isn't causing problems.
- The **actual** RT problem — deadline misses at buf ≤ 256 — is driven by
  **`ssl_comp`**, the path the doc scopes as **hard** (its ORT output IS the audio,
  so it needs a bounded-queue/deadline pipeline, not a latency-tolerant param
  handoff). And misses are already **zero at buf ≥ 512**, i.e. typical DAW sizes.

### Verdict
**Do NOT implement the controller-first offload as scoped.** It's disproportionate:
a high-risk RT-threading build to zero-out allocations on the one ORT path that
demonstrably does *not* miss deadlines, while leaving the real miss-driver
(`ssl_comp`) untouched. The debt it retires has no measured throughput cost (#18)
and no measured RT-miss cost (this pass).

The meaningful RT lever is `ssl_comp`'s forward-latency spike, which is a different,
harder problem better served by: the existing hop-stagger jitter work
([[ssl-comp-hop-stagger]] #22), a faster/quantized bus-comp model, or a dedicated
`ssl_comp` off-thread *pipeline* (its own doc if pursued) — not the controller
handoff this doc leads with. And the misses are small (≤0.01% of callbacks) and
vanish at buf ≥ 512.

Cheaper controller-specific lever if on-thread ORT purity is still wanted: default
`EQ_ENGINE` to the deterministic cascade (allocation-free, already shipped) — but
that's a *quality* decision (neural is presumably the better-sounding default) and
belongs to the owner, not this RT doc.

## Outcome

Concluded **not-implemented** 2026-07-08 (approved: the recommended verdict).

The doc leads with the controller offload as the "good candidate," but the
instrumented bench (Results above) shows that path causes **zero** RT deadline
misses at any buffer — the LSTM controller's 830µs worst case fits even a buf=64
budget (~1451µs). The actual misses come only from `ssl_comp` (2829µs forward
spike; misses at buf ≤ 256, **zero at buf ≥ 512**) — the path this doc scopes as
hard and defers. So the controller-first offload is a large, high-risk
RT-threading build (SPSC lock-free + double-buffer + graceful degradation) to
retire an allocation debt that #18 already measured as no-throughput-cost /
theoretical-priority-inversion-only, on a path with **no measured RT-miss
impact**. Disproportionate.

Corrected a factual error shared by this doc and #18: the shipped **DEFAULT**
auto-EQ engine is the **NEURAL LSTM** (`EQ_ENGINE=0` → `eq_adaptive=false` →
`run_controller`), NOT the deterministic cascade — the cascade is the
`EQ_ENGINE=1` opt-in (added in b948672). Confirmed by the non-zero `AutoEqOrtCtrl`
sub-timer at default.

Nothing shipped. If the `ssl_comp` forward-latency spike is worth attacking later,
it's a distinct effort — see [[ssl-comp-hop-stagger]] (#22), a faster/quantized
bus-comp model, or a dedicated `ssl_comp` off-thread *pipeline* (its own doc). A
cheap controller-only RT lever — default `EQ_ENGINE` to the deterministic cascade —
exists but is a quality/product decision, not an RT one.
