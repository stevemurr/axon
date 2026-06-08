// C3 — Perceptual-weighted target match (Paradigm 2, Gullfoss/Moore-Glasberg).
//
// Same backbone as C1 (target match → smooth → zero-mean → clamp → deadband →
// time-smooth) but the per-band error is weighted by a PERCEPTUAL sensitivity
// w_[b], so the solver spends its correction where a dB error costs the most
// perceived loudness instead of treating every band's dB equally.
//
// w_[b] derivation (Glasberg & Moore 1990 excitation / specific loudness):
//   1. Frequency sensitivity s(f): the ear is most sensitive in ~1–5 kHz and
//      rolls off at the extremes. We model it with a smooth Cam-domain window
//      centred at the 60-phon equal-loudness minimum (~3.3 kHz), width set on the
//      ERB-rate (Cam) scale so it is perceptually uniform. s≈1 mid, →~0.3 edges.
//   2. Compressive specific-loudness nonlinearity: perceived loudness is
//      N' ∝ E^ALPHA with ALPHA≈0.25 (Moore's exponent), so the perceptual COST of
//      a small dB error scales with the local specific-loudness gain. We do NOT
//      read loudness straight off excitation (refuted); instead we use the
//      compressive exponent to shape how strongly sensitivity translates into
//      correction weight:  w_raw = s(f)^(1-ALPHA)  — compression flattens the
//      window so the extremes are de-emphasised but not zeroed.
//   3. Normalise so the mid-band (peak-sensitivity) weight == 1.
// w_[b] is the tuning surface; Gullfoss internals are an unverified reconstruction.
// Analysis-only → zero added output latency.
#pragma once
#include "eq_ctrl.hpp"

namespace eqctrl {

class C3_Perceptual : public IEqController {
public:
    const char* name() const override { return "C3_perceptual"; }
    void reset(const EqCtrlCfg& cfg) override {
        cfg_ = cfg; nb_ = cfg.n_bands;
        spec_.reset(cfg);
        centers_ = mel_centers(cfg);
        out_db_.assign(nb_, 0.0f);
        scratch_.assign(nb_, 0.0f);
        in_db_.assign(nb_, 0.0f);
        spec_db_.assign(nb_, -120.0f);

        // --- perceptual weight per band ------------------------------------
        // Cam-domain Gaussian sensitivity window centred at the equal-loudness
        // minimum, half-width in Cam (ERB-rate units → perceptually uniform).
        const double cam_peak = hz_to_cam(F_PEAK_HZ);
        w_.assign(nb_, 1.0f);
        float wmax = 1e-9f;
        for (int b = 0; b < nb_; ++b) {
            const double cam = hz_to_cam(centers_[b]);
            const double d   = (cam - cam_peak) / CAM_SIGMA;
            const double s   = std::exp(-0.5 * d * d);          // s∈(0,1], 1 at peak
            // compressive specific-loudness shaping: s^(1-ALPHA) flattens the
            // window (de-emphasise extremes, never zero them).
            const double wr  = std::pow(s, 1.0 - ALPHA);
            // floor so the extremes still get *some* correction.
            w_[b] = (float)std::max(wr, W_FLOOR);
            wmax = std::max(wmax, w_[b]);
        }
        // normalise so the mid-band (peak-sensitivity) weight == 1.
        for (int b = 0; b < nb_; ++b) w_[b] /= wmax;

        // --- time smoother alpha at block rate -----------------------------
        const double bpt = (double)cfg_.sample_rate * SMOOTH_TAU_S
                         / std::max(1, cfg_.block_size);
        alpha_ = (float)std::exp(-1.0 / std::max(1.0, bpt));
    }
    void observe(const float* in, std::size_t n) override { spec_.observe(in, n); }
    void target(float* per_band_db, int nb) override {
        if (!spec_.primed()) {                         // no data yet → flat
            for (int b = 0; b < nb; ++b) per_band_db[b] = 0.0f;
            return;
        }
        // running spectrum (the "have"); kept in spec_db_ so it survives the
        // gaussian_smooth_db scratch reuse and feeds makeup_db at the end.
        spec_.mel_db(spec_db_.data());

        // perceptually-weighted raw error toward the tonal-balance target. The
        // perceptual weight w_[b] is the whole point of C3 — keep it.
        for (int b = 0; b < nb_; ++b)
            scratch_[b] = (float)((double)target_db(centers_[b]) - spec_db_[b]) * w_[b];

        // reject ripples (band-domain low-quefrency lifter).
        gaussian_smooth_db(scratch_.data(), in_db_.data(), nb_, SMOOTH_SIGMA);

        // zero-mean: weighted correction must not fight broadband loudness.
        double mean = 0.0;
        for (int b = 0; b < nb_; ++b) mean += in_db_[b];
        mean /= std::max(1, nb_);

        for (int b = 0; b < nb_; ++b) {
            double d = in_db_[b] - mean;
            // (1) NOISE GATE: near-silent bands carry no program → don't EQ them
            // (boosting the floor is the main source of zipper + over-boost).
            if (!band_active(spec_db_.data(), nb_, b)) d = 0.0;
            // clamp to renderer range.
            d = std::min(std::max(d, (double)cfg_.min_gain_db), (double)cfg_.max_gain_db);
            // (2) SOFT DEADBAND: shrink toward 0 by the tolerance continuously
            // instead of a hard gate, so a band dithering across the edge makes a
            // small continuous change rather than a 0↔±tol jump (kills zipper).
            d = soft_deadband((float)d, (float)tolerance_db(centers_[b]));
            // (4) SLOWER TC (SMOOTH_TAU_S ~0.40 s) → emitted curve never jumps.
            out_db_[b] = alpha_ * out_db_[b] + (1.0f - alpha_) * (float)d;
        }

        // (3) ENERGY MAKEUP: a perceptually-weighted, zero-mean-in-dB curve is
        // NOT loudness-neutral — concentrating boosts into the sensitive mid
        // bands raised realized energy (+14 dB blowup). Compute the broadband
        // trim that nets realized energy back to the input in the ENERGY domain
        // (using our own running spectrum, no output measurement) and fold it
        // into every published band, clamped to the renderer span.
        const double mk = makeup_db(out_db_.data(), spec_db_.data(), nb_);
        const int m = std::min(nb, nb_);
        for (int b = 0; b < m; ++b)
            per_band_db[b] = (float)std::min(std::max(out_db_[b] + mk,
                                 (double)cfg_.min_gain_db), (double)cfg_.max_gain_db);
        for (int b = m; b < nb; ++b) per_band_db[b] = 0.0f;
    }

private:
    // --- tunables (the perceptual weight is the surface to study) -----------
    static constexpr double F_PEAK_HZ   = 3300.0; // equal-loudness sensitivity peak
    static constexpr double CAM_SIGMA   = 8.0;    // window half-width (Cam units)
    static constexpr double ALPHA       = 0.25;   // Moore specific-loudness exponent
    static constexpr double W_FLOOR     = 0.30;   // min weight at the extremes
    static constexpr double SMOOTH_SIGMA= 3.0;    // gaussian_smooth_db sigma (bands)
    static constexpr double SMOOTH_TAU_S= 0.40;   // emitted-curve time constant (s) — slower TC kills zipper

    EqCtrlCfg cfg_{};
    int nb_ = 0;
    float alpha_ = 0.0f;
    RunningMelSpectrum spec_;
    std::vector<double> centers_;
    std::vector<float>  w_;        // perceptual per-band weight (mid == 1)
    std::vector<float>  out_db_;   // time-smoothed emitted curve
    std::vector<float>  in_db_;    // scratch: smoothed weighted error
    std::vector<float>  scratch_;  // scratch: weighted raw error
    std::vector<float>  spec_db_;  // running mel-dB spectrum (for gate + makeup)
};

}  // namespace eqctrl
