#include "lufs_leveler.hpp"

#include <algorithm>
#include <cmath>

#include "k_weighting.hpp"

namespace nablafx {

namespace {

constexpr std::size_t kSubBlockMs = 100;

}  // namespace

void LufsLeveler::set_k_weighting_coeffs_(double sr) {
    // BS.1770-4 constants + rate selection shared with LoudnessMeter
    // (k_weighting.hpp) — exact tables for 44.1/48 kHz, proportional z-plane
    // warp from the 48 kHz reference otherwise.
    const KCoeffs c = k_weighting_coeffs(sr);
    pre_.b0 = c.pre_b0; pre_.b1 = c.pre_b1; pre_.b2 = c.pre_b2;
    pre_.a1 = c.pre_a1; pre_.a2 = c.pre_a2;
    rlb_.b0 = c.rlb_b0; rlb_.b1 = c.rlb_b1; rlb_.b2 = c.rlb_b2;
    rlb_.a1 = c.rlb_a1; rlb_.a2 = c.rlb_a2;
    pre_.clear();
    rlb_.clear();
}

void LufsLeveler::reset(double sample_rate, double target_lufs) {
    // Guard against invalid configuration: a non-positive sample rate would
    // cause division by zero in the coefficient math (warp + pole filters),
    // and a non-positive short-term window would produce a zero-length ring,
    // causing an undefined modulo-0 in process()/process_linked().
    if (sample_rate <= 0.0) sample_rate = 48000.0;
    cfg_.short_term_s = std::max(cfg_.short_term_s, 0.1);

    sample_rate_ = sample_rate;
    target_lufs_ = target_lufs;
    set_k_weighting_coeffs_(sample_rate);

    sub_block_samples_ = static_cast<std::size_t>((kSubBlockMs * sample_rate) / 1000.0);
    ring_blocks_       = static_cast<std::size_t>(
        std::ceil(cfg_.short_term_s * 1000.0 / kSubBlockMs));
    ms_ring_.assign(ring_blocks_, 0.0);

    ring_idx_        = 0;
    ring_filled_     = 0;
    sub_block_fill_  = 0;
    sub_block_sum_sq_ = 0.0;
    ring_sum_ms_     = 0.0;

    smooth_gain_lin_ = 1.0;
    target_gain_lin_ = 1.0;

    // One-pole smoothing coeffs. y[n] = a*y[n-1] + (1-a)*x[n];
    //   a = exp(-1 / (tau_s * fs))
    auto pole = [&](double ms) {
        double tau_s = std::max(ms, 1e-3) * 1e-3;
        return std::exp(-1.0 / (tau_s * sample_rate));
    };
    attack_coeff_  = pole(cfg_.attack_ms);
    release_coeff_ = pole(cfg_.release_ms);

    last_lufs_ = -120.0;
}

double LufsLeveler::current_gain_db() const {
    if (smooth_gain_lin_ <= 0.0) return cfg_.min_gain_db;
    return 20.0 * std::log10(smooth_gain_lin_);
}

void LufsLeveler::process(const float* in, float* out, std::size_t n) {
    const double tgt_lin_max = std::pow(10.0, cfg_.max_gain_db / 20.0);
    const double tgt_lin_min = std::pow(10.0, cfg_.min_gain_db / 20.0);
    const double silence_ms  = std::pow(10.0, cfg_.silence_floor_dbfs / 10.0);

    for (std::size_t i = 0; i < n; ++i) {
        double x = static_cast<double>(in[i]);

        // Measurement chain: K-weight a copy of the input sample.
        double km = pre_.step_l(x);
        km = rlb_.step_l(km);
        sub_block_sum_sq_ += km * km;
        ++sub_block_fill_;

        // Sub-block boundary: commit mean-square to the ring, update LUFS,
        // update target gain.
        if (sub_block_fill_ >= sub_block_samples_) {
            double ms = sub_block_sum_sq_ / static_cast<double>(sub_block_fill_);
            ring_sum_ms_ += ms - ms_ring_[ring_idx_];
            ms_ring_[ring_idx_] = ms;
            ring_idx_ = (ring_idx_ + 1) % ring_blocks_;
            if (ring_filled_ < ring_blocks_) ++ring_filled_;

            double window_ms = ring_sum_ms_ / static_cast<double>(ring_filled_);
            if (window_ms >= silence_ms) {
                last_lufs_ = lufs_from_ms(window_ms);
                double delta_db = target_lufs_ - last_lufs_;
                if (delta_db > cfg_.max_gain_db) delta_db = cfg_.max_gain_db;
                if (delta_db < cfg_.min_gain_db) delta_db = cfg_.min_gain_db;
                target_gain_lin_ = std::pow(10.0, delta_db / 20.0);
            }
            // In silence, leave target_gain_lin_ alone.

            sub_block_fill_  = 0;
            sub_block_sum_sq_ = 0.0;
        }

        // Sample-accurate one-pole ride toward the target gain.
        double coeff = (target_gain_lin_ > smooth_gain_lin_) ? attack_coeff_ : release_coeff_;
        smooth_gain_lin_ = coeff * smooth_gain_lin_ + (1.0 - coeff) * target_gain_lin_;
        // Safety clamp in case numerical drift pushes us out.
        if (smooth_gain_lin_ > tgt_lin_max) smooth_gain_lin_ = tgt_lin_max;
        if (smooth_gain_lin_ < tgt_lin_min) smooth_gain_lin_ = tgt_lin_min;

        out[i] = static_cast<float>(x * smooth_gain_lin_);
    }
}

void LufsLeveler::process_linked(const float* lin, const float* rin,
                                 float* lout, float* rout, std::size_t n) {
    const double tgt_lin_max = std::pow(10.0, cfg_.max_gain_db / 20.0);
    const double tgt_lin_min = std::pow(10.0, cfg_.min_gain_db / 20.0);
    const double silence_ms  = std::pow(10.0, cfg_.silence_floor_dbfs / 10.0);

    for (std::size_t i = 0; i < n; ++i) {
        double xl = static_cast<double>(lin[i]);
        double xr = static_cast<double>(rin[i]);

        // K-weight each channel, use equal-weighted sum for BS.1770 stereo.
        double kl = pre_.step_l(xl); kl = rlb_.step_l(kl);
        double kr = pre_.step_r(xr); kr = rlb_.step_r(kr);
        sub_block_sum_sq_ += (kl * kl) + (kr * kr);
        sub_block_fill_ += 1;

        if (sub_block_fill_ >= sub_block_samples_) {
            // Mean-square per *sample-pair* (matches BS.1770 channel-sum).
            double ms = sub_block_sum_sq_ / static_cast<double>(sub_block_fill_);
            ring_sum_ms_ += ms - ms_ring_[ring_idx_];
            ms_ring_[ring_idx_] = ms;
            ring_idx_ = (ring_idx_ + 1) % ring_blocks_;
            if (ring_filled_ < ring_blocks_) ++ring_filled_;

            double window_ms = ring_sum_ms_ / static_cast<double>(ring_filled_);
            if (window_ms >= silence_ms) {
                last_lufs_ = lufs_from_ms(window_ms);
                double delta_db = target_lufs_ - last_lufs_;
                if (delta_db > cfg_.max_gain_db) delta_db = cfg_.max_gain_db;
                if (delta_db < cfg_.min_gain_db) delta_db = cfg_.min_gain_db;
                target_gain_lin_ = std::pow(10.0, delta_db / 20.0);
            }

            sub_block_fill_  = 0;
            sub_block_sum_sq_ = 0.0;
        }

        double coeff = (target_gain_lin_ > smooth_gain_lin_) ? attack_coeff_ : release_coeff_;
        smooth_gain_lin_ = coeff * smooth_gain_lin_ + (1.0 - coeff) * target_gain_lin_;
        if (smooth_gain_lin_ > tgt_lin_max) smooth_gain_lin_ = tgt_lin_max;
        if (smooth_gain_lin_ < tgt_lin_min) smooth_gain_lin_ = tgt_lin_min;

        lout[i] = static_cast<float>(xl * smooth_gain_lin_);
        rout[i] = static_cast<float>(xr * smooth_gain_lin_);
    }
}

}  // namespace nablafx
