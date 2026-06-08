// Unit tests for MultibandExciter — the narrowband clean exciter behind the
// HARMONICS stage. The defining property is LOW intermodulation: shaping many
// narrow bands instead of one wide band keeps the soloed harmonics tonal rather
// than broadband "noise".
//   c++ -O2 -std=c++17 -UNDEBUG -I src tests/test_multiband_exciter.cpp \
//       -o tests/test_multiband_exciter && tests/test_multiband_exciter

#include "../src/multiband_exciter.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

constexpr int    kSR = 48000;
constexpr double kPi = 3.14159265358979323846;

bool finite(const float* x, int n) {
    for (int i = 0; i < n; ++i) if (!std::isfinite(x[i])) return false;
    return true;
}
double goertzel(const std::vector<float>& x, int skip, double f) {
    const double w = 2.0 * kPi * f / kSR, cw = std::cos(w), co = 2.0 * cw;
    double s1 = 0, s2 = 0;
    for (int i = skip; i < (int)x.size(); ++i) { double s0 = x[i] + co * s1 - s2; s2 = s1; s1 = s0; }
    const double re = s1 - s2 * cw, im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im) / (0.5 * (x.size() - skip));
}
// spectral flatness over [lo,hi] via a log-spaced Goertzel bank (1 = noise-like).
double flatness(const std::vector<float>& y, double lo, double hi, int K = 240) {
    double sumlog = 0, sumlin = 0; int n = 0; const int skip = (int)y.size() / 4;
    for (int k = 1; k <= K; ++k) {
        const double f = lo * std::pow(hi / lo, (double)k / K);
        const double a = goertzel(y, skip, f); const double p = a * a + 1e-20;
        sumlog += std::log(p); sumlin += p; ++n;
    }
    return std::exp(sumlog / n) / (sumlin / n);
}

// Run a mono buffer through a configured MultibandExciter; return the wet tap.
std::vector<float> wet_of(std::vector<float> in, double lo, double hi, int bands,
                          float chr, float drive, float amount) {
    nablafx::MultibandExciter mx; mx.prepare(kSR);
    mx.configure(lo, hi, bands, chr, drive); mx.set_amount(amount);
    std::vector<float> wet(in.size());
    for (size_t i = 0; i + 128 <= in.size(); i += 128)
        mx.process(&in[i], nullptr, 128, &wet[i]);
    return wet;
}

// ---------------------------------------------------------------------------
// Test 1: amount == 0 → bit-identical bypass; wet tap is all zeros.
// ---------------------------------------------------------------------------
void test_bypass() {
    const int N = 4096;
    std::vector<float> in(N), in0;
    for (int i = 0; i < N; ++i) in[i] = 0.4f * std::sin(2.0 * kPi * 5000.0 * i / kSR);
    in0 = in;
    nablafx::MultibandExciter mx; mx.prepare(kSR);
    mx.configure(3500, 16500, 5, 0.5f, 6.f); mx.set_amount(0.f);
    std::vector<float> wet(N, 1.f);
    mx.process(in.data(), nullptr, N, wet.data());
    double maxd = 0, maxw = 0;
    for (int i = 0; i < N; ++i) { maxd = std::max(maxd, (double)std::fabs(in[i] - in0[i])); maxw = std::max(maxw, (double)std::fabs(wet[i])); }
    std::fprintf(stderr, "[bypass] max|out-in|=%.2e wetmax=%.2e\n", maxd, maxw);
    assert(maxd == 0.0 && maxw == 0.0);
    std::fprintf(stderr, "[bypass] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: generates the expected harmonics (2nd for even char; +3rd for char up).
// ---------------------------------------------------------------------------
void test_harmonics() {
    const int N = 1 << 15; const double f = 200.0; const int skip = N / 4;
    std::vector<float> tone(N);
    for (int i = 0; i < N; ++i) tone[i] = 0.4f * std::sin(2.0 * kPi * f * i / kSR);
    auto w = wet_of(tone, 100, 1000, 5, 0.0f, 6.f, 1.f);   // warmth voicing, pure even
    assert(finite(w.data(), N));
    const double h2 = goertzel(w, skip, 2 * f), h3 = goertzel(w, skip, 3 * f);
    std::fprintf(stderr, "[harm]  h2=%.3e h3=%.3e (want h2>0, h2>h3 at char 0)\n", h2, h3);
    assert(h2 > 1e-4 && h2 > h3);
    std::fprintf(stderr, "[harm]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: IMD reduction — the whole point. On dense inharmonic high-band input,
//         the narrowband bank (5 bands) must be far more tonal (lower spectral
//         flatness) than a single WIDE band (1 band) at matched config.
// ---------------------------------------------------------------------------
void test_imd_reduction() {
    const int N = 1 << 15;
    // dense, inharmonic high-band content (mimics cymbals/air a wide shaper
    // turns to noise).
    std::mt19937 rng(7); std::uniform_real_distribution<double> ph(0, 2 * kPi);
    std::vector<float> in(N, 0.f);
    double freqs[12]; for (int k = 0; k < 12; ++k) freqs[k] = 3700.0 + 1000.0 * k + 137.0 * std::sin(k);
    for (int i = 0; i < N; ++i) { double s = 0; for (double fr : freqs) s += std::sin(2.0 * kPi * fr * i / kSR); in[i] = (float)(0.06 * s); }

    auto wide   = wet_of(in, 3500, 16500, 1, 0.5f, 6.f, 1.f);
    auto narrow = wet_of(in, 3500, 16500, 5, 0.5f, 6.f, 1.f);
    const double fw = flatness(wide,   3500, 16500);
    const double fn = flatness(narrow, 3500, 16500);
    std::fprintf(stderr, "[imd]   flatness wide(1 band)=%.3f narrow(5 bands)=%.3f (want narrow << wide)\n", fw, fn);
    assert(finite(narrow.data(), N));
    assert(fn < fw * 0.7);          // narrowband is markedly more tonal
    std::fprintf(stderr, "[imd]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 4: stereo finite + bounded under hot input; mono sum sane.
// ---------------------------------------------------------------------------
void test_stability() {
    const int N = kSR;
    std::vector<float> l(N), r(N);
    std::mt19937 rng(3); std::uniform_real_distribution<float> d(-0.9f, 0.9f);
    for (int i = 0; i < N; ++i) { l[i] = d(rng); r[i] = d(rng); }
    nablafx::MultibandExciter mx; mx.prepare(kSR);
    mx.configure(100, 1000, 5, 0.0f, 6.f); mx.set_amount(1.f);
    for (int i = 0; i + 128 <= N; i += 128) mx.process(&l[i], &r[i], 128);
    assert(finite(l.data(), N) && finite(r.data(), N));
    double pk = 0; for (int i = 0; i < N; ++i) pk = std::max(pk, (double)std::fabs(l[i]));
    std::fprintf(stderr, "[stab]  peak=%.3f (want < 4)\n", pk);
    assert(pk < 4.0);
    std::fprintf(stderr, "[stab]  PASS\n");
}

}  // namespace

int main() {
    test_bypass();
    test_harmonics();
    test_imd_reduction();
    test_stability();
    std::fprintf(stderr, "ALL MULTIBAND-EXCITER TESTS PASSED\n");
    return 0;
}
