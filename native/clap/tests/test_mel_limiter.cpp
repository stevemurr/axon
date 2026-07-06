// Standalone unit tests for MelLimiter.
// Requires macOS arm64 (Accelerate vDSP).
//
// Build from native/clap/:
//   g++ -O2 -std=c++17 -I src \
//       tests/test_mel_limiter.cpp src/mel_limiter.cpp \
//       -framework Accelerate -o tests/test_mel_limiter \
//       && tests/test_mel_limiter
//
// Or via CMake after configuring the build dir:
//   cmake --build build --target test_mel_limiter && build/test_mel_limiter

#include "../src/mel_limiter.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

constexpr int    kSR = 44100;
constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Signal generators
// ---------------------------------------------------------------------------

std::vector<float> make_sine(double freq, double amp, int n, int sr = kSR) {
    std::vector<float> out(n);
    for (int i = 0; i < n; ++i)
        out[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * i / sr));
    return out;
}

// Approximate pink noise normalised to peak amplitude `amp`.
std::vector<float> make_pink(int n, double amp = 1.0, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    std::vector<float> out(n);
    double acc = 0.0;
    for (int i = 0; i < n; ++i) {
        acc = 0.995 * acc + 0.05 * nd(rng);
        out[i] = static_cast<float>(acc);
    }
    double peak = 0.0;
    for (float v : out) peak = std::max(peak, std::abs(static_cast<double>(v)));
    if (peak > 1e-9) for (float& v : out) v = static_cast<float>(v * amp / peak);
    return out;
}

// ---------------------------------------------------------------------------
// Measurement helpers
// ---------------------------------------------------------------------------

bool all_finite(const float* x, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(x[i])) return false;
    return true;
}

double rms(const float* x, int n, int skip = 0) {
    double s = 0.0;
    int count = n - skip;
    for (int i = skip; i < n; ++i) s += x[i] * x[i];
    return std::sqrt(s / count);
}

double peak_abs(const float* x, int n, int skip = 0) {
    double p = 0.0;
    for (int i = skip; i < n; ++i) p = std::max(p, std::abs(static_cast<double>(x[i])));
    return p;
}

// Residual fraction after projecting out the fundamental at `freq` — a simple
// THD-ish measure. ~0 for a pure sine, large for a clipped one.
double thd_frac(const float* x, int start, int len, double freq, int sr = kSR) {
    double w = 2.0 * kPi * freq / sr, ss = 0, sc = 0;
    for (int i = 0; i < len; ++i) { double ph = w * (start + i); ss += x[start+i]*std::sin(ph); sc += x[start+i]*std::cos(ph); }
    double as = 2.0 * ss / len, ac = 2.0 * sc / len;
    double resid = 0, tot = 0;
    for (int i = 0; i < len; ++i) {
        double ph = w * (start + i);
        double fund = as*std::sin(ph) + ac*std::cos(ph);
        double r = x[start+i] - fund;
        resid += r*r; tot += x[start+i]*x[start+i];
    }
    return tot > 0 ? std::sqrt(resid / tot) : 0.0;
}

nablafx::MelLimiter::Params default_params(float ceiling = 0.891f) {
    nablafx::MelLimiter::Params p;
    p.ceiling_lin    = ceiling;
    p.adaptive_gain  = 0.5f;
    p.adaptive_speed = 0.5f;
    p.wet_mix        = 1.f;
    return p;
}

// ---------------------------------------------------------------------------
// Test 1: silence in → silence out, no NaN/inf
// ---------------------------------------------------------------------------
void test_silence() {
    nablafx::MelLimiter ml;
    ml.init(kSR);

    const int N = kSR;
    std::vector<float> buf(N, 0.f);
    ml.process(buf.data(), nullptr, 1, N, default_params());

    assert(all_finite(buf.data(), N));
    double pk = peak_abs(buf.data(), N);
    std::fprintf(stderr, "[silence]    peak = %.2e (want < 1e-4)\n", pk);
    assert(pk < 1e-4);
    std::fprintf(stderr, "[silence]    PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: wet_mix=0 is an exact dry delay of kLatency samples
// ---------------------------------------------------------------------------
void test_dry_bypass() {
    nablafx::MelLimiter ml;
    ml.init(kSR);

    const int N = kSR;
    auto src = make_sine(1000.0, 0.5, N);
    std::vector<float> buf(src);

    auto p = default_params();
    p.wet_mix = 0.f;
    ml.process(buf.data(), nullptr, 1, N, p);

    assert(all_finite(buf.data(), N));

    const int L = nablafx::MelLimiter::kLatency;
    double max_err = 0.0;
    for (int i = L; i < N; ++i)
        max_err = std::max(max_err, std::abs(static_cast<double>(buf[i]) -
                                             static_cast<double>(src[i - L])));

    std::fprintf(stderr, "[dry_bypass] max err vs src[n-kLatency] = %.2e (want < 1e-5)\n",
                max_err);
    assert(max_err < 1e-5);
    std::fprintf(stderr, "[dry_bypass] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: loud signal → output RMS substantially reduced, all finite
// ---------------------------------------------------------------------------
void test_ceiling_reduces_loud_signal() {
    nablafx::MelLimiter ml;
    ml.init(kSR);

    const float ceiling = 0.891f;
    const int   N       = 4 * kSR;
    auto src = make_pink(N, 5.0 * ceiling);
    std::vector<float> buf(src);

    ml.process(buf.data(), nullptr, 1, N, default_params(ceiling));

    assert(all_finite(buf.data(), N));

    // Skip priming period; measure in the settled region.
    const int skip     = 3 * nablafx::MelLimiter::kFFTSize;
    double in_rms      = rms(src.data(), N, skip);
    double out_rms     = rms(buf.data(), N, skip);

    std::fprintf(stderr, "[ceiling]    in_rms=%.4f  out_rms=%.4f  ceiling=%.4f\n",
                in_rms, out_rms, static_cast<double>(ceiling));

    // Output must be quieter than input (gain was applied).
    assert(out_rms < in_rms);
    // Output should be within a reasonable factor of the ceiling.
    assert(out_rms <= ceiling * 2.f);
    std::fprintf(stderr, "[ceiling]    PASS\n");
}

// ---------------------------------------------------------------------------
// Test 4: no NaN/inf on extreme inputs
// ---------------------------------------------------------------------------
void test_no_nan_on_extremes() {
    auto p = default_params();
    const int N = kSR;

    // Full-scale DC
    {
        nablafx::MelLimiter ml; ml.init(kSR);
        std::vector<float> buf(N, 1.f);
        ml.process(buf.data(), nullptr, 1, N, p);
        assert(all_finite(buf.data(), N));
    }
    // Alternating +/-10 (non-physical torture signal)
    {
        nablafx::MelLimiter ml; ml.init(kSR);
        std::vector<float> buf(N);
        for (int i = 0; i < N; ++i) buf[i] = (i & 1) ? 10.f : -10.f;
        ml.process(buf.data(), nullptr, 1, N, p);
        assert(all_finite(buf.data(), N));
    }
    // Clipped uniform noise ±2
    {
        nablafx::MelLimiter ml; ml.init(kSR);
        std::mt19937 rng(99);
        std::uniform_real_distribution<float> ud(-2.f, 2.f);
        std::vector<float> buf(N);
        for (auto& v : buf) v = ud(rng);
        ml.process(buf.data(), nullptr, 1, N, p);
        assert(all_finite(buf.data(), N));
    }
    std::fprintf(stderr, "[extremes]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 5: reset() clears all state — silence after reset → near-zero output
// ---------------------------------------------------------------------------
void test_reset_clears_state() {
    nablafx::MelLimiter ml;
    ml.init(kSR);

    auto p = default_params();

    // Prime with loud audio.
    auto loud = make_pink(kSR, 5.0);
    ml.process(loud.data(), nullptr, 1, kSR, p);

    ml.reset();

    // Process silence; output should be silent.
    const int N = kSR;
    std::vector<float> buf(N, 0.f);
    ml.process(buf.data(), nullptr, 1, N, p);

    assert(all_finite(buf.data(), N));
    double pk = peak_abs(buf.data(), N);
    std::fprintf(stderr, "[reset]      peak after reset+silence = %.2e (want < 1e-4)\n", pk);
    assert(pk < 1e-4);
    std::fprintf(stderr, "[reset]      PASS\n");
}

// ---------------------------------------------------------------------------
// Test 6: stereo linking — hot L forces gain reduction on quiet R
// ---------------------------------------------------------------------------
void test_stereo_linked() {
    nablafx::MelLimiter ml;
    ml.init(kSR);

    const float ceiling = 0.891f;
    const int   N       = 4 * kSR;

    auto l_buf = make_pink(N, 5.f * ceiling, 1);   // very loud
    auto r_buf = make_pink(N, 0.1f * ceiling, 2);  // much quieter
    auto r_orig = r_buf;

    ml.process(l_buf.data(), r_buf.data(), 2, N, default_params(ceiling));

    assert(all_finite(l_buf.data(), N));
    assert(all_finite(r_buf.data(), N));

    const int skip   = 3 * nablafx::MelLimiter::kFFTSize;
    double r_in_rms  = rms(r_orig.data(), N, skip);
    double r_out_rms = rms(r_buf.data(),  N, skip);

    std::fprintf(stderr, "[stereo]     R: in_rms=%.6f  out_rms=%.6f\n",
                r_in_rms, r_out_rms);

    // R should be attenuated — it received the same gains computed from loud L.
    assert(r_out_rms < r_in_rms);
    std::fprintf(stderr, "[stereo]     PASS\n");
}

// ---------------------------------------------------------------------------
// Test 7: STFT reconstruction quality — below ceiling, wet ≈ dry
//
// With G=1 (no limiting), Hann WOLA at 75% overlap satisfies the COLA
// condition, so the wet path should reconstruct the input with the same
// kLatency delay as the dry path.  We verify by running two instances on
// the same signal: one with wet_mix=1, one with wet_mix=0 (known-good dry
// delay), and confirming their outputs agree after priming.
// ---------------------------------------------------------------------------
void test_stft_reconstruction() {
    const float ceiling = 0.891f;
    const float amp     = 0.01f;   // far below ceiling → gains stay at 1
    const int   N       = 4 * kSR;

    auto src = make_sine(440.0, amp, N);

    nablafx::MelLimiter ml_wet, ml_dry;
    ml_wet.init(kSR);
    ml_dry.init(kSR);

    auto p_wet = default_params(ceiling); p_wet.wet_mix = 1.f;
    auto p_dry = default_params(ceiling); p_dry.wet_mix = 0.f;

    std::vector<float> out_wet(src), out_dry(src);
    ml_wet.process(out_wet.data(), nullptr, 1, N, p_wet);
    ml_dry.process(out_dry.data(), nullptr, 1, N, p_dry);

    assert(all_finite(out_wet.data(), N));
    assert(all_finite(out_dry.data(), N));

    // After priming (4×kFFTSize), wet and dry should track each other.
    const int skip = 4 * nablafx::MelLimiter::kFFTSize;

    double max_err  = 0.0;
    double ref_peak = 0.0;
    for (int i = skip; i < N; ++i) {
        max_err  = std::max(max_err, std::abs(static_cast<double>(out_wet[i]) -
                                              static_cast<double>(out_dry[i])));
        ref_peak = std::max(ref_peak, std::abs(static_cast<double>(out_dry[i])));
    }
    double rel_err = (ref_peak > 1e-9) ? max_err / ref_peak : max_err;

    std::fprintf(stderr, "[recon]      max_err=%.2e  rel_err=%.2f%%  signal_peak=%.4f\n",
                max_err, rel_err * 100.0, amp);
    // Allow up to 3% relative reconstruction error (windowing + float ops).
    assert(rel_err < 0.03);
    std::fprintf(stderr, "[recon]      PASS\n");
}

// ---------------------------------------------------------------------------
// Test 8: brickwall guarantees output peak ≤ ceiling on a hot, driven signal
// ---------------------------------------------------------------------------
void test_brickwall_caps_peak() {
    nablafx::MelLimiter ml;
    ml.init(kSR);

    const float ceiling = 0.5f;          // -6 dBFS
    const int   N       = 4 * kSR;
    auto src = make_pink(N, 0.9);        // hot input, well above ceiling

    auto p = default_params(ceiling);
    p.drive_lin = std::pow(10.f, 12.f / 20.f);  // +12 dB drive — push hard

    std::vector<float> buf(src);
    ml.process(buf.data(), nullptr, 1, N, p);

    assert(all_finite(buf.data(), N));
    const int skip = 3 * nablafx::MelLimiter::kFFTSize;
    double pk = peak_abs(buf.data(), N, skip);
    std::fprintf(stderr, "[brickwall]  out_peak=%.4f  ceiling=%.4f (want ≤ ceiling)\n",
                pk, static_cast<double>(ceiling));
    // Hard ceiling guarantee (tiny float epsilon allowed).
    assert(pk <= ceiling + 1e-4);
    std::fprintf(stderr, "[brickwall]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 9: drive makes a quiet mix LOUDER while staying under the ceiling
// ---------------------------------------------------------------------------
void test_drive_increases_loudness() {
    const float ceiling = std::pow(10.f, -1.f / 20.f);  // -1 dBFS
    const int   N       = 4 * kSR;
    auto src = make_pink(N, std::pow(10.f, -6.f / 20.f)); // -6 dBFS peak mix

    const int skip = 3 * nablafx::MelLimiter::kFFTSize;
    double in_rms = rms(src.data(), N, skip);

    // No drive: roughly unity (a maximizer at 0 drive shouldn't pump it up).
    nablafx::MelLimiter ml0; ml0.init(kSR);
    auto p0 = default_params(ceiling); p0.drive_lin = 1.f;
    std::vector<float> b0(src);
    ml0.process(b0.data(), nullptr, 1, N, p0);
    double rms0 = rms(b0.data(), N, skip);

    // +18 dB drive: must be substantially louder, and still capped.
    nablafx::MelLimiter ml1; ml1.init(kSR);
    auto p1 = default_params(ceiling); p1.drive_lin = std::pow(10.f, 18.f / 20.f);
    std::vector<float> b1(src);
    ml1.process(b1.data(), nullptr, 1, N, p1);
    double rms1 = rms(b1.data(), N, skip);
    double pk1  = peak_abs(b1.data(), N, skip);

    std::fprintf(stderr,
        "[drive]      in_rms=%.4f  rms(0dB)=%.4f  rms(+18dB)=%.4f  peak(+18dB)=%.4f\n",
        in_rms, rms0, rms1, pk1);

    assert(all_finite(b1.data(), N));
    assert(rms1 > rms0 * 1.5);          // driving made it clearly louder
    assert(rms1 > in_rms);              // louder than the original mix
    assert(pk1 <= ceiling + 1e-4);     // still under the ceiling
    std::fprintf(stderr, "[drive]      PASS\n");
}

// ---------------------------------------------------------------------------
// Test 10: with drive engaged, the Ceiling control actually changes output
// (regression for "controls do nothing")
// ---------------------------------------------------------------------------
void test_ceiling_control_is_audible() {
    const int N = 4 * kSR;
    auto src = make_pink(N, std::pow(10.f, -6.f / 20.f));
    const int skip = 3 * nablafx::MelLimiter::kFFTSize;
    const float drive = std::pow(10.f, 18.f / 20.f);

    auto run_ceiling = [&](float ceil_db) {
        nablafx::MelLimiter ml; ml.init(kSR);
        auto p = default_params(std::pow(10.f, ceil_db / 20.f));
        p.drive_lin = drive;
        std::vector<float> b(src);
        ml.process(b.data(), nullptr, 1, N, p);
        return peak_abs(b.data(), N, skip);
    };

    double pk_hi = run_ceiling(-1.f);
    double pk_lo = run_ceiling(-12.f);
    std::fprintf(stderr, "[audible]    peak(ceil -1dB)=%.4f  peak(ceil -12dB)=%.4f\n",
                pk_hi, pk_lo);
    // Lower ceiling must yield a lower output peak — the control does something.
    assert(pk_lo < pk_hi * 0.6);
    std::fprintf(stderr, "[audible]    PASS\n");
}

// ---------------------------------------------------------------------------
// Test 11: adaptive-brickwall toggle slows the brickwall release
//
// Same params except adaptive_brickwall on/off. A loud section that drops to a
// quiet level lets the brickwall gain recover. The toggle changes ONLY the
// brickwall, so any difference isolates it. Slow release ⇒ the region right
// after the drop stays more attenuated (lower RMS). The ceiling must hold in
// both modes.
// ---------------------------------------------------------------------------
void test_adaptive_brickwall_release() {
    const float ceiling = 0.5f;            // -6 dBFS
    const int   sec     = kSR;
    const int   N       = 2 * sec;

    // Quiet tone (amp 0.35 < ceiling) so the broadband spectral solver stays
    // inert (total < C, early-out), with one brief loud burst whose energy is
    // small enough to keep the 1024-window total below the ceiling but whose
    // PEAK exceeds it. Only the brickwall reacts → its release is isolated.
    const int bstart = sec / 2;
    const int blen   = 32;                  // ~0.7 ms
    std::vector<float> src = make_sine(1000.0, 0.35, N);
    {
        auto burst = make_sine(1000.0, 1.0, blen);
        std::copy(burst.begin(), burst.end(), src.begin() + bstart);
    }

    auto run = [&](bool adaptive_bw) {
        nablafx::MelLimiter ml; ml.init(kSR);
        auto p = default_params(ceiling);
        p.drive_lin         = 1.f;
        p.adaptive_gain     = 0.f;          // tight attack — isolate release
        p.adaptive_speed    = 1.f;          // → 400 ms brickwall release when on
        p.adaptive_brickwall = adaptive_bw;
        std::vector<float> b(src);
        ml.process(b.data(), nullptr, 1, N, p);
        return b;
    };

    auto b_off = run(false);
    auto b_on  = run(true);

    assert(all_finite(b_off.data(), N));
    assert(all_finite(b_on.data(),  N));

    // After the burst, the brickwall gain releases back to unity. Measure a
    // window 5–60 ms after the burst: slow (dynamic) release keeps the tone
    // more ducked than the fast (even) release.
    const int L  = nablafx::MelLimiter::kLatency;
    const int w0 = bstart + L + blen + kSR / 200;     // +5 ms after burst
    const int w1 = bstart + L + blen + (3 * kSR) / 50; // +60 ms after burst
    double rms_off = rms(b_off.data() + w0, w1 - w0);
    double rms_on  = rms(b_on.data()  + w0, w1 - w0);
    double pk_off  = peak_abs(b_off.data(), N);
    double pk_on   = peak_abs(b_on.data(),  N);

    std::fprintf(stderr,
        "[adapt-bw]   post-burst rms: off=%.4f on=%.4f (ratio %.2f)   peak: off=%.4f on=%.4f (ceil %.2f)\n",
        rms_off, rms_on, rms_on / rms_off, pk_off, pk_on, (double)ceiling);

    assert(pk_off <= ceiling + 1e-4);       // ceiling held, fixed release
    assert(pk_on  <= ceiling + 1e-4);       // ceiling held, dynamic release
    assert(rms_on < rms_off * 0.9);         // dynamic release recovers slower
    std::fprintf(stderr, "[adapt-bw]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 12: in Dynamic mode, adaptive_gain shapes the brickwall ATTACK
//
// Loose attack (gain=1) pre-ducks less in the lookahead window before a peak,
// so more of the pre-transient signal survives than with tight attack (gain=0).
// The ceiling must still hold in both (the safety clip catches the overshoot).
// ---------------------------------------------------------------------------
void test_adaptive_brickwall_attack() {
    const float ceiling = 0.5f;
    const int   sec     = kSR;
    const int   N       = 2 * sec;
    const int   bstart  = sec / 2;
    const int   blen    = 32;

    std::vector<float> src = make_sine(1000.0, 0.35, N);
    {
        auto burst = make_sine(1000.0, 1.0, blen);
        std::copy(burst.begin(), burst.end(), src.begin() + bstart);
    }

    auto run = [&](float gain) {
        nablafx::MelLimiter ml; ml.init(kSR);
        auto p = default_params(ceiling);
        p.drive_lin          = 1.f;
        p.adaptive_speed     = 0.f;         // fast release — isolate attack
        p.adaptive_gain      = gain;        // attack character
        p.adaptive_brickwall = true;
        std::vector<float> b(src);
        ml.process(b.data(), nullptr, 1, N, p);
        return b;
    };

    auto tight = run(0.f);   // clamped attack
    auto loose = run(1.f);   // punchy attack

    assert(all_finite(tight.data(), N));
    assert(all_finite(loose.data(), N));

    // Pre-duck window: the lookahead-length region right before the burst peak
    // reaches the output. Tight attack ducks this harder than loose.
    const int L  = nablafx::MelLimiter::kLatency;
    const int LA = nablafx::MelLimiter::kBrickLA;
    const int w0 = bstart + L - LA;
    const int w1 = bstart + L;
    double rms_tight = rms(tight.data() + w0, w1 - w0);
    double rms_loose = rms(loose.data() + w0, w1 - w0);
    double pk_tight  = peak_abs(tight.data(), N);
    double pk_loose  = peak_abs(loose.data(), N);

    std::fprintf(stderr,
        "[adapt-atk]  pre-duck rms: tight=%.4f loose=%.4f (ratio %.2f)   peak: tight=%.4f loose=%.4f\n",
        rms_tight, rms_loose, rms_loose / rms_tight, pk_tight, pk_loose);

    assert(pk_tight <= ceiling + 1e-4);     // ceiling held, clamped attack
    assert(pk_loose <= ceiling + 1e-4);     // ceiling held, punchy attack
    assert(rms_loose > rms_tight * 1.1);    // loose attack lets more through
    std::fprintf(stderr, "[adapt-atk]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 13: the lookahead limiter holds the ceiling on a bass tone with far
// less distortion than a pure clipper (the no-lookahead degenerate case).
// ---------------------------------------------------------------------------
void test_lookahead_low_distortion() {
    const float ceiling = 0.5f;
    const int   N       = 2 * kSR;
    const double f      = 60.0;
    auto src = make_sine(f, 1.0, N);        // bass tone, well above ceiling

    // Limiter (Even mode — clean attack within the lookahead).
    nablafx::MelLimiter ml; ml.init(kSR);
    auto p = default_params(ceiling);
    p.drive_lin = 1.f;
    std::vector<float> lim(src);
    ml.process(lim.data(), nullptr, 1, N, p);

    // Pure clipper baseline: hard-clip to ±ceiling, no lookahead/gain ride.
    std::vector<float> clip(src);
    for (auto& v : clip) v = std::clamp(v, -ceiling, ceiling);

    const int start = kSR / 2, len = kSR;   // settled region, ~60 cycles
    double thd_lim  = thd_frac(lim.data(),  start, len, f);
    double thd_clip = thd_frac(clip.data(), start, len, f);
    double pk = peak_abs(lim.data(), N, 3 * nablafx::MelLimiter::kFFTSize);

    std::fprintf(stderr,
        "[lookahead]  THD limiter=%.1f%%  clipper=%.1f%%   limiter peak=%.4f (ceil %.2f)\n",
        thd_lim * 100, thd_clip * 100, pk, (double)ceiling);

    assert(pk <= ceiling + 1e-4);            // ceiling held
    assert(thd_lim < thd_clip * 0.4);        // lookahead ⇒ much cleaner
    std::fprintf(stderr, "[lookahead]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 14: per-hop bin-gain scratch buffer is reused correctly across calls
//
// Regression for promoting the per-hop per-bin gain scratch from a heap-
// allocated `std::vector<float> bin_gain_arr(n_freq_, 0.f)` inside process()
// to a pre-allocated member `bin_gain_arr_`. The contract: the member is
// pre-sized in init() and cleared (std::fill) before use each hop, so reusing
// it across process() calls must produce byte-identical output to processing
// the same signal in one shot. If the buffer were not properly cleared/reused
// (e.g. carried stale gains from a previous call's last hop, or were left
// mis-sized), call-boundary outputs would diverge from the single-call run.
//
// This drives the hop loop many times across many process() invocations of
// varied block sizes, exactly exercising the reused-member path that replaced
// the per-call allocation.
// ---------------------------------------------------------------------------
void test_bin_gain_scratch_reuse_block_invariance() {
    const float ceiling = 0.5f;
    const int   N       = 4 * kSR;
    // Hot, spectrally rich signal so the water-filling solver runs and the
    // per-bin gain scratch is actively written every hop (not an early-out).
    auto src = make_pink(N, 3.0 * ceiling);

    auto p = default_params(ceiling);
    p.drive_lin = std::pow(10.f, 6.f / 20.f);

    // Reference: one big process() call.
    nablafx::MelLimiter ref; ref.init(kSR);
    std::vector<float> out_ref(src);
    ref.process(out_ref.data(), nullptr, 1, N, p);
    assert(all_finite(out_ref.data(), N));

    // Chunked: many process() calls of awkward, varying sizes. Each call
    // re-enters process() and reuses the SAME member scratch buffer; the only
    // way the per-hop fill+reuse is correct is if this matches the reference
    // sample-for-sample. Sizes are deliberately not multiples of kHopSize so
    // hop boundaries straddle call boundaries.
    nablafx::MelLimiter chk; chk.init(kSR);
    std::vector<float> out_chk(src);
    const int sizes[] = {1, 7, 13, 64, 100, 255, 256, 257, 333, 1000};
    int pos = 0, si = 0;
    while (pos < N) {
        int blk = sizes[si % (int)(sizeof(sizes) / sizeof(sizes[0]))];
        if (pos + blk > N) blk = N - pos;
        chk.process(out_chk.data() + pos, nullptr, 1, blk, p);
        pos += blk; ++si;
    }
    assert(all_finite(out_chk.data(), N));

    double max_err = 0.0;
    for (int i = 0; i < N; ++i)
        max_err = std::max(max_err,
            std::abs(static_cast<double>(out_chk[i]) -
                     static_cast<double>(out_ref[i])));

    std::fprintf(stderr,
        "[scratch]    max |chunked - single| = %.3e (want exactly 0)\n", max_err);
    // Reused, per-hop-cleared scratch ⇒ identical math ⇒ bit-exact result.
    assert(max_err == 0.0);
    std::fprintf(stderr, "[scratch]    PASS\n");
}

// ---------------------------------------------------------------------------
// Test 15: the bin-gain scratch is sized/cleared by init(), not by the first
// process() call, and carries no stale state between instances.
//
// The old code sized AND zero-filled the scratch on every process() call via
// the vector ctor. The fix sizes it once in init() and relies on the per-hop
// std::fill for zeroing. This verifies init() leaves the limiter in a clean,
// fully-allocated state by:
//   (a) running a loud signal immediately after init() (no warm-up call) and
//       requiring finite output + the hard ceiling to hold — a mis-sized or
//       unallocated member scratch would read/write out of bounds or skip the
//       gain apply, breaking the ceiling on the very first hops; and
//   (b) confirming two independently-init()'d instances yield identical output
//       for the same input — no construction-time state leaks through.
// ---------------------------------------------------------------------------
void test_bin_gain_scratch_init_state() {
    const float ceiling = 0.5f;
    const int   N       = 2 * kSR;
    auto src = make_pink(N, 4.0 * ceiling, 7);

    auto p = default_params(ceiling);
    p.drive_lin = std::pow(10.f, 6.f / 20.f);

    // (a) First-call correctness straight after init() (no prior process()).
    nablafx::MelLimiter a; a.init(kSR);
    std::vector<float> ba(src);
    a.process(ba.data(), nullptr, 1, N, p);
    assert(all_finite(ba.data(), N));
    double pk = peak_abs(ba.data(), N);
    std::fprintf(stderr,
        "[scratch2]   first-call peak=%.4f ceiling=%.4f (want <= ceiling)\n",
        pk, static_cast<double>(ceiling));
    assert(pk <= ceiling + 1e-4);

    // (b) Two fresh instances must agree bit-for-bit (no leaked member state).
    nablafx::MelLimiter b; b.init(kSR);
    std::vector<float> bb(src);
    b.process(bb.data(), nullptr, 1, N, p);
    assert(all_finite(bb.data(), N));

    double max_err = 0.0;
    for (int i = 0; i < N; ++i)
        max_err = std::max(max_err,
            std::abs(static_cast<double>(ba[i]) - static_cast<double>(bb[i])));
    std::fprintf(stderr,
        "[scratch2]   max |instA - instB| = %.3e (want exactly 0)\n", max_err);
    assert(max_err == 0.0);
    std::fprintf(stderr, "[scratch2]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 16: num_bands() accessor + copy_display() band centres are the HTK-mel
// grid (known-answer, independent oracle)
//
// num_bands() is the public sizing contract for the display API: a UI client
// allocates num_bands() floats per array and calls copy_display(). Verify the
// accessor both at compile time and at runtime, then verify the centres it
// sizes are exactly the 26-point HTK-mel grid over [20, 20000] Hz. The oracle
// re-derives the grid from the published HTK formula (mel = 2595·log10(1+f/700))
// in double — an independent copy on purpose (repo convention: tests keep
// their own oracles rather than including src/mel_scale.hpp).
//
// Tolerance derivation (not tuned): the implementation runs the same formula
// in float (log10f/powf ≤ 2 ulp ≈ 2.4e-7 rel each), the mel→hz step's
// (10^x − 1) amplifies relative error by x/(x−1) ≤ 7.1 (worst at band 0,
// ~115 Hz), and the hz→bin→hz quantisation roundtrip adds 2 ulp (no Nyquist
// clamp bites: max bin ≈ 464 @ 44.1k / 427 @ 48k, both < 512). Worst-case
// bound ≈ 4e-6 relative; assert < 2e-5 (5× headroom, still far below the
// half-band spacing that any formula/off-by-one regression would cause).
// ---------------------------------------------------------------------------
static_assert(nablafx::MelLimiter::num_bands() == nablafx::MelLimiter::kNumBands,
              "num_bands() must expose kNumBands");
static_assert(nablafx::MelLimiter::num_bands() == 26,
              "display contract: 26 mel bands");

void test_num_bands_and_mel_centers() {
    // Runtime accessor call, used the way a display client uses it: to size
    // the copy_display() arrays.
    const int nb = nablafx::MelLimiter::num_bands();
    assert(nb == 26);

    for (int sr : {44100, 48000}) {
        nablafx::MelLimiter ml;
        ml.init(sr);

        std::vector<float> lev(nb, -1.f), gn(nb, -1.f), ctr(nb, -1.f);
        ml.copy_display(lev.data(), gn.data(), ctr.data());

        // Independent double-precision HTK-mel oracle.
        const double mmin = 2595.0 * std::log10(1.0 + 20.0    / 700.0);
        const double mmax = 2595.0 * std::log10(1.0 + 20000.0 / 700.0);
        double max_rel = 0.0;
        for (int b = 0; b < nb; ++b) {
            const double mel = mmin + (mmax - mmin) * (b + 1) / (nb + 1);
            const double hz  = 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
            const double rel = std::abs(static_cast<double>(ctr[b]) - hz) / hz;
            max_rel = std::max(max_rel, rel);

            assert(std::isfinite(ctr[b]));
            assert(ctr[b] > 20.f);                                // above f_min
            assert(ctr[b] < static_cast<float>(sr) / 2.f);        // below Nyquist
            if (b > 0) assert(ctr[b] > ctr[b - 1]);               // strictly ascending
        }
        std::fprintf(stderr,
            "[bands]      sr=%d  centers[0]=%.2f Hz  centers[%d]=%.2f Hz  "
            "max_rel_err=%.2e (want < 2e-5)\n",
            sr, ctr[0], nb - 1, ctr[nb - 1], max_rel);
        assert(max_rel < 2e-5);
    }
    std::fprintf(stderr, "[bands]      PASS\n");
}

// ---------------------------------------------------------------------------
// Test 17: brickwall_gain() accessor tracks the true-peak gain envelope
//
// Same isolated-brickwall signal as tests 11/12: a 0.35-amp tone (spectral
// solver stays inert: window total < ceiling) with one 32-sample amp-1.0
// burst whose PEAK alone exceeds the 0.5 ceiling. Process in 64-sample blocks
// and sample brickwall_gain() after each block:
//   (a) always in (0, 1];
//   (b) EXACTLY 1.0f for every block that ends before the burst enters the
//       input — below the ceiling g_req = 1 and the one-pole update
//       g = c·g + (1−c)·1 stays exactly 1.0f in float (c ∈ [0.5,1) ⇒ 1−c is
//       exact by Sterbenz, and c + (1−c) sums to exactly 1.0);
//   (c) ducked near ceiling/peak around the burst: target g_req = 0.5/wmax
//       with wmax = wet burst peak ∈ [0.97, 1.03] (≤3% WOLA recon error,
//       test 7) ⇒ gain never drops below 0.45; the attack (τ = kBrickLA/4 =
//       64 samples) holds while the burst sits in the 256-sample lookahead
//       window (~288 steps ≈ 4.5τ), converging to ≈0.505, so the sampled
//       minimum must be < 0.6;
//   (d) recovered to ≈1 by the end (release τ = 50 ms ⇒ ~29τ of quiet tone
//       remain after the burst ⇒ > 0.999);
//   (e) deterministic: a second identical run yields a bitwise-identical
//       gain trace;
//   (f) linked stereo: a burst on R only must duck the (shared) gain too.
// ---------------------------------------------------------------------------
void test_brickwall_gain_accessor() {
    const float ceiling = 0.5f;
    const int   sec     = kSR;
    const int   N       = 2 * sec;
    const int   bstart  = sec / 2;
    const int   blen    = 32;
    const int   blk     = 64;

    std::vector<float> src = make_sine(1000.0, 0.35, N);
    {
        auto burst = make_sine(1000.0, 1.0, blen);
        std::copy(burst.begin(), burst.end(), src.begin() + bstart);
    }

    auto trace_gains = [&](const std::vector<float>& in) {
        nablafx::MelLimiter ml;
        ml.init(kSR);
        assert(ml.brickwall_gain() == 1.0f);        // exact after init
        std::vector<float> buf(in);
        std::vector<float> g;
        for (int pos = 0; pos < N; pos += blk) {
            const int n = std::min(blk, N - pos);
            ml.process(buf.data() + pos, nullptr, 1, n, default_params(ceiling));
            g.push_back(ml.brickwall_gain());
        }
        assert(all_finite(buf.data(), N));
        return g;
    };

    const auto g = trace_gains(src);

    float gmin = 1.f;
    for (size_t i = 0; i < g.size(); ++i) {
        assert(g[i] > 0.f && g[i] <= 1.f);                     // (a) bounds
        if (static_cast<int>(i + 1) * blk <= bstart)
            assert(g[i] == 1.0f);                              // (b) exact unity
        gmin = std::min(gmin, g[i]);
    }
    std::fprintf(stderr,
        "[bw-gain]    min=%.4f (want 0.45 < min < 0.6)  final=%.6f (want > 0.999)\n",
        gmin, g.back());
    assert(gmin < 0.6f && gmin > 0.45f);                       // (c) ducked, bounded
    assert(g.back() > 0.999f);                                 // (d) recovered

    // (e) determinism: identical fresh run ⇒ bitwise-identical trace.
    const auto g2 = trace_gains(src);
    assert(g2.size() == g.size());
    for (size_t i = 0; i < g.size(); ++i) assert(g2[i] == g[i]);

    // (f) linked stereo: burst on R only still ducks the shared gain.
    {
        nablafx::MelLimiter ml;
        ml.init(kSR);
        std::vector<float> lb = make_sine(1000.0, 0.35, N);    // no burst on L
        std::vector<float> rb(src);                            // burst on R
        float smin = 1.f;
        for (int pos = 0; pos < N; pos += blk) {
            const int n = std::min(blk, N - pos);
            ml.process(lb.data() + pos, rb.data() + pos, 2, n,
                       default_params(ceiling));
            smin = std::min(smin, ml.brickwall_gain());
        }
        assert(all_finite(lb.data(), N));
        assert(all_finite(rb.data(), N));
        std::fprintf(stderr,
            "[bw-gain]    stereo-linked min=%.4f (want < 0.6, burst on R only)\n",
            smin);
        assert(smin < 0.6f && smin > 0.45f);
    }
    std::fprintf(stderr, "[bw-gain]    PASS\n");
}

// ---------------------------------------------------------------------------
// Test 18: reset() restores brickwall_gain() to exactly 1 and makes the
// instance bitwise-equivalent to a freshly init()'d one
//
// Duck the gain (stop mid-attack right after the burst reaches the brickwall:
// at that point g ≈ 0.5 + 0.5·e^(−~85/64) ≈ 0.64, so 0.4 < g < 0.9), then
// reset() and require:
//   - brickwall_gain() == 1.0f exactly (reset writes 1.f);
//   - it STAYS exactly 1.0f through silence and a below-ceiling tone
//     (same exact-unity argument as test 17b);
//   - reprocessing the full signal after reset() matches a fresh instance
//     bitwise — reset clears every piece of process()-visible state.
// ---------------------------------------------------------------------------
void test_brickwall_gain_reset() {
    const float ceiling = 0.5f;
    const int   sec     = kSR;
    const int   N       = 2 * sec;
    const int   bstart  = sec / 2;
    const int   blen    = 32;

    std::vector<float> src = make_sine(1000.0, 0.35, N);
    {
        auto burst = make_sine(1000.0, 1.0, blen);
        std::copy(burst.begin(), burst.end(), src.begin() + bstart);
    }

    nablafx::MelLimiter ml;
    ml.init(kSR);
    const auto p = default_params(ceiling);

    // Stop mid-attack: the burst reaches the brickwall input kFFTSize+1
    // samples after bstart (WOLA group delay + z1); give it ~64 more samples.
    const int upto = bstart + blen + nablafx::MelLimiter::kFFTSize + 64;
    std::vector<float> head(src.begin(), src.begin() + upto);
    ml.process(head.data(), nullptr, 1, upto, p);
    const float ducked = ml.brickwall_gain();
    std::fprintf(stderr,
        "[bw-reset]   mid-attack gain=%.4f (want 0.4 < g < 0.9)\n", ducked);
    assert(ducked > 0.4f && ducked < 0.9f);

    ml.reset();
    assert(ml.brickwall_gain() == 1.0f);                       // exact

    // Silence keeps it pinned at exactly 1.
    std::vector<float> zeros(4096, 0.f);
    ml.process(zeros.data(), nullptr, 1, 4096, p);
    assert(ml.brickwall_gain() == 1.0f);

    // A below-ceiling tone keeps it pinned at exactly 1 as well.
    ml.reset();
    auto quiet = make_sine(1000.0, 0.2, 8192);
    ml.process(quiet.data(), nullptr, 1, 8192, p);
    assert(ml.brickwall_gain() == 1.0f);

    // reset() ≡ fresh instance: reprocess the whole signal on the reset
    // limiter and on a brand-new one — outputs must be bitwise identical.
    ml.reset();
    std::vector<float> out_reset(src);
    ml.process(out_reset.data(), nullptr, 1, N, p);

    nablafx::MelLimiter fresh;
    fresh.init(kSR);
    std::vector<float> out_fresh(src);
    fresh.process(out_fresh.data(), nullptr, 1, N, p);

    double max_err = 0.0;
    for (int i = 0; i < N; ++i)
        max_err = std::max(max_err,
            std::abs(static_cast<double>(out_reset[i]) -
                     static_cast<double>(out_fresh[i])));
    std::fprintf(stderr,
        "[bw-reset]   max |reset - fresh| = %.3e (want exactly 0)\n", max_err);
    assert(max_err == 0.0);
    assert(ml.brickwall_gain() == fresh.brickwall_gain());
    std::fprintf(stderr, "[bw-reset]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 19: copy_display() taps follow the limiter's state machine
//
//   init      → gains exactly 1.0f, levels exactly 0.0f;
//   hot pink  → limiting engaged: measured total level exceeds the ceiling
//               AND smoothed gains dropped (min < 0.9), all gains ∈ [0,1];
//   2 s quiet → gains release back to ≈1 (τ = 30+0.5·370 ≈ 215 ms ⇒ ~9τ);
//   reset     → gains exactly 1.0f again;
//   centres   → bitwise-static through all of the above (built once in init).
// ---------------------------------------------------------------------------
void test_copy_display_state_machine() {
    const int nb = nablafx::MelLimiter::num_bands();
    const float ceiling = 0.5f;

    nablafx::MelLimiter ml;
    ml.init(kSR);

    std::vector<float> lev(nb, -1.f), gn(nb, -1.f), ctr(nb, -1.f);
    ml.copy_display(lev.data(), gn.data(), ctr.data());
    const std::vector<float> ctr0(ctr);          // snapshot: static centres
    for (int b = 0; b < nb; ++b) {
        assert(gn[b]  == 1.0f);                  // no limiting yet — exact
        assert(lev[b] == 0.0f);                  // nothing measured yet — exact
    }

    // Hot pink noise well above the ceiling ⇒ solver active every hop.
    auto p = default_params(ceiling);
    p.drive_lin = std::pow(10.f, 6.f / 20.f);
    auto loud = make_pink(2 * kSR, 4.0 * ceiling, 11);
    ml.process(loud.data(), nullptr, 1, 2 * kSR, p);

    ml.copy_display(lev.data(), gn.data(), ctr.data());
    float gmin = 1.f;
    double sum_sq = 0.0;
    for (int b = 0; b < nb; ++b) {
        assert(std::isfinite(lev[b]) && lev[b] >= 0.f);
        assert(std::isfinite(gn[b]) && gn[b] >= 0.f && gn[b] <= 1.f);
        assert(ctr[b] == ctr0[b]);               // centres bitwise-static
        gmin   = std::min(gmin, gn[b]);
        sum_sq += static_cast<double>(lev[b]) * lev[b];
    }
    const double total = std::sqrt(sum_sq);
    std::fprintf(stderr,
        "[display]    loud: total_level=%.3f (want > ceiling %.2f)  "
        "min_gain=%.3f (want < 0.9)\n", total, (double)ceiling, gmin);
    assert(total > ceiling);                     // over ceiling ⇒ limiting…
    assert(gmin < 0.9f);                         // …and gains actually dropped

    // 2 s of near-silence: release (τ ≈ 215 ms at speed 0.5) ⇒ gains ≈ 1.
    auto tiny = make_sine(1000.0, 1e-4, 2 * kSR);
    ml.process(tiny.data(), nullptr, 1, 2 * kSR, p);
    ml.copy_display(lev.data(), gn.data(), ctr.data());
    float gmin_rel = 1.f;
    for (int b = 0; b < nb; ++b) {
        gmin_rel = std::min(gmin_rel, gn[b]);
        assert(ctr[b] == ctr0[b]);
    }
    std::fprintf(stderr,
        "[display]    after 2s quiet: min_gain=%.5f (want > 0.99)\n", gmin_rel);
    assert(gmin_rel > 0.99f);

    // reset(): per-band gains snap back to exactly 1; centres untouched.
    ml.reset();
    ml.copy_display(lev.data(), gn.data(), ctr.data());
    for (int b = 0; b < nb; ++b) {
        assert(gn[b] == 1.0f);
        assert(ctr[b] == ctr0[b]);
    }
    std::fprintf(stderr, "[display]    PASS\n");
}

} // namespace

int main() {
    test_silence();
    test_dry_bypass();
    test_ceiling_reduces_loud_signal();
    test_no_nan_on_extremes();
    test_reset_clears_state();
    test_stereo_linked();
    test_stft_reconstruction();
    test_brickwall_caps_peak();
    test_drive_increases_loudness();
    test_ceiling_control_is_audible();
    test_adaptive_brickwall_release();
    test_adaptive_brickwall_attack();
    test_lookahead_low_distortion();
    test_bin_gain_scratch_reuse_block_invariance();
    test_bin_gain_scratch_init_state();
    test_num_bands_and_mel_centers();
    test_brickwall_gain_accessor();
    test_brickwall_gain_reset();
    test_copy_display_state_machine();
    std::fprintf(stderr, "ALL TESTS PASSED\n");
    return 0;
}
