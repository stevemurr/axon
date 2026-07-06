// test_tolerance_stages — platform-independent steady-state oracle for the
// DSP chain (the Windows/Linux port's per-platform acceptance test).
//
// WHY THIS EXISTS (docs/future/active/windows-linux-builds.md): cross-platform
// builds can never be null-tested against mac renders — the FFT backend
// differs (vDSP vs pffft) and two stages amplify 1-ULP noise to −75..−90 dBFS.
// So instead of bit-exactness, each platform must prove the same PHYSICS:
// every stage, driven with deterministic signals (sines at band centers,
// deterministic noise), must show the documented magnitude-response / level
// invariants within tolerances far wider than any FP/backend noise yet far
// tighter than any real DSP defect.
//
// TOLERANCE DOCTRINE (each assertion documents its own bound):
//   * exact           — passthrough paths contractually bit-identical
//                       (Widener at width=1, Reverb at mix=0).
//   * ±0.1..0.5 dB    — steady-state magnitude through linear/WOLA paths.
//                       Backend FP noise is ~1e-7 relative (test_accelerate_
//                       shim), i.e. ~1e-6 dB — these bounds are ~1e5 x above
//                       the noise floor and catch scaling/packing regressions
//                       of the smallest interesting size (a wrong 2x is 6 dB).
//   * band asserts    — nonlinear/adaptive stages (MelLimiter brickwall,
//                       Reverb tail) get invariant bands (ceiling never
//                       exceeded, tail decays) rather than point values.
//
// Runs on macOS (real vDSP) AND on the portable-shim platforms — the same
// binary semantics, two backends, one oracle. Keep it green everywhere.
//
// Build (macOS):
//   c++ -O2 -std=c++17 -UNDEBUG -I src tests/test_tolerance_stages.cpp \
//       src/mel_limiter.cpp src/meter.cpp -framework Accelerate \
//       -o /tmp/ttol && /tmp/ttol
// Or via CMake/ctest: target test_tolerance_stages.

#include "../src/bass_mono.hpp"
#include "../src/iir_filterbank_eq.hpp"
#include "../src/mel_limiter.hpp"
#include "../src/meter.hpp"
#include "../src/reverb.hpp"
#include "../src/spectral_mask_eq.hpp"
#include "../src/widener.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int    kSR = 44100;
constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Deterministic signal generators. NOTE: no std::*_distribution here — their
// output is implementation-defined, so libstdc++ and libc++ would disagree.
// Raw mt19937 draws are specified bit-exactly by the standard.
// ---------------------------------------------------------------------------

std::vector<float> make_sine(double freq, double amp, int n, int sr = kSR) {
    std::vector<float> out(n);
    for (int i = 0; i < n; ++i)
        out[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * i / sr));
    return out;
}

// xorshift64* white noise in [-1, 1) — bit-deterministic on every platform.
struct DetRng {
    uint64_t s;
    explicit DetRng(uint64_t seed) : s(seed ? seed : 0x9e3779b97f4a7c15ull) {}
    double next() {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        const uint64_t r = s * 0x2545f4914f6cdd1dull;
        return (static_cast<double>(r >> 11) / 9007199254740992.0) * 2.0 - 1.0;
    }
};

// Pink-ish noise (one-pole filtered white), peak-normalized to `amp`.
std::vector<float> make_pink(int n, double amp, uint64_t seed = 42) {
    DetRng rng(seed);
    std::vector<float> out(n);
    double acc = 0.0;
    for (int i = 0; i < n; ++i) {
        acc = 0.995 * acc + 0.05 * rng.next();
        out[i] = static_cast<float>(acc);
    }
    double peak = 0.0;
    for (float v : out) peak = std::max(peak, std::fabs(static_cast<double>(v)));
    if (peak > 1e-9)
        for (float& v : out) v = static_cast<float>(v * amp / peak);
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
    for (int i = skip; i < n; ++i) s += static_cast<double>(x[i]) * x[i];
    return std::sqrt(s / std::max(1, n - skip));
}

double peak_abs(const float* x, int n, int skip = 0) {
    double p = 0.0;
    for (int i = skip; i < n; ++i)
        p = std::max(p, std::fabs(static_cast<double>(x[i])));
    return p;
}

// Goertzel amplitude of the component at `freq` over x[start .. start+len).
// Use len = an integer number of periods for a clean estimate.
double goertzel_amp(const float* x, int start, int len, double freq, int sr = kSR) {
    const double k = 2.0 * std::cos(2.0 * kPi * freq / sr);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < len; ++i) {
        const double s0 = x[start + i] + k * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - k * s1 * s2;
    return std::sqrt(std::max(power, 0.0)) * (2.0 / len);
}

double db(double lin) { return 20.0 * std::log10(std::max(lin, 1e-30)); }

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::fprintf(stderr, "FAIL: " __VA_ARGS__);      \
            std::fprintf(stderr, "  [%s:%d: %s]\n",          \
                         __FILE__, __LINE__, #cond);         \
            std::exit(1);                                    \
        }                                                    \
    } while (0)

// ---------------------------------------------------------------------------
// 1) MelLimiter — below-threshold unity at band centers; brickwall ceiling
//    invariant + no crush when limiting.
// ---------------------------------------------------------------------------

void test_mel_limiter() {
    using nablafx::MelLimiter;

    // (a) Unity: a −20 dBFS sine at a mel band center is far below the
    // ceiling — the WOLA path must pass it at unity gain. Tolerance ±0.30 dB:
    // covers Hann-WOLA reconstruction ripple + band-gain smoother dither;
    // a wrong FFT scale (2x = 6 dB) or window bug is orders of magnitude out.
    MelLimiter probe;  // just for the band-center table
    probe.init(kSR);
    float lv[MelLimiter::kNumBands], gn[MelLimiter::kNumBands],
        ctr[MelLimiter::kNumBands];
    probe.copy_display(lv, gn, ctr);

    for (int band : {8, 13, 20}) {
        const double f = ctr[band];
        MelLimiter ml;
        ml.init(kSR);
        MelLimiter::Params p;  // ceiling 1.0, drive 1.0, wet 1.0
        const int n = kSR * 2;
        auto sig = make_sine(f, 0.1, n);
        std::vector<float> l = sig, r = sig;
        for (int i = 0; i + 256 <= n; i += 256)
            ml.process(&l[i], &r[i], 2, 256, p);
        CHECK(all_finite(l.data(), n), "mel unity: non-finite output\n");
        // Steady-state window: last 0.5 s (integer periods of f not needed —
        // Goertzel over 22050 samples of a locked sine is stable to <0.01 dB).
        const int start = n - kSR / 2, len = kSR / 2;
        const double a_out = goertzel_amp(l.data(), start, len, f);
        const double err_db = db(a_out / 0.1);
        std::fprintf(stderr,
                     "[mel] band %2d (%7.1f Hz): unity error %+.3f dB (tol ±0.30)\n",
                     band, f, err_db);
        CHECK(std::fabs(err_db) < 0.30, "mel unity off at band %d\n", band);
    }

    // (b) Limiting: pink noise driven 4x into a 0.5 ceiling. Invariants:
    //   - brickwall: |out| <= ceiling * (1 + 1e-3) after warmup (hard bound);
    //   - no crush: steady-state RMS >= ceiling * 0.15 (a solver that reads
    //     everything as over-ceiling — the old FFT-normalization bug — lands
    //     ~40 dB below this).
    {
        MelLimiter ml;
        ml.init(kSR);
        MelLimiter::Params p;
        p.ceiling_lin = 0.5f;
        p.drive_lin   = 4.0f;
        const int n = kSR * 3;
        auto sig = make_pink(n, 0.9, /*seed=*/7);
        std::vector<float> l = sig, r = sig;
        for (int i = 0; i + 256 <= n; i += 256)
            ml.process(&l[i], &r[i], 2, 256, p);
        CHECK(all_finite(l.data(), n), "mel limit: non-finite output\n");
        const int skip = MelLimiter::kLatency * 3;
        const double pk = peak_abs(l.data(), n, skip);
        const double rm = rms(l.data(), n, skip);
        std::fprintf(stderr,
                     "[mel] limit: peak %.4f (ceiling 0.5), rms %.4f\n", pk, rm);
        CHECK(pk <= 0.5 * (1.0 + 1e-3), "mel brickwall exceeded ceiling\n");
        CHECK(rm >= 0.5 * 0.15, "mel limiter crushed the signal\n");
    }
    std::fprintf(stderr, "[mel] PASS\n");
}

// ---------------------------------------------------------------------------
// 2) SpectralMaskEq render path — flat mask = unity; uniform ±dB mask moves a
//    mid-band sine by exactly that many dB.
// ---------------------------------------------------------------------------

nablafx::SpectralMaskEqParams smask_cfg() {
    nablafx::SpectralMaskEqParams p;
    p.sample_rate = kSR;
    p.block_size  = 128;
    p.num_control_params = 24;
    p.n_fft   = 2048;
    p.hop     = 512;
    p.n_bands = 24;
    p.min_gain_db = -12.f;
    p.max_gain_db = 12.f;
    p.f_min = 40.f;
    p.f_max = 18000.f;
    return p;
}

// Drive a sine through the mask EQ with a constant band vector; return the
// steady-state 1 kHz amplitude change in dB.
double smask_gain_at_1k(float band_sigmoid) {
    const auto cfg = smask_cfg();
    nablafx::SpectralMaskEq eq;
    eq.reset(cfg);
    std::vector<float> bands(cfg.n_bands, band_sigmoid);
    const int n = kSR * 3;  // mask smoother tau = 25 ms; 3 s >> converged
    auto in = make_sine(1000.0, 0.3, n);
    std::vector<float> out(n, 0.f);
    for (int i = 0; i + cfg.block_size <= n; i += cfg.block_size) {
        eq.set_params(bands.data(), bands.size());
        eq.process(&in[i], &out[i], cfg.block_size);
    }
    CHECK(all_finite(out.data(), n), "smask: non-finite output\n");
    const int start = n - kSR / 2, len = kSR / 2;
    return db(goertzel_amp(out.data(), start, len, 1000.0) / 0.3);
}

void test_spectral_mask_eq() {
    // Flat (sigmoid 0.5 → 0 dB): unity within ±0.30 dB (WOLA ripple +
    // band→bin interpolation wiggle; a backend scale bug is >= 6 dB).
    const double flat = smask_gain_at_1k(0.5f);
    std::fprintf(stderr, "[smask] flat 0 dB mask: %+0.3f dB (tol ±0.30)\n", flat);
    CHECK(std::fabs(flat) < 0.30, "smask flat mask not unity\n");

    // Uniform full cut (sigmoid 0 → −12 dB across every band): a mid-band
    // sine must drop by 12 dB. Tolerance ±0.75 dB: the 1/6-octave dB-domain
    // kernel and mel band→bin accumulation smear a uniform mask slightly at
    // band edges; 1 kHz sits mid-band so the residual is small. The failure
    // modes this hunts (min-phase/cepstral round-trip broken by a bad FFT
    // packing) miss by many dB.
    const double cut = smask_gain_at_1k(0.0f);
    std::fprintf(stderr, "[smask] uniform -12 dB mask: %+0.3f dB (want -12 ±0.75)\n", cut);
    CHECK(std::fabs(cut - (-12.0)) < 0.75, "smask uniform cut off\n");

    // Uniform full boost (sigmoid 1 → +12 dB): symmetric check.
    const double boost = smask_gain_at_1k(1.0f);
    std::fprintf(stderr, "[smask] uniform +12 dB mask: %+0.3f dB (want +12 ±0.75)\n", boost);
    CHECK(std::fabs(boost - 12.0) < 0.75, "smask uniform boost off\n");
    std::fprintf(stderr, "[smask] PASS\n");
}

// ---------------------------------------------------------------------------
// 3) IirFilterbankEq — flat = unity; measured signal gain must agree with the
//    filter's own analytic magnitude response (render path == design).
// ---------------------------------------------------------------------------

void test_iir_filterbank_eq() {
    using nablafx::IirFilterbankEq;
    const auto cfg = smask_cfg();

    // Flat: analytic magnitude ~0 dB everywhere, measured unity within ±0.10.
    {
        IirFilterbankEq eq;
        eq.reset(cfg);
        std::vector<float> bands(cfg.n_bands, 0.5f);
        for (int k = 0; k < 400; ++k) eq.set_params(bands.data(), bands.size());
        double worst = 0.0;
        for (double f : {60.0, 250.0, 1000.0, 4000.0, 12000.0})
            worst = std::max(worst, std::fabs(eq.magnitude_db(f)));
        std::fprintf(stderr, "[iireq] flat worst |mag| %.4f dB (tol 0.10)\n", worst);
        CHECK(worst < 0.10, "iireq flat magnitude not unity\n");

        const int n = kSR;
        auto in = make_sine(1000.0, 0.3, n);
        std::vector<float> out(n, 0.f);
        for (int i = 0; i + 128 <= n; i += 128) {
            eq.set_params(bands.data(), bands.size());
            eq.process(&in[i], &out[i], 128);
        }
        const double g = db(goertzel_amp(out.data(), n - kSR / 2, kSR / 2, 1000.0) / 0.3);
        std::fprintf(stderr, "[iireq] flat measured %+0.3f dB (tol ±0.10)\n", g);
        CHECK(std::fabs(g) < 0.10, "iireq flat render not unity\n");
    }

    // Boost: uniform +6 dB target (sigmoid 0.75 with ±12 range). The solver's
    // realized curve is its own contract — assert the RENDERED gain matches
    // the ANALYTIC magnitude_db(1 kHz) within ±0.25 dB (IIR is sample-exact;
    // the margin covers only the smoother tail + Goertzel leakage), and that
    // the analytic value landed in a sane window around +6.
    {
        IirFilterbankEq eq;
        eq.reset(cfg);
        std::vector<float> bands(cfg.n_bands, 0.75f);
        for (int k = 0; k < 400; ++k) eq.set_params(bands.data(), bands.size());
        const double mag = eq.magnitude_db(1000.0);

        const int n = kSR * 2;
        auto in = make_sine(1000.0, 0.1, n);
        std::vector<float> out(n, 0.f);
        for (int i = 0; i + 128 <= n; i += 128) {
            eq.set_params(bands.data(), bands.size());
            eq.process(&in[i], &out[i], 128);
        }
        const double g = db(goertzel_amp(out.data(), n - kSR / 2, kSR / 2, 1000.0) / 0.1);
        std::fprintf(stderr,
                     "[iireq] +6 dB target: analytic %+0.3f, rendered %+0.3f dB "
                     "(agree ±0.25; analytic in [4,8])\n", mag, g);
        CHECK(std::fabs(g - mag) < 0.25, "iireq render != analytic design\n");
        CHECK(mag > 4.0 && mag < 8.0, "iireq +6 dB solve landed outside [4,8] dB\n");
    }
    std::fprintf(stderr, "[iireq] PASS\n");
}

// ---------------------------------------------------------------------------
// 4) BassMono — side is killed below cutoff (LR4: ~50 dB at 60 Hz for a
//    250 Hz cutoff), untouched well above it; mono sum preserved.
// ---------------------------------------------------------------------------

void test_bass_mono() {
    nablafx::BassMono bm;
    bm.prepare(kSR);
    bm.set_cutoff(250.f);

    const int n = kSR * 2;
    // Side-only content: L = +x, R = -x (mid = 0, side = x).
    auto x60 = make_sine(60.0, 0.4, n);
    auto x4k = make_sine(4000.0, 0.4, n);
    std::vector<float> l(n), r(n);
    for (int i = 0; i < n; ++i) { l[i] = x60[i] + x4k[i]; r[i] = -(x60[i] + x4k[i]); }
    std::vector<float> l0 = l, r0 = r;
    bm.process(l.data(), r.data(), n);
    CHECK(all_finite(l.data(), n) && all_finite(r.data(), n), "bassmono: non-finite\n");

    std::vector<float> side(n), mono_err(n);
    for (int i = 0; i < n; ++i) {
        side[i]     = 0.5f * (l[i] - r[i]);
        mono_err[i] = (l[i] + r[i]) - (l0[i] + r0[i]);
    }
    const int start = n - kSR / 2, len = kSR / 2;
    const double s60 = goertzel_amp(side.data(), start, len, 60.0);
    const double s4k = goertzel_amp(side.data(), start, len, 4000.0);
    // 60 Hz side: LR4 @250 Hz gives ~-49 dB. Assert <= -35 dB (documented
    // margin for filter-warp differences; a broken HPF passes it at ~0 dB).
    const double atten60 = db(s60 / 0.4);
    // 4 kHz side: analytic LR4 loss at 16x cutoff is < 0.01 dB. Tol ±0.10.
    const double atten4k = db(s4k / 0.4);
    // Mono sum: process only touches the side; mid path is algebraically
    // exact up to float rounding of the recombine. Bound 1e-5 absolute
    // (inputs <= 0.8; float eps rounding is ~1e-7 per op).
    const double mono_peak = peak_abs(mono_err.data(), n);
    std::fprintf(stderr,
                 "[bassmono] side@60 %.1f dB (<= -35), side@4k %+0.3f dB (±0.10), "
                 "mono-sum err %.2e (<= 1e-5)\n", atten60, atten4k, mono_peak);
    CHECK(atten60 <= -35.0, "bassmono: 60 Hz side not collapsed\n");
    CHECK(std::fabs(atten4k) < 0.10, "bassmono: 4 kHz side altered\n");
    CHECK(mono_peak <= 1e-5, "bassmono: mono sum not preserved\n");
    std::fprintf(stderr, "[bassmono] PASS\n");
}

// ---------------------------------------------------------------------------
// 5) Widener — width=1/air=0 bit-identical; width=2 doubles the side above
//    the crossover, leaves it (and the mid) alone below/elsewhere.
// ---------------------------------------------------------------------------

void test_widener() {
    const int n = kSR * 2;

    // (a) Neutral: bit-identical passthrough (contractual early-out).
    {
        nablafx::Widener w;
        w.prepare(kSR);
        w.set_params(1.0f, 250.f, 0.0f);
        auto x = make_pink(n, 0.5, /*seed=*/11);
        std::vector<float> l = x, r = x;
        for (int i = 0; i < n; ++i) r[i] = 0.5f * x[i];  // decorrelate a bit
        std::vector<float> l0 = l, r0 = r;
        w.process(l.data(), r.data(), n);
        CHECK(std::memcmp(l.data(), l0.data(), n * sizeof(float)) == 0 &&
                  std::memcmp(r.data(), r0.data(), n * sizeof(float)) == 0,
              "widener: width=1 not bit-identical\n");
    }

    // (b) Mid-only input stays bit-identical at any width (side == 0).
    {
        nablafx::Widener w;
        w.prepare(kSR);
        w.set_params(2.0f, 250.f, 0.0f);
        auto x = make_sine(1000.0, 0.3, n);
        std::vector<float> l = x, r = x, l0 = l, r0 = r;
        w.process(l.data(), r.data(), n);
        CHECK(std::memcmp(l.data(), l0.data(), n * sizeof(float)) == 0 &&
                  std::memcmp(r.data(), r0.data(), n * sizeof(float)) == 0,
              "widener: mid-only input altered\n");
    }

    // (c) Side gain: width=2, side-only sines. 4 kHz (16x above the 250 Hz
    // crossover): S' = S + S_hi -> |1 + H_LR4| ≈ 1.99x (≈ +5.98 dB); assert
    // within [+5.6, +6.2] dB. 60 Hz (0.24x): |1 + H| ≈ 1.003; assert ±0.30.
    {
        nablafx::Widener w;
        w.prepare(kSR);
        w.set_params(2.0f, 250.f, 0.0f);
        auto x60 = make_sine(60.0, 0.2, n);
        auto x4k = make_sine(4000.0, 0.2, n);
        std::vector<float> l(n), r(n);
        for (int i = 0; i < n; ++i) { l[i] = x60[i] + x4k[i]; r[i] = -(x60[i] + x4k[i]); }
        w.process(l.data(), r.data(), n);
        CHECK(all_finite(l.data(), n), "widener: non-finite\n");
        std::vector<float> side(n);
        for (int i = 0; i < n; ++i) side[i] = 0.5f * (l[i] - r[i]);
        const int start = n - kSR / 2, len = kSR / 2;
        const double g4k = db(goertzel_amp(side.data(), start, len, 4000.0) / 0.2);
        const double g60 = db(goertzel_amp(side.data(), start, len, 60.0) / 0.2);
        std::fprintf(stderr,
                     "[widener] width=2 side gain: @4k %+0.3f dB (in [5.6,6.2]), "
                     "@60 %+0.3f dB (±0.30)\n", g4k, g60);
        CHECK(g4k > 5.6 && g4k < 6.2, "widener: 4 kHz side gain off\n");
        CHECK(std::fabs(g60) < 0.30, "widener: 60 Hz side not neutral\n");
    }
    std::fprintf(stderr, "[widener] PASS\n");
}

// ---------------------------------------------------------------------------
// 6) Reverb — mix=0 bit-identical; with mix>0 an impulse leaves a tail that
//    exists, stays finite/bounded, and decays.
// ---------------------------------------------------------------------------

void test_reverb() {
    const int n = kSR * 3;

    // (a) mix = 0: contractual bit-identical bypass.
    {
        nablafx::Reverb rv;
        rv.prepare(kSR);
        rv.set_params(0.0f, 0.5f, 0.8f, 7000.f, 250.f);
        auto x = make_pink(n, 0.5, /*seed=*/23);
        std::vector<float> l = x, r = x, l0 = l, r0 = r;
        rv.process(l.data(), r.data(), n);
        CHECK(std::memcmp(l.data(), l0.data(), n * sizeof(float)) == 0 &&
                  std::memcmp(r.data(), r0.data(), n * sizeof(float)) == 0,
              "reverb: mix=0 not bit-identical\n");
    }

    // (b) Impulse response, mix=0.3 size=0.5: tail present after the dry
    // instant, everything finite and bounded, energy decays over time.
    {
        nablafx::Reverb rv;
        rv.prepare(kSR);
        rv.set_params(0.3f, 0.5f, 0.8f, 7000.f, 250.f);
        std::vector<float> l(n, 0.f), r(n, 0.f);
        l[0] = r[0] = 1.0f;
        rv.process(l.data(), r.data(), n);
        CHECK(all_finite(l.data(), n) && all_finite(r.data(), n),
              "reverb: non-finite tail\n");
        const double early = rms(l.data(), kSR, kSR / 10);          // 0.1..1.0 s
        const double late  = rms(l.data() + 2 * kSR, kSR);          // 2.0..3.0 s
        const double pk    = peak_abs(l.data(), n, 1);              // tail peak
        std::fprintf(stderr,
                     "[reverb] tail rms early %.2e late %.2e (decay), peak %.3f\n",
                     early, late, pk);
        CHECK(early > 1e-6, "reverb: no tail at all\n");
        CHECK(late < early * 0.5, "reverb: tail not decaying\n");
        CHECK(pk < 1.0, "reverb: tail louder than the impulse\n");
    }
    std::fprintf(stderr, "[reverb] PASS\n");
}

// ---------------------------------------------------------------------------
// 7) LoudnessMeter — a 1 kHz sine at known level reads back at the BS.1770 /
//    RMS / peak values arithmetic says it must.
// ---------------------------------------------------------------------------

void test_meter() {
    nablafx::LoudnessMeter m;
    m.reset(kSR);
    const int n = kSR * 4;
    auto x = make_sine(1000.0, 0.1, n);  // −20 dBFS peak per channel
    for (int i = 0; i + 512 <= n; i += 512)
        m.process(&x[i], &x[i], 2, 512);
    const auto r = m.readout();

    // peak: decaying sample peak, refreshed every period → ≈ 20*log10(0.1)
    // = −20 dBFS. Tol ±0.2 dB (decay between refreshes is ~0.01 dB).
    // rms: mean power of the channel AVERAGE (= the same sine) → −23.01 dBFS
    // (sine RMS = amp/√2). Tol ±0.3 dB (window quantization).
    // LUFS: −0.691 + 10*log10(Σch k-weighted ms). The BS.1770 pre-shelf is
    // NOT flat at 1 kHz — it contributes ≈ +0.69 dB (the spec's calibration
    // point: a 0 dBFS single-channel 1 kHz sine reads −3.01 LKFS). So:
    // −0.691 + 10*log10(2·0.005) + 0.69 ≈ −20.0 LUFS. Tol ±0.5 dB.
    std::fprintf(stderr,
                 "[meter] peak %+0.2f (want -20 ±0.2)  rms %+0.2f (want -23.01 ±0.3)  "
                 "lufs_s %+0.2f lufs_m %+0.2f (want -20.0 ±0.5)\n",
                 r.peak_db, r.rms_db, r.lufs_s, r.lufs_m);
    CHECK(std::fabs(r.peak_db - (-20.0)) < 0.2, "meter peak off\n");
    CHECK(std::fabs(r.rms_db - (-23.01)) < 0.3, "meter rms off\n");
    CHECK(std::fabs(r.lufs_s - (-20.0)) < 0.5, "meter lufs_s off\n");
    CHECK(std::fabs(r.lufs_m - (-20.0)) < 0.5, "meter lufs_m off\n");
    CHECK(std::fabs(r.lufs_s - r.lufs_m) < 0.2, "meter S/M disagree at steady state\n");
    std::fprintf(stderr, "[meter] PASS\n");
}

}  // namespace

int main() {
    test_mel_limiter();
    test_spectral_mask_eq();
    test_iir_filterbank_eq();
    test_bass_mono();
    test_widener();
    test_reverb();
    test_meter();
    std::fprintf(stderr, "ALL TOLERANCE-ORACLE TESTS PASSED\n");
    return 0;
}
