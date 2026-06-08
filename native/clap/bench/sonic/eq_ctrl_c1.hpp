// C1 — Deterministic target-spectrum match (Paradigm 1). STUB.
//
// To implement (verified recipe, Mockenhaupt/Nercessian DAFx 2024 + Ma/Reiss/Black):
//   diff[b]   = target_db(center[b]) - running_mel_db[b]
//   diff      = gaussian_smooth_db(diff, sigma≈3 bands)     // reject ripples
//   diff      = diff - mean(diff)                           // zero-mean: don't fight loudness
//   diff[b]   = clamp(diff[b], ±12 dB)
//   if |diff[b]| < tolerance_db(center[b]): diff[b] = 0     // deadband: no chasing/pumping
//   apply a slow per-band attack/release smoother on the emitted curve
// Zero added output latency (analysis-only). The recommended backbone.
#pragma once
#include "eq_ctrl.hpp"

namespace eqctrl {

class C1_TargetMatch : public IEqController {
public:
    const char* name() const override { return "C1_target_match"; }
    void reset(const EqCtrlCfg& cfg) override {
        cfg_ = cfg; nb_ = cfg.n_bands;
        spec_.reset(cfg);
        centers_ = mel_centers(cfg);
        out_db_.assign(nb_, 0.0f);
        in_db_.assign(nb_, 0.0f);
        raw_.assign(nb_, 0.0f);
        sm_.assign(nb_, 0.0f);
        // Emitted-curve time smoother: slow attack/release so target() can be
        // called every block without jumps. tau ~ 0.40 s at the block rate.
        const double blocks_per_tau =
            (double)cfg_.sample_rate * tau_s_ / std::max(1, cfg_.block_size);
        alpha_ = std::exp(-1.0 / std::max(1.0, blocks_per_tau));
    }
    void observe(const float* in, std::size_t n) override { spec_.observe(in, n); }
    void target(float* per_band_db, int nb) override {
        // No analysis yet → emit nothing (renderer stays flat).
        if (!spec_.primed()) { for (int b = 0; b < nb; ++b) per_band_db[b] = 0.0f; return; }

        // 1. running spectrum (flat-in ⇒ flat); 2. want − have.
        spec_.mel_db(in_db_.data());
        for (int b = 0; b < nb_; ++b)
            raw_[b] = (float)target_db(centers_[b]) - in_db_[b];

        // 2b. NOISE GATE: don't EQ near-silent bands. Boosting the floor of an
        // inactive band is the main steady-tone zipper + over-boost source, so
        // force its correction to exactly 0 before smoothing.
        for (int b = 0; b < nb_; ++b)
            if (!band_active(in_db_.data(), nb_, b)) raw_[b] = 0.0f;

        // 3. smooth out ripples (band-domain low-quefrency lifter).
        gaussian_smooth_db(raw_.data(), sm_.data(), nb_, sigma_);

        // 4. zero-mean: don't fight the downstream loudness stage.
        double mean = 0.0;
        for (int b = 0; b < nb_; ++b) mean += sm_[b];
        mean /= nb_;
        for (int b = 0; b < nb_; ++b) {
            float v = (float)(sm_[b] - mean);
            // 5. clamp to the renderer's gain span.
            v = std::min(std::max(v, cfg_.min_gain_db), cfg_.max_gain_db);
            // 6. SOFT deadband: continuous shrink-toward-0 instead of a hard
            // gate, so a band dithering across the tolerance edge no longer
            // toggles 0↔±tol (that toggle was the zipper).
            v = soft_deadband(v, (float)tolerance_db(centers_[b]));
            sm_[b] = v;   // target for this block (post clamp+soft-deadband)
        }
        // 7. slow per-band attack/release on the emitted curve.
        for (int b = 0; b < nb_; ++b)
            out_db_[b] = (float)(alpha_ * out_db_[b] + (1.0 - alpha_) * sm_[b]);

        // 8. ENERGY MAKEUP + publish. Add a single broadband trim so the
        // realized energy of out_db_ over the running spectrum is unchanged
        // (zero-mean-in-dB is NOT loudness-neutral). Clamp the result to the
        // renderer's gain span.
        const float mk = (float)makeup_db(out_db_.data(), in_db_.data(), nb_);
        for (int b = 0; b < nb; ++b)
            per_band_db[b] = std::min(std::max(out_db_[b] + mk,
                                               cfg_.min_gain_db), cfg_.max_gain_db);
    }

private:
    EqCtrlCfg cfg_{};
    int nb_ = 0;
    RunningMelSpectrum spec_;
    std::vector<double> centers_;
    std::vector<float>  out_db_;   // emitted, time-smoothed curve
    std::vector<float>  in_db_, raw_, sm_;  // scratch (no allocs in target())
    // Tunables.
    double sigma_ = 3.0;   // gaussian smoothing width, bands
    double tau_s_ = 0.40;  // emitted-curve attack/release time constant, s
    double alpha_ = 0.0;   // block-rate smoother coefficient
};

}  // namespace eqctrl
