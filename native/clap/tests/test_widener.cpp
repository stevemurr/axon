// Unit tests for the transparent M/S stereo Widener.
//   g++ -O2 -std=c++17 -I src tests/test_widener.cpp -o tests/test_widener \
//       && tests/test_widener

#include "../src/widener.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int    kSR = 44100;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double f, double a, int n, double phase = 0.0) {
    std::vector<float> o(n);
    for (int i = 0; i < n; ++i)
        o[i] = (float)(a * std::sin(2.0 * kPi * f * i / kSR + phase));
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
double mean(const float* x, int n, int skip = 0) {
    double s = 0; for (int i = skip; i < n; ++i) s += x[i];
    return s / (n - skip);
}
// side = 0.5(L-R) energy.
double side_rms(const float* l, const float* r, int n, int skip) {
    std::vector<float> s(n);
    for (int i = 0; i < n; ++i) s[i] = 0.5f * (l[i] - r[i]);
    return rms(s.data(), n, skip);
}

// ---------------------------------------------------------------------------
// Test 1: IDENTITY — width=1, air=0 → output bit-identical to input.
// ---------------------------------------------------------------------------
void test_identity() {
    const int N = 4096;
    auto l = sine(800.0, 0.4, N);
    auto r = sine(5000.0, 0.4, N, 0.7);   // independent stereo content
    std::vector<float> l0(l), r0(r);

    nablafx::Widener w; w.prepare(kSR);
    w.set_params(/*width*/1.0f, /*low_hz*/250.f, /*air*/0.0f);
    w.process(l.data(), r.data(), N);

    double maxd = 0;
    for (int i = 0; i < N; ++i) {
        maxd = std::max(maxd, (double)std::fabs(l[i] - l0[i]));
        maxd = std::max(maxd, (double)std::fabs(r[i] - r0[i]));
    }
    std::fprintf(stderr, "[ident] max|out-in| = %.2e (want 0)\n", maxd);
    assert(maxd == 0.0);
    std::fprintf(stderr, "[ident] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: MONO-SUM INVARIANCE — the headline mastering property. For ANY
//         width/air/low settings, (L+R) is unchanged vs input's (L+R).
// ---------------------------------------------------------------------------
void test_mono_sum_invariance() {
    const int N = kSR;
    const float widths[] = {0.0f, 0.5f, 1.0f, 1.5f, 2.0f};
    const float lows[]   = {50.f, 250.f, 600.f, 1000.f};
    const float airs[]   = {0.0f, 0.5f, 1.0f};

    double worst = 0;
    for (float wd : widths) for (float lo : lows) for (float ar : airs) {
        auto l = sine(70.0, 0.4, N);
        auto r = sine(3200.0, 0.6, N, 1.1);   // arbitrary independent content
        std::vector<float> sum_in(N);
        for (int i = 0; i < N; ++i) sum_in[i] = l[i] + r[i];

        nablafx::Widener w; w.prepare(kSR);
        w.set_params(wd, lo, ar);
        w.process(l.data(), r.data(), N);
        assert(finite(l.data(), N) && finite(r.data(), N));

        double max_err = 0;
        for (int i = 0; i < N; ++i)
            max_err = std::max(max_err, std::fabs((double)(l[i] + r[i]) - sum_in[i]));
        worst = std::max(worst, max_err);
    }
    std::fprintf(stderr, "[sum]   worst max|(L+R)_out-(L+R)_in| = %.2e (want <1e-4)\n", worst);
    assert(worst < 1e-4);     // the mid is never touched, across the whole sweep
    std::fprintf(stderr, "[sum]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: WIDTH WORKS — width=2 ~doubles side energy ABOVE the crossover, while
//         side energy BELOW the crossover is ~unchanged.
// ---------------------------------------------------------------------------
void test_width_works() {
    const int N = kSR;
    const int skip = kSR / 10;
    // High band (well above 250 Hz crossover): width should ~2× the side.
    {
        auto l = sine(5000.0, 0.5, N);
        std::vector<float> r(N, 0.f);          // hard-panned → all side
        const double side_in = side_rms(l.data(), r.data(), N, skip);
        nablafx::Widener w; w.prepare(kSR); w.set_params(2.0f, 250.f, 0.0f);
        w.process(l.data(), r.data(), N);
        const double side_out = side_rms(l.data(), r.data(), N, skip);
        std::fprintf(stderr, "[wide]  HIGH side_in=%.4f side_out=%.4f ratio=%.3f (want ~2)\n",
                     side_in, side_out, side_out / side_in);
        assert(side_out > side_in * 1.8 && side_out < side_in * 2.2);
    }
    // Low band (well below crossover): side ~unchanged by width.
    {
        auto l = sine(60.0, 0.5, N);
        std::vector<float> r(N, 0.f);
        const double side_in = side_rms(l.data(), r.data(), N, skip);
        nablafx::Widener w; w.prepare(kSR); w.set_params(2.0f, 250.f, 0.0f);
        w.process(l.data(), r.data(), N);
        const double side_out = side_rms(l.data(), r.data(), N, skip);
        std::fprintf(stderr, "[wide]  LOW  side_in=%.4f side_out=%.4f ratio=%.3f (want ~1)\n",
                     side_in, side_out, side_out / side_in);
        assert(side_out > side_in * 0.9 && side_out < side_in * 1.1);
    }
    std::fprintf(stderr, "[wide]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 4: AIR — air>0 increases side energy above ~6 kHz; leaves mids alone.
// ---------------------------------------------------------------------------
void test_air() {
    const int N = kSR;
    const int skip = kSR / 10;
    // 10 kHz (above the 6 kHz air crossover) — air boosts the side.
    {
        auto l = sine(10000.0, 0.5, N);
        std::vector<float> r(N, 0.f);
        const double side_in = side_rms(l.data(), r.data(), N, skip);
        nablafx::Widener w; w.prepare(kSR);
        w.set_params(/*width*/1.0f, 250.f, /*air*/1.0f);  // width neutral, air on
        w.process(l.data(), r.data(), N);
        const double side_out = side_rms(l.data(), r.data(), N, skip);
        std::fprintf(stderr, "[air]   10k side_in=%.4f side_out=%.4f ratio=%.3f (want >1.25)\n",
                     side_in, side_out, side_out / side_in);
        // air adds the (IIR-phase-rotated) HF side on top of the existing side, so
        // the RMS grows but not coherently to 2× — a clear, audible widening.
        assert(side_out > side_in * 1.25);
    }
    // 2 kHz (below the air crossover, width neutral) — side ~unchanged.
    {
        auto l = sine(2000.0, 0.5, N);
        std::vector<float> r(N, 0.f);
        const double side_in = side_rms(l.data(), r.data(), N, skip);
        nablafx::Widener w; w.prepare(kSR);
        w.set_params(1.0f, 250.f, 1.0f);
        w.process(l.data(), r.data(), N);
        const double side_out = side_rms(l.data(), r.data(), N, skip);
        std::fprintf(stderr, "[air]   2k  side_in=%.4f side_out=%.4f ratio=%.3f (want ~1)\n",
                     side_in, side_out, side_out / side_in);
        assert(side_out > side_in * 0.9 && side_out < side_in * 1.2);
    }
    std::fprintf(stderr, "[air]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 5: STABILITY/NaN — long run, bounded, finite, no DC.
// ---------------------------------------------------------------------------
void test_stability() {
    const int N = kSR * 4;
    auto l = sine(440.0, 0.5, N);
    auto r = sine(7000.0, 0.5, N, 0.3);
    nablafx::Widener w; w.prepare(kSR);
    w.set_params(2.0f, 200.f, 1.0f);
    w.process(l.data(), r.data(), N);
    assert(finite(l.data(), N) && finite(r.data(), N));
    double peak = 0;
    for (int i = 0; i < N; ++i)
        peak = std::max(peak, std::max(std::fabs((double)l[i]), std::fabs((double)r[i])));
    const double dcL = mean(l.data(), N, N / 4), dcR = mean(r.data(), N, N / 4);
    std::fprintf(stderr, "[stab]  peak=%.3f dcL=%.2e dcR=%.2e (want bounded, ~0 DC)\n",
                 peak, dcL, dcR);
    assert(peak < 8.0);
    assert(std::fabs(dcL) < 1e-3 && std::fabs(dcR) < 1e-3);
    std::fprintf(stderr, "[stab]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 6: MONO INPUT — r == nullptr is a graceful no-op (no crash).
// ---------------------------------------------------------------------------
void test_mono_input() {
    const int N = 4096;
    auto l = sine(1000.0, 0.5, N);
    std::vector<float> l0(l);
    nablafx::Widener w; w.prepare(kSR); w.set_params(2.0f, 250.f, 1.0f);
    w.process(l.data(), nullptr, N);
    double maxd = 0;
    for (int i = 0; i < N; ++i) maxd = std::max(maxd, std::fabs((double)l[i] - l0[i]));
    std::fprintf(stderr, "[mono]  max|out-in| = %.2e (want 0)\n", maxd);
    assert(maxd == 0.0);
    std::fprintf(stderr, "[mono]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 7: INIT (regression for the exciter-class bug). After prepare()+reset()
//         and a set_params() whose low_hz equals the default, a width=2 call
//         must STILL widen (the filters were re-designed, not stranded at
//         passthrough). If reset() stranded the biquads, the high band would
//         not be split out and width would do nothing / something wrong.
// ---------------------------------------------------------------------------
void test_init_does_not_strand_filters() {
    const int N = kSR;
    const int skip = kSR / 10;

    nablafx::Widener w;
    w.prepare(kSR);
    w.reset();                       // CLAP calls reset() AFTER activate/prepare
    w.set_params(2.0f, 250.f, 0.0f); // low_hz == the design fallback default (250)

    // High band: must be widened (~2×) → filters were re-designed by set_params.
    auto lh = sine(5000.0, 0.5, N);
    std::vector<float> rh(N, 0.f);
    const double hi_in = side_rms(lh.data(), rh.data(), N, skip);
    w.process(lh.data(), rh.data(), N);
    const double hi_out = side_rms(lh.data(), rh.data(), N, skip);

    // Low band: must be ~unchanged → the split is real (not full-range passthrough
    // of the side gain). If filters were stranded at passthrough, ALL side would
    // scale by 2 (including the lows), failing this check.
    nablafx::Widener w2;
    w2.prepare(kSR);
    w2.reset();
    w2.set_params(2.0f, 250.f, 0.0f);
    auto ll = sine(60.0, 0.5, N);
    std::vector<float> rl(N, 0.f);
    const double lo_in = side_rms(ll.data(), rl.data(), N, skip);
    w2.process(ll.data(), rl.data(), N);
    const double lo_out = side_rms(ll.data(), rl.data(), N, skip);

    std::fprintf(stderr, "[init]  hi ratio=%.3f (want ~2)  lo ratio=%.3f (want ~1)\n",
                 hi_out / hi_in, lo_out / lo_in);
    assert(hi_out > hi_in * 1.8);          // width actually applied above crossover
    assert(lo_out < lo_in * 1.1);          // lows untouched → the split is valid
    std::fprintf(stderr, "[init]  PASS\n");
}

}  // namespace

int main() {
    test_identity();
    test_mono_sum_invariance();
    test_width_works();
    test_air();
    test_stability();
    test_mono_input();
    test_init_does_not_strand_filters();
    std::fprintf(stderr, "ALL WIDENER TESTS PASSED\n");
    return 0;
}
