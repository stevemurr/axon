// Unit tests for BassMono.
//   g++ -O2 -std=c++17 -I src tests/test_bass_mono.cpp -o tests/test_bass_mono \
//       && tests/test_bass_mono

#include "../src/bass_mono.hpp"

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
double rms(const float* x, int n, int skip = 0) {
    double s = 0; for (int i = skip; i < n; ++i) s += (double)x[i] * x[i];
    return std::sqrt(s / (n - skip));
}
bool finite(const float* x, int n) {
    for (int i = 0; i < n; ++i) if (!std::isfinite(x[i])) return false;
    return true;
}

// side = 0.5(L-R) energy of a buffer pair.
double side_rms(const float* l, const float* r, int n, int skip) {
    std::vector<float> s(n);
    for (int i = 0; i < n; ++i) s[i] = 0.5f * (l[i] - r[i]);
    return rms(s.data(), n, skip);
}

// ---------------------------------------------------------------------------
// Test 1: low-frequency width is removed (bass → mono)
// ---------------------------------------------------------------------------
void test_low_freq_collapses() {
    const int N = kSR;
    auto l = sine(50.0, 0.5, N);          // hard-panned 50 Hz (all in L)
    std::vector<float> r(N, 0.f);
    const double side_in = side_rms(l.data(), r.data(), N, 0);

    nablafx::BassMono bm; bm.prepare(kSR); bm.set_cutoff(250.f);
    bm.process(l.data(), r.data(), N);

    assert(finite(l.data(), N) && finite(r.data(), N));
    const int skip = kSR / 10;            // let the filter settle
    const double side_out = side_rms(l.data(), r.data(), N, skip);
    std::fprintf(stderr, "[low]   side_in=%.4f side_out=%.4f (ratio %.3f, want <0.1)\n",
                side_in, side_out, side_out / side_in);
    assert(side_out < side_in * 0.1);     // >20 dB of width removed at 50 Hz
    std::fprintf(stderr, "[low]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: high-frequency width is preserved (stereo above cutoff)
// ---------------------------------------------------------------------------
void test_high_freq_preserved() {
    const int N = kSR;
    auto l = sine(5000.0, 0.5, N);        // hard-panned 5 kHz
    std::vector<float> r(N, 0.f);
    const double side_in = side_rms(l.data(), r.data(), N, 0);

    nablafx::BassMono bm; bm.prepare(kSR); bm.set_cutoff(250.f);
    bm.process(l.data(), r.data(), N);

    const int skip = kSR / 10;
    const double side_out = side_rms(l.data(), r.data(), N, skip);
    std::fprintf(stderr, "[high]  side_in=%.4f side_out=%.4f (ratio %.3f, want >0.9)\n",
                side_in, side_out, side_out / side_in);
    assert(side_out > side_in * 0.9);     // width essentially intact at 5 kHz
    std::fprintf(stderr, "[high]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: the mono sum (L+R) is preserved exactly at all frequencies
// ---------------------------------------------------------------------------
void test_mono_sum_preserved() {
    const int N = kSR;
    auto l = sine(80.0, 0.4, N);
    auto r = sine(3000.0, 0.6, N);        // arbitrary independent content
    std::vector<float> sum_in(N);
    for (int i = 0; i < N; ++i) sum_in[i] = l[i] + r[i];

    nablafx::BassMono bm; bm.prepare(kSR); bm.set_cutoff(250.f);
    bm.process(l.data(), r.data(), N);

    double max_err = 0;
    for (int i = 0; i < N; ++i)
        max_err = std::max(max_err, std::fabs((double)(l[i] + r[i]) - sum_in[i]));
    std::fprintf(stderr, "[sum]   max |(L+R)_out - (L+R)_in| = %.2e (want <1e-4)\n", max_err);
    assert(max_err < 1e-4);               // mid is never touched
    std::fprintf(stderr, "[sum]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 4: already-mono input is unchanged
// ---------------------------------------------------------------------------
void test_mono_input_unchanged() {
    const int N = kSR;
    auto base = sine(60.0, 0.5, N);
    std::vector<float> l(base), r(base);  // identical channels

    nablafx::BassMono bm; bm.prepare(kSR); bm.set_cutoff(250.f);
    bm.process(l.data(), r.data(), N);

    double max_err = 0;
    const int skip = kSR / 10;
    for (int i = skip; i < N; ++i)
        max_err = std::max(max_err, std::fabs((double)l[i] - base[i]));
    std::fprintf(stderr, "[mono]  max |L_out - L_in| = %.2e (want <1e-4)\n", max_err);
    assert(max_err < 1e-4);
    assert(finite(r.data(), N));
    std::fprintf(stderr, "[mono]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 5: prepare() with sample_rate == 0 must not divide by zero.
//   Bug: prepare() assigned sr_ = sample_rate unguarded, so design_() computed
//   w0 = 2*pi*fc/0 = inf, yielding NaN/inf filter coefficients that poison the
//   output. The fix clamps a non-positive rate to a safe 44100.0.
//   With the bug present, process() produces non-finite samples → asserts fire.
// ---------------------------------------------------------------------------
void test_prepare_zero_sample_rate() {
    const int N = kSR;
    auto l = sine(50.0, 0.5, N);          // hard-panned 50 Hz
    std::vector<float> r(N, 0.f);

    nablafx::BassMono bm;
    bm.prepare(0.0);                      // <-- the previously-broken path
    bm.set_cutoff(250.f);
    bm.process(l.data(), r.data(), N);

    // Pre-fix this fails: inf/NaN coeffs propagate non-finite samples.
    assert(finite(l.data(), N) && finite(r.data(), N));

    // And the clamped 44100 fallback must still behave like a real bass-mono:
    // low-frequency width collapses just as in test_low_freq_collapses().
    const int skip = kSR / 10;
    const double side_out = side_rms(l.data(), r.data(), N, skip);
    std::fprintf(stderr, "[sr0]   side_out=%.4f (finite ok, want <0.05)\n", side_out);
    assert(side_out < 0.05);              // sane coeffs, not a dead/garbage filter
    std::fprintf(stderr, "[sr0]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 6: prepare() with a negative sample_rate is likewise clamped.
//   A negative rate gave a finite-but-wrong w0 (negative), corrupting the
//   filter; the fix routes any non-positive rate to 44100.0.
// ---------------------------------------------------------------------------
void test_prepare_negative_sample_rate() {
    const int N = kSR;
    auto l = sine(50.0, 0.5, N);
    std::vector<float> r(N, 0.f);

    nablafx::BassMono bm;
    bm.prepare(-48000.0);                 // <-- non-positive, must be clamped
    bm.set_cutoff(250.f);
    bm.process(l.data(), r.data(), N);

    assert(finite(l.data(), N) && finite(r.data(), N));
    const int skip = kSR / 10;
    const double side_out = side_rms(l.data(), r.data(), N, skip);
    std::fprintf(stderr, "[srNeg] side_out=%.4f (finite ok, want <0.05)\n", side_out);
    assert(side_out < 0.05);
    std::fprintf(stderr, "[srNeg] PASS\n");
}

}  // namespace

int main() {
    test_low_freq_collapses();
    test_high_freq_preserved();
    test_mono_sum_preserved();
    test_mono_input_unchanged();
    test_prepare_zero_sample_rate();
    test_prepare_negative_sample_rate();
    std::fprintf(stderr, "ALL BASS-MONO TESTS PASSED\n");
    return 0;
}
