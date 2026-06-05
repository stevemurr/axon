// Loudness / level meter for the in & out streams.
//
// Computes BS.1770-4 short-term (3 s) and momentary (400 ms) LUFS, a windowed
// RMS (dBFS) and a decaying sample peak (dBFS). Pure DSP — no CLAP / ORT deps
// so it can be unit-tested standalone and cross-checked against LufsLeveler
// (which uses the same K-weighting constants).
//
// Real-time safe: reset() allocates the ring; process() does no allocation.
//
//   LoudnessMeter m;
//   m.reset(44100.0);
//   m.process(L, R, n_ch, n);     // call once per block on the audio thread
//   auto r = m.readout();         // r.lufs_s, r.lufs_m, r.rms_db, r.peak_db

#pragma once

#include <cstddef>
#include <vector>

namespace nablafx {

class LoudnessMeter {
public:
    struct Readout {
        float lufs_s  = -120.f;  // short-term (3 s) LUFS
        float lufs_m  = -120.f;  // momentary (400 ms) LUFS
        float rms_db  = -120.f;  // windowed RMS, dBFS
        float peak_db = -120.f;  // decaying sample peak, dBFS
    };

    void reset(double sample_rate);

    // Process a block (out may be the same buffers as in — read only).
    // R may be nullptr when n_ch == 1.
    void process(const float* L, const float* R, int n_ch, int n);

    Readout readout() const;

private:
    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1l = 0.0, z2l = 0.0, z1r = 0.0, z2r = 0.0;
        double step_l(double x) {
            double y = b0 * x + z1l;
            z1l = b1 * x - a1 * y + z2l;
            z2l = b2 * x - a2 * y;
            return y;
        }
        double step_r(double x) {
            double y = b0 * x + z1r;
            z1r = b1 * x - a1 * y + z2r;
            z2r = b2 * x - a2 * y;
            return y;
        }
        void clear() { z1l = z2l = z1r = z2r = 0.0; }
    };

    void set_k_weighting_(double sr);

    double sr_ = 0.0;

    // K-weighting (two biquads, per-channel state).
    Biquad pre_{}, rlb_{};

    // 100 ms sub-block accumulation of K-weighted mean-square.
    std::size_t sub_len_   = 0;   // samples per 100 ms sub-block
    std::size_t sub_fill_  = 0;
    double      sub_sum_   = 0.0;  // Σ K-weighted (L²+R²) over the sub-block

    // Ring of sub-block mean-square values; short-term = mean of all filled,
    // momentary = mean of the most recent kMomBlocks.
    static constexpr std::size_t kSubMs      = 100;
    static constexpr std::size_t kShortMs    = 3000;  // 30 sub-blocks
    static constexpr std::size_t kMomBlocks  = 4;     // 400 ms
    std::vector<double> ms_ring_;
    std::size_t ring_n_      = 0;  // ring capacity (short-term blocks)
    std::size_t ring_idx_    = 0;
    std::size_t ring_filled_ = 0;
    double      ring_sum_    = 0.0;

    // Unweighted RMS sliding window (mean-square of per-sample channel mean).
    std::vector<double> rms_ring_;
    std::size_t rms_n_      = 0;
    std::size_t rms_idx_    = 0;
    std::size_t rms_filled_ = 0;
    double      rms_sum_    = 0.0;
    std::size_t rms_sub_len_  = 0;
    std::size_t rms_sub_fill_ = 0;
    double      rms_sub_sum_   = 0.0;

    // Peak with decay.
    double peak_lin_  = 0.0;
    double peak_decay_per_sample_ = 1.0;

    // Published values (updated on sub-block boundaries / each block).
    double lufs_s_  = -120.0;
    double lufs_m_  = -120.0;
    double rms_db_  = -120.0;
};

}  // namespace nablafx
