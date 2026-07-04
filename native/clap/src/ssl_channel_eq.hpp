// SSL 9000 J channel-strip EQ — native biquad cascade + harmonic stage.
//
//   HPF(×2) -> LF(shelf|bell) -> LMF(bell) -> HMF(bell) -> HF(shelf|bell) -> LPF
//                                                                    -> harmonic
//
// The linear cascade mirrors nablafx.processors.dsp.biquad (RBJ cookbook) and
// the training-side nablafx.processors.SSLConsoleEQ EXACTLY, so params learned
// by the grey-box transfer here bit-for-bit. Coefficients are recomputed at the
// host sample rate from (freq, gain, Q), so the response is correct at any rate
// (unlike the FFT/ONNX stages locked to 44.1 kHz — issue #11). Minimum-phase
// IIR: ZERO added latency.
//
// The engine consumes RESOLVED per-band (freq, gain, Q, shelf/bell mix). Those
// come from one of two sources selected by the plugin's SEQ_MODEL toggle:
//   * "Analytic" — closed-form RBJ-from-knob using the Phase-0 calibration.
//   * "Learned"  — the grey-box controller MLP output (corrects toward the
//                  measured console). Both feed this same cascade.
// The harmonic/analog colour is a RationalA waveshaper (== the trained
// StaticRationalNonlinearity version "A"); load its coeffs via set_harmonic().
//
// SslEqSolver implements the auto-EQ -> SSL coupling: a closed-form least-squares
// fit of the SSL band gains (fixed centres) to a target dB curve.
//
// Pure DSP — no CLAP/ORT/Accelerate deps, unit-testable standalone (cf.
// exciter.hpp, bass_mono.hpp, rational_a.hpp).

#pragma once

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "rational_a.hpp"

namespace nablafx {

// ---------------------------------------------------------------------------
// Direct-form-I biquad (double state). set() takes already-a0-normalized coeffs.
// ---------------------------------------------------------------------------
struct SslBiquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    double process(double x) {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void clear() { z1 = z2 = 0; }
    void set(double nb0, double nb1, double nb2, double na1, double na2) {
        b0 = nb0; b1 = nb1; b2 = nb2; a1 = na1; a2 = na2;
    }
    // |H(e^{jw})| for w = 2*pi*f/fs.
    double mag(double w) const {
        std::complex<double> z1c = std::polar(1.0, -w);
        std::complex<double> z2c = z1c * z1c;
        std::complex<double> num = b0 + b1 * z1c + b2 * z2c;
        std::complex<double> den = 1.0 + a1 * z1c + a2 * z2c;
        return std::abs(num / den);
    }
};

// ---------------------------------------------------------------------------
// RBJ coefficient design (matches nablafx dsp.biquad exactly, a0-normalized).
//   type: 0 peaking, 1 low_shelf, 2 high_shelf, 3 high_pass, 4 low_pass
// ---------------------------------------------------------------------------
inline SslBiquad ssl_design(int type, double gain_db, double freq_hz, double q, double fs) {
    SslBiquad bq;
    double f = freq_hz;
    if (f < 1.0) f = 1.0;
    if (f > 0.49 * fs) f = 0.49 * fs;       // clamp below Nyquist
    if (q < 1e-3) q = 1e-3;
    const double A = std::pow(10.0, gain_db / 40.0);
    const double w0 = 2.0 * M_PI * f / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * q);
    const double sA = std::sqrt(A);
    double b0, b1, b2, a0, a1, a2;
    switch (type) {
        case 1:  // low_shelf
            b0 = A * ((A + 1) - (A - 1) * cw + 2 * sA * alpha);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - 2 * sA * alpha);
            a0 = (A + 1) + (A - 1) * cw + 2 * sA * alpha;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - 2 * sA * alpha;
            break;
        case 2:  // high_shelf
            b0 = A * ((A + 1) + (A - 1) * cw + 2 * sA * alpha);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - 2 * sA * alpha);
            a0 = (A + 1) - (A - 1) * cw + 2 * sA * alpha;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - 2 * sA * alpha;
            break;
        case 3:  // high_pass
            b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
            a0 = 1 + alpha;    a1 = -2 * cw;   a2 = 1 - alpha;
            break;
        case 4:  // low_pass
            b0 = (1 - cw) / 2; b1 = 1 - cw;    b2 = (1 - cw) / 2;
            a0 = 1 + alpha;    a1 = -2 * cw;   a2 = 1 - alpha;
            break;
        default:  // peaking
            b0 = 1 + alpha * A; b1 = -2 * cw;  b2 = 1 - alpha * A;
            a0 = 1 + alpha / A; a1 = -2 * cw;  a2 = 1 - alpha / A;
            break;
    }
    bq.set(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0);
    return bq;
}

// Resolved per-band parameters fed to the cascade (from analytic map, learned
// controller, or user knobs). Frequencies/gains/Q are physical.
struct SslEqParamsRT {
    bool  eq_on   = false;
    bool  hpf_on  = false;  float hpf_hz = 100.f;   float hpf_q = 0.70710678f;
    bool  lpf_on  = false;  float lpf_hz = 20000.f; float lpf_q = 0.70710678f;
    float lf_gain = 0.f;  float lf_hz = 100.f;   float lf_q = 0.70710678f; float lf_bellmix = 0.f;
    float lmf_gain = 0.f; float lmf_hz = 500.f;  float lmf_q = 1.f;
    float hmf_gain = 0.f; float hmf_hz = 3000.f; float hmf_q = 1.f;
    float hf_gain = 0.f;  float hf_hz = 10000.f; float hf_q = 0.70710678f; float hf_bellmix = 0.f;
    float harmonic_mix = 0.f;   // 0 = no colour (bypass), 1 = full waveshaper

    bool operator==(const SslEqParamsRT& o) const {
        return eq_on == o.eq_on && hpf_on == o.hpf_on && lpf_on == o.lpf_on &&
               hpf_hz == o.hpf_hz && hpf_q == o.hpf_q && lpf_hz == o.lpf_hz && lpf_q == o.lpf_q &&
               lf_gain == o.lf_gain && lf_hz == o.lf_hz && lf_q == o.lf_q && lf_bellmix == o.lf_bellmix &&
               lmf_gain == o.lmf_gain && lmf_hz == o.lmf_hz && lmf_q == o.lmf_q &&
               hmf_gain == o.hmf_gain && hmf_hz == o.hmf_hz && hmf_q == o.hmf_q &&
               hf_gain == o.hf_gain && hf_hz == o.hf_hz && hf_q == o.hf_q && hf_bellmix == o.hf_bellmix &&
               harmonic_mix == o.harmonic_mix;
    }
    bool operator!=(const SslEqParamsRT& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------------------
// The stereo cascade.
// ---------------------------------------------------------------------------
class SslChannelEq {
public:
    static constexpr int kNumSsl    = 7;   // hpf, hpf, lf, lmf, hmf, hf, lpf (character core)
    static constexpr int kNumAssist = 6;   // interior solver-driven bells (auto-EQ offload)
    static constexpr int kNumBq     = kNumSsl + kNumAssist;   // 13

    // Fixed centres/Q of the assist bands (peaking); the coupling solver fits
    // their gains to the auto-EQ target curve. Log-spaced to fill the SSL gaps.
    static const std::array<double, kNumAssist>& assist_freqs() {
        static const std::array<double, kNumAssist> f =
            {60.0, 175.0, 500.0, 1400.0, 4000.0, 11000.0};
        return f;
    }
    static constexpr double kAssistQ = 1.4;

    void prepare(double sample_rate) {
        sr_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        reset();
        dirty_ = true;
    }

    void reset() {
        for (auto& ch : ch_)
            for (auto& b : ch) b.clear();
        // force redesign on next set_params (cf. exciter.hpp reset())
        have_last_ = false;
        dirty_ = true;
    }

    // Load the harmonic waveshaper (the trained RationalA). Empty = identity.
    void set_harmonic(const std::vector<float>& num, const std::vector<float>& den) {
        rational_.reset(num, den);
    }

    void set_params(const SslEqParamsRT& p) {
        if (have_last_ && p == last_) return;   // change-guard (cf. exciter sat_hpf)
        last_ = p; have_last_ = true;
        design_(p);
    }

    // Set the assist-band gains (dB) from the coupling solver — the auto-EQ
    // offload. Redesigns the 6 assist biquads (cheap, change-guarded). The plugin
    // should pre-smooth these per block so a re-solve can't zipper.
    void set_assist_gains(const float* gains_db, int n) {
        bool changed = false;
        for (int i = 0; i < kNumAssist; ++i) {
            float g = (i < n) ? gains_db[i] : 0.f;
            if (assist_gain_[i] != g) { assist_gain_[i] = g; changed = true; }
        }
        if (changed) design_assist_();
    }

    // In place, stereo. r may be null (mono).
    void process(float* l, float* r, int n) {
        process_ch_(0, l, n);
        if (r) process_ch_(1, r, n);
    }

    // Linear-cascade magnitude in dB at `hz` (excludes the harmonic stage).
    // Reflects the CURRENT design (call after set_params).
    double magnitude_db(double hz) const {
        const double w = 2.0 * M_PI * hz / sr_;
        double m = 1.0;
        for (int k = 0; k < kNumBq; ++k) m *= coeff_[k].mag(w);
        return 20.0 * std::log10(m > 1e-12 ? m : 1e-12);
    }

    int  latency_samples() const { return 0; }   // min-phase IIR
    double sample_rate() const { return sr_; }

private:
    static SslBiquad identity_() { SslBiquad b; b.set(1, 0, 0, 0, 0); return b; }

    // Shelf<->bell blend: linear in the (a0-normalized) coefficients; exact at
    // mix∈{0,1} and a smooth differentiable morph between (matches SSLConsoleEQ).
    static SslBiquad blend_(const SslBiquad& shelf, const SslBiquad& bell, double mix) {
        SslBiquad o;
        o.set((1 - mix) * shelf.b0 + mix * bell.b0,
              (1 - mix) * shelf.b1 + mix * bell.b1,
              (1 - mix) * shelf.b2 + mix * bell.b2,
              (1 - mix) * shelf.a1 + mix * bell.a1,
              (1 - mix) * shelf.a2 + mix * bell.a2);
        return o;
    }

    void design_(const SslEqParamsRT& p) {
        coeff_[0] = p.hpf_on ? ssl_design(3, 0, p.hpf_hz, p.hpf_q, sr_) : identity_();
        coeff_[1] = coeff_[0];   // second HPF section (18 dB/oct total)
        if (p.eq_on) {
            const double lfm = std::min(std::max((double)p.lf_bellmix, 0.0), 1.0);
            coeff_[2] = blend_(ssl_design(1, p.lf_gain, p.lf_hz, p.lf_q, sr_),
                               ssl_design(0, p.lf_gain, p.lf_hz, p.lf_q, sr_), lfm);
            coeff_[3] = ssl_design(0, p.lmf_gain, p.lmf_hz, p.lmf_q, sr_);
            coeff_[4] = ssl_design(0, p.hmf_gain, p.hmf_hz, p.hmf_q, sr_);
            const double hfm = std::min(std::max((double)p.hf_bellmix, 0.0), 1.0);
            coeff_[5] = blend_(ssl_design(2, p.hf_gain, p.hf_hz, p.hf_q, sr_),
                               ssl_design(0, p.hf_gain, p.hf_hz, p.hf_q, sr_), hfm);
        } else {
            coeff_[2] = coeff_[3] = coeff_[4] = coeff_[5] = identity_();
        }
        coeff_[6] = p.lpf_on ? ssl_design(4, 0, p.lpf_hz, p.lpf_q, sr_) : identity_();
        // copy the SSL sections into both channels, preserving each channel's z-state
        for (auto& ch : ch_)
            for (int k = 0; k < kNumSsl; ++k)
                ch[k].set(coeff_[k].b0, coeff_[k].b1, coeff_[k].b2, coeff_[k].a1, coeff_[k].a2);
        hmix_ = std::min(std::max((double)p.harmonic_mix, 0.0), 1.0);
        dirty_ = false;
    }

    // Build the 6 assist sections (peaking at the fixed assist freqs) from the
    // current assist gains. Copies into both channels, preserving z-state.
    void design_assist_() {
        const auto& f = assist_freqs();
        for (int i = 0; i < kNumAssist; ++i)
            coeff_[kNumSsl + i] = ssl_design(0, assist_gain_[i], f[i], kAssistQ, sr_);
        for (auto& ch : ch_)
            for (int k = kNumSsl; k < kNumBq; ++k)
                ch[k].set(coeff_[k].b0, coeff_[k].b1, coeff_[k].b2, coeff_[k].a1, coeff_[k].a2);
    }

    void process_ch_(int c, float* buf, int n) {
        auto& ch = ch_[c];
        const bool harm = hmix_ > 0.0 && !rational_.empty();
        for (int i = 0; i < n; ++i) {
            double x = (double)buf[i];
            for (int k = 0; k < kNumSsl; ++k) x = ch[k].process(x);        // SSL character core
            if (harm) x = (1.0 - hmix_) * x + hmix_ * rational_.eval(x);   // analog colour
            for (int k = kNumSsl; k < kNumBq; ++k) x = ch[k].process(x);   // assist bands (post-harmonic)
            buf[i] = (float)x;
        }
    }

    double sr_ = 48000.0;
    std::array<std::array<SslBiquad, kNumBq>, 2> ch_{};
    std::array<SslBiquad, kNumBq> coeff_{};   // shared design (magnitude source)
    std::array<float, kNumAssist> assist_gain_{};   // dB, from the coupling solver
    RationalA rational_{};
    double hmix_ = 0.0;
    SslEqParamsRT last_{};
    bool have_last_ = false;
    bool dirty_ = true;
};

// ---------------------------------------------------------------------------
// Auto-EQ -> SSL coupling solver.
//
// Fit the gains of a fixed set of SSL bands (centres/Qs/types frozen) to a
// target dB curve so the SSL EQ takes the coarse tonal correction the auto-EQ
// predicts. The per-band dB response is treated as linear in the band's gain
// (a unit-gain basis), so the fit is the closed-form least-squares solution
//   (BᵀB) g = Bᵀ t ,  B[k][b] = dB response of band b at 1 dB gain, freq k.
// Solved by Gaussian elimination; gains clamped to ±max_gain_db.
// ---------------------------------------------------------------------------
struct SslSolverBand { int type; double freq; double q; };   // type: 0 bell,1 lo-shelf,2 hi-shelf

class SslEqSolver {
public:
    // Default 4-band voicing (broad; can only take coarse tilt/shelves so it
    // cannot fight the auto-EQ's fine detail).
    SslEqSolver()
        : bands_{{{1, 90.0, 0.7}, {0, 500.0, 0.8}, {0, 3000.0, 0.9}, {2, 10000.0, 0.7}}} {}

    explicit SslEqSolver(std::vector<SslSolverBand> bands) : bands_(std::move(bands)) {}

    // Solver whose bands ARE the engine's assist bands, so solve() output maps
    // directly onto SslChannelEq::set_assist_gains (the auto-EQ coupling path).
    static SslEqSolver assist() {
        std::vector<SslSolverBand> b;
        for (double f : SslChannelEq::assist_freqs())
            b.push_back({0, f, SslChannelEq::kAssistQ});
        return SslEqSolver(std::move(b));
    }

    int num_bands() const { return (int)bands_.size(); }
    const SslSolverBand& band(int i) const { return bands_[i]; }

    // Fit gains (dB) so the cascade best approximates target_db at freqs (n pts).
    // `ref_gain_db` sets the unit-basis reference; clamp to ±max_gain_db.
    // Returns fitted gains (length num_bands()).
    std::vector<double> solve(const double* freqs, const double* target_db, int n,
                              double fs, double max_gain_db = 12.0,
                              double ref_gain_db = 6.0) const {
        const int B = (int)bands_.size();
        // basis[k][b] = (dB response of band b at ref_gain) / ref_gain
        std::vector<std::vector<double>> basis(n, std::vector<double>(B, 0.0));
        for (int b = 0; b < B; ++b) {
            SslBiquad bq = ssl_design(bands_[b].type, ref_gain_db, bands_[b].freq, bands_[b].q, fs);
            for (int k = 0; k < n; ++k) {
                double w = 2.0 * M_PI * freqs[k] / fs;
                basis[k][b] = 20.0 * std::log10(bq.mag(w) > 1e-12 ? bq.mag(w) : 1e-12) / ref_gain_db;
            }
        }
        // normal equations  A = BᵀB (B×B),  y = Bᵀt
        std::vector<std::vector<double>> A(B, std::vector<double>(B, 0.0));
        std::vector<double> y(B, 0.0);
        for (int i = 0; i < B; ++i) {
            for (int j = 0; j < B; ++j)
                for (int k = 0; k < n; ++k) A[i][j] += basis[k][i] * basis[k][j];
            for (int k = 0; k < n; ++k) y[i] += basis[k][i] * target_db[k];
        }
        // tiny ridge for conditioning
        for (int i = 0; i < B; ++i) A[i][i] += 1e-6;
        std::vector<double> g = solve_dense_(A, y);
        for (double& v : g) v = std::min(std::max(v, -max_gain_db), max_gain_db);
        return g;
    }

private:
    // Gaussian elimination with partial pivoting (small dense system).
    static std::vector<double> solve_dense_(std::vector<std::vector<double>> A,
                                            std::vector<double> b) {
        const int n = (int)b.size();
        for (int col = 0; col < n; ++col) {
            int piv = col;
            for (int r = col + 1; r < n; ++r)
                if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
            std::swap(A[piv], A[col]); std::swap(b[piv], b[col]);
            double d = A[col][col];
            if (std::fabs(d) < 1e-18) continue;
            for (int r = 0; r < n; ++r) {
                if (r == col) continue;
                double f = A[r][col] / d;
                for (int c = col; c < n; ++c) A[r][c] -= f * A[col][c];
                b[r] -= f * b[col];
            }
        }
        std::vector<double> x(n, 0.0);
        for (int i = 0; i < n; ++i)
            x[i] = std::fabs(A[i][i]) < 1e-18 ? 0.0 : b[i] / A[i][i];
        return x;
    }

    std::vector<SslSolverBand> bands_;
};

}  // namespace nablafx
