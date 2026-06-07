# Bus Comp (neural TCN) — performance findings

Module 6 of the DSP perf/quality pass. **No in-code change shipped** — the one
meaningful win requires a model/bundle re-export (a stop condition), so it is
recorded here as a recommendation rather than applied.

## What it is
SSL-style bus compressor: a stateless, long-receptive-field causal TCN
(`ssl_comp/model.onnx`, rf=631, trace_len=2048, no state tensors) run through
`OrtMiniSession`. To avoid a forward pass every 128-sample block, the plugin
accumulates `kSslHop = 1024` samples, shifts a `trace_len`-sized ring, and runs
ORT **once per hop**, adding `kSslHop - kBlockSize` samples of latency.

## Baseline (axon_bench, Axon.clap, 20 s fixture, Release)
Per-block wall time, bus comp only (`EQ=0,SSC=100`):

| buffer | p50 | p95 | max | deadline | misses |
|-------:|----:|----:|----:|---------:|-------:|
| 128 | 269 µs | 2360 µs | 7259 µs | 2902 µs | 7 (0.02%) |
| 256 | 518 µs | 2764 µs | 4558 µs | 5805 µs | 0 |
| 512 | 1556 µs | 3402 µs | 5648 µs | 11610 µs | 0 |

The p50 is cheap (most blocks just drain the output queue). The **p95/max are
the hop-boundary blocks** where the full TCN forward runs (~2.1–2.4 ms). At
**buffer = 128 the forward spike nearly/occasionally busts the 2.9 ms realtime
deadline** (0.02% misses); at >= 256 there is comfortable headroom.

## The cost is the model forward, which is off-limits in a refactor
A 69 KB model taking ~2.4 ms looked suspicious, so the forward was inspected:

- The ONNX `audio_in` is **statically shaped `[1, 1, 2048]`** (confirmed via
  onnxruntime: a 1654-sample input is rejected with INVALID_ARGUMENT).
- It therefore produces `2048 - (rf-1) = 1418` output samples every hop, but
  **only the last `kSslHop = 1024` are used** — ~**28% of the forward is
  computed and discarded**.

Because the TCN is causal and stateless, the 1024 consumed outputs depend only
on the last `1024 + (rf-1) = 1654` input samples, so computing just those would
be **bit-identical** for the consumed range.

### Recommendation (requires your decision — bundle/format change)
Re-export `ssl_comp` (and check the auto-EQ models) with a **dynamic time axis**
(or `trace_len` set to `kSslHop + rf - 1 = 1654` instead of 2048). Expected
**~28% reduction in the bus-comp forward cost, bit-identical** for the consumed
samples — directly shrinking the p95/max spike that threatens the small-buffer
deadline. This is a model/bundle change, so it is **out of scope for an in-code
refactor** and left to you.

Other latency/jitter levers (also product decisions, not free):
- enable intra-op threading for the forward (faster spike, but audio-thread
  thread-pool jitter — the session is deliberately single-threaded today);
- spread/offload the forward across blocks or onto a worker thread with a
  lookahead buffer (adds latency / complexity);
- raise the minimum supported buffer, or gate SSC at very small buffers.

## Secondary (RT-safety smell, negligible perf)
`OrtMiniSession::run()` / `run_controller()` allocate small `std::vector`s per
call (`inputs`, `in_names`, `out_names`) and use the *returning* form of
`Ort::Session::Run` (which allocates the output vector); the auto-EQ/saturator
paths also do `unordered_map<string,…>` state lookups and an O(n²) name search.
These run once per hop and are **microseconds against a millisecond forward**,
so they do not move the needle — but they are heap allocations on the audio
thread. `ort_session.hpp` already documents (but does not implement) an
`Ort::IoBinding` design that would remove them; worth doing as a separate
robustness/hardening task, not for measurable speed.
