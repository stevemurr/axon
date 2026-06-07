// Unit tests for TruePeakCeiling (4x oversampled true-peak limiter).
//   c++ -O2 -std=c++17 -UNDEBUG -I src tests/test_true_peak.cpp \
//       src/true_peak_ceiling.cpp -o tests/test_true_peak && tests/test_true_peak
//
// TruePeakCeiling had no test in the CMake suite (only the un-built
// tests/test_dsp.cpp referenced it). This brings it in with the two invariants
// that matter — the sample ceiling is NEVER exceeded, and a regression baseline
// that locks the output so a future perf refactor can't silently drift it
// (guards the per-sample `% delay_.size()` -> branch-wrap change, which
// null-tested bit-identical).

#include "../src/true_peak_ceiling.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int    kSR = 44100;
constexpr double kPi = 3.14159265358979323846;

bool finite(const float* x, int n) {
    for (int i = 0; i < n; ++i) if (!std::isfinite(x[i])) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Test 1: the sample ceiling is never exceeded, even on a hot, peaky signal.
//         The hard clip after the smoothed gain guarantees |out| <= ceiling.
// ---------------------------------------------------------------------------
void test_ceiling_never_exceeded() {
    const int N = kSR;  // 1 s
    std::vector<float> x(N), o(N);
    // Deliberately hot: sum of sines + transients well over 0 dBFS.
    for (int i = 0; i < N; ++i) {
        float s = 1.4f * (float)std::sin(2.0 * kPi * 220.0 * i / kSR)
                + 0.6f * (float)std::sin(2.0 * kPi * 7000.0 * i / kSR);
        if (i % 4096 < 8) s += 2.0f;   // periodic spikes
        x[i] = s;
    }
    nablafx::TruePeakCeiling tp; tp.reset(kSR);   // default -1 dBTP ceiling
    tp.process(x.data(), o.data(), N);

    assert(finite(o.data(), N));
    const double ceiling = std::pow(10.0, -1.0 / 20.0);   // -1 dBTP
    double peak = 0;
    for (int i = 0; i < N; ++i) peak = std::max(peak, (double)std::fabs(o[i]));
    std::fprintf(stderr, "[ceil]  out peak = %.9f  ceiling = %.9f\n", peak, ceiling);
    // Sample ceiling is a hard guarantee (clip); allow a single ULP of slack.
    assert(peak <= ceiling + 1e-6);
    std::fprintf(stderr, "[ceil]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: latency report is the lookahead in samples, and quiet signal below
//         the ceiling passes through unchanged after the lookahead delay.
// ---------------------------------------------------------------------------
void test_latency_and_transparency() {
    nablafx::TruePeakCeiling::Config cfg;  // lookahead 1.5 ms
    nablafx::TruePeakCeiling tp(cfg); tp.reset(kSR);
    const int la = static_cast<int>(tp.latency_samples());
    std::fprintf(stderr, "[lat]   latency = %d samples (~%.2f ms)\n", la, 1000.0 * la / kSR);
    assert(la == (int)std::round(1.5e-3 * kSR));

    const int N = kSR / 2;
    std::vector<float> x(N), o(N);
    for (int i = 0; i < N; ++i) x[i] = 0.3f * (float)std::sin(2.0 * kPi * 1000.0 * i / kSR);
    tp.process(x.data(), o.data(), N);

    // Well below ceiling → no gain reduction → output == input delayed by `la`.
    double maxd = 0;
    const int skip = la + 100;
    for (int i = skip; i < N; ++i) maxd = std::max(maxd, (double)std::fabs(o[i] - x[i - la]));
    std::fprintf(stderr, "[lat]   max|out - in_delayed| = %.2e (want ~0)\n", maxd);
    assert(maxd < 1e-6);
    std::fprintf(stderr, "[lat]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: regression baseline (known-answer test). Bit-stable across -O2/-O3.
// ---------------------------------------------------------------------------
void test_regression_kat() {
    const int N = 4096;
    std::vector<float> x(N), o(N);
    for (int i = 0; i < N; ++i)
        x[i] = 1.3f * std::sin(2.0 * kPi * 997.0  * i / kSR)
             + 0.4f * std::sin(2.0 * kPi * 5000.0 * i / kSR);
    nablafx::TruePeakCeiling tp; tp.reset(kSR);
    tp.process(x.data(), o.data(), N);
    assert(finite(o.data(), N));

    const double kRefRms  = 5.506599990705e-01;
    const double kRefPeak = 8.912509083748e-01;
    const int    kIdx[4]  = {500, 1500, 2500, 3500};
    const float  kRef[4]  = {-4.690421224e-01f, 2.608478367e-01f,
                              7.440607250e-02f, -3.614770174e-01f};

    double sum = 0, pk = 0;
    for (int i = 0; i < N; ++i) { sum += (double)o[i] * o[i]; pk = std::max(pk, (double)std::fabs(o[i])); }
    const double rms = std::sqrt(sum / N);
    std::fprintf(stderr, "[kat]   rms=%.12e peak=%.12e\n", rms, pk);
    assert(std::fabs(rms - kRefRms) < 1e-9);
    assert(std::fabs(pk  - kRefPeak) < 1e-7);
    for (int j = 0; j < 4; ++j)
        assert(std::fabs((double)o[kIdx[j]] - kRef[j]) < 1e-5);
    std::fprintf(stderr, "[kat]   PASS\n");
}

}  // namespace

int main() {
    test_ceiling_never_exceeded();
    test_latency_and_transparency();
    test_regression_kat();
    std::fprintf(stderr, "ALL TRUE-PEAK TESTS PASSED\n");
    return 0;
}
