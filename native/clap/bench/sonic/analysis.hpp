// Measurement primitives for the sonic A/B harness. Header-only, no FFT
// dependency — single-bin DFT (Goertzel) for spectral probes + time-domain
// statistics. See docs/sonic_harness_spec.md.
#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace sonic {

constexpr double kPi = 3.14159265358979323846;

// Device under test: mono in -> mono out, fixed-configured.
using DUT = std::function<void(const float* in, float* out, int n)>;

// ---- single-bin magnitude (amplitude) over [skip, n) -----------------------
inline double goertzel(const float* x, int n, int skip, double f, double sr) {
    const double w = 2.0 * kPi * f / sr, cw = std::cos(w), coeff = 2.0 * cw;
    double s1 = 0, s2 = 0;
    for (int i = skip; i < n; ++i) { double s0 = x[i] + coeff * s1 - s2; s2 = s1; s1 = s0; }
    const double re = s1 - s2 * cw, im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im) / (0.5 * (n - skip));
}
inline double goertzel(const std::vector<float>& x, int skip, double f, double sr) {
    return goertzel(x.data(), (int)x.size(), skip, f, sr);
}

inline double lin2db(double a) { return a > 1e-12 ? 20.0 * std::log10(a) : -240.0; }

// ---- THD: harmonics 2..K relative to the fundamental -----------------------
struct Thd { double ratio, db, percent; };
inline Thd thd(const std::vector<float>& y, double f0, double sr, int K = 8, int skip = -1) {
    if (skip < 0) skip = (int)y.size() / 4;
    const double h1 = goertzel(y, skip, f0, sr);
    double s = 0;
    for (int k = 2; k <= K; ++k) {
        const double fk = k * f0;
        if (fk >= 0.49 * sr) break;
        const double hk = goertzel(y, skip, fk, sr);
        s += hk * hk;
    }
    const double r = (h1 > 1e-12) ? std::sqrt(s) / h1 : 0.0;
    return {r, lin2db(r), 100.0 * r};
}

// nth harmonic level, dB relative to fundamental.
inline double harmonic_db(const std::vector<float>& y, double f0, int k, double sr, int skip = -1) {
    if (skip < 0) skip = (int)y.size() / 4;
    return lin2db(goertzel(y, skip, k * f0, sr) / std::max(1e-12, goertzel(y, skip, f0, sr)));
}

// ---- aliasing: inharmonic low-band products relative to the fundamental ----
// Probe a band where no real harmonic of f0 lives; foldback shows up here.
inline double alias_over_fund_db(const std::vector<float>& y, double f0, double sr,
                                 double lo = 300.0, double hi = 2500.0, double step = 50.0,
                                 int skip = -1) {
    if (skip < 0) skip = (int)y.size() / 4;
    const double fund = goertzel(y, skip, f0, sr);
    double e = 0; int n = 0;
    for (double f = lo; f <= hi; f += step) {
        // skip if f is within 1 step of a true harmonic of f0
        bool harmonic = false;
        for (int k = 1; k * f0 < 0.5 * sr; ++k) if (std::abs(k * f0 - f) < step) { harmonic = true; break; }
        if (harmonic) continue;
        const double a = goertzel(y, skip, f, sr); e += a * a; ++n;
    }
    const double rms = n ? std::sqrt(e / n) : 0.0;
    return lin2db(rms / std::max(1e-12, fund));
}

// ---- IMD (CCIF twin-tone): difference product at f2-f1 + neighbors ---------
inline double imd_ccif_db(const std::vector<float>& y, double f1, double f2, double sr,
                          int skip = -1) {
    if (skip < 0) skip = (int)y.size() / 4;
    const double carriers = goertzel(y, skip, f1, sr) + goertzel(y, skip, f2, sr);
    const double d = f2 - f1;
    double prod = goertzel(y, skip, d, sr);          // 2nd-order difference tone
    prod = std::max(prod, goertzel(y, skip, 2 * d, sr));
    return lin2db(prod / std::max(1e-12, 0.5 * carriers));
}

// ---- IMD (SMPTE): sidebands around the HF carrier f2 from LF carrier f1 ----
inline double imd_smpte_db(const std::vector<float>& y, double f1, double f2, double sr,
                           int skip = -1) {
    if (skip < 0) skip = (int)y.size() / 4;
    const double carrier = goertzel(y, skip, f2, sr);
    double sb = goertzel(y, skip, f2 - f1, sr) + goertzel(y, skip, f2 + f1, sr);
    sb += goertzel(y, skip, f2 - 2 * f1, sr) + goertzel(y, skip, f2 + 2 * f1, sr);
    return lin2db(sb / std::max(1e-12, carrier));
}

// ---- realized gain at f (amplitude ratio out/in), dB -----------------------
inline double gain_db_at(const DUT& dut, double f, double sr, double amp = 0.25,
                         int n = -1) {
    if (n < 0) n = (int)(sr * 0.5);
    std::vector<float> in(n), out(n);
    for (int i = 0; i < n; ++i) in[i] = (float)(amp * std::sin(2.0 * kPi * f * i / sr));
    dut(in.data(), out.data(), n);
    const int skip = n / 2;
    const double gi = goertzel(in, skip, f, sr), go = goertzel(out, skip, f, sr);
    return lin2db(go / std::max(1e-12, gi));
}

// ---- crest factor (peak/RMS), dB, over [skip,n) ----------------------------
inline double crest_db(const float* x, int n, int skip = 0) {
    double pk = 0, s = 0; int c = 0;
    for (int i = skip; i < n; ++i) { double a = std::fabs(x[i]); pk = std::max(pk, a); s += (double)x[i] * x[i]; ++c; }
    const double rms = c ? std::sqrt(s / c) : 0.0;
    return lin2db(pk / std::max(1e-12, rms));
}

// ---- modulation depth of the short-time RMS over a steady region -----------
// For a steady input + static processing, output STRMS should be flat; STFT
// framing adds a hop-rate ripple. Returns 20*log10(rms_std/rms_mean) (dB).
inline double modulation_depth_db(const float* x, int n, double sr, double win_ms = 5.0,
                                  int skip = 0) {
    const int w = std::max(1, (int)(win_ms * 1e-3 * sr));
    std::vector<double> e;
    for (int i = skip; i + w <= n; i += w) {
        double s = 0; for (int j = 0; j < w; ++j) s += (double)x[i + j] * x[i + j];
        e.push_back(std::sqrt(s / w));
    }
    if (e.size() < 2) return -240.0;
    double mean = 0; for (double v : e) mean += v; mean /= e.size();
    double var = 0; for (double v : e) var += (v - mean) * (v - mean); var /= e.size();
    return lin2db(std::sqrt(var) / std::max(1e-12, mean));
}

// ---- null residual: best-aligned max|out - in| over a delay search (dBFS) --
inline double null_residual_db(const std::vector<float>& in, const std::vector<float>& out,
                               int max_delay) {
    const int n = (int)in.size();
    double best = 1e9;
    for (int d = 0; d <= max_delay; ++d) {
        double m = 0;
        for (int i = d + n / 4; i < n; ++i) m = std::max(m, (double)std::fabs(out[i] - in[i - d]));
        best = std::min(best, m);
    }
    return lin2db(best);
}

// ---- measured latency: delay that maximizes cross-correlation of an impulse-
//      ish response (peak of |out|) -----------------------------------------
inline int measured_latency(const DUT& dut, int probe_n = 8192, int impulse_at = 1024) {
    std::vector<float> in(probe_n, 0.f), out(probe_n, 0.f);
    in[impulse_at] = 1.0f;
    dut(in.data(), out.data(), probe_n);
    int peak = impulse_at; double pv = 0;
    for (int i = 0; i < probe_n; ++i) if (std::fabs(out[i]) > pv) { pv = std::fabs(out[i]); peak = i; }
    return peak - impulse_at;
}

// ---- calibrate a scalar drive so THD(f0) hits target_thd_percent -----------
// "Matched effect" = same total harmonic energy added, which is well-posed for
// any Character (unlike matching a single harmonic that may be absent). THD is
// monotonic in drive over the useful range, so binary search converges.
// factory(drive_db) returns a configured DUT.
inline double calibrate_drive(const std::function<DUT(double)>& factory,
                              double f0, double sr, double target_thd_percent,
                              double lo = -36.0, double hi = 48.0, int iters = 28) {
    auto thd_pct = [&](double drv) {
        DUT d = factory(drv);
        const int n = (int)(sr * 0.5);
        std::vector<float> in(n), out(n);
        for (int i = 0; i < n; ++i) in[i] = (float)(0.25 * std::sin(2.0 * kPi * f0 * i / sr));
        d(in.data(), out.data(), n);
        return thd(out, f0, sr).percent;
    };
    for (int i = 0; i < iters; ++i) {
        const double mid = 0.5 * (lo + hi);
        (thd_pct(mid) < target_thd_percent ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

}  // namespace sonic
