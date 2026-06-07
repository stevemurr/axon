// Unit tests for IirFilterbankEq — the minimum-phase IIR filterbank Auto-EQ
// renderer (drop-in alternative to SpectralMaskEq).
//   c++ -O2 -std=c++17 -UNDEBUG -I src tests/test_iir_filterbank_eq.cpp \
//       -o tests/test_iir_filterbank_eq && tests/test_iir_filterbank_eq

#include "../src/iir_filterbank_eq.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int    kSR = 44100;
constexpr double kPi = 3.14159265358979323846;
constexpr int    NB  = 24;

nablafx::SpectralMaskEqParams cfg() {
    nablafx::SpectralMaskEqParams p;
    p.sample_rate = kSR; p.block_size = 128; p.num_control_params = NB;
    p.n_fft = 2048; p.hop = 512; p.n_bands = NB;
    p.min_gain_db = -12.f; p.max_gain_db = 12.f; p.f_min = 40.f; p.f_max = 18000.f;
    return p;
}

bool finite(const float* x, int n) { for (int i = 0; i < n; ++i) if (!std::isfinite(x[i])) return false; return true; }
double rms(const float* x, int n, int skip) { double s = 0; for (int i = skip; i < n; ++i) s += (double)x[i] * x[i]; return std::sqrt(s / (n - skip)); }

// Drive the smoother to steady state with a fixed band vector.
void converge(nablafx::IirFilterbankEq& eq, const std::vector<float>& bands) {
    for (int k = 0; k < 400; ++k) eq.set_params(bands.data(), NB);
}

// ---------------------------------------------------------------------------
// Test 1: flat (0 dB) curve → unity. Magnitude ~0 dB everywhere, signal passes
//         through unchanged, zero latency.
// ---------------------------------------------------------------------------
void test_flat_is_unity() {
    nablafx::IirFilterbankEq eq; eq.reset(cfg());
    std::vector<float> flat(NB, 0.5f);   // 0.5 → 0 dB
    converge(eq, flat);

    assert(eq.latency_samples() == 0);
    double worst = 0;
    for (double f : {50.0, 200.0, 1000.0, 5000.0, 12000.0})
        worst = std::max(worst, std::fabs(eq.magnitude_db(f)));
    std::fprintf(stderr, "[flat]  worst |mag| = %.3f dB (want ~0)\n", worst);
    assert(worst < 0.1);

    const int N = kSR;
    std::vector<float> in(N), out(N);
    for (int i = 0; i < N; ++i) in[i] = (float)(0.3 * std::sin(2.0 * kPi * 1000.0 * i / kSR));
    for (int i = 0; i + 128 <= N; i += 128) { eq.set_params(flat.data(), NB); eq.process(&in[i], &out[i], 128); }
    assert(finite(out.data(), N));
    const double r_in = rms(in.data(), N, N / 4), r_out = rms(out.data(), N, N / 4);
    std::fprintf(stderr, "[flat]  rms ratio = %.4f (want ~1)\n", r_out / r_in);
    assert(r_out > r_in * 0.98 && r_out < r_in * 1.02);
    std::fprintf(stderr, "[flat]  PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: magnitude-match — a known tilt curve is realized within tolerance at
//         the band centers (the summed-bell solve must hit the targets).
// ---------------------------------------------------------------------------
void test_magnitude_match() {
    nablafx::IirFilterbankEq eq; eq.reset(cfg());
    // mel centers (same formula as the renderer) to build a per-band target.
    const double mmin = 2595.0 * std::log10(1.0 + 40.0 / 700.0);
    const double mmax = 2595.0 * std::log10(1.0 + 18000.0 / 700.0);
    auto target_db = [&](double f) {  // gentle tilt: +6 @40 → −6 @16k (log-f)
        const double t = std::log2(std::max(f, 20.0) / 40.0) / std::log2(16000.0 / 40.0);
        return 6.0 - 12.0 * t;
    };
    std::vector<double> ctr(NB); std::vector<float> bands(NB);
    for (int b = 0; b < NB; ++b) {
        const double mel = mmin + (mmax - mmin) * (b + 1) / (NB + 1);
        ctr[b] = 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
        const double g = (target_db(ctr[b]) - (-12.0)) / 24.0;     // dB → sigmoid
        bands[b] = (float)std::clamp(g, 0.0, 1.0);
    }
    converge(eq, bands);

    double err2 = 0, emax = 0; int n = 0;
    for (int b = 1; b < NB - 1; ++b) {   // interior centers (edges are shelves)
        const double e = eq.magnitude_db(ctr[b]) - target_db(ctr[b]);
        err2 += e * e; emax = std::max(emax, std::fabs(e)); ++n;
    }
    const double rmsdb = std::sqrt(err2 / n);
    std::fprintf(stderr, "[match] RMS=%.2f dB max=%.2f dB (want RMS<0.5)\n", rmsdb, emax);
    assert(rmsdb < 0.5);
    assert(emax < 2.0);
    std::fprintf(stderr, "[match] PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: stability — extreme targets stay finite/bounded; zero latency.
// ---------------------------------------------------------------------------
void test_stability() {
    nablafx::IirFilterbankEq eq; eq.reset(cfg());
    const int N = kSR;
    std::vector<float> in(N), out(N);
    for (int i = 0; i < N; ++i) in[i] = (float)(0.5 * std::sin(2.0 * kPi * 220.0 * i / kSR)
                                              + 0.3 * std::sin(2.0 * kPi * 6000.0 * i / kSR));
    std::vector<float> hi(NB, 1.0f), lo(NB, 0.0f);
    for (int i = 0; i + 128 <= N; i += 128) {
        eq.set_params((i < N / 2 ? hi : lo).data(), NB);   // slam +12 then −12
        eq.process(&in[i], &out[i], 128);
    }
    assert(finite(out.data(), N));
    double pk = 0; for (int i = 0; i < N; ++i) pk = std::max(pk, (double)std::fabs(out[i]));
    std::fprintf(stderr, "[stab]  peak = %.3f (want < 8)\n", pk);
    assert(pk < 8.0);
    std::fprintf(stderr, "[stab]  PASS\n");
}

}  // namespace

int main() {
    test_flat_is_unity();
    test_magnitude_match();
    test_stability();
    std::fprintf(stderr, "ALL IIR-FILTERBANK-EQ TESTS PASSED\n");
    return 0;
}
