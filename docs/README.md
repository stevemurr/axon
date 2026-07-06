# Axon Documentation

The core of the documentation is the **deep dives** in
[`deep-dives/`](deep-dives/): one long-form write-up per module, covering the
design story, the precise math, verified `file:line` anchors into the source,
the measured numbers, and the decisions (and mistakes) behind them. Each page
starts from zero — no audio/DSP background assumed — and builds up to the
implementation, so it's useful to newcomers and veteran engineers alike.

## The signal chain

The default processing order (stages are reorderable in the UI):

```
input ─▶ Bass Mono ─▶ EQ ─▶ Auto EQ ─▶ Reverb ─▶ Widener ─▶ Bus Comp ─▶ Limiter ─▶ True-Peak Ceiling ─▶ output
                            (neural)                        (neural)                                          │
                                                                                                             ▼
  in/out taps ─▶ Loudness Meter ─▶ Auto Gain (monitoring-only level-matched bypass)
  per-stage taps ─▶ Spectrum Analyzer (UI visualization)
```

## Deep dives (in chain order)

| # | Stage | Deep dive | The hook |
|---|-------|-----------|----------|
| 1 | Bass Mono | [bass-mono.md](deep-dives/bass-mono.md) | The 4.7-nanosecond stage that cannot break your mono sum |
| 2 | EQ (channel strip) | [ssl-channel-eq.md](deep-dives/ssl-channel-eq.md) | An SSL 9000 J strip in 13 biquads, with a seqlock-coupled auto-calibration that turns the knobs for you — plus the Rational-A waveshaper math |
| 3 | Auto EQ 🧠 | [auto-eq.md](deep-dives/auto-eq.md) | Two controllers, two renderers, and the mode-collapse diagnosis we had to retract |
| 4 | Reverb | [reverb.md](deep-dives/reverb.md) | An 8-line FDN designed by subtraction — no bass, no colour, no latency, no loudness |
| 5 | Widener | [widener.md](deep-dives/widener.md) | A Blumlein shuffler where mono-compatibility is algebra, not aspiration |
| 6 | Bus Comp 🧠 | [bus-comp.md](deep-dives/bus-comp.md) | Streaming a causal TCN on the audio thread, and shrinking it 20 % with byte-identical graph surgery |
| 7 | Limiter | [mel-limiter.md](deep-dives/mel-limiter.md) | A 26-band water-filling loudness maximizer, and how to prove a 3× rewrite changed nothing |
| 8 | True-Peak Ceiling | [true-peak-ceiling.md](deep-dives/true-peak-ceiling.md) | The always-last stage that makes −1 dBTP non-negotiable |

## Monitoring

| Area | Deep dive | The hook |
|------|-----------|----------|
| Meters, auto-gain, spectrum (+ the legacy LUFS Leveler) | [monitoring.md](deep-dives/monitoring.md) | BS.1770 meters, a level-matched bypass, and a Goertzel spectrum that doubles as a data bus |

---

## Where documentation lives

| Area | Location | What belongs there |
|---|---|---|
| **Deep dives** | `docs/deep-dives/*.md` | The durable per-module write-ups above — design, math, measurements, line-referenced walkthroughs. Each dive is the documentation of record for its module and lists the older docs it superseded (git history keeps the originals). |
| **Implementation findings** | `native/clap/docs/` | *Current* measured cross-stage findings (e.g. [`perf_stage_ranking.md`](../native/clap/docs/perf_stage_ranking.md), the chain-wide CPU ranking) |
| **Future work** | `docs/future/` | Every unexplored idea / open investigation, lifecycle-managed (see its README; worked via the `/next-idea` skill; mirrored to GitHub issues) |
| **Training & verification** | [`README.md`](../README.md) (repo root) | The per-model training recipes/contracts and the tests / benchmarks / evals bundle |

Task-based documents (agent handoffs, one-shot research reports) are deleted
once consumed — git history keeps them; the durable conclusions live in the
deep dives, the findings docs, or a future-work outcome.

Source lives in [`../native/clap/src`](../native/clap/src). Each dive links
back to the exact `file:line` it describes.
