// Unit tests for the SSL 9000 J channel EQ engine + coupling solver.
//   g++ -O2 -std=c++17 -I src tests/test_ssl_channel_eq.cpp -o tests/test_ssl_channel_eq \
//       && tests/test_ssl_channel_eq
//
// Pure DSP (no Accelerate) — runs anywhere. Verifies the biquad cascade matches
// the RBJ/SSLConsoleEQ definition, is sample-rate correct, that process()
// agrees with magnitude_db(), shelf/bell blend, the harmonic stage, and that the
// least-squares coupling solver recovers known gains and clamps.

#include "../src/ssl_channel_eq.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace nablafx;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double f, double a, int n, double fs) {
    std::vector<float> o(n);
    for (int i = 0; i < n; ++i) o[i] = (float)(a * std::sin(2.0 * kPi * f * i / fs));
    return o;
}
bool finite(const std::vector<float>& x) {
    for (float v : x) if (!std::isfinite(v)) return false;
    return true;
}
// Goertzel single-bin magnitude of x over [skip, n).
double goertzel(const std::vector<float>& x, int skip, double f, double fs) {
    const double w = 2.0 * kPi * f / fs, cw = std::cos(w), coeff = 2.0 * cw;
    double s1 = 0, s2 = 0;
    for (int i = skip; i < (int)x.size(); ++i) { double s0 = x[i] + coeff * s1 - s2; s2 = s1; s1 = s0; }
    const double re = s1 - s2 * cw, im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im) / (0.5 * (x.size() - skip));
}
double rms(const std::vector<float>& x, int skip) {
    double s = 0; for (int i = skip; i < (int)x.size(); ++i) s += (double)x[i] * x[i];
    return std::sqrt(s / (x.size() - skip));
}

SslEqParamsRT neutral() {
    SslEqParamsRT p; p.eq_on = true; p.hpf_on = false; p.lpf_on = false;
    p.lf_gain = p.lmf_gain = p.hmf_gain = p.hf_gain = 0.f; return p;
}

// --- tests -----------------------------------------------------------------

void test_neutral_is_transparent() {
    SslChannelEq eq; eq.prepare(48000.0);
    eq.set_params(neutral());
    auto x = sine(1000.0, 0.3, 8192, 48000.0);
    auto y = x;
    eq.process(y.data(), nullptr, (int)y.size());
    assert(finite(y));
    double err = 0; for (size_t i = 0; i < x.size(); ++i) err = std::max(err, (double)std::fabs(y[i] - x[i]));
    printf("[neutral] max|y-x| = %.3e\n", err);
    assert(err < 1e-5);                       // all-flat, filters off => identity
    assert(std::fabs(eq.magnitude_db(1000.0)) < 1e-4);
}

void test_bell_placement_and_gain() {
    SslChannelEq eq; eq.prepare(48000.0);
    auto p = neutral(); p.lmf_gain = 12.f; p.lmf_hz = 1000.f; p.lmf_q = 1.5f;
    eq.set_params(p);
    printf("[bell] mag@1000 = %.3f dB (want ~12)\n", eq.magnitude_db(1000.0));
    assert(std::fabs(eq.magnitude_db(1000.0) - 12.0) < 0.2);
    assert(eq.magnitude_db(1000.0) > eq.magnitude_db(250.0) + 6.0);   // localized peak
    assert(eq.magnitude_db(1000.0) > eq.magnitude_db(4000.0) + 6.0);
}

void test_process_matches_magnitude() {
    // A steady sine through the EQ should be scaled by the modelled magnitude.
    SslChannelEq eq; eq.prepare(48000.0);
    auto p = neutral(); p.hmf_gain = -9.f; p.hmf_hz = 3000.f; p.hmf_q = 1.f;
    eq.set_params(p);
    const double f = 3000.0;
    auto x = sine(f, 0.25, 16384, 48000.0);
    auto y = x; eq.process(y.data(), nullptr, (int)y.size());
    const int skip = 4000;   // let the IIR settle
    double meas_db = 20.0 * std::log10(goertzel(y, skip, f, 48000.0) / goertzel(x, skip, f, 48000.0));
    printf("[process==mag] measured %.3f dB vs model %.3f dB\n", meas_db, eq.magnitude_db(f));
    assert(std::fabs(meas_db - eq.magnitude_db(f)) < 0.15);
}

void test_sample_rate_correct() {
    // Same physical (freq,gain,Q): the magnitude at an audio freq must match
    // across sample rates (analog response is SR-independent). This is the whole
    // point of recomputing coefficients at the host rate.
    auto p = neutral(); p.lmf_gain = 6.f; p.lmf_hz = 1000.f; p.lmf_q = 1.f;
    p.hf_gain = 4.f; p.hf_hz = 8000.f; p.hf_bellmix = 0.f;
    double ref = 0;
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        SslChannelEq eq; eq.prepare(fs); eq.set_params(p);
        double m = eq.magnitude_db(1000.0);
        printf("[SR] fs=%.0f  mag@1000 = %.4f dB\n", fs, m);
        if (ref == 0) ref = m; else assert(std::fabs(m - ref) < 0.05);
    }
}

void test_shelf_vs_bell() {
    SslChannelEq eq; eq.prepare(48000.0);
    auto ps = neutral(); ps.lf_gain = 10.f; ps.lf_hz = 120.f; ps.lf_bellmix = 0.f;  // shelf
    eq.set_params(ps); double shelf20 = eq.magnitude_db(20.0);
    auto pb = ps; pb.lf_bellmix = 1.f;                                              // bell
    eq.set_params(pb); double bell20 = eq.magnitude_db(20.0);
    printf("[shelf/bell] LF@20Hz shelf=%.2f bell=%.2f dB\n", shelf20, bell20);
    assert(shelf20 > 6.0);      // shelf holds the low end up
    assert(bell20 < 2.0);       // bell has rolled back to ~flat well below centre
}

void test_hpf_lpf() {
    SslChannelEq eq; eq.prepare(48000.0);
    auto p = neutral(); p.hpf_on = true; p.hpf_hz = 100.f; p.lpf_on = true; p.lpf_hz = 8000.f;
    eq.set_params(p);
    printf("[filters] mag@20=%.1f mag@1000=%.2f mag@20000=%.1f dB\n",
           eq.magnitude_db(20.0), eq.magnitude_db(1000.0), eq.magnitude_db(20000.0));
    assert(eq.magnitude_db(20.0) < -6.0);         // HPF attenuates low end
    assert(std::fabs(eq.magnitude_db(1000.0)) < 1.0);  // passband ~flat
    assert(eq.magnitude_db(20000.0) < -6.0);      // LPF attenuates high end
}

void test_harmonic_stage() {
    SslChannelEq eq; eq.prepare(48000.0);
    // A gentle cubic-ish rational (adds odd harmonics); num = x - 0.15 x^3.
    eq.set_harmonic({0.f, 1.f, 0.f, -0.15f}, {});
    auto p = neutral();
    // mix = 0 -> identical to bypass
    // Integer-period window (500 Hz -> 96 samples, 1500 Hz -> 32) for clean bins.
    const int skip = 960, len = skip + 96 * 64;   // 6144 analysis samples
    p.harmonic_mix = 0.f; eq.set_params(p);
    auto x = sine(500.0, 0.4, len, 48000.0);
    auto y0 = x; eq.process(y0.data(), nullptr, (int)y0.size());
    double d = 0; for (size_t i = 0; i < x.size(); ++i) d = std::max(d, (double)std::fabs(y0[i] - x[i]));
    printf("[harmonic] mix=0 max|dev| = %.3e\n", d);
    assert(d < 1e-5);
    // mix > 0 -> introduces a 3rd harmonic that was absent at the input
    p.harmonic_mix = 1.f; eq.set_params(p);
    auto y1 = x; eq.process(y1.data(), nullptr, (int)y1.size());
    double h3_in = goertzel(x, skip, 1500.0, 48000.0);
    double h3_out = goertzel(y1, skip, 1500.0, 48000.0);
    printf("[harmonic] 3rd-harm in=%.2e out=%.2e\n", h3_in, h3_out);
    assert(h3_out > 1e-3 && h3_out > 50.0 * h3_in);
    assert(finite(y1));
}

void test_reset_clears_state() {
    SslChannelEq eq; eq.prepare(48000.0);
    auto p = neutral(); p.lmf_gain = 12.f; eq.set_params(p);
    auto x = sine(1000.0, 0.5, 2048, 48000.0);
    auto y = x; eq.process(y.data(), nullptr, (int)y.size());
    eq.reset();
    // after reset the same params must redesign (change-guard invalidated)
    eq.set_params(p);
    auto y2 = x; eq.process(y2.data(), nullptr, (int)y2.size());
    assert(finite(y2));
    // steady-state tail should match (state cleared identically both runs)
    double err = 0; for (int i = 1500; i < (int)x.size(); ++i) err = std::max(err, (double)std::fabs(y[i] - y2[i]));
    printf("[reset] tail max|dev| = %.3e\n", err);
    assert(err < 1e-5);
}

void test_solver_recovers_gains() {
    SslEqSolver solver;
    const double fs = 48000.0;
    const int B = solver.num_bands();
    // build a log freq grid (mimic the auto-EQ 50-point curve)
    const int N = 60;
    std::vector<double> freqs(N), target(N, 0.0);
    for (int k = 0; k < N; ++k) freqs[k] = 20.0 * std::pow(1000.0, (double)k / (N - 1));
    // synthesize a target = exact response of known band gains
    std::vector<double> truth = {3.0, -4.0, 2.5, -1.5};
    for (int b = 0; b < B; ++b) {
        auto bd = solver.band(b);
        SslBiquad bq = ssl_design(bd.type, truth[b], bd.freq, bd.q, fs);
        for (int k = 0; k < N; ++k)
            target[k] += 20.0 * std::log10(bq.mag(2.0 * kPi * freqs[k] / fs));
    }
    auto g = solver.solve(freqs.data(), target.data(), N, fs, 12.0);
    double err = 0; for (int b = 0; b < B; ++b) err = std::max(err, std::fabs(g[b] - truth[b]));
    printf("[solver] recovered gains: ");
    for (double v : g) printf("%.2f ", v);
    printf("(truth 3,-4,2.5,-1.5)  max err %.3f dB\n", err);
    assert(err < 0.5);   // linearized basis -> near-exact for moderate gains
}

void test_assist_bands_and_coupling() {
    SslChannelEq eq; eq.prepare(48000.0);
    eq.set_params(neutral());                         // SSL character bands flat
    // default assist gains = 0 -> still transparent
    assert(std::fabs(eq.magnitude_db(1000.0)) < 1e-3);
    // solve the assist bands to a broadband target (mimics an auto-EQ correction)
    SslEqSolver solver = SslEqSolver::assist();
    const int N = 50;
    std::vector<double> f(N), t(N);
    for (int k = 0; k < N; ++k) {
        f[k] = 20.0 * std::pow(1000.0, (double)k / (N - 1));
        t[k] = -4.0 + 8.0 * (double)k / (N - 1);      // -4 .. +4 dB tilt
    }
    auto g = solver.solve(f.data(), t.data(), N, 48000.0, 12.0);
    std::vector<float> gf(g.begin(), g.end());
    eq.set_assist_gains(gf.data(), (int)gf.size());
    double err = 0;
    for (int k = 0; k < N; ++k)
        if (f[k] >= 60 && f[k] <= 12000) err = std::max(err, std::fabs(eq.magnitude_db(f[k]) - t[k]));
    printf("[assist coupling] max|eq-target| over 60-12kHz = %.2f dB (6 assist bands vs tilt)\n", err);
    assert(err < 2.5);
    // zeroing the assist gains returns to neutral (offload fully disengaged)
    std::array<float, 6> zero{};
    eq.set_assist_gains(zero.data(), 6);
    assert(std::fabs(eq.magnitude_db(1000.0)) < 0.1);
    // process stays finite with assist engaged + harmonic on
    eq.set_assist_gains(gf.data(), (int)gf.size());
    auto p = neutral(); p.harmonic_mix = 0.5f; eq.set_harmonic({0.f, 1.f, 0.f, -0.1f}, {}); eq.set_params(p);
    auto x = sine(1000.0, 0.3, 4096, 48000.0);
    eq.process(x.data(), nullptr, (int)x.size());
    assert(finite(x));
}

void test_solver_clamps() {
    SslEqSolver solver;
    const double fs = 48000.0;
    const int N = 60;
    std::vector<double> freqs(N), target(N);
    for (int k = 0; k < N; ++k) { freqs[k] = 20.0 * std::pow(1000.0, (double)k / (N - 1)); target[k] = 40.0; }
    auto g = solver.solve(freqs.data(), target.data(), N, fs, 12.0);
    for (double v : g) assert(v <= 12.0 + 1e-9 && v >= -12.0 - 1e-9);
    printf("[solver clamp] all gains within +-12 dB\n");
}

}  // namespace

int main() {
    test_neutral_is_transparent();
    test_bell_placement_and_gain();
    test_process_matches_magnitude();
    test_sample_rate_correct();
    test_shelf_vs_bell();
    test_hpf_lpf();
    test_harmonic_stage();
    test_reset_clears_state();
    test_solver_recovers_gains();
    test_assist_bands_and_coupling();
    test_solver_clamps();
    printf("\nAll SSL channel EQ tests passed.\n");
    return 0;
}
