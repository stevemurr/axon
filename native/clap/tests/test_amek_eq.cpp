// Unit tests for the AMEK 9099-style EQ voicing (amek_eq.hpp via SslChannelEq).
//
// Pure DSP. Verifies that the C++ voicing reproduces scripts/fit_amek_eq.py
// (golden vectors at 44.1/48/96 kHz), that it matches held-out MEASURED console
// curves from the eqds dataset, and the engineering contracts: exact flat
// identity, stability over every knob range and rate, process() == magnitude,
// the duplicated RBJ helpers staying bit-identical to ssl_design, and the
// voicing-aware calibration solver.

#include "../src/ssl_channel_eq.hpp"
#include "amek_eq_golden.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace nablafx;

namespace {

constexpr double kPi = 3.14159265358979323846;

SslEqParamsRT params_from(const amek_golden::Knobs& k, bool eq_on = true) {
    SslEqParamsRT p;
    p.voicing = EqVoicing::Amek;
    p.eq_on = eq_on;
    p.hpf_on = k.hpf_on; p.hpf_hz = (float)k.hpf_f;
    p.lpf_on = k.lpf_on; p.lpf_hz = (float)k.lpf_f;
    p.lf_gain = (float)k.lf_g;   p.lf_hz = (float)k.lf_f;   p.lf_bellmix = k.lf_bell ? 1.f : 0.f;
    p.lmf_gain = (float)k.lmf_g; p.lmf_hz = (float)k.lmf_f; p.lmf_q = (float)k.lmf_q;
    p.hmf_gain = (float)k.hmf_g; p.hmf_hz = (float)k.hmf_f; p.hmf_q = (float)k.hmf_q;
    p.hf_gain = (float)k.hf_g;   p.hf_hz = (float)k.hf_f;   p.hf_bellmix = k.hf_bell ? 1.f : 0.f;
    return p;
}

double goertzel(const std::vector<float>& x, int skip, double f, double fs) {
    const double w = 2.0 * kPi * f / fs, cw = std::cos(w), coeff = 2.0 * cw;
    double s1 = 0, s2 = 0;
    for (int i = skip; i < (int)x.size(); ++i) { double s0 = x[i] + coeff * s1 - s2; s2 = s1; s1 = s0; }
    const double re = s1 - s2 * cw, im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im) / (0.5 * (x.size() - skip));
}

bool stable(const amek::Coeffs& c) {
    // Schur-Cohn triangle for 1 + a1 z^-1 + a2 z^-2.
    return std::isfinite(c.a1) && std::isfinite(c.a2) && std::isfinite(c.b0) &&
           std::isfinite(c.b1) && std::isfinite(c.b2) &&
           std::fabs(c.a2) < 1.0 && std::fabs(c.a1) < 1.0 + c.a2;
}

double coeff_mag_db(const amek::Coeffs& c, double f, double fs) {
    SslBiquad b; b.set(c.b0, c.b1, c.b2, c.a1, c.a2);
    return 20.0 * std::log10(std::max(b.mag(2.0 * kPi * f / fs), 1e-12));
}

// --- tests -----------------------------------------------------------------

// C++ == the Python model the tables were fitted with.
void test_matches_fit_script() {
    double worst = 0.0;
    for (const auto& g : amek_golden::kModel) {
        SslChannelEq eq; eq.prepare(g.fs);
        eq.set_params(params_from(g.k));
        for (size_t i = 0; i < g.db.size(); ++i) {
            if (amek_golden::kFreqs[i] >= 0.49 * g.fs) continue;
            worst = std::max(worst, std::fabs(eq.magnitude_db(amek_golden::kFreqs[i]) - g.db[i]));
        }
    }
    printf("[golden] max |C++ - python| = %.2e dB over %zu settings x 3 rates\n",
           worst, sizeof(amek_golden::kModel) / sizeof(amek_golden::kModel[0]) / 3);
    assert(worst < 1e-4);
}

// The shipped voicing vs curves measured from the console (held-out eval split,
// panel settings mapped to knob values by the fit script). Per-example RMS over
// 20 Hz-20 kHz, ignoring deep filter stop-bands (< -40 dB, noise floor).
void test_matches_measured_console() {
    std::vector<double> amek_rms, classic_rms;
    for (const auto& m : amek_golden::kMeasured) {
        for (int voicing = 0; voicing < 2; ++voicing) {
            SslChannelEq eq; eq.prepare(48000.0);
            SslEqParamsRT p = params_from(m.k, m.eq_on);
            p.voicing = voicing ? EqVoicing::Amek : EqVoicing::Ssl;
            eq.set_params(p);
            double se = 0.0; int n = 0;
            for (size_t i = 0; i < m.db.size(); ++i) {
                if (m.db[i] < -40.0) continue;
                const double e = eq.magnitude_db(amek_golden::kMeasFreqs[i]) - m.db[i];
                se += e * e; ++n;
            }
            (voicing ? amek_rms : classic_rms).push_back(std::sqrt(se / std::max(n, 1)));
        }
    }
    auto median = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    const double amek_max = *std::max_element(amek_rms.begin(), amek_rms.end());
    printf("[measured] %zu held-out console curves: Broad rms median %.3f max %.3f dB | Classic median %.3f dB\n",
           amek_rms.size(), median(amek_rms), amek_max, median(classic_rms));
    assert(median(amek_rms) < 0.4);
    assert(amek_max < 2.5);
    assert(median(classic_rms) > 3.0 * median(amek_rms));   // the voicings genuinely differ
}

// Flat bands + filters off => every core section is an exact identity.
void test_flat_is_bit_transparent() {
    SslChannelEq eq; eq.prepare(48000.0);
    SslEqParamsRT p; p.voicing = EqVoicing::Amek; p.eq_on = true;
    p.lf_bellmix = 1.f; p.lmf_q = 0.3f;   // shape knobs must not matter at 0 dB
    eq.set_params(p);
    std::mt19937 rng(7); std::uniform_real_distribution<float> U(-1.f, 1.f);
    std::vector<float> x(4096), y;
    for (auto& v : x) v = U(rng);
    y = x;
    eq.process(y.data(), nullptr, (int)y.size());
    for (size_t i = 0; i < x.size(); ++i) assert(y[i] == x[i]);
    printf("[flat] Broad voicing at 0 dB is bit-transparent\n");
}

// The fitted shelf law keeps pole Q == zero Q at 0 dB, so even the designer
// itself (not just the 0 dB short-circuit) returns a flat section.
void test_zero_gain_shelf_designs_flat() {
    double worst = 0.0;
    for (double fs : {44100.0, 48000.0, 96000.0})
        for (double f : {40.0, 150.0, 500.0, 2000.0, 6000.0, 15000.0})
            for (bool hf : {false, true})
                for (double hz : {30.0, 1000.0, 0.45 * fs})
                    worst = std::max(worst, std::fabs(coeff_mag_db(amek::shelf(hf, 0.0, f, fs), hz, fs)));
    printf("[zero-shelf] max |mag| of a 0 dB shelf = %.2e dB\n", worst);
    assert(worst < 1e-6);
}

// Every section stays strictly stable and finite over the plugin's full knob
// ranges (well beyond the measured console range) at common host rates.
void test_stable_over_all_knob_ranges() {
    int n = 0;
    const double gains[] = {-18, -9, -1, 1, 9, 18};
    for (double fs : {22050.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
        for (double g : gains) {
            for (double f = 30; f <= 600; f *= 1.25) {
                assert(stable(amek::shelf(false, g, f, fs))); assert(stable(amek::edge_bell(false, g, f, fs))); n += 2;
            }
            for (double f = 1500; f <= 20000; f *= 1.2) {
                assert(stable(amek::shelf(true, g, f, fs))); assert(stable(amek::edge_bell(true, g, f, fs))); n += 2;
            }
            for (double q : {0.1, 0.5, 1.0, 2.0, 4.0}) {
                for (double f = 60; f <= 3000; f *= 1.3) { assert(stable(amek::mid_bell(false, g, f, q, fs))); ++n; }
                for (double f = 400; f <= 20000; f *= 1.3) { assert(stable(amek::mid_bell(true, g, f, q, fs))); ++n; }
            }
        }
        amek::Coeffs two[2];
        for (double f = 20; f <= 500; f *= 1.2) { amek::hpf(f, fs, two); assert(stable(two[0]) && stable(two[1])); n += 2; }
        for (double f = 3000; f <= 22000; f *= 1.1) { amek::lpf(f, fs, two); assert(stable(two[0]) && stable(two[1])); n += 2; }
    }
    printf("[stability] %d section designs stable + finite (22.05-192 kHz, full knob ranges)\n", n);
}

// process() agrees with magnitude_db() — the curve the GUI draws is what you hear.
void test_process_matches_magnitude() {
    const double fs = 48000.0;
    amek_golden::Knobs k{true, 40.0, true, 16000.0, 5.0, 90.0, false, -6.0, 350.0, 1.2,
                         7.0, 2500.0, 0.8, 6.0, 9000.0, false};
    SslChannelEq eq; eq.prepare(fs);
    eq.set_params(params_from(k));
    double worst = 0.0;
    for (double f : {100.0, 350.0, 1000.0, 2500.0, 6000.0, 12000.0}) {
        std::vector<float> x(16384);
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float)(0.1 * std::sin(2.0 * kPi * f * i / fs));
        SslChannelEq e2; e2.prepare(fs); e2.set_params(params_from(k));
        e2.process(x.data(), nullptr, (int)x.size());
        const double meas = 20.0 * std::log10(goertzel(x, 8192, f, fs) / 0.1);
        worst = std::max(worst, std::fabs(meas - eq.magnitude_db(f)));
    }
    printf("[process] max |goertzel - magnitude_db| = %.3f dB\n", worst);
    assert(worst < 0.15);
}

// Decramped sections draw the same physical curve at any host rate. The mid
// bells are deliberately bilinear (the measured HMF cramps to 0 dB at Nyquist),
// so they are rate-dependent near Nyquist only, exactly like an RBJ bell.
void test_rate_invariance() {
    double worst = 0.0, hmf_low = 0.0;
    amek::Coeffs a[2], b[2];
    for (double f : {50.0, 80.0, 300.0, 1000.0, 3000.0, 8000.0, 12000.0, 15000.0, 19000.0}) {
        worst = std::max(worst, std::fabs(coeff_mag_db(amek::edge_bell(false, 8.0, 80.0, 44100.0), f, 44100.0) -
                                          coeff_mag_db(amek::edge_bell(false, 8.0, 80.0, 96000.0), f, 96000.0)));
        worst = std::max(worst, std::fabs(coeff_mag_db(amek::shelf(true, 10.0, 6000.0, 44100.0), f, 44100.0) -
                                          coeff_mag_db(amek::shelf(true, 10.0, 6000.0, 96000.0), f, 96000.0)));
        worst = std::max(worst, std::fabs(coeff_mag_db(amek::edge_bell(true, -9.0, 9000.0, 44100.0), f, 44100.0) -
                                          coeff_mag_db(amek::edge_bell(true, -9.0, 9000.0, 96000.0), f, 96000.0)));
        // LPF: compare the audible region only (>-20 dB); deep in the stop-band
        // the two rates legitimately differ by a dB or two at -25 dB and below.
        amek::lpf(9000.0, 44100.0, a); amek::lpf(9000.0, 96000.0, b);
        const double lp96 = coeff_mag_db(b[0], f, 96000.0) + coeff_mag_db(b[1], f, 96000.0);
        if (lp96 > -20.0)
            worst = std::max(worst, std::fabs(coeff_mag_db(a[0], f, 44100.0) + coeff_mag_db(a[1], f, 44100.0) - lp96));
        if (f <= 3000.0)
            hmf_low = std::max(hmf_low, std::fabs(coeff_mag_db(amek::mid_bell(true, -9.0, 3000.0, 1.0, 44100.0), f, 44100.0) -
                                                  coeff_mag_db(amek::mid_bell(true, -9.0, 3000.0, 1.0, 96000.0), f, 96000.0)));
    }
    printf("[rate] decramped sections max |44.1k - 96k| = %.3f dB; HMF bell <= 3 kHz %.3f dB\n", worst, hmf_low);
    assert(worst < 0.75);  // HF bell / LPF transition near 44.1k Nyquist: 2nd-order matching limit
    assert(hmf_low < 0.1);
    // The HMF bell's cramping is the measured behaviour: 0 dB at Nyquist.
    assert(std::fabs(coeff_mag_db(amek::mid_bell(true, 12.0, 6000.0, 1.0, 48000.0), 23999.0, 48000.0)) < 0.01);
}

// The mid bells share RBJ math with ssl_design; the local copy must stay exact.
void test_rbj_helpers_match_ssl_design() {
    for (double fs : {44100.0, 48000.0})
        for (double g : {-12.0, 3.0})
            for (double f : {100.0, 5000.0})
                for (double q : {0.3, 1.7}) {
                    const amek::Coeffs a = amek::rbj_peak(g, f, q, fs);
                    const SslBiquad s = ssl_design(0, g, f, q, fs);
                    assert(a.b0 == s.b0 && a.b1 == s.b1 && a.b2 == s.b2 && a.a1 == s.a1 && a.a2 == s.a2);
                    const amek::Coeffs h = amek::bilinear_hp2(f, q, fs);
                    const SslBiquad sh = ssl_design(3, 0, f, q, fs);
                    assert(h.b0 == sh.b0 && h.b1 == sh.b1 && h.b2 == sh.b2 && h.a1 == sh.a1 && h.a2 == sh.a2);
                }
    printf("[rbj] amek::rbj_peak / bilinear_hp2 bit-identical to ssl_design\n");
}

// Characteristic shapes the fit found in the measured console.
void test_console_character() {
    const double fs = 48000.0;
    // Both filters measure ~18 dB/oct on the console (the LPF adds a small
    // pre-corner bump): -3 dB at the knob, slope between two and three octaves out.
    auto pair_db = [&](const amek::Coeffs (&c)[2], double f) {
        return coeff_mag_db(c[0], f, fs) + coeff_mag_db(c[1], f, fs);
    };
    amek::Coeffs h[2]; amek::hpf(400.0, fs, h);
    amek::Coeffs l[2]; amek::lpf(1500.0, fs, l);
    const double h3 = pair_db(h, 400.0), h_slope = pair_db(h, 100.0) - pair_db(h, 50.0);
    const double l3 = pair_db(l, 1500.0), l_slope = pair_db(l, 6000.0) - pair_db(l, 12000.0);
    printf("[character] HPF400: %.2f dB @knob, %.1f dB/oct | LPF1.5k: %.2f dB @knob, %.1f dB/oct\n",
           h3, h_slope, l3, l_slope);
    assert(std::fabs(h3 + 3.0) < 0.6 && h_slope > 16.0 && h_slope < 20.0);
    assert(std::fabs(l3 + 3.0) < 0.6 && l_slope > 16.0 && l_slope < 21.0);
    // Proportional-Q HF bell: narrower (higher Q) at high gain than at low gain.
    auto width_oct = [&](double g) {
        const amek::Coeffs c = amek::edge_bell(true, g, 4000.0, fs);
        double lo = 4000.0, hi = 4000.0;
        while (coeff_mag_db(c, lo, fs) > g / 2.0) lo /= 1.01;
        while (coeff_mag_db(c, hi, fs) > g / 2.0) hi *= 1.01;
        return std::log2(hi / lo);
    };
    printf("[character] HF bell half-gain width: +3 dB %.2f oct, +15 dB %.2f oct\n", width_oct(3.0), width_oct(15.0));
    assert(width_oct(15.0) < 0.85 * width_oct(3.0));
    // Decramped top: at 23.9 kHz the 48 kHz shelf matches the same shelf designed
    // at 192 kHz (effectively analog there) instead of collapsing toward Nyquist.
    const double nyq = coeff_mag_db(amek::shelf(true, 12.0, 4000.0, fs), 23900.0, fs);
    const double ref = coeff_mag_db(amek::shelf(true, 12.0, 4000.0, 192000.0), 23900.0, 192000.0);
    printf("[character] HF shelf +12 @4k at 23.9 kHz: %.2f dB (48k) vs %.2f dB (192k)\n", nyq, ref);
    assert(std::fabs(nyq - ref) < 0.3);
}

// Toggling the voicing clears filter state (sections change meaning) and
// changes the response; the change guard still skips identical params.
void test_voicing_switch() {
    SslChannelEq eq; eq.prepare(48000.0);
    SslEqParamsRT p; p.eq_on = true; p.hf_gain = 9.f; p.hf_hz = 8000.f;
    eq.set_params(p);
    const double classic = eq.magnitude_db(16000.0);
    std::vector<float> x(512, 0.5f);
    eq.process(x.data(), nullptr, (int)x.size());
    p.voicing = EqVoicing::Amek;
    eq.set_params(p);
    const double broad = eq.magnitude_db(16000.0);
    printf("[switch] HF shelf +9 @8k at 16 kHz: Classic %.2f dB, Broad %.2f dB\n", classic, broad);
    assert(std::fabs(classic - broad) > 0.5);
    std::vector<float> z(1, 0.f);   // cleared state: zero in -> zero out
    eq.process(z.data(), nullptr, 1);
    assert(z[0] == 0.f);
}

// The calibration solver fits the Broad band shapes when asked to.
void test_solver_uses_voicing() {
    const double fs = 48000.0;
    const int N = 50;
    double f[N], tgt[N];
    const double truth[4] = {4.0, -3.0, 5.0, -4.0};
    std::vector<SslSolverBand> bands = {
        {1, 120.0, 0.707, EqVoicing::Amek, 0}, {0, 400.0, 1.0, EqVoicing::Amek, 1},
        {0, 2500.0, 1.0, EqVoicing::Amek, 2},  {2, 7000.0, 0.707, EqVoicing::Amek, 3},
    };
    for (int k = 0; k < N; ++k) {
        f[k] = 20.0 * std::pow(1000.0, (double)k / (N - 1));
        tgt[k] = coeff_mag_db(amek::shelf(false, truth[0], 120.0, fs), f[k], fs) +
                 coeff_mag_db(amek::mid_bell(false, truth[1], 400.0, 1.0, fs), f[k], fs) +
                 coeff_mag_db(amek::mid_bell(true, truth[2], 2500.0, 1.0, fs), f[k], fs) +
                 coeff_mag_db(amek::shelf(true, truth[3], 7000.0, fs), f[k], fs);
    }
    const auto g = SslEqSolver(bands).solve(f, tgt, N, fs, 18.0);
    double worst = 0.0;
    for (int b = 0; b < 4; ++b) worst = std::max(worst, std::fabs(g[b] - truth[b]));
    printf("[solver] Broad-voiced recovery: %.2f %.2f %.2f %.2f (max err %.2f dB)\n", g[0], g[1], g[2], g[3], worst);
    assert(worst < 1.0);
}

}  // namespace

int main() {
    test_matches_fit_script();
    test_matches_measured_console();
    test_flat_is_bit_transparent();
    test_zero_gain_shelf_designs_flat();
    test_stable_over_all_knob_ranges();
    test_process_matches_magnitude();
    test_rate_invariance();
    test_rbj_helpers_match_ssl_design();
    test_console_character();
    test_voicing_switch();
    test_solver_uses_voicing();
    printf("ALL AMEK EQ TESTS PASSED\n");
    return 0;
}
