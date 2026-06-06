// Unit tests for the transparent mastering room Reverb (FDN).
//   g++ -O2 -std=c++17 -I src tests/test_reverb.cpp -o tests/test_reverb \
//       && tests/test_reverb

#include "../src/reverb.hpp"

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

double rms(const float* x, int from, int to) {
    double s = 0; for (int i = from; i < to; ++i) s += (double)x[i] * x[i];
    return std::sqrt(s / std::max(1, to - from));
}

double mean(const float* x, int n) {
    double s = 0; for (int i = 0; i < n; ++i) s += x[i];
    return s / n;
}

double maxabs(const float* x, int n) {
    double m = 0; for (int i = 0; i < n; ++i) m = std::max(m, (double)std::fabs(x[i]));
    return m;
}

// ---------------------------------------------------------------------------
// Test 1: mix = 0 → output is bit-identical to input (bypass guarantee)
// ---------------------------------------------------------------------------
void test_bypass_identity() {
    const int N = 8192;
    auto l = sine(220.0, 0.4, N);
    auto r = sine(330.0, 0.4, N);
    std::vector<float> l0(l), r0(r);

    nablafx::Reverb rv; rv.prepare(kSR);
    rv.set_params(/*mix*/0.f, 0.3f, 0.8f, 7000.f, 250.f);
    rv.process(l.data(), r.data(), N);

    double maxd = 0;
    for (int i = 0; i < N; ++i) {
        maxd = std::max(maxd, (double)std::fabs(l[i] - l0[i]));
        maxd = std::max(maxd, (double)std::fabs(r[i] - r0[i]));
    }
    std::fprintf(stderr, "[bypass] max|out-in| = %.2e (want 0)\n", maxd);
    assert(maxd == 0.0);
    assert(rv.latency_samples() == 0);
    std::fprintf(stderr, "[bypass] PASS (zero latency)\n");
}

// ---------------------------------------------------------------------------
// Test 2: a tail exists shortly after a burst, decays toward ~0, and RT
//         roughly tracks Size (bigger Size = longer tail).
// ---------------------------------------------------------------------------
double tail_run(float size, int N, std::vector<float>& l, std::vector<float>& r) {
    l.assign(N, 0.f); r.assign(N, 0.f);
    // A short mid-band burst at the start (mid freq so the low-cut passes it).
    const int burst = kSR / 100;   // 10 ms
    auto b = sine(1000.0, 0.5, burst);
    for (int i = 0; i < burst; ++i) { l[i] = b[i]; r[i] = b[i]; }

    nablafx::Reverb rv; rv.prepare(kSR);
    rv.set_params(/*mix*/0.7f, size, 0.8f, 9000.f, 200.f);
    rv.process(l.data(), r.data(), N);
    assert(finite(l.data(), N));
    assert(finite(r.data(), N));

    // "Tail energy half-life": find where the wet energy drops 40 dB below its
    // early-tail peak (measured after the burst ends).
    const int t0 = burst + kSR / 50;     // 20 ms after burst → tail region
    const int win = kSR / 20;            // 50 ms windows
    const double early = rms(l.data(), t0, t0 + win);
    int decay_idx = N;
    for (int i = t0; i + win < N; i += win) {
        const double e = rms(l.data(), i, i + win);
        if (e < early * 0.01) { decay_idx = i; break; }   // -40 dB
    }
    return (double)decay_idx / kSR;   // seconds to -40 dB
}

void test_tail_and_size() {
    const int N = kSR * 6;
    std::vector<float> l, r, l2, r2;
    const double tSmall = tail_run(0.20f, N, l,  r);
    const double tBig   = tail_run(0.80f, N, l2, r2);

    // Tail must actually exist (non-zero a bit after the burst).
    const int t0 = kSR / 100 + kSR / 50;
    const double early = rms(l.data(), t0, t0 + kSR / 20);
    std::fprintf(stderr, "[tail]   early tail rms = %.4e (want > 0)\n", early);
    assert(early > 1e-4);

    // And it must decay (late tail much quieter than early tail).
    const double late = rms(l.data(), N - kSR / 2, N);
    std::fprintf(stderr, "[tail]   late tail rms  = %.4e (want << early)\n", late);
    assert(late < early * 0.1);

    std::fprintf(stderr, "[tail]   -40dB time: small=%.2fs big=%.2fs (want big>small)\n",
                 tSmall, tBig);
    assert(tBig > tSmall);
    std::fprintf(stderr, "[tail]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: stability — 10 s of impulse + silence, no NaN/Inf, bounded.
// ---------------------------------------------------------------------------
void test_stability() {
    const int N = kSR * 10;
    std::vector<float> l(N, 0.f), r(N, 0.f);
    l[0] = r[0] = 1.0f;                          // unit impulse
    for (int i = 100; i < 200; ++i) l[i] = r[i] = 0.3f;  // a little extra energy

    nablafx::Reverb rv; rv.prepare(kSR);
    // Worst case for stability: max size (longest RT60), low damping.
    rv.set_params(/*mix*/1.0f, /*size*/1.0f, 1.0f, /*damp*/18000.f, 100.f);
    rv.process(l.data(), r.data(), N);

    assert(finite(l.data(), N));
    assert(finite(r.data(), N));
    const double mx = std::max(maxabs(l.data(), N), maxabs(r.data(), N));
    std::fprintf(stderr, "[stab]   max|out| over 10s = %.3f (want bounded)\n", mx);
    assert(mx < 8.0);
    // Decays toward ~0 by the end.
    const double tailEnd = rms(l.data(), N - kSR, N);
    std::fprintf(stderr, "[stab]   last-second rms = %.4e (want ~0)\n", tailEnd);
    assert(tailEnd < 1e-3);
    std::fprintf(stderr, "[stab]   PASS\n");
}

// ---------------------------------------------------------------------------
// Test 4: Low Cut — a low sine (80 Hz) is strongly attenuated in the wet
//         vs a mid sine (1 kHz). Measured as wet-only (dry subtracted).
// ---------------------------------------------------------------------------
double wet_energy(double freq, float lowcut) {
    const int N = kSR * 2;
    auto l = sine(freq, 0.5, N);
    auto r = sine(freq, 0.5, N);
    std::vector<float> dry(l);

    nablafx::Reverb rv; rv.prepare(kSR);
    rv.set_params(/*mix*/1.0f, 0.5f, 0.8f, 12000.f, lowcut);
    rv.process(l.data(), r.data(), N);

    // wet = out - dry (dry is passed at unity, undelayed). Measure 2nd half.
    std::vector<float> w(N);
    for (int i = 0; i < N; ++i) w[i] = l[i] - dry[i];
    return rms(w.data(), N / 2, N);
}

void test_lowcut() {
    const float lc = 250.f;
    const double wLow = wet_energy(80.0,   lc);
    const double wMid = wet_energy(1000.0, lc);
    std::fprintf(stderr, "[lowcut] wet rms: 80Hz=%.4e 1kHz=%.4e (want 80Hz << 1kHz)\n",
                 wLow, wMid);
    assert(wLow < wMid * 0.25);   // low end strongly attenuated in the wet
    std::fprintf(stderr, "[lowcut] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 5: width / decorrelation + mono compatibility.
//   mono input (L==R), width>0 → L != R (decorrelated), AND L+R does not
//   collapse to ~0 (no deep cancellation when summed to mono).
// ---------------------------------------------------------------------------
void test_width_mono_compat() {
    const int N = kSR * 2;
    auto l = sine(800.0, 0.5, N);
    std::vector<float> r(l);   // mono input: L == R

    nablafx::Reverb rv; rv.prepare(kSR);
    rv.set_params(/*mix*/1.0f, 0.5f, /*width*/1.0f, 10000.f, 200.f);
    rv.process(l.data(), r.data(), N);
    assert(finite(l.data(), N));
    assert(finite(r.data(), N));

    // Decorrelation: L and R differ.
    double diff = 0;
    for (int i = N / 2; i < N; ++i) diff = std::max(diff, (double)std::fabs(l[i] - r[i]));
    std::fprintf(stderr, "[width]  max|L-R| = %.4e (want > 0, decorrelated)\n", diff);
    assert(diff > 1e-3);

    // Mono compatibility: the summed mono (L+R) must retain substantial energy
    // relative to the individual channels — no phase-inversion cancellation.
    std::vector<float> sum(N);
    for (int i = 0; i < N; ++i) sum[i] = 0.5f * (l[i] + r[i]);
    const double monoRms = rms(sum.data(), N / 2, N);
    const double lRms    = rms(l.data(),   N / 2, N);
    std::fprintf(stderr, "[width]  mono(L+R)/2 rms=%.4e  L rms=%.4e (want mono not ~0)\n",
                 monoRms, lRms);
    assert(monoRms > lRms * 0.3);   // mono sum survives — no deep cancellation
    std::fprintf(stderr, "[width]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 6: no DC offset in the output.
// ---------------------------------------------------------------------------
void test_no_dc() {
    const int N = kSR * 3;
    auto l = sine(500.0, 0.4, N);
    auto r = sine(700.0, 0.4, N);
    nablafx::Reverb rv; rv.prepare(kSR);
    rv.set_params(/*mix*/0.6f, 0.5f, 0.8f, 8000.f, 200.f);
    rv.process(l.data(), r.data(), N);
    const double dcL = mean(l.data() + N / 2, N / 2);
    const double dcR = mean(r.data() + N / 2, N / 2);
    std::fprintf(stderr, "[dc]     mean L=%.2e R=%.2e (want ~0)\n", dcL, dcR);
    assert(std::fabs(dcL) < 1e-3);
    assert(std::fabs(dcR) < 1e-3);
    std::fprintf(stderr, "[dc]     PASS\n");
}

}  // namespace

int main() {
    test_bypass_identity();
    test_tail_and_size();
    test_stability();
    test_lowcut();
    test_width_mono_compat();
    test_no_dc();
    std::fprintf(stderr, "ALL REVERB TESTS PASSED\n");
    return 0;
}
