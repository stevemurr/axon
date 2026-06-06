# Rational-A — a learnable rational-function waveshaper

> **One sentence:** Rational-A is a static, sample-by-sample nonlinearity of the form `y = P(x) / Q(x)` (a ratio of two polynomials whose coefficients were *learned* during training) that gives Axon's neural saturator its analog-flavored harmonic character.

---

## 1. What is this?

### A nonlinearity, in plain English

Most of the math we do to audio is *linear*: turn the volume up, and every sample is multiplied by the same number. Linear processing can change the *level* of a signal and its *frequency balance* (EQ, filters), but it can never add anything that wasn't already there — no new harmonics, no "warmth," no "drive."

A **nonlinearity** breaks that rule on purpose. It is a function `y = f(x)` where the output is *not* simply a constant times the input. The moment you bend the input-vs-output relationship even slightly, you start generating new frequencies (harmonics and intermodulation products). That is exactly what tubes, tape, transformers, and transistors do when you push them — and it's why those devices "sound like something" instead of being perfectly clean.

In audio-DSP terms a static nonlinearity applied to a waveform is called a **waveshaper**: it reshapes the *shape* of the wave. "Static" means it has no memory — the output for a given input sample depends only on that one input value, never on past samples. (Axon adds memory *around* the waveshaper with filters and gain, but the Rational-A core itself is memoryless.)

A useful mental picture: imagine a flexible ruler that maps "input voltage" on one axis to "output voltage" on the other. A straight ruler is linear (clean). Bend the ruler so the ends flatten out, and loud signals get squashed while quiet ones pass through nearly untouched — that bent curve is a saturation/soft-clip nonlinearity.

### An *activation function*, in ML terms

If you've seen neural networks, you already know nonlinearities by another name: **activation functions** (ReLU, tanh, sigmoid, GELU…). They are the "bend" that sits between the linear layers of a network; without them a deep network would collapse into a single linear layer and could only ever do EQ-like things. Activations are what let a network model *curved*, complicated relationships.

### What makes it "rational"?

In math, a **rational function** is one polynomial divided by another:

```
            P(x)     a0 + a1·x + a2·x² + … + an·xⁿ
   y = f(x) = ──── = ─────────────────────────────
            Q(x)     b0 + b1·x + b2·x² + … + bm·xᵐ
```

A *polynomial* is just a weighted sum of powers of `x` (`x`, `x²`, `x³`, …). A single polynomial can already make a curve, but it has an annoying habit: as `|x|` grows, the highest power dominates and the output shoots off to ±infinity. That's terrible for a waveshaper, because a loud transient would explode.

Dividing by a *second* polynomial fixes this. If the denominator also grows with `x`, the ratio can *level off* (saturate) instead of running away. This is why rational functions are such a good fit for modeling saturation: built into their algebra is the tendency to flatten out at the extremes, just like real analog gear running out of headroom. With only a handful of coefficients a rational function can approximate tanh-like soft clipping, asymmetric tube curves, and many shapes in between.

### Why a *learnable* activation matters

Classic waveshapers use a fixed formula (`tanh(x)`, a hard clip, a polynomial someone tuned by ear). A **learnable** activation instead exposes its coefficients (`a0…an`, `b1…bm`) as trainable parameters. During training, the network is shown real recordings of analog gear and adjusts these coefficients so its own curve matches the device's measured input/output transfer characteristic. The result is a nonlinearity whose exact bend was *discovered from data* rather than guessed — it can capture the subtle asymmetry and "knee" of a specific piece of hardware that no off-the-shelf `tanh` would reproduce.

This idea (rational functions as trainable activations) comes from the **Rational Activation** family of layers; Axon's C++ implementation deliberately mirrors the reference PyTorch class `rational.torch.Rational_PYTORCH_A_F` so that a model trained in Python produces *bit-for-bit comparable* output when it runs in the plugin.

---

## 2. Why it matters

Rational-A is the heart of Axon's **Saturator** stage — the block that gives the master its drive, density, and harmonic "glue." In the signal flow documented at the top of the plugin:

```
audio → ort(autoeq controller) → SpectralMaskEq
      → RationalA (saturator)  → ort(ssl_comp) → BassMono
      → MelLimiter             → TruePeakCeiling → output trim
```
*(`native/clap/src/axon_plugin.cpp:1-5`)*

Everything before the saturator shapes the *tone* (adaptive EQ); the saturator is the first stage that intentionally adds *harmonic content*. Its sonic job:

- **Warmth / character** — the learned curve adds low-order harmonics that the ear reads as "analog," "tube," or "tape" depending on the trained shape.
- **Density & loudness** — soft saturation rounds peaks, letting the later limiter push the track louder without obvious distortion.
- **Cohesion** — gentle nonlinearity applied across the full mix subtly fuses elements together ("glue").

Crucially, because the curve was *learned* to match a reference device, Axon's saturation can sound like a specific analog target rather than a generic math function. That fidelity to the trained model is the whole point of using Rational-A instead of a hand-tuned `tanh`.

---

## 3. The DSP / math behind it

### The exact formula (version "A")

The header documents the contract precisely *(`native/clap/src/rational_a.hpp:2-6`)*:

```
P(x) = a_0 + a_1·x + a_2·x² + … + a_n·xⁿ
Q(x) = 1 + |b_1·x| + |b_2·x²| + … + |b_m·xᵐ|
y    = P(x) / Q(x)
```

There are two things that make this **version "A"** specifically, and both live in the **denominator**:

1. **The leading `1`.** `Q(x)` is *not* a free polynomial; its constant term is pinned to exactly `1`. This is why the stored denominator coefficient list starts at `b_1` (the `x¹` term), not `b_0` — there is no learnable `b_0`. Pinning `Q(0) = 1` guarantees the function is well-defined and equals `P(0) = a_0` at the origin, and it removes a redundant scaling degree of freedom (you could otherwise multiply every `a` and `b` by the same constant and get the same curve).

2. **The absolute values around each denominator term.** Every denominator term is `|b_j · xʲ|`, so `Q(x) = 1 + Σ|b_j·xʲ| ≥ 1` for *all* real `x`. This is the safety mechanism. (Continued below under "poles.")

### Numerator vs. denominator degrees

- **Numerator** `P` has degree `n` and is stored as `n+1` coefficients (`a_0 … a_n`), because it includes the constant term `a_0`.
- **Denominator** `Q` has degree `m` and is stored as `m` coefficients (`b_1 … b_m`), because its constant term is the fixed `1`.

This asymmetric layout is spelled out in the `reset()` doc comment *(`native/clap/src/rational_a.hpp:23-24`)*:

```cpp
// numerator: length n+1 (a_0 .. a_n)
// denominator: length m   (b_1 .. b_m)
```

The two degrees are independent — the code makes no assumption that `n == m` or `n == m+1`. Whatever lengths the trained weights ship with are used as-is. (The common Rational-A configuration in the original paper is degree-5 numerator / degree-4 denominator, i.e. 6 numerator and 4 denominator coefficients, but Axon does **not** hard-code this; see §7–8.)

### Poles, stability, and the absolute value

A *pole* is an input value where the denominator hits zero and the function blows up to infinity. For audio that is catastrophic — a single sample landing on a pole produces ±inf, then NaNs, then silence or a speaker-destroying spike. A naive rational function `P(x)/Q(x)` with a free `Q` can have real poles right in the middle of the audio range.

Version "A" makes poles *impossible on the real axis*. Because every denominator term is wrapped in `|·|` and added to a starting value of `1`:

```
Q(x) = 1 + |b_1·x| + |b_2·x²| + … + |b_m·xᵐ|  ≥ 1   for every real x
```

The denominator can never be smaller than `1`, so it can never be zero, so the output can never blow up. You trade a little expressive freedom (the curve is forced to be symmetric in how its *denominator* grows) for a hard, built-in stability guarantee — exactly what you want in a real-time audio waveshaper that will see arbitrary, occasionally very loud, input.

### Coefficient layout summary

| Symbol | Meaning | Stored as | Length |
|--------|---------|-----------|--------|
| `a_0 … a_n` | numerator coefficients | `numerator[]` (index `i` = coeff of `xⁱ`) | `n+1` |
| `1` | fixed denominator constant | *not stored* (hard-coded) | — |
| `b_1 … b_m` | denominator coefficients | `denominator[]` (index `j-1` = coeff of `xʲ`) | `m` |

Note the index offset on the denominator: `denominator[0]` is the coefficient of `x¹`, `denominator[1]` is the coefficient of `x²`, and so on.

---

## 4. How it works in the code

The entire module is a single header, `native/clap/src/rational_a.hpp` (65 lines). It is deliberately dependency-free — no CLAP, no ONNX Runtime, no `std::variant` — so it can be unit-tested standalone *(`native/clap/src/rational_a.hpp:8-9`)*.

### Construction / loading coefficients

The class default-constructs empty and is filled via `reset()` *(`rational_a.hpp:25-29`)*:

```cpp
void reset(const std::vector<float>& numerator,
           const std::vector<float>& denominator) {
    num_ = numerator;
    den_ = denominator;
}
```

It just copies the two coefficient vectors into the private members `num_` and `den_` *(`rational_a.hpp:60-61`)*. There is no validation or reshaping here; correctness of the lengths is the caller's responsibility (see §8).

### The evaluation core — `eval(double x)`

This is the math, sample by sample *(`rational_a.hpp:38-55`)*:

```cpp
double eval(double x) const {
    // Numerator: Horner's method, starting from highest-degree coeff.
    double p = 0.0;
    if (!num_.empty()) {
        p = num_.back();
        for (std::size_t i = num_.size() - 1; i-- > 0;) {
            p = p * x + num_[i];
        }
    }
    // Denominator: 1 + sum_j |b_j * x^j| for j = 1..m
    double q = 1.0;
    double xj = 1.0;
    for (float b : den_) {
        xj *= x;
        q += std::fabs(static_cast<double>(b) * xj);
    }
    return p / q;
}
```

Two different strategies, for two good reasons:

- **Numerator uses Horner's method.** Instead of computing each power `xⁱ` separately, Horner factors the polynomial as `(((aₙ·x + aₙ₋₁)·x + aₙ₋₂)·x + …)`. It starts from the highest-degree coefficient (`num_.back()`) and folds in one lower coefficient per multiply-add. This is the numerically tightest and cheapest way to evaluate a polynomial: `n` multiplies and `n` adds, no `pow()` calls, and less round-off accumulation. (Note the loop idiom `for (i = size-1; i-- > 0;)` — it post-decrements `i` in the test, so it walks indices `size-2` down to `0`, having already consumed `size-1` via `num_.back()`.)

- **Denominator builds powers directly** because of the absolute value. Horner can't be used cleanly here: each term must be individually wrapped in `|·|` *before* summing (`|b_1·x| + |b_2·x²| + …`), not folded together. So the loop keeps a running power `xj` (`x¹, x², x³, …`) by multiplying by `x` each iteration, scales by the coefficient, takes `std::fabs`, and accumulates into `q`, which started at the fixed `1.0`.

The final return is the single division `p / q`.

**Precision note:** the input `float` sample is promoted to `double` for the whole computation, and the coefficients (stored as `float`) are also cast to `double` inside the loops. All the arithmetic happens in double precision; only the final result is narrowed back to `float`. This keeps the high powers (`x⁵`, `x⁶`…) from losing precision and helps match the PyTorch reference (see §8).

### Vector / buffer handling — `process()`

The block-level entry point just loops `eval()` over a buffer *(`rational_a.hpp:31-36`)*:

```cpp
// Apply the rational nonlinearity sample-wise. In-place safe.
void process(const float* in, float* out, std::size_t n) const {
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<float>(eval(static_cast<double>(in[i])));
    }
}
```

It is **in-place safe** (you may pass the same pointer for `in` and `out`) because each output sample depends only on the corresponding input sample — there is no state and no look-back. In Axon's actual hot path the plugin doesn't even call `process()`; it calls `eval()` directly per sample so it can interleave the surrounding pre-gain, bias, filtering, and wet/dry math (see §9).

### Helpers

`empty()` reports whether both coefficient lists are unset *(`rational_a.hpp:57`)* — used to detect an unconfigured instance.

---

## 5. Performance

**Per-sample cost.** For a numerator of degree `n` and denominator of degree `m`, one `eval()` is roughly:

- Numerator (Horner): `n` multiplies + `n` adds.
- Denominator: `m` multiplies for the running power, `m` multiplies by the coefficient, `m` `fabs`, `m` adds, plus the initial `1.0`.
- One final division.

So on the order of `~2n + 4m` double-precision FLOPs plus one divide and `m` `fabs` calls per sample — a couple dozen operations for typical small degrees. There are **no `pow()` / `exp()` / transcendental calls** (unlike a `tanh` waveshaper), which makes it cheap and branch-free in the inner loops.

**Real-time safety.** The hot path (`eval` / `process`) performs **zero heap allocation** — it only reads the two pre-sized `std::vector`s and uses stack scalars. All allocation happens once, off the audio thread, inside `reset()`. There are no locks, no system calls, and no I/O. This is exactly the discipline a CLAP audio callback requires.

**One caveat for the vets:** iterating a `std::vector<float>` and casting each element to `double` inside the denominator loop is not auto-vectorizer-friendly, and the per-sample `eval()` call in Axon's hot loop is not inlined across the surrounding filter math. For the small coefficient counts in use this is negligible, but if degrees ever grew large, hoisting the coefficients into a fixed-size array and processing in SIMD-width chunks would be the obvious optimization.

---

## 6. Parameters / coefficients — where they come from

Rational-A has **no user-facing knobs of its own.** Its `numerator` and `denominator` are **trained weights**, baked into the model bundle at export time. The pipeline:

1. **In the bundle.** The saturator sub-bundle ships a `plugin_meta` JSON whose `dsp_blocks` array contains a block of `kind == "rational_a"` with a `params` object holding `version`, `numerator`, and `denominator` arrays.

2. **Parsing.** `parse_dsp_block()` reads them *(`native/clap/src/meta.cpp:28-37`)*. It first rejects anything that isn't version "A":

   ```cpp
   if (out.kind == "rational_a") {
       if (p.value("version", "A") != "A") {
           throw std::runtime_error(
               "rational_a block " + out.name +
               ": only Rational version 'A' is supported");
       }
       RationalAParams r;
       r.numerator   = p.at("numerator").get<std::vector<float>>();
       r.denominator = p.at("denominator").get<std::vector<float>>();
       out.params = std::move(r);
   }
   ```

   The parsed result lands in a `RationalAParams` struct *(`native/clap/src/meta.hpp:28-34`)*:

   ```cpp
   // Rational version A nonlinearity: P(x)/Q(x) where
   //   P(x) = sum_i numerator[i] * x^i           (length n+1)
   //   Q(x) = 1 + sum_j |denominator[j] * x^j|   (length m, j starts at 1)
   struct RationalAParams {
       std::vector<float> numerator;
       std::vector<float> denominator;
   };
   ```

3. **Load.** At plugin activation the saturator meta is loaded from the sub-bundle, and the first DSP block is pulled out once into `g_state->sat_rational` *(`native/clap/src/axon_plugin.cpp:1969-2007`)*:

   ```cpp
   if (st->sat_meta.dsp_blocks.empty()) {
       throw std::runtime_error("saturator sub-bundle has no dsp_blocks");
   }
   st->sat_rational = std::get<RationalAParams>(st->sat_meta.dsp_blocks[0].params);
   ```

4. **Install.** Each audio channel's `RationalA` instance gets the coefficients via `reset()` *(`native/clap/src/axon_plugin.cpp:998-999`)*:

   ```cpp
   ch.saturator.reset(g_state->sat_rational.numerator,
                      g_state->sat_rational.denominator);
   ```

So the coefficients are fixed for the lifetime of a loaded model; they only change if you load a different saturator bundle.

### What the *user* controls (the surrounding stage)

The expressive controls all live in the Saturator *stage* around the fixed curve, resolved per block in `resolve_amount_()` *(`native/clap/src/axon_plugin.cpp:1108`, `1121-1152`)*:

| Control ID | Meaning | Role relative to the curve |
|-----------|---------|---------------------------|
| `SDR` | `sat_pre_db` | input drive (gain *into* the curve — pushes harder up the bend) |
| `SVO` | `sat_post_db` | output makeup gain after the curve |
| `SMX` | `sat_wet_mix` | wet/dry blend of the saturated signal |
| `SHF` | `sat_hpf_hz` | high-pass: only saturate above this freq (bass-preserving) |
| `SLF` | `sat_lpf_hz` | low-pass: only saturate below this freq (treble-limiting) |
| `STH` | `sat_thresh_lin` | threshold `T` that normalizes the input into the curve's active region |
| `SBS` | `sat_bias` | DC bias added before the curve (asymmetry → even harmonics) |

These do **not** touch `numerator`/`denominator`; they scale, bias, band-limit, and blend the input/output *around* the learned shape (see §9).

---

## 7. Gotchas / things to watch

- **Coefficient lengths are not validated.** `reset()` accepts whatever vectors it's given. If the bundle ships the wrong-length arrays, `eval()` will still run but compute a different polynomial than intended. The degrees are implicit in the array lengths — `numerator.size()` is `n+1`, `denominator.size()` is `m`. There is no separate "degree" field to cross-check against.

- **The denominator's missing `b_0`.** A frequent mistake when exporting weights is to include the constant `1` (or a learned `b_0`) at the front of the `denominator` array. **Don't.** This code hard-codes `q = 1.0` as the starting value and treats `denominator[0]` as the coefficient of `x¹`. Prepending an extra term shifts every power up by one and silently produces the wrong curve.

- **Empty numerator edge case.** If `num_` is empty, `eval()` returns `0.0 / q == 0.0` for all inputs (the `if (!num_.empty())` guard leaves `p = 0`). A model with no numerator coefficients would therefore mute the saturated path — almost certainly an export bug, not an intended passthrough.

- **Matching PyTorch exactly.** The whole reason for the `double`-precision arithmetic and the per-term `fabs` (rather than any "clever" reformulation) is to reproduce `Rational_PYTORCH_A_F` numerically. If you "optimize" the denominator into a Horner form, or do the math in `float`, the high-order powers will diverge from the Python reference and the plugin's saturation will subtly differ from what was trained/auditioned. Keep the term-by-term `1 + Σ|b_j·xʲ|` structure.

- **Version gating.** Only version "A" is accepted (`meta.cpp:29-32`). A bundle marked version "B"/"C"/"D" (other Rational variants exist in the reference library, differing in how the denominator is built) will throw at load, by design — this header implements *only* the absolute-value-denominator "A" form.

- **Numerical conditioning at extreme input.** The `|·|` denominator guarantees no poles, but it does **not** guarantee a bounded output for arbitrary coefficients: if `deg(P) > deg(Q)`, `y` still grows without bound as `|x| → ∞` (just polynomially, never to a pole). In practice the trained models keep `deg(P) ≤ deg(Q)+`small and the stage's input threshold/drive keep `x` in range, but if you feed a runaway value the output can still get large. The DC-bias handling in the stage (subtracting `eval(bias)`) assumes a sane, finite curve.

---

## 8. Where it sits in the Axon chain / neural graph

### Position in the chain

Rational-A is the engine of the **Saturator** stage, third in Axon's processing order *(`axon_plugin.cpp:1-5`)*: it runs after the adaptive EQ (auto-EQ controller + `SpectralMaskEq`) and before the SSL-style bus compressor, bass-mono, limiter, and ceiling. It is a **`Dsp` / `NnDsp`-style block**, not an ONNX graph: in Axon's stage taxonomy *(`meta.hpp:61-68`)*, the saturator's nonlinearity is a learned-coefficient DSP block (`StageKind::Dsp`-style payload via `dsp_blocks`), so it runs **natively in C++** rather than going through ONNX Runtime. The coefficients were learned in PyTorch, exported as numbers, and are evaluated by `RationalA::eval` at runtime — no neural-network inference cost for this stage.

### How the stage wraps the curve (the per-sample recipe)

The Saturator stage in `process_stage()` *(`axon_plugin.cpp:1272-1349`)* turns the bare curve into a musical processor. Each channel does, conceptually:

1. **Compute the DC offset of the bias** once per block so the output stays AC-coupled *(`:1283-1285`)*:
   `dc = eval(sat_bias)`.
2. **(Optional) Band-split** with a 1st-order high-pass and/or low-pass (bilinear-transform biquad-lite) so only a chosen frequency band is saturated — bass-preserving / treble-limiting *(`:1286-1328`)*. The HPF/LPF coefficients are recomputed only when the cutoff knob actually moves (`>0.5 Hz` change), caching the biquad state on the channel.
3. **Pre-gain + threshold + bias, then evaluate the curve** *(`:1330-1332`, `:1340-1343`)*:
   ```cpp
   const float x_in = blk[i] * pre * invT + amt.sat_bias;
   wet = (eval(x_in) - dc) * T * pst;
   ```
   where `pre = 10^(SDR/20)`, `pst = 10^(SVO/20)`, `T = sat_thresh_lin`, `invT = 1/T`. The input is driven and normalized into the curve's active region, biased for asymmetry, shaped, de-biased, re-scaled by the threshold, and given makeup gain.
4. **Wet/dry blend** (`SMX`) — and when band-split is active, recombine the un-saturated bypass band with the blended wet band *(`:1334-1337`, `:1345`)*.

This is why the saturator can sound clean-but-glued at low settings and overtly driven at high settings, all on top of a single fixed learned curve.

### Related modules

- **`native/clap/src/meta.hpp` / `meta.cpp`** — define and parse `RationalAParams` from the bundle JSON (the sole producers of the coefficient vectors).
- **`native/clap/src/axon_plugin.cpp`** — owns the per-channel `RationalA saturator` instances and the surrounding Saturator stage (gain/bias/filter/blend).
- **`native/clap/src/spectral_mask_eq.hpp`** — the other `dsp_blocks` payload type (`SpectralMaskEqParams`); the auto-EQ stage that runs immediately *before* the saturator.
- **`native/clap/src/mel_limiter.hpp`, `true_peak_ceiling.hpp`, `bass_mono.hpp`** — downstream stages that the saturated signal flows into.
- **Reference:** `rational.torch.Rational_PYTORCH_A_F` (external PyTorch library) — the Python class this header is a numerical mirror of.
