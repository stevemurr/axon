# axon_bench — performance test suite

Headless CLAP host + scripted scenario matrix for benchmarking the
Axon composite plugin. Designed so an agent (or CI job) can
run the whole suite with one command and either dump JSON or diff
against a committed baseline.

## Layout

```
bench/
  axon_bench.cpp      C++ harness: dlopens a .clap, drives audio in fixed
                      blocks, emits per-block timing as JSON
  run_bench.py        Python runner: runs scenarios × buffer sizes, writes
                      last_run.json + last_run.md, optionally diffs vs
                      baseline.json
  prepare_fixture.py  Regenerates fixtures/bench_input_20s.wav from MUSDB18
                      (only needed if you want to change the fixture)
  fixtures/
    bench_input_20s.wav   committed 20 s mastering-grade stereo input
                           (44.1 kHz, peak-normalized to -1 dBFS)
  baseline.json       committed reference numbers; populated by
                      `run_bench.py --update-baseline`
  last_run.json       gitignored; latest results dump
  last_run.md         gitignored; latest human-readable summary
```

## One-shot run (the agent path)

```bash
brew install libsndfile pkg-config         # one-time
./build.sh axon <staging-dir> /tmp/Axon.clap
./native/clap/bench/run_bench.py
```

Exit codes:
- `0` — pass (no regression vs baseline, or no baseline to compare against)
- `1` — setup error (missing binary / bundle / fixture)
- `2` — regression detected (p99 > baseline + 15% **or** RTF < baseline − 10%
         **or** any new deadline miss)

`last_run.md` is the human-readable summary. `last_run.json` is the
machine-readable dump with every cell's stats.

## Scenario × buffer matrix

| scenario         | what's exercised                                        |
|------------------|---------------------------------------------------------|
| `full_chain`     | historical "full" mix: AutoEQ + bus comp + limiter      |
| `eq_only`        | just the auto-EQ (controller + SpectralMaskEq)          |
| `bus_comp_only`  | just the bus comp TCN                               |
| `bypass`         | every stage's amount at 0 — measures plumbing overhead  |
| `full_chain_all` | every wired stage on: `full_chain` + SslEq (SEQ_ON=1, non-flat bands) + BassMono + Reverb + Widener |

Buffers: `64, 128, 256, 512, 1024` frames @ 44.1 kHz (deadlines:
1.45, 2.90, 5.81, 11.61, 23.22 ms).

Default cell config: 10 timed iters + 2 warmup. Override with
`--iters N --warmup N` if you want quicker feedback or tighter percentiles.

## Subset runs

```bash
# Just the bus comp on small buffers
./run_bench.py --scenarios bus_comp_only --buffers 64,128

# Faster iteration: 3 iters, no warmup, no compare
./run_bench.py --iters 3 --warmup 0 --no-compare
```

## Per-stage timing (instrumented builds — do NOT ship)

The plugin can be compiled with per-stage CPU timing probes
(`src/axon_stage_timing.h`) that time every chain stage, the fixed
plumbing (meters / spectrum push / trim+ceiling / auto gain), and the two
ORT hot spots (bus-comp TCN forward, auto-EQ LSTM controller). The bench
reads them out through a custom CLAP extension (`axon.stage-timing/1`)
after the timed loop, so the readout itself never perturbs a measured
block.

Build instrumented — set the `AXON_STAGE_TIMING=1` env var in front of any
build entry point that reaches `native/clap/build.sh` (it forces a cmake
reconfigure with `-DAXON_STAGE_TIMING=ON`):

```bash
AXON_STAGE_TIMING=1 bash scripts/install_axon_mac.sh --no-install   # canonical (uses weights/axon_bundle)
# or, driving build.sh directly:
AXON_STAGE_TIMING=1 native/clap/build.sh axon <staging-dir> build/Axon.clap
# (scripts/install_axon_mac.sh works too, on checkouts that have artifacts/axon-bundles/)
```

Then run the bench as usual; `run_bench.py` appends a
"Per-stage timing" section to `last_run.md` per (scenario, buffer) cell
and stores the raw data under `stage_timing` in `last_run.json`.
`axon_bench` (non-`--json`) prints the same ranked table. The
`full_chain_all` scenario turns on every wired stage (incl. SslEq via
`SEQ_ON=1` with non-flat band gains) so nothing is hidden by gating.

Reading the table:

- ranked by **% of process** = stage total / total recorded `process()`
  wall time. The **accounted** line sums the non-subtimer rows — expect
  roughly 85–98%; the remainder is block plumbing outside
  `flush_chain_block_` (ring copies, event handling).
- rows prefixed `↳` are **sub-timers** *nested inside* their parent stage
  (`SslOrtForward` ⊂ `SslComp`, `AutoEqOrtCtrl` ⊂ `AutoEQ`) — don't add
  them to the parent, they're already counted there.
- `p50/p95/p99` come from log2-bucket histograms (linear interpolation
  within a bucket, so within ~2×); `max` is exact.
- gated-off stages still show ~40 ns/call (two clock reads around an
  immediate `break`). AutoEQ is NOT ~0 in `bypass`: its LSTM controller
  runs ungated every block by design.
- `timer_overhead_ns` is the cost of one clock read (calibrated at bench
  startup); each recorded interval includes about one read of overhead.

Guards (observer effect):

- **Do not ship an instrumented build** — the probes cost two clock reads
  per stage per 128-sample block in the audio hot path.
- `run_bench.py` **refuses `--update-baseline`** while the plugin reports
  per-stage data (exit 1): instrumented numbers must never become the
  committed baseline.
- `build.sh` auto-detects a stale instrumented CMake cache and forces the
  option back OFF (loud notice) when `AXON_STAGE_TIMING=1` isn't set, so
  a later default build can't silently ship with probes in.

## Updating the baseline

After landing a perf change you believe is a real improvement:

```bash
./native/clap/bench/run_bench.py --update-baseline
git add native/clap/bench/baseline.json
git commit -m "perf: update bench baseline (<reason>)"
```

The baseline is committed so PR review can see the diff; never overwrite
it without explaining why in the commit message.

## Direct `axon_bench` invocation

For ad-hoc profiling (e.g. when attaching Instruments), call the binary
directly:

```bash
native/clap/build/axon_bench \
    --plugin /tmp/Axon.clap \
    --in     native/clap/bench/fixtures/bench_input_20s.wav \
    --buffer 256 --iters 200 --warmup 5 \
    --params 'EQ=0,SDR=0,SSC=1.0,CLS=4,LVL=0,OLV=0' \
    --json
```

JSON shape:

```json
{
  "plugin": "Axon",
  "sample_rate": 44100,
  "buffer_size": 256,
  "channels": 2,
  "iters": 200,
  "warmup": 5,
  "frames_per_iter": 882000,
  "audio_seconds_per_iter": 20.0,
  "per_block_us": {"min": 410.2, "p50": 482.1, "p95": 612.8, "p99": 851.4, "max": 1203.0, "mean": 503.6, "count": 690000},
  "per_iter_seconds_mean": 0.347,
  "realtime_factor_mean": 57.6,
  "block_deadline_us": 5805.4,
  "deadline_miss_count": 0,
  "deadline_miss_pct": 0.0
}
```

### What the numbers mean

- **per_block_us.p99** — wall time of the 99th-percentile `process()`
  call. **This is the realtime-deadline number**: if p99 exceeds
  `block_deadline_us`, the audio thread will glitch in a DAW at that
  buffer size.
- **block_deadline_us** — `buffer_size / sr * 1e6`. The wall-clock
  budget for one block.
- **deadline_miss_count / pct** — blocks slower than the deadline.
  Should be `0`.
- **realtime_factor_mean** — `audio_seconds / wall_seconds` averaged
  over iters. >1 = faster than realtime offline. (Realtime DAW
  playback wants RTF ≫ 1 because the audio thread shares the core.)

## Param IDs

`--params` takes the **short IDs** (3-letter symbols, not human names).
The bench computes the CLAP integer param id via the same FNV-1a hash
the plugin uses (`param_id_for(effect_name, short_id)`).

```
LVL  Leveler                  [0..1]
LVT  Lev Target  LUFS         [-36..-6]
SDR  Sat Drive   dB           [0..24]
SVO  Sat Output  dB           [-24..12]
SMX  Sat Mix                  [0..1]
SHF  Sat HPF     Hz           [20..500]
STH  Sat Thresh  dB           [-24..0]
SBS  Sat Bias                 [-0.5..0.5]
SSC  Bus Comp                 [0..1]
CLS  EQ Class    enum         [0..n_classes-1]   (4 = full_mix)
EQ   Auto EQ                  [0..1]
EQR  EQ Range                 [0..1]
EQS  EQ Speed    ms           [10..500]
EQB  EQ Boost                 [0..1]
OLV  Out Leveler              [0..1]
OLT  Out Lev Target  LUFS     [-36..-6]
TRM  Output Trim dB           [-12..12]
```
