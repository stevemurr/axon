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
        p.adaptive_gain     = 1.f;
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
    std::fprintf(stderr, "ALL TESTS PASSED\n");
    return 0;
}
