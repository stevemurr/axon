# Axon per-stage CPU performance ranking

Date: 2026-07-05

## 1. Methodology

Per-stage timing uses the `AXON_STAGE_TIMING` instrumentation (`native/clap/src/axon_stage_timing.h`, off by default; zero-cost-off validated). Reproduce with `AXON_STAGE_TIMING=1 bash scripts/install_axon_mac.sh --no-install` followed by `python3 native/clap/bench/run_bench.py --no-compare --scenarios ...` (see `native/clap/bench/README.md`). Accounting closure was 98.4% of process() time at buf=128 (bar: >=75%); timer overhead measured 10.8 ns/probe; all validation checks passed (floor, spike-shape, cross-validation, unit tests). Observer-effect caveat: this is an instrumented -O3 Release build — per-stage *shares* are the meaningful output; never compare absolute times to, or update, the uninstrumented `baseline.json` from an instrumented build.

## 2. Ranking at buf=128 (all-stages scenario, 10 iters)

Deadline at buf=128 is 2902.5 us/block; measured per-block p50 196.8 us, p95 2256.3 us (15/68910 deadline misses, driven by the ssl_comp hop spike). Shares are buffer-size-invariant (buf=512 within 1 pt). Sub-timers are indented under their parent and are included in the parent's total.

| Stage | pct_128 | pct_512 | calls_128 | mean us | p95 us | max us |
|---|---:|---:|---:|---:|---:|---:|
| SslComp | 58.458 | 59.526 | 68,900 | 263.24 | 1906.43 | 8624.54 |
| &nbsp;&nbsp;&nbsp;&nbsp;SslOrtForward | 58.048 | 59.101 | 17,226 | 1045.51 | 1900.74 | 4749.83 |
| AutoEQ | 33.478 | 33.483 | 68,900 | 150.75 | 255.80 | 1985.71 |
| &nbsp;&nbsp;&nbsp;&nbsp;AutoEqOrtCtrl | 24.007 | 24.018 | 137,800 | 54.05 | 64.38 | 1378.79 |
| MelLimiter | 4.002 | 3.979 | 68,900 | 18.02 | 53.13 | 224.50 |
| SslEq | 0.589 | 0.582 | 68,900 | 2.65 | 4.02 | 79.88 |
| TrimCeiling | 0.581 | 0.575 | 68,900 | 2.62 | 4.01 | 44.63 |
| Reverb | 0.419 | 0.412 | 68,900 | 1.89 | 3.47 | 89.29 |
| MeterIn | 0.253 | 0.253 | 68,900 | 1.14 | 2.00 | 35.08 |
| MeterOut | 0.252 | 0.252 | 68,900 | 1.14 | 2.00 | 23.50 |
| BassMono | 0.149 | 0.148 | 68,900 | 0.67 | 1.00 | 49.21 |
| Widener | 0.143 | 0.143 | 68,900 | 0.65 | 1.00 | 37.58 |
| SpectrumPush | 0.027 | 0.029 | 482,300 | 0.018 | 0.06 | 13.13 |
| AutoGain | 0.011 | 0.012 | 68,900 | 0.048 | 0.11 | 9.13 |

The two ONNX models are the whole story: SslComp is 58.5% and its SslOrtForward sub-timer is 58.0% — the bus-comp TCN forward *is* the stage. It is hop-spiky by construction: both channels' forwards fire back-to-back once per 1024-sample hop (1 in 8 blocks at buf=128), giving max/mean ~33x. AutoEQ is second at 33.5%, with the ungated per-channel LSTM controller alone at 24.0%. MelLimiter is 4.0%. Everything else is under 0.6%. Saturator and Exciter reported zero calls (dormant stages) and are excluded. Bypass sanity: gated stages read ~0 in bypass (SslComp 0.37 us, SslEq 0.01 us); AutoEQ stays hot (ungated controller, expected) and MelLimiter also runs there (bypass scenario omits MLI, default 1.0 — known quirk).

## 3. Bus-comp model verification

**Verdict: VERIFIED** (`scripts/verify_ssl_comp_model.py`, exits 0). Three checks:

- **Static shape — PASS.** Declared input `audio_in` [1,1,2048] float32, output `audio_out` [1,1,1418]; a [1,1,1654] input is rejected with `INVALID_ARGUMENT ... Got: 1654 Expected: 2048` under the plugin's exact session opts (intra=1, inter=1, sequential, ORT_ENABLE_ALL, CPU EP).
- **Causality — PASS.** Perturbing inputs [0..393] by 10x noise left all 1024 consumed outputs (indices [394..1417]) byte-identical (max|diff| = 0.0) while changing non-consumed outputs (non-vacuous). Positive controls bound the receptive field: first single input affecting any consumed output is index 396 (effective RF 629 < declared 631 — extra margin); perturbing input 1500 changed consumed outputs exactly in [870..1417] at 98.9% density.
- **Timing.** 20 warmups + 200 runs: per-channel forward median 1.052 ms, p95 1.171 ms — consistent with the plugin's ~2.1–2.4 ms hop-boundary blocks running 2 channels. (Dynamic in-memory re-export probe skipped: no `onnx` module available; analytic bounds only.)

**What this licenses:** re-exporting `ssl_comp` with `trace_len = 1654` is bit-identical over the consumed output range, with an analytic forward saving between 19.24% (input 1654/2048) and 27.79% (output 1024/1418; 394/1418 of output compute is currently discarded per hop).

## 4. Improvement proposals (ranked by expected impact x confidence)

| # | Stage | Proposal | Expected gain (of total process) | Bit-identical | Effort | Owner decision |
|---|---|---|---|---|---|---|
| 1 | SslComp | Re-export ssl_comp with trace_len=1654 | -11.2% to -16.1% CPU; hop pair 2.09 -> 1.51–1.69 ms | yes (verified) | M | **yes** |
| 2 | SslComp | Stagger L/R hop phases | p95/jitter: hop block ~2.09 -> ~1.05 ms; block p95 2256 -> ~1300 us; 0% throughput | unknown (likely) | M | no |
| 3 | AutoEQ | Mono-sum LSTM controller (1 call/block instead of 2) | -12.0% CPU (~54 us/block) | no | M | **yes** |
| 4 | AutoEQ | Decimate ch0 display-curve eval to transfer cadence | -3.7% CPU (~16–17 us/block) | yes | S | no |
| 5 | MelLimiter | Vectorize+sparsify solve_gains_ (vDSP_zvmags + sparse dotpr) | -2.4% CPU (18.0 -> ~7.0 us/block) | no (null -129.7 dBFS ss) | S | **yes** |
| 6 | SslComp | Forward off audio thread (worker, +1024 samples latency) | RT-thread -58%; spike gone; total machine CPU unchanged | yes | L | **yes** |
| 7 | SslComp | ORT intra-op threads 2–4 for ssl_comp | forward 1081 -> 603/409 us; RT CPU -26%/-36% | unknown | S | **yes** |
| 8 | AutoEQ | Hard bypass gate at EQ=0 | -33.5% conditionally (EQ off); 0% in measured scenario | yes while gated | S | **yes** |
| 9 | AutoEQ | Interleave L/R IIR cascades | -1.4% CPU (x1.52 on renderer) | yes (verified diff=0) | M | no |
| 10 | MelLimiter | Sparse prenormalized bin-gain map (with #5) | -0.4% CPU increment (combined 3.1x module) | no (same null envelope) | S | **yes** |
| 11 | AutoEQ | Preallocate/IoBinding run_controller I/O | -0.7% CPU; RT-safety (no audio-thread mallocs) | yes | S | no |
| 12 | SslComp | IoBinding in OrtMiniSession | -0.13% CPU; RT-safety only (measured 2.3 us/forward) | yes | S | no |
| 13 | AutoEQ | Re-export controller with decimating frontend (block-rate LSTM) | est. -19–22% CPU; retrain all 5 class models | no | L | **yes** |

### Prose notes

1. **ssl_comp re-export (T=1654).** 99.3% of SslComp is the forward itself (17,226 calls x 1045.5 us); ring memmove/dry-delay/telemetry/blends are ~0.7% and not worth touching. The stage consumes only the last kSslHop=1024 of 1418 outputs (`axon_plugin.cpp:1837-1838`), and everything is sized from `plugin_meta.json` (`:1750-1752`, ring alloc `:1160`), so only the bundle changes. Verified bit-identical over the consumed range (Section 3). Exporter lives in the external nablafx fork — owner decision; null-test the new export (ssl_comp is well-conditioned; steady-state null feasible). Prior recommendation from the June 2026 bus-comp findings (doc since retired; shipped 2026-07-05, see section 6).
2. **L/R hop stagger.** Both channels' `ssl_comp_in_fill` start at 0 (`:636`, reset `:1163`), so every hop boundary runs both ~1.05 ms forwards in one block (`:1781`, trigger `:1810`). Offsetting channel 1 by kSslHop/2 puts one forward per hop block; output timing is unchanged (wet always trails by 896 samples, queue pop independent of hop phase) and steady-state values are mathematically identical at any phase. FP-position effects in ORT SIMD tails make bit-identity likely but unprovable; needs first-cycle pass-through gating for the offset channel.
3. **Mono-sum controller.** The per-channel loop (`axon_plugin.cpp:1555`, call `:1584`) runs a full 128-timestep 2-layer/64-hidden LSTM per channel. One call on 0.5*(L+R) removes exactly half of the 24.0% AutoEqOrtCtrl share. Behavior change on stereo-asymmetric material (both channels get the same curve — arguably better mastering imaging, but audible): owner decision.
4. **Display-curve decimation.** ch0 evaluates 55 `magnitude_db` points across 64 biquads every block (`:1611-1619`, `:1631-1639`) but the spectrum transfer consumes it only every 16th block (2048-sample gate, `:202-226`); 15/16 evaluations are overwritten unseen, and this runs even with no GUI attached. Audio path untouched — bit-identical.
5. **MelLimiter solve_gains_.** 66% of the module is the scalar band-energy loop (`mel_limiter.cpp:144-160`): dense 26x513 mel matrix per channel, re^2+im^2 recomputed 26x, while the filterbank is 15.3x sparse (870/13338 nonzeros). Prototype (vDSP_zvmags power spectrum once + per-band sparse vDSP_dotpr): module 143 -> ~54.5 ns/sample; with #10, ~40 ns/sample (3.1x). Steady-state null vs baseline -129.7 dBFS — inside the stage's documented no-change instance variance (-119..-131 dBFS, `docs/mel_limiter_perf_findings.md:27-39`) but the stage can never meet the strict full-signal -120 dB bar even unchanged, so shipping requires the owner to accept steady-state-only validation.
6. **Worker-thread forward.** +1024 samples (~23.2 ms at 44.1k) latency gives the worker ~23 ms per ~1–1.9 ms forward. The cheaper +1-block variant is NOT safe unstaggered (2 x p95 1.9 ms > 2.9 ms block period). Latency must be reported via the CLAP latency extension — product decision.
7. **ORT intra-op threads.** `SetIntraOpNumThreads(1)` is deliberate (`axon_plugin.cpp:402`). Measured on the real model: 1081 us (1t) -> 603 us (2t) -> 409 us (4t). Real priority-inversion/jitter risk from a non-RT thread pool inside the audio callback; likely unnecessary if #1+#2 land. Owner decision on DAW CPU citizenship.
8. **AutoEQ EQ=0 gate.** The AutoEQ case has no gate; at wet_mix 0 the controller/renderer/display all run and `blend_` (`:1453-1454`) discards the wet exactly. Bit-identical while gated; re-enable pays the same LSTM state-reset warm-up already accepted for class switches (`:1532-1538`). Owner decision (behavior on re-enable, frozen overlay).
9. **IIR interleave.** 64 serial double biquads per sample (`iir_filterbank_eq.hpp:95-101`) leave the FPU idle on the FMA chain; interleaving both channels measured x1.52 with bitwise-identical output.
10–12. **Small/RT-safety items.** #10 ships only with #5. #11/#12: IoBinding removes per-call heap allocations from the audio thread (17,226 ssl_comp forwards + 137,800 controller calls share `OrtMiniSession`, per-call vectors/tensors at `:439-441`, `:446`, `:471-473`, name search `:477-482`); measured speed effect is negligible (2.3 us per ~1000 us forward; controller x1.033) — do these for RT-safety, not throughput.
13. **Controller re-export.** The exported LSTM runs 128 recurrent timesteps/block but only sample 0 of the output is consumed (`:551-559`); a strided frontend cuts steps 8–16x. Requires retraining all 5 auto_eq class models and revalidating the training/runtime distribution match — highest gain, highest product risk.

### Owner decisions required

- **ssl_comp re-export, trace_len=1654 (#1):** exporter is in the external nablafx fork (pinned in pyproject.toml); bundle + `plugin_meta.json` trace_len must change together. Verified bit-identical; -11 to -16% total CPU.
- **AutoEQ mono-sum controller (#3):** audible behavior change on asymmetric stereo material; -12% CPU.
- **MelLimiter vDSP rewrite (#5, #10):** requires accepting steady-state-only null validation for this ill-conditioned stage (measured -129.7 dBFS, inside no-change variance).
- **Worker-thread forward (#6):** +1024 samples reported latency — product decision.
- **ORT intra-op threading (#7):** plugin CPU citizenship / RT-scheduling risk in a DAW.
- **AutoEQ EQ=0 gate (#8):** re-enable state-reset transient + frozen UI overlay while gated.
- **Controller decimating re-export (#13):** retrain + re-export all 5 class models.

## 5. Explicitly-cheap stages — leave alone

SslEq (0.59%), TrimCeiling (0.58%), Reverb (0.42%), MeterIn/MeterOut (0.25% each), BassMono (0.15%), Widener (0.14%), SpectrumPush (0.03%), AutoGain (0.01%): each under 0.6% of process time; no proposal clears any reasonable ROI bar. Within the hot stages, also leave alone: SslComp's non-forward bookkeeping (~0.7% of the stage), MelLimiter's per-sample brickwall/drain loop (~29 ns/sample post-fix, poor ROI), and the previously documented MelLimiter modulo->branch candidate (re-measured at noise). Saturator and Exciter are dormant (zero calls) and excluded.

## 6. Implementation status (updated 2026-07-05, same day)

- **#4 (display-curve decimation) — SHIPPED.** `axon_plugin.cpp`: the ch0 EQ
  overlay curves are now evaluated every 8th flush (UI consumes them every
  16th). Audio path untouched. Measured in-context: AutoEQ 150.8 → 132.6
  µs/block.
- **#5 + #10 (MelLimiter vDSP/sparse rewrite) — SHIPPED**, owner accepted the
  steady-state-only null policy. `mel_limiter.{hpp,cpp}`: power spectrum once
  per channel (`vDSP_zvmags`) + per-band sparse `vDSP_dotpr` in `solve_gains_`;
  prenormalized sparse band→bin map (`vDSP_vsma`) + vectorized bin-gain apply.
  Measured: 18.0 → 6.0 µs/block in-chain (share 4.0 % → 1.45 %); standalone
  143 → 49 ns/sample; null vs pre-change build: **−129.7 dBFS steady-state**
  (full-signal −69.5 dBFS confined to the first 0.5 s warmup — both inside the
  stage's documented no-change variance). All unit tests pass.
- **Post-ship ranking (buf=128, 5 iters):** SslComp 62.0 % (unchanged in
  absolute µs; larger share of a smaller total), AutoEQ 31.9 %
  (AutoEqOrtCtrl 25.6 %), MelLimiter 1.45 %. Non-subtimer stage means sum
  ≈ 408 µs/block vs ≈ 443 before (~−7 %).
- **#1 (ssl_comp resize) — SHIPPED same day**, via in-place ONNX graph surgery
  (no retraining): implementation record in commit 634f980 and the conf yaml note; the
  full record. Byte-identical at every level (graph rewrite at T=2048,
  consumed outputs at T=1655, and a full old-vs-new plugin render). Measured:
  forward −20.5% standalone (0.999 → 0.794 ms), in-chain SslOrtForward mean
  1024 → 824 µs/call, hop-block spike ~2.1 → ~1.65 ms. Combined with #4/#5,
  non-subtimer stage means dropped ≈ 443 → ≈ 358 µs/block (~−19% total).
- **#3 (mono-sum controller) — REJECTED** by owner (stereo behavior change).
- **Measured datapoint for the controller discussion:** with `EQ_ENGINE=1`
  (the existing deterministic cascade controller instead of the LSTM), AutoEQ
  measures 84.8 µs/block vs 132.6 neural — the LSTM premium is ~48 µs/block
  ≈ 12 % of total process CPU.
