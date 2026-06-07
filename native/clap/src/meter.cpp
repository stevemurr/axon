#include "meter.hpp"

#include <algorithm>
#include <cmath>

namespace nablafx {

namespace {

// BS.1770-4 K-weighting coefficients (same constants as LufsLeveler /
// libebur128). Exact for 44.1 and 48 kHz; otherwise a proportional z-plane
// warp from 48 kHz (good enough — LUFS integrates over seconds).
struct KCoeffs {
    double pb0, pb1, pb2, pa1, pa2;
    double rb0, rb1, rb2, ra1, ra2;
};
constexpr KCoeffs k48{
    1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585,
    1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621,
};
constexpr KCoeffs k44{
    1.5308412300503478, -2.6509799000031379, 1.1690790340624427, -1.6636551132560902, 0.7125954280732254,
    1.0, -2.0, 1.0, -1.9891696736297957, 0.9891959257876969,
};

inline double lufs_from_ms(double ms) {
    if (ms <= 0.0) return -120.0;
    return -0.691 + 10.0 * std::log10(ms);
}

}  // namespace

void LoudnessMeter::set_k_weighting_(double sr) {
    if (sr < 1.0) sr = 1.0;  // guard against div-by-zero / Inf coeffs in the warp path below

    const KCoeffs* c = nullptr;
    if (std::abs(sr - 44100.0) < 0.5)      c = &k44;
    else if (std::abs(sr - 48000.0) < 0.5) c = &k48;

    if (c) {
        pre_.b0 = c->pb0; pre_.b1 = c->pb1; pre_.b2 = c->pb2; pre_.a1 = c->pa1; pre_.a2 = c->pa2;
        rlb_.b0 = c->rb0; rlb_.b1 = c->rb1; rlb_.b2 = c->rb2; rlb_.a1 = c->ra1; rlb_.a2 = c->ra2;
    } else {
        // Proportional warp from 48 kHz.
        const double s = 48000.0 / sr;
        pre_.b0 = k48.pb0; pre_.b1 = k48.pb1 * s; pre_.b2 = k48.pb2 * s * s;
        pre_.a1 = k48.pa1 * s; pre_.a2 = k48.pa2 * s * s;
        rlb_.b0 = k48.rb0; rlb_.b1 = k48.rb1 * s; rlb_.b2 = k48.rb2 * s * s;
        rlb_.a1 = k48.ra1 * s; rlb_.a2 = k48.ra2 * s * s;
    }
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
