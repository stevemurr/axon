# Offload neural inference off the audio thread (lock-free handoff)

Status: not-started
Opened: 2026-07-06
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
