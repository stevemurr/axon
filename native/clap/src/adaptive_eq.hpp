// Adaptive Auto-EQ controller — the productionized C1→C2 cascade.
//
// A deterministic, real-time replacement for the LSTM Auto-EQ controller that
// mode-collapses to the class-mean curve. It measures the running input
// spectrum and emits a per-band gain curve that (C1) matches an empirical
// tonal-balance target and (C2) dynamically suppresses resonances, with one
// energy-domain makeup so the stage is loudness-neutral. It cannot mode-collapse
// (the curve is a deterministic function of the input spectrum) and needs no
// per-class model — one instance per channel.
//
// Drop-in for the existing renderer: feed observe() the input block, then push
// target_bands() into SpectralMaskEq/IirFilterbankEq::set_params (it emits the
// same [0,1] sigmoid contract). Analysis is internal (a 2048-pt running FFT) and
// adds NO output latency — the renderer applies the curve.
//
// Validated in bench/sonic/harness_eq_control.cpp (the C1C2_Cascade candidate):
// strongly adaptive, target-matching, loudness-neutral, transparent on
// transients, low modulation. Header-only; uses Accelerate (vDSP), like
// spectral_mask_eq.hpp.
#pragma once

#include <Accelerate/Accelerate.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include "meta.hpp"   // SpectralMaskEqParams (shared config with the renderer)
#include "adaptive_eq_targets.hpp"   // GENERATED empirical tonal-balance curves

namespace nablafx {

namespace adaptive_eq_detail {

// Glasberg & Moore ERB helpers (kept for future perceptual weighting).
inline double erb_hz(double f) { return 24.7 * (4.37e-3 * f + 1.0); }

// HTK-mel band centers — matches SpectralMaskEq::build_mel_ so the controller's
// bands line up with the renderer's.
inline std::vector<double> mel_centers(const SpectralMaskEqParams& c) {
    const double mmin = 2595.0 * std::log10(1.0 + c.f_min / 700.0);
    const double mmax = 2595.0 * std::log10(1.0 + c.f_max / 700.0);
    std::vector<double> out(c.n_bands);
    for (int b = 0; b < c.n_bands; ++b) {
        const double mel = mmin + (mmax - mmin) * (b + 1) / (c.n_bands + 1);
        out[b] = std::min(700.0 * (std::pow(10.0, mel / 2595.0) - 1.0), 0.49 * c.sample_rate);
    }
    return out;
}

// Empirical tonal-balance target (relative dB): ~flat below 100 Hz, ~5 dB/oct
// decay 100 Hz→4 kHz (Ma/Reiss/Black), gentler above. Zero-meaned downstream.
inline double target_db(double f) {
    const double knee_lo = 100.0, knee_hi = 4000.0, slope = 5.0;
    if (f <= knee_lo) return 0.0;
    if (f <= knee_hi) return -slope * std::log2(f / knee_lo);
    const double at_hi = -slope * std::log2(knee_hi / knee_lo);
    return at_hi - 0.5 * slope * std::log2(f / knee_hi);
}

// Per-band deadband half-width (dB): tonal-balance refs are ranges, not lines.
inline double tolerance_db(double f) {
    if (f < 80.0 || f > 8000.0) return 2.5;
    return 1.5;
}

inline void gaussian_smooth_db(const float* in, float* out, int nb, double sigma) {
    if (sigma <= 1e-6) { for (int i = 0; i < nb; ++i) out[i] = in[i]; return; }
    const int half = (int)std::ceil(3.0 * sigma);
    for (int i = 0; i < nb; ++i) {
        double acc = 0.0, wsum = 0.0;
        for (int j = -half; j <= half; ++j) {
            const int k = i + j;
            if (k < 0 || k >= nb) continue;
            const double w = std::exp(-0.5 * (double)j * j / (sigma * sigma));
            acc += w * in[k]; wsum += w;
        }
        out[i] = (float)(wsum > 0 ? acc / wsum : in[i]);
    }
}

// Broadband trim (dB) so the realized broadband energy is unchanged given the
// running spectrum: a zero-mean-dB curve is NOT loudness-neutral (dB↔power is
// nonlinear). Uses the controller's own estimate — no output measurement.
inline double makeup_db(const float* gain_db, const float* spec_db, int nb) {
    double ein = 0.0, eout = 0.0;
    for (int b = 0; b < nb; ++b) {
        const double p = std::pow(10.0, (double)spec_db[b] / 10.0);
        ein  += p;
        eout += p * std::pow(10.0, (double)gain_db[b] / 10.0);
    }
    if (ein <= 1e-30 || eout <= 1e-30) return 0.0;
    return -10.0 * std::log10(eout / ein);
}

// Continuous deadband (shrink toward 0 by tol) — no 0↔±tol toggling (anti-zipper).
inline float soft_deadband(float v, float tol) {
    if (v >  tol) return v - tol;
    if (v < -tol) return v + tol;
    return 0.0f;
}

// Per-band noise gate: false for near-silent bands (>gate_db below the loudest).
inline bool band_active(const float* spec_db, int nb, int b, double gate_db) {
    float peak = -1e30f;
    for (int i = 0; i < nb; ++i) peak = std::max(peak, spec_db[i]);
    return spec_db[b] >= peak - (float)gate_db;
}

// Running mel-band spectrum: EWMA of windowed-FFT mean power per band, in dB.
// Flat-in ⇒ flat-dB (per-band mean, not sum).
class RunningMelSpectrum {
public:
    void reset(const SpectralMaskEqParams& cfg, double tau_s = 2.0) {
        cfg_ = cfg;
        n_fft_ = (cfg.n_fft > 0 && (cfg.n_fft & (cfg.n_fft - 1)) == 0) ? cfg.n_fft : 2048;
        hop_   = (cfg.hop > 0 && cfg.hop <= n_fft_) ? cfg.hop : (n_fft_ / 4);
        n_freq_ = n_fft_ / 2 + 1;
        log2_  = (vDSP_Length)std::lround(std::log2((double)n_fft_));
        if (fft_) vDSP_destroy_fftsetup(fft_);
        fft_ = vDSP_create_fftsetup(log2_, kFFTRadix2);

        window_.assign(n_fft_, 0.0f);
        for (int n = 0; n < n_fft_; ++n)
            window_[n] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * n / n_fft_));
        in_ring_.assign(n_fft_, 0.0f); in_fill_ = 0; since_ = 0;
        windowed_.assign(n_fft_, 0.0f);
        re_.assign(n_fft_ / 2, 0.0f); im_.assign(n_fft_ / 2, 0.0f);
        power_.assign(n_freq_, 0.0f);
        build_mel_();
        mel_db_.assign(cfg_.n_bands, -120.0f);
        primed_ = false;
        const double frames_per_tau = (cfg_.sample_rate * tau_s) / std::max(1, hop_);
        alpha_ = std::exp(-1.0 / std::max(1.0, frames_per_tau));
    }
    ~RunningMelSpectrum() { if (fft_) vDSP_destroy_fftsetup(fft_); }
    RunningMelSpectrum() = default;
    RunningMelSpectrum(const RunningMelSpectrum&) = delete;
    RunningMelSpectrum& operator=(const RunningMelSpectrum&) = delete;
    // Movable (transfers the FFTSetup; nulls the source) so an owning
    // AdaptiveEqController/ChannelChain can live in a std::vector.
    RunningMelSpectrum(RunningMelSpectrum&& o) noexcept { swap_(o); }
    RunningMelSpectrum& operator=(RunningMelSpectrum&& o) noexcept {
        if (this != &o) { if (fft_) vDSP_destroy_fftsetup(fft_), fft_ = nullptr; swap_(o); }
        return *this;
    }

    void observe(const float* in, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            in_ring_[in_fill_] = in[i];
            in_fill_ = (in_fill_ + 1) % n_fft_;
            if (++since_ >= hop_) { since_ -= hop_; frame_(); }
        }
    }
    void mel_db(float* out) const { for (int b = 0; b < cfg_.n_bands; ++b) out[b] = mel_db_[b]; }
    const std::vector<double>& centers() const { return centers_; }
    bool primed() const { return primed_; }

private:
    void swap_(RunningMelSpectrum& o) noexcept {
        std::swap(cfg_, o.cfg_); std::swap(n_fft_, o.n_fft_); std::swap(hop_, o.hop_);
        std::swap(n_freq_, o.n_freq_); std::swap(in_fill_, o.in_fill_); std::swap(since_, o.since_);
        std::swap(log2_, o.log2_); std::swap(fft_, o.fft_); std::swap(alpha_, o.alpha_);
        std::swap(primed_, o.primed_);
        window_.swap(o.window_); in_ring_.swap(o.in_ring_); windowed_.swap(o.windowed_);
        re_.swap(o.re_); im_.swap(o.im_); power_.swap(o.power_); band_to_bin_.swap(o.band_to_bin_);
        band_wsum_.swap(o.band_wsum_); mel_db_.swap(o.mel_db_); centers_.swap(o.centers_);
    }
    void frame_() {
        const int tail = n_fft_ - in_fill_;
        vDSP_vmul(in_ring_.data() + in_fill_, 1, window_.data(), 1, windowed_.data(), 1, tail);
        if (in_fill_ > 0)
            vDSP_vmul(in_ring_.data(), 1, window_.data() + tail, 1, windowed_.data() + tail, 1, in_fill_);
        DSPSplitComplex sc{re_.data(), im_.data()};
        vDSP_ctoz(reinterpret_cast<DSPComplex*>(windowed_.data()), 2, &sc, 1, n_fft_ / 2);
        vDSP_fft_zrip(fft_, &sc, 1, log2_, kFFTDirection_Forward);
        power_[0] = re_[0] * re_[0];
        power_[n_freq_ - 1] = im_[0] * im_[0];
        for (int k = 1; k < n_fft_ / 2; ++k) power_[k] = re_[k] * re_[k] + im_[k] * im_[k];
        for (int b = 0; b < cfg_.n_bands; ++b) {
            double p = 0.0;
            const float* w = band_to_bin_.data() + (size_t)b * n_freq_;
            for (int k = 0; k < n_freq_; ++k) p += (double)w[k] * power_[k];
            p /= band_wsum_[b];
            const float db = 10.0f * std::log10((float)std::max(p, 1e-20));
            mel_db_[b] = primed_ ? (float)(alpha_ * mel_db_[b] + (1.0 - alpha_) * db) : db;
        }
        primed_ = true;
    }
    void build_mel_() {
        const double mel_min = 2595.0 * std::log10(1.0 + cfg_.f_min / 700.0);
        const double mel_max = 2595.0 * std::log10(1.0 + cfg_.f_max / 700.0);
        std::vector<double> binpts(cfg_.n_bands + 2);
        centers_.assign(cfg_.n_bands, 0.0);
        for (int i = 0; i < cfg_.n_bands + 2; ++i) {
            const double mel = mel_min + (mel_max - mel_min) * i / (cfg_.n_bands + 1);
            const double hz  = 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
            binpts[i] = std::min(std::max(hz * n_fft_ / cfg_.sample_rate, 0.0), (double)(n_freq_ - 1));
            if (i >= 1 && i <= cfg_.n_bands) centers_[i - 1] = hz;
        }
        band_to_bin_.assign((size_t)cfg_.n_bands * n_freq_, 0.0f);
        band_wsum_.assign(cfg_.n_bands, 1e-9f);
        for (int b = 0; b < cfg_.n_bands; ++b) {
            const double l = binpts[b], c = binpts[b + 1], r = binpts[b + 2];
            const double ls = std::max(c - l, 1e-6), rs = std::max(r - c, 1e-6);
            double ws = 0.0;
            for (int k = 0; k < n_freq_; ++k) {
                const double up = (k - l) / ls, dn = (r - k) / rs;
                float wv = (float)std::min(up, dn); wv = wv > 0 ? wv : 0.0f;
                band_to_bin_[(size_t)b * n_freq_ + k] = wv; ws += wv;
            }
            band_wsum_[b] = (float)std::max(ws, 1e-9);
        }
    }
    SpectralMaskEqParams cfg_{};
    int n_fft_ = 2048, hop_ = 512, n_freq_ = 1025, in_fill_ = 0, since_ = 0;
    vDSP_Length log2_ = 11;
    FFTSetup fft_ = nullptr;
    double alpha_ = 0.0; bool primed_ = false;
    std::vector<float> window_, in_ring_, windowed_, re_, im_, power_, band_to_bin_, band_wsum_, mel_db_;
    std::vector<double> centers_;
};

}  // namespace adaptive_eq_detail

// ----------------------------------------------------------------------------
// The controller. One per channel; class-agnostic (no per-class model).
// ----------------------------------------------------------------------------
class AdaptiveEqController {
public:
    void reset(const SpectralMaskEqParams& cfg) {
        cfg_ = cfg; nb_ = cfg.n_bands;
        spec_.reset(cfg);
        centers_ = adaptive_eq_detail::mel_centers(cfg);
        out_db_.assign(nb_, 0.0f);
        in_db_.assign(nb_, 0.0f); raw_.assign(nb_, 0.0f); sm_.assign(nb_, 0.0f); base_.assign(nb_, 0.0f);
        cut_db_.assign(nb_, 0.0f);

        const double bpt = (double)cfg_.sample_rate * tau_s_ / std::max(1, cfg_.block_size);
        alpha_ = std::exp(-1.0 / std::max(1.0, bpt));

        a_att_.assign(nb_, 0.0f); a_rel_.assign(nb_, 0.0f);
        const double lf = std::log2(std::max(20.0, (double)cfg_.f_min));
        const double hf = std::log2(std::max(lf + 1.0, (double)cfg_.f_max));
        for (int b = 0; b < nb_; ++b) {
            const double l = std::log2(std::max(20.0, centers_[b]));
            const double frac = std::min(std::max((l - lf) / (hf - lf), 0.0), 1.0);
            const double hf_scale = 1.0 - 0.55 * frac;
            a_att_[b] = block_alpha_(attack_ms_  * hf_scale);
            a_rel_[b] = block_alpha_(release_ms_ * hf_scale);
        }
    }

    void observe(const float* in, std::size_t n) { spec_.observe(in, n); }

    // Per-band gain target in dB (for tests / UI display).
    void target_db(float* per_band_db, int nb) {
        using namespace adaptive_eq_detail;
        if (!spec_.primed()) { for (int b = 0; b < nb; ++b) per_band_db[b] = 0.0f; return; }
        spec_.mel_db(in_db_.data());

        // C1 — tonal target match: want−have, smooth, zero-mean, gate, soft deadband.
        // Target is the empirical corpus curve (full_mix by default; see
        // set_target_curve), density-normalized to match the running spectrum.
        for (int b = 0; b < nb_; ++b)
            raw_[b] = (float)adaptive_eq_detail::target_curve_db(centers_[b], target_idx_) - in_db_[b];
        gaussian_smooth_db(raw_.data(), sm_.data(), nb_, match_sigma_);
        double mean = 0.0; for (int b = 0; b < nb_; ++b) mean += sm_[b]; mean /= nb_;
        for (int b = 0; b < nb_; ++b) {
            if (!band_active(in_db_.data(), nb_, b, gate_db_)) { sm_[b] = 0.0f; continue; }
            sm_[b] = soft_deadband((float)(sm_[b] - mean), (float)tolerance_db(centers_[b]));
        }

        // C2 — dynamic resonance suppression over a strong local baseline (cuts only).
        gaussian_smooth_db(in_db_.data(), base_.data(), nb_, cut_sigma_);
        for (int b = 0; b < nb_; ++b) {
            const float excess = in_db_[b] - base_[b];
            float raw = 0.0f;
            if (excess > thresh_db_) {
                const float over = excess - thresh_db_;
                raw = -depth_ * (sharpness_ == 1.0f ? over : (float)std::pow((double)over, (double)sharpness_));
            }
            if (raw < cfg_.min_gain_db) raw = cfg_.min_gain_db;
            if (raw > 0.0f) raw = 0.0f;
            const float a = (raw < cut_db_[b]) ? a_att_[b] : a_rel_[b];
            cut_db_[b] = a * cut_db_[b] + (1.0f - a) * raw;
        }

        // Sum, smooth FIRST (so makeup sees the steady curve → low modulation),
        // then one broadband makeup so the stage is loudness-neutral.
        for (int b = 0; b < nb_; ++b) sm_[b] += cut_db_[b];
        for (int b = 0; b < nb_; ++b)
            out_db_[b] = (float)(alpha_ * out_db_[b] + (1.0 - alpha_) * sm_[b]);
        const double c = makeup_db(out_db_.data(), in_db_.data(), nb_);
        for (int b = 0; b < nb; ++b) {
            if (b >= nb_) { per_band_db[b] = 0.0f; continue; }
            const float g = (float)(out_db_[b] + c);
            per_band_db[b] = std::min(std::max(g, cfg_.min_gain_db), cfg_.max_gain_db);
        }
    }

    // Per-band [0,1] sigmoid values — the SpectralMaskEq / IirFilterbankEq
    // set_params contract. ``depth`` ∈ [0,1] scales the correction (param hook).
    void target_bands(float* bands01, int nb, float depth01 = 1.0f) {
        if ((int)db_scratch_.size() < nb) db_scratch_.assign(nb, 0.0f);
        target_db(db_scratch_.data(), nb);
        const double span = cfg_.max_gain_db - cfg_.min_gain_db;
        const float d = std::min(std::max(depth01, 0.0f), 1.0f);
        for (int b = 0; b < nb; ++b) {
            const double g = (db_scratch_[b] * d - cfg_.min_gain_db) / span;
            bands01[b] = (float)std::min(std::max(g, 0.0), 1.0);
        }
    }

    int latency_samples() const { return 0; }   // analysis-only; renderer applies the curve

    // Select the empirical target curve by index (0 = full_mix; see
    // adaptive_eq_targets.hpp kTargetCurves) or by class name. Unknown → full_mix.
    void set_target_curve(int idx) {
        target_idx_ = (idx >= 0 && idx < adaptive_eq_detail::kNumTargetCurves) ? idx : 0;
    }
    void set_target_curve(const char* class_name) {
        const int i = adaptive_eq_detail::target_curve_index(class_name);
        target_idx_ = i >= 0 ? i : 0;
    }
    int  target_curve() const { return target_idx_; }

private:
    float block_alpha_(float tau_ms) const {
        if (tau_ms <= 0.0f) return 0.0f;
        const double bpt = (double)cfg_.sample_rate * (tau_ms * 1e-3) / std::max(1, cfg_.block_size);
        return (float)std::exp(-1.0 / std::max(1e-3, bpt));
    }

    SpectralMaskEqParams cfg_{};
    int nb_ = 0;
    adaptive_eq_detail::RunningMelSpectrum spec_;
    std::vector<double> centers_;
    std::vector<float> out_db_, in_db_, raw_, sm_, base_, cut_db_, a_att_, a_rel_, db_scratch_;
    double alpha_ = 0.0;
    int target_idx_ = 0;   // empirical target curve (0 = full_mix)
    // Tunables (validated defaults from harness_eq_control C1C2_Cascade).
    double match_sigma_ = 3.0, cut_sigma_ = 4.0, tau_s_ = 0.40;
    double gate_db_ = 45.0;
    float depth_ = 0.7f, thresh_db_ = 4.0f, sharpness_ = 1.3f, attack_ms_ = 50.0f, release_ms_ = 180.0f;
};

}  // namespace nablafx
