#include "meter.hpp"

#include <algorithm>
#include <cmath>

#include "k_weighting.hpp"

namespace nablafx {

void LoudnessMeter::set_k_weighting_(double sr) {
    if (sr < 1.0) sr = 1.0;  // guard against div-by-zero / Inf coeffs in the warp fallback

    // BS.1770-4 constants + rate selection shared with LufsLeveler
    // (k_weighting.hpp) — exact tables for 44.1/48 kHz, warp otherwise.
    const KCoeffs c = k_weighting_coeffs(sr);
    pre_.b0 = c.pre_b0; pre_.b1 = c.pre_b1; pre_.b2 = c.pre_b2; pre_.a1 = c.pre_a1; pre_.a2 = c.pre_a2;
    rlb_.b0 = c.rlb_b0; rlb_.b1 = c.rlb_b1; rlb_.b2 = c.rlb_b2; rlb_.a1 = c.rlb_a1; rlb_.a2 = c.rlb_a2;
    pre_.clear();
    rlb_.clear();
}

void LoudnessMeter::reset(double sample_rate) {
    sr_ = sample_rate;
    set_k_weighting_(sample_rate);

    sub_len_  = static_cast<std::size_t>((kSubMs * sample_rate) / 1000.0);
    if (sub_len_ == 0) sub_len_ = 1;
    sub_fill_ = 0;
    sub_sum_  = 0.0;

    ring_n_ = kShortMs / kSubMs;            // 30
    ms_ring_.assign(ring_n_, 0.0);
    ring_idx_ = ring_filled_ = 0;
    ring_sum_ = 0.0;

    // RMS sliding window ≈ 300 ms in 100 ms sub-blocks.
    rms_sub_len_  = sub_len_;
    rms_sub_fill_ = 0;
    rms_sub_sum_  = 0.0;
    rms_n_ = 3;
    rms_ring_.assign(rms_n_, 0.0);
    rms_idx_ = rms_filled_ = 0;
    rms_sum_ = 0.0;

    peak_lin_ = 0.0;
    // Peak decay ≈ 11.6 dB/s (τ ≈ 0.375 s).
    peak_decay_per_sample_ = std::exp(-1.0 / (0.375 * sample_rate));

    lufs_s_ = lufs_m_ = -120.0;
    rms_db_ = -120.0;
}

void LoudnessMeter::process(const float* L, const float* R, int n_ch, int n) {
    const bool stereo = (n_ch >= 2) && (R != nullptr);

    for (int i = 0; i < n; ++i) {
        const double xl = static_cast<double>(L[i]);
        const double xr = stereo ? static_cast<double>(R[i]) : 0.0;

        // ---- LUFS: K-weighted channel-summed mean square ----
        double kl = pre_.step_l(xl); kl = rlb_.step_l(kl);
        double kw = kl * kl;
        if (stereo) { double kr = pre_.step_r(xr); kr = rlb_.step_r(kr); kw += kr * kr; }
        sub_sum_ += kw;

        // ---- Peak: decaying sample peak across channels ----
        double a = std::fabs(xl);
        if (stereo) a = std::max(a, std::fabs(xr));
        peak_lin_ *= peak_decay_per_sample_;
        if (a > peak_lin_) peak_lin_ = a;

        // ---- RMS: unweighted mean power of the channel average ----
        double m = stereo ? 0.5 * (xl + xr) : xl;
        rms_sub_sum_ += m * m;

        if (++sub_fill_ >= sub_len_) {
            const double ms = sub_sum_ / static_cast<double>(sub_fill_);
            ring_sum_ += ms - ms_ring_[ring_idx_];
            ms_ring_[ring_idx_] = ms;
            ring_idx_ = (ring_idx_ + 1) % ring_n_;
            if (ring_filled_ < ring_n_) ++ring_filled_;

            // Short-term: mean over all filled sub-blocks.
            lufs_s_ = lufs_from_ms(ring_sum_ / static_cast<double>(ring_filled_));

            // Momentary: mean over the most recent kMomBlocks sub-blocks.
            const std::size_t mblk = std::min<std::size_t>(kMomBlocks, ring_filled_);
            double msum = 0.0;
            for (std::size_t k = 0; k < mblk; ++k) {
                // ring_idx_ now points one past the newest entry.
                std::size_t idx = (ring_idx_ + ring_n_ - 1 - k) % ring_n_;
                msum += ms_ring_[idx];
            }
            lufs_m_ = lufs_from_ms(msum / static_cast<double>(mblk));

            sub_fill_ = 0;
            sub_sum_  = 0.0;
        }

        if (++rms_sub_fill_ >= rms_sub_len_) {
            const double ms = rms_sub_sum_ / static_cast<double>(rms_sub_fill_);
            rms_sum_ += ms - rms_ring_[rms_idx_];
            rms_ring_[rms_idx_] = ms;
            rms_idx_ = (rms_idx_ + 1) % rms_n_;
            if (rms_filled_ < rms_n_) ++rms_filled_;
            const double mean = rms_sum_ / static_cast<double>(rms_filled_);
            rms_db_ = (mean > 0.0) ? 10.0 * std::log10(mean) : -120.0;
            rms_sub_fill_ = 0;
            rms_sub_sum_  = 0.0;
        }
    }
}

LoudnessMeter::Readout LoudnessMeter::readout() const {
    Readout r;
    r.lufs_s  = static_cast<float>(lufs_s_);
    r.lufs_m  = static_cast<float>(lufs_m_);
    r.rms_db  = static_cast<float>(rms_db_);
    r.peak_db = (peak_lin_ > 1e-12)
              ? static_cast<float>(20.0 * std::log10(peak_lin_)) : -120.f;
    return r;
}

}  // namespace nablafx
