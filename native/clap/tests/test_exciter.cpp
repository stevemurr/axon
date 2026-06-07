// Unit tests for the Aphex-style Exciter.
//   g++ -O2 -std=c++17 -I src tests/test_exciter.cpp -o tests/test_exciter \
//       && tests/test_exciter

#include "../src/exciter.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int    kSR = 44100;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double f, double a, int n) {
    std::vector<float> o(n);
    for (int i = 0; i < n; ++i) o[i] = (float)(a * std::sin(2.0 * kPi * f * i / kSR));
    return o;
}

bool finite(const float* x, int n) {
    for (int i = 0; i < n; ++i) if (!std::isfinite(x[i])) return false;
    return true;
}

// Goertzel magnitude (single-bin DFT) at frequency f over [skip, n).
double goertzel(const float* x, int n, int skip, double f) {
    const double w  = 2.0 * kPi * f / kSR;
    const double cw = std::cos(w);
    const double coeff = 2.0 * cw;
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = skip; i < n; ++i) {
        s0 = x[i] + coeff * s1 - s2;
        s2 = s1; s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im) / (0.5 * (n - skip));
}

double mean(const float* x, int n, int skip) {
    double s = 0; for (int i = skip; i < n; ++i) s += x[i];
    return s / (n - skip);
}

// ---------------------------------------------------------------------------
// Test 1: amount = 0 → output is bit-identical to input (bypass guarantee)
// ---------------------------------------------------------------------------
void test_bypass_identity() {
    const int N = 4096;
    auto l = sine(4000.0, 0.4, N);
    auto r = sine(6000.0, 0.4, N);
    std::vector<float> l0(l), r0(r);

    nablafx::Exciter ex; ex.prepare(kSR);
    ex.set_params(/*amount*/0.f, 3000.f, 6.f, 0.25f, 18000.f);
    ex.process(l.data(), r.data(), N);

    double maxd = 0;
    for (int i = 0; i < N; ++i) {
        maxd = std::max(maxd, (double)std::fabs(l[i] - l0[i]));
        maxd = std::max(maxd, (double)std::fabs(r[i] - r0[i]));
    }
    std::fprintf(stderr, "[bypass] max|out-in| = %.2e (want 0)\n", maxd);
    assert(maxd == 0.0);
    std::fprintf(stderr, "[bypass] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: a sine in the excite band produces a 2nd harmonic; even-dominant
//         when Character = 0, more 3rd when Character is up.
// ---------------------------------------------------------------------------
void test_harmonics() {
    const int N = 1 << 15;
    const double f = 4000.0;     // well above the 3 kHz HPF
    const int skip = N / 4;

    // Character = 0 → even-dominant: 2nd > 3rd.
    {
        auto x = sine(f, 0.5, N);
        nablafx::Exciter ex; ex.prepare(kSR);
        ex.set_params(/*amount*/1.0f, 3000.f, /*drive*/18.f, /*char*/0.f, 19000.f);
        ex.process(x.data(), nullptr, N);
        assert(finite(x.data(), N));
        const double h2 = goertzel(x.data(), N, skip, 2 * f);
        const double h3 = goertzel(x.data(), N, skip, 3 * f);
        std::fprintf(stderr, "[harm even=0] h2=%.4e h3=%.4e (want h2>0, h2>h3)\n", h2, h3);
        assert(h2 > 1e-3);
        assert(h2 > h3);
    }
    // Character = 1 → grit: relative 3rd content rises vs Character=0.
    {
        auto x0 = sine(f, 0.5, N);
        auto x1 = sine(f, 0.5, N);
        nablafx::Exciter e0; e0.prepare(kSR);
        e0.set_params(1.0f, 3000.f, 18.f, /*char*/0.f, 19000.f);
        e0.process(x0.data(), nullptr, N);
        nablafx::Exciter e1; e1.prepare(kSR);
        e1.set_params(1.0f, 3000.f, 18.f, /*char*/1.f, 19000.f);
        e1.process(x1.data(), nullptr, N);
        const double r0 = goertzel(x0.data(), N, skip, 3 * f)
                        / goertzel(x0.data(), N, skip, 2 * f);
        const double r1 = goertzel(x1.data(), N, skip, 3 * f)
                        / goertzel(x1.data(), N, skip, 2 * f);
        std::fprintf(stderr, "[harm char] 3rd/2nd: char0=%.3f char1=%.3f (want char1>char0)\n",
                     r0, r1);
        assert(r1 > r0);
    }
    std::fprintf(stderr, "[harm]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: output has no DC offset (asymmetric even curve is DC-corrected)
// ---------------------------------------------------------------------------
void test_no_dc() {
    const int N = 1 << 15;
    auto x = sine(4000.0, 0.5, N);
    nablafx::Exciter ex; ex.prepare(kSR);
    ex.set_params(1.0f, 3000.f, /*drive*/18.f, /*char*/0.f, 19000.f);  // pure even
    ex.process(x.data(), nullptr, N);
    const double dc = mean(x.data(), N, N / 4);
    std::fprintf(stderr, "[dc]     mean = %.2e (want ~0)\n", dc);
    assert(std::fabs(dc) < 1e-3);
    std::fprintf(stderr, "[dc]     PASS\n");
}

// ---------------------------------------------------------------------------
// Test 4: anti-aliasing — a high sine near Fs/4 driven hard must not dump big
//         energy into low, inharmonic alias bins. Compare against a naive
//         (non-oversampled) shaper to confirm oversampling actually helps.
// ---------------------------------------------------------------------------
void test_antialias() {
    const int N = 1 << 15;
    const double f = 9000.0;      // 2f=18k, 3f=27k → folds; 3f alias = 44.1-27 ≈ 17.1k
    const int skip = N / 4;
    auto x = sine(f, 0.6, N);

    nablafx::Exciter ex; ex.prepare(kSR);
    ex.set_params(1.0f, 3000.f, /*drive*/24.f, /*char*/0.5f, 19000.f);
    ex.process(x.data(), nullptr, N);
    assert(finite(x.data(), N));

    // The fundamental passes through (dry path). Any large component well BELOW
    // the fundamental at an inharmonic frequency would be alias fold-back.
    const double fund  = goertzel(x.data(), N, skip, f);
    // Probe a low, clearly inharmonic region (1–2.5 kHz) where no real harmonic
    // of a 9 kHz tone lives; alias products from the 4×-filtered shaper should
    // be tiny here.
    double alias_max = 0;
    for (double fa = 800.0; fa <= 2500.0; fa += 100.0)
        alias_max = std::max(alias_max, goertzel(x.data(), N, skip, fa));
    const double ratio = alias_max / fund;
    std::fprintf(stderr, "[alias]  low-band alias/fund = %.4e (want < 1e-2)\n", ratio);
    assert(ratio < 1e-2);

    // Sanity: a naive per-sample shaper (no oversampling) at the same drive
    // dumps far more alias energy into that low band.
    auto y = sine(f, 0.6, N);
    const double drive = std::pow(10.0, 24.0 / 20.0);
    const double bias = 0.5;
    const double dc = std::tanh(bias) - bias;
    for (int i = 0; i < N; ++i) {
        const double xe = drive * y[i];
        const double even = std::tanh(xe + bias) - bias - dc;
        const double odd  = std::tanh(xe);
        y[i] = (float)(y[i] + (0.575 * even + 0.425 * odd));
    }
    double naive_alias = 0;
    for (double fa = 800.0; fa <= 2500.0; fa += 100.0)
        naive_alias = std::max(naive_alias, goertzel(y.data(), N, skip, fa));
    const double naive_ratio = naive_alias / goertzel(y.data(), N, skip, f);
    std::fprintf(stderr, "[alias]  naive (no OVS) alias/fund = %.4e (oversampled is lower)\n",
                 naive_ratio);
    assert(ratio < naive_ratio);
    std::fprintf(stderr, "[alias]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 5: regression baseline (known-answer test). Locks the exact output of a
//         fixed input through fixed params so a future perf refactor can't
//         silently drift the sound. The reference was captured from the
//         oversampled-waveshaper output and is bit-stable across -O0/-O2/-O3.
//         This is the durable guard for the modulo->mask perf change (which
//         null-tested bit-identical to the prior implementation).
// ---------------------------------------------------------------------------
void test_regression_kat() {
    const int N = 2048;
    std::vector<float> x(N);
    for (int i = 0; i < N; ++i)
        x[i] = 0.4f * std::sin(2.0 * kPi * 4000.0 * i / kSR)
             + 0.2f * std::sin(2.0 * kPi * 9000.0 * i / kSR);

    nablafx::Exciter ex; ex.prepare(kSR);
    ex.set_params(/*amount*/0.6f, /*freq*/3000.f, /*drive*/15.f,
                  /*char*/0.4f, /*tame*/17000.f);
    ex.process(x.data(), nullptr, N);
    assert(finite(x.data(), N));

    const double kRefRms = 2.271556805297e-01;
    const int    kRefIdx[5] = {64, 256, 512, 1024, 2000};
    const float  kRefVal[5] = {-2.100853622e-01f, -8.789835125e-02f,
                               -4.631935433e-02f,  4.954358935e-02f,
                               -2.841520607e-01f};

    double sum = 0; for (int i = 0; i < N; ++i) sum += (double)x[i] * x[i];
    const double rms = std::sqrt(sum / N);
    std::fprintf(stderr, "[kat]    rms=%.12e (ref %.12e)\n", rms, kRefRms);
    assert(std::fabs(rms - kRefRms) < 1e-9);
    for (int j = 0; j < 5; ++j) {
        const double d = std::fabs((double)x[kRefIdx[j]] - kRefVal[j]);
        std::fprintf(stderr, "[kat]    x[%d]=%.9e ref=%.9e |d|=%.2e\n",
                     kRefIdx[j], x[kRefIdx[j]], kRefVal[j], d);
        assert(d < 1e-5);
    }
    std::fprintf(stderr, "[kat]    PASS\n");
}

}  // namespace

int main() {
    test_bypass_identity();
    test_harmonics();
    test_no_dc();
    test_antialias();
    test_regression_kat();
    std::fprintf(stderr, "ALL EXCITER TESTS PASSED\n");
    return 0;
}
