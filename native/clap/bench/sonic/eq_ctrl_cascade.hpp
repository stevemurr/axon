// C1→C2 CASCADE — the indicated production architecture: deterministic tonal
// target-match (C1) with dynamic resonance suppression (C2) layered on top,
// then a single energy-domain makeup so the whole stage is loudness-neutral.
//
// One shared RunningMelSpectrum feeds both decisions per block; the two curves
// are SUMMED (resonance cuts ride on the tonal match), one broadband makeup_db()
// nets out the loudness change, and the summed result is time-smoothed (~0.4 s)
// into out_db_ before publishing.
//
// Architecture (per block, spec_.mel_db(in_db_) computed once):
//   MATCH (C1): raw[b]   = target_db(center[b]) - in_db[b]
//               sm       = gaussian_smooth_db(raw, σ=3); zero-mean(sm)
//               per band: !band_active → 0; else soft_deadband(sm, tolerance_db)
//   CUT  (C2):  base     = gaussian_smooth_db(in_db, σ=4)
//               excess   = in_db - base
//               cut      = excess>thresh ? -depth*(excess-thresh)^sharp : 0
//                          clamp(cut, [min_gain_db, 0])
//               freq-dependent attack/release smoothing of cut (per-band state)
//   SUM:        g[b]     = sm[b] + cut[b]
//   MAKEUP:     g[b]     = clamp(g[b] + makeup_db(g, in_db, nb), min..max)
//   SMOOTH:     out_db_[b] = a*out_db_[b] + (1-a)*g[b]   (tau ~0.40 s)
//   publish out_db_.
#pragma once
#include "eq_ctrl.hpp"

namespace eqctrl {

class C1C2_Cascade : public IEqController {
public:
    const char* name() const override { return "C1C2_cascade"; }
    void reset(const EqCtrlCfg& cfg) override {
        cfg_ = cfg; nb_ = cfg.n_bands;
        spec_.reset(cfg);
        centers_ = mel_centers(cfg);
        out_db_.assign(nb_, 0.0f);
        in_db_.assign(nb_, 0.0f);
        raw_.assign(nb_, 0.0f);
        sm_.assign(nb_, 0.0f);
        base_.assign(nb_, 0.0f);
        cut_db_.assign(nb_, 0.0f);

        // Emitted-curve time smoother (the summed result rides this one TC, so
        // the match half needs no separate smoother). tau ~0.40 s at block rate.
        const double blocks_per_tau =
            (double)cfg_.sample_rate * tau_s_ / std::max(1, cfg_.block_size);
        alpha_ = std::exp(-1.0 / std::max(1.0, blocks_per_tau));

        // Per-band attack/release poles for the C2 cut state (BLOCK rate). tau is
        // scaled down at HF (HF resonances flare/decay faster), hf_scale 1→0.45
        // log-spaced f_min→f_max. attack = toward MORE cut (fast), release slower.
        a_att_.assign(nb_, 0.0f);
        a_rel_.assign(nb_, 0.0f);
        const double lf = std::log2(std::max(20.0, (double)cfg_.f_min));
        const double hf = std::log2(std::max(lf + 1.0, (double)cfg_.f_max));
        for (int b = 0; b < nb_; ++b) {
            const double l = std::log2(std::max(20.0, centers_[b]));
            const double frac = std::min(std::max((l - lf) / (hf - lf), 0.0), 1.0);
            const double hf_scale = 1.0 - 0.55 * frac;          // 1 → 0.45
            a_att_[b] = block_alpha_(attack_ms_  * hf_scale);
            a_rel_[b] = block_alpha_(release_ms_ * hf_scale);
        }
    }
    void observe(const float* in, std::size_t n) override { spec_.observe(in, n); }
    void target(float* per_band_db, int nb) override {
        // No analysis yet → emit nothing (renderer stays flat).
        if (!spec_.primed()) { for (int b = 0; b < nb; ++b) per_band_db[b] = 0.0f; return; }

        // Shared running mel-dB spectrum (computed once for both halves).
        spec_.mel_db(in_db_.data());

        // ---- MATCH (C1): broad tonal target-match, zero-mean, gated, deadbanded.
        for (int b = 0; b < nb_; ++b)
            raw_[b] = (float)target_db(centers_[b]) - in_db_[b];
        gaussian_smooth_db(raw_.data(), sm_.data(), nb_, match_sigma_);
        double mean = 0.0;
        for (int b = 0; b < nb_; ++b) mean += sm_[b];
        mean /= nb_;
        for (int b = 0; b < nb_; ++b) {
            float v = (float)(sm_[b] - mean);
            // Don't chase near-silent bands (boosting the floor → zipper/over-boost).
            if (!band_active(in_db_.data(), nb_, b)) {
                sm_[b] = 0.0f;
            } else {
                // Continuous deadband: tonal-balance refs are ranges, not lines.
                sm_[b] = soft_deadband(v, (float)tolerance_db(centers_[b]));
            }
        }

        // ---- CUT (C2): dynamic resonance suppression over a strong local floor.
        gaussian_smooth_db(in_db_.data(), base_.data(), nb_, cut_sigma_);
        for (int b = 0; b < nb_; ++b) {
            const float excess = in_db_[b] - base_[b];   // how resonant now
            float raw = 0.0f;
            if (excess > thresh_db_) {
                const float over = excess - thresh_db_;
                const float shaped = (sharpness_ == 1.0f)
                                   ? over
                                   : (float)std::pow((double)over, (double)sharpness_);
                raw = -depth_ * shaped;                  // CUTS ONLY
            }
            // Clamp to cuts only, within the renderer's range.
            if (raw < cfg_.min_gain_db) raw = cfg_.min_gain_db;
            if (raw > 0.0f) raw = 0.0f;
            // Freq-dependent attack/release: toward MORE cut = attack (fast),
            // toward less cut = release (slow → no breathing/chirp).
            const float a = (raw < cut_db_[b]) ? a_att_[b] : a_rel_[b];
            cut_db_[b] = a * cut_db_[b] + (1.0f - a) * raw;
        }

        // ---- SUM: resonance cuts ride on the tonal match.
        for (int b = 0; b < nb_; ++b) sm_[b] = sm_[b] + cut_db_[b];

        // ---- SMOOTH FIRST: the slow per-band attack/release runs on the summed
        //      curve, so the makeup (next) is computed on the STEADY emitted curve
        //      and doesn't pump with the C2 cut's frame-to-frame jitter (this was
        //      the cascade's residual tonal-modulation source).
        for (int b = 0; b < nb_; ++b)
            out_db_[b] = (float)(alpha_ * out_db_[b] + (1.0 - alpha_) * sm_[b]);

        // ---- MAKEUP on the smoothed curve → loudness-neutral with low modulation.
        const double c = makeup_db(out_db_.data(), in_db_.data(), nb_);
        for (int b = 0; b < nb; ++b) {
            if (b >= nb_) { per_band_db[b] = 0.0f; continue; }
            const float g = (float)(out_db_[b] + c);
            per_band_db[b] = std::min(std::max(g, cfg_.min_gain_db), cfg_.max_gain_db);
        }
    }

private:
    // EWMA pole from a time constant (ms) at the block rate.
    float block_alpha_(float tau_ms) const {
        if (tau_ms <= 0.0f) return 0.0f;
        const double blocks_per_tau =
            (double)cfg_.sample_rate * (tau_ms * 1e-3) / std::max(1, cfg_.block_size);
        return (float)std::exp(-1.0 / std::max(1e-3, blocks_per_tau));
    }

    EqCtrlCfg cfg_{};
    int nb_ = 0;
    RunningMelSpectrum spec_;
    std::vector<double> centers_;
    std::vector<float>  out_db_;             // emitted, time-smoothed summed curve
    std::vector<float>  in_db_, raw_, sm_, base_;  // scratch (no allocs in target())
    std::vector<float>  cut_db_;             // C2 cut state (per-band, smoothed)
    std::vector<float>  a_att_, a_rel_;      // C2 per-band attack/release poles
    // Tunables.
    double match_sigma_ = 3.0;    // C1 gaussian smoothing width, bands
    double cut_sigma_   = 4.0;    // C2 local-baseline smoothing width, bands
    float  depth_       = 0.7f;   // C2 dB cut per dB excess above thresh
    float  thresh_db_   = 4.0f;   // C2 selectivity (raised: don't chew sustained tonal energy)
    float  sharpness_   = 1.3f;   // C2 exponent on (excess-thresh)
    float  attack_ms_   = 50.0f;  // C2 toward more cut (slower: less pure-tone modulation)
    float  release_ms_  = 180.0f; // C2 toward less cut (slow)
    double tau_s_       = 0.40f;  // emitted-curve attack/release time constant, s
    double alpha_       = 0.0;    // block-rate smoother coefficient
};

}  // namespace eqctrl
