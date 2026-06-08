// C2 — Dynamic resonance suppression (Paradigm 3, soothe2 / DSEQ3 family).
//
// Verified recipe (oeksound soothe2 manual + JOS cepstral smoothing), here in
// the band domain:
//   baseline[b] = gaussian_smooth_db(running_mel_db, strong sigma)   // local spectral
//                 floor — the band-domain analogue of a low-quefrency cepstral lifter
//   excess[b]   = running_mel_db[b] - baseline[b]                    // "how resonant now"
//   raw_cut[b]  = 0                       if excess <= Selectivity   // deadband
//               = -Depth * shape(excess - Selectivity, Sharpness)    // CUTS ONLY
//   apply FREQUENCY-DEPENDENT attack/release smoothing on cut_db_ (faster at HF);
//   release slower than attack to avoid chirpiness / breathing.
// Designed to LAYER on top of C1 (suppress resonances after the broad tonal match),
// but here it runs standalone for A/B. Analysis-only → near-zero output latency.
#pragma once
#include "eq_ctrl.hpp"

namespace eqctrl {

class C2_ResonanceSuppress : public IEqController {
public:
    const char* name() const override { return "C2_resonance_suppress"; }

    // Tunables (soothe2-style). Sane defaults; the harness uses them as-is.
    float depth_      = 0.7f;    // amount: dB of cut per dB of excess above thresh
    float thresh_db_  = 3.0f;    // Selectivity: only peaks taller than this are cut
    float sharpness_  = 1.3f;    // exponent on (excess-thresh); >1 = more selective
    float attack_ms_  = 12.0f;   // toward MORE cut (fast clamps a flaring resonance)
    float release_ms_ = 180.0f;  // toward LESS cut (slow → no breathing/chirp)

    void reset(const EqCtrlCfg& cfg) override {
        cfg_ = cfg; nb_ = cfg.n_bands;
        spec_.reset(cfg);
        centers_ = mel_centers(cfg);
        cut_db_.assign(nb_, 0.0f);

        // Per-band attack/release alphas at the BLOCK rate. tau is scaled DOWN at
        // high frequency (HF resonances flare/decay faster than LF ones), so the
        // suppressor tracks them quicker up top. hf_scale ∈ [0.45,1]: 1 at f_min,
        // ~0.45 at f_max, log-spaced.
        a_att_.assign(nb_, 0.0f);
        a_rel_.assign(nb_, 0.0f);
        const double lf = std::log2(std::max(20.0, (double)cfg_.f_min));
        const double hf = std::log2(std::max(lf + 1.0, (double)cfg_.f_max));
        for (int b = 0; b < nb_; ++b) {
            const double l = std::log2(std::max(20.0, centers_[b]));
            const double frac = std::min(std::max((l - lf) / (hf - lf), 0.0), 1.0);
            const double hf_scale = 1.0 - 0.55 * frac;              // 1 → 0.45
            a_att_[b] = block_alpha_(attack_ms_  * hf_scale);
            a_rel_[b] = block_alpha_(release_ms_ * hf_scale);
        }
    }

    void observe(const float* in, std::size_t n) override { spec_.observe(in, n); }

    void target(float* per_band_db, int nb) override {
        if (!spec_.primed()) { for (int b = 0; b < nb; ++b) per_band_db[b] = 0.0f; return; }

        // 1. running mel-dB spectrum.
        spec_.mel_db(in_db_);
        // 2. strong local baseline (low-quefrency lifter analogue).
        gaussian_smooth_db(in_db_, base_, nb_, 4.0);

        for (int b = 0; b < nb_; ++b) {
            // 3. excess = how much this band sticks out of its local neighbourhood.
            const float excess = in_db_[b] - base_[b];
            // 4. gated, shaped, CUT-ONLY raw target.
            float raw = 0.0f;
            if (excess > thresh_db_) {
                const float over = excess - thresh_db_;
                const float shaped = (sharpness_ == 1.0f)
                                   ? over
                                   : (float)std::pow((double)over, (double)sharpness_);
                raw = -depth_ * shaped;
            }
            // 5. clamp to cuts only, within the renderer's range.
            if (raw < cfg_.min_gain_db) raw = cfg_.min_gain_db;
            if (raw > 0.0f) raw = 0.0f;
            // 6. freq-dependent attack/release: moving toward MORE cut (raw more
            //    negative than current) is attack (fast); toward less cut is release.
            const float a = (raw < cut_db_[b]) ? a_att_[b] : a_rel_[b];
            cut_db_[b] = a * cut_db_[b] + (1.0f - a) * raw;
        }
        // 7. emit (this candidate only cuts).
        for (int b = 0; b < nb && b < nb_; ++b) per_band_db[b] = cut_db_[b];
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
    std::vector<float>  cut_db_;            // per-band time-smoothed cut (state)
    std::vector<float>  a_att_, a_rel_;     // per-band attack/release poles
    float in_db_[256] = {0};                // scratch (nb_ ≤ 256), no alloc in target()
    float base_[256]  = {0};
};

}  // namespace eqctrl
