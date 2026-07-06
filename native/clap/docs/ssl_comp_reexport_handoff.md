# Handoff: resize the ssl_comp TCN model (trace_len 2048 → 1655)

Date: 2026-07-05. Status: **IMPLEMENTED same day via Route A** (in-place graph
surgery; no retraining needed). What was done: the six residual-crop Slice
nodes' `ends` constants (absolute, baked to the 2048 shape: 2047/2037/2017/
1977/1897/1737) were rewritten to the length-relative `-1`, then the input/
output time dims set to 1655/1025. Proof chain: the `-1` rewrite alone at
T=2048 is byte-identical to the original (20/20 seeds); the resized model's
last-1024 outputs are byte-identical to the original's (20/20 seeds); a full
plugin render (bus-comp-only params, 20 s fixture) through old vs new bundle
is byte-identical; `scripts/verify_ssl_comp_model.py` passes at N=1655.
Measured: forward median 0.999 → 0.794 ms standalone (−20.5%); in-chain
SslOrtForward mean 1024 → 824 µs/call, hop-block pair ~2.1 → ~1.65 ms vs the
2902 µs deadline. `plugin_meta.json` trace_len updated to 1655. The rest of
this document is preserved as the design record (and as the playbook if the
model is ever re-exported from a checkpoint).

## The problem (verified empirically 2026-07-05)

The bus compressor is a **causal, stateless** TCN (`weights/axon_bundle/ssl_comp/
model.onnx`, ~69 KB, receptive field rf=631, no state tensors) run once per
`kSslHop = 1024` accumulated samples per channel:

- ONNX input `audio_in` is **statically shaped `[1,1,2048]`** (trace_len=2048).
  A `[1,1,1654]` input is rejected with `INVALID_ARGUMENT` (confirmed).
- One forward computes `2048 − (631−1) = 1418` output samples, but the plugin
  consumes only the **last 1024** (`std::copy_n(obuf.data() + actual_olen -
  kSslHop, kSslHop, ...)`, `native/clap/src/axon_plugin.cpp` ≈ line 1837).
  **394/1418 = 27.8 % of every forward is computed and discarded.**
- Causality was verified byte-exactly: perturbing inputs `[0..393]` at 10×
  amplitude leaves all 1024 consumed outputs **bit-identical** (positive
  controls confirmed the test is not vacuous; measured effective RF is 629,
  i.e. one sample of extra margin vs the declared 631).
- Measured forward (plugin-matched ORT opts: intra=1, inter=1, ORT_ENABLE_ALL,
  CPU EP, Apple Silicon): **median 1.052 ms, p95 1.171 ms per channel**. This
  single call is **~58–62 % of total plugin process time** (both channels fire
  back-to-back in each hop-boundary block → ~2.1 ms spike vs the 2902 µs
  block deadline at host buffer 128).

**Fix:** rebuild the model with time axis **T = 1655** (see next section for
why not 1654). Because the TCN is causal + stateless, the consumed outputs are
**bit-identical**; expected saving is **19.2 %** (input-length scaling
1655/2048) **to 27.8 %** (output-count scaling) of every forward ⇒ roughly
**−11 to −16 % of total plugin CPU**, and the hop-boundary spike shrinks
proportionally.

## Why 1655 and not the mathematical minimum 1654

The math: consumed outputs need `actual_olen = N − rf + 1 ≥ kSslHop` ⇒
`N ≥ 1024 + 631 − 1 = 1654`.

The code: `plugin_activate` enforces a **one-sample-stricter** guard
(`native/clap/src/axon_plugin.cpp` ≈ lines 1142–1154):

```cpp
if (kSslHop > N - rf) throw std::runtime_error(...);  // requires N ≥ 1655
```

At **N = 1655** the existing check passes and **zero plugin code changes are
needed** — everything downstream is derived from `plugin_meta.json` at runtime:
ring/out-buffer sizes (`≈1160–1161`), `actual_olen` (`≈1752`), the ring
`memmove` shift (`≈1818`), and the output-slice offset (`actual_olen − kSslHop`,
which becomes 1). The cost of the extra sample vs 1654 is ~0.06 % — noise.

(If you prefer N = 1654: relax the check to `kSslHop > N - (rf - 1)` and update
its comment; not worth the code churn.)

## Two implementation routes

### Route A (recommended): offline ONNX graph surgery — no training artifacts needed

The weights don't change; only the declared input length does. A pure-conv
causal TCN graph is shape-agnostic in its ops, so editing the input tensor's
dim is sufficient. Requires `pip install onnx` (NOT currently installed;
`onnxruntime 1.25.1` + `numpy` are).

```python
import onnx
m = onnx.load("weights/axon_bundle/ssl_comp/model.onnx")
dim = m.graph.input[0].type.tensor_type.shape.dim[2]
assert dim.dim_value == 2048
dim.dim_value = 1655
# If the graph output also declares a static time dim, fix it too:
odim = m.graph.output[0].type.tensor_type.shape.dim[2]
if odim.HasField("dim_value"): odim.dim_value = 1655 - 630   # 1025
onnx.checker.check_model(m)
onnx.save(m, "weights/axon_bundle/ssl_comp/model.onnx")
```

Caveats to check while doing it: any `Reshape`/`Pad`/`Slice` nodes with
hardcoded shape initializers referencing 2048 (grep the graph; a plain nn.Conv1d
TCN export has none, but verify). If such nodes exist and resist editing, fall
back to Route B.

### Route B: re-export from the training checkpoint

The exporter is **not in this repo**. It lives in the pinned external fork:
`pyproject.toml` line ~15 → `nablafx @ git+https://github.com/stevemurr/
nablafx.git@1f1f49b951d961ccb64545540d005f6776269f6e`, invoked as
`nablafx-export`. Model architecture config: `conf/model/tcn/
model_bb_tcn_ssl_comp.yaml` (causal TCN, 6 blocks, kernel 11, dilations
1..32, rf = 631; note its line ~12 comment still says "kSslHop=2048" — stale,
the code constant is 1024; fix the comment while there). The original training
checkpoint location is **not recorded in this repo** — if it can't be found,
use Route A.

## Files to update (either route)

1. `weights/axon_bundle/ssl_comp/model.onnx` — the resized model.
2. `weights/axon_bundle/ssl_comp/plugin_meta.json` — set `"trace_len": 1655`
   (currently 2048; `receptive_field` stays 631). The plugin binds N from this
   meta, and `axon/export/composite.py` re-stages this sub-bundle into the
   composite bundle on every build (it validates but does not regenerate it).
3. `conf/model/tcn/model_bb_tcn_ssl_comp.yaml` — fix the stale kSslHop comment
   (cosmetic).
4. `scripts/verify_ssl_comp_model.py` — module constant `N = 2048` (line ~28);
   set to 1655 (or parameterize) so the script verifies the new model.

Then rebuild the bundle: `bash scripts/install_axon_mac.sh --no-install`.

## Acceptance criteria (run all)

1. **Bit-identical consumed range vs the OLD model** (the whole point). Load
   both models in Python; for ≥10 random seeds: run old on `x[..., -2048:]`
   and new on `x[..., -1655:]`; assert
   `np.array_equal(y_old[..., -1024:], y_new[..., -1024:])` (byte-exact,
   float32). Session opts must be intra=1/inter=1/ORT_ENABLE_ALL.
2. **`python3 scripts/verify_ssl_comp_model.py`** (after updating N) exits 0:
   static-shape check now expects `[1,1,1655]`, causality holds with the new
   boundary arithmetic (consumed = last 1024 of 1025 outputs; inert-input
   prefix is now just index 0).
3. **Plugin activates and unit tests pass**: rebuild, run all
   `native/clap/build/test_*` binaries (the activate-time guard at
   `axon_plugin.cpp:1148` must not throw).
4. **End-to-end null**: render the bench fixture through the OLD and NEW
   bundles with identical params (`native/clap/build/axon_bench --plugin ...
   --in native/clap/bench/fixtures/bench_input_20s.wav --out <wav>`, params
   `SSC=1.0`, everything else 0) and diff the wavs. Expect bit-identical
   steady state (ssl_comp is well-conditioned, unlike Auto-EQ/MelLimiter);
   the first hop may differ only if warmup ring semantics change (they
   shouldn't — zero-initialized rings shrink but the consumed range's history
   is preserved).
5. **Perf delta**: with the instrumented build
   (`AXON_STAGE_TIMING=1 bash scripts/install_axon_mac.sh --no-install`, then
   `python3 native/clap/bench/run_bench.py --no-compare --buffers 128
   --scenarios full_chain_all`), `SslOrtForward` mean should drop from
   ~1.02–1.05 ms to ~0.75–0.85 ms/call and the SslComp share from ~60 % of
   process time to ~50 %. Do NOT update `bench/baseline.json` from an
   instrumented build (the runner refuses anyway).

## Explicit non-goals

- Do not change `kSslHop`, the dry-delay/latency math (`kSslHop − kBlockSize =
  896` samples, unchanged), or `OrtMiniSession`.
- The auto-EQ LSTM models are a different contract (stateful controller,
  `[1,1,128]` per block) — nothing to resize there; out of scope.
- L/R hop-phase staggering and off-thread inference are separate proposals
  (see `perf_stage_ranking.md`), intentionally not bundled here.
