#include "mel_limiter.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace nablafx {

static constexpr float kPi = static_cast<float>(M_PI);

// ---------------------------------------------------------------------------
// Construction / init
// ---------------------------------------------------------------------------

MelLimiter::MelLimiter() {
    band_gain_.fill(1.f);
}

MelLimiter::~MelLimiter() {
    if (fft_setup_) vDSP_destroy_fftsetup(fft_setup_);
}

void MelLimiter::init(int sample_rate) {
    sr_    = sample_rate;
    log2n_ = static_cast<vDSP_Length>(std::lround(std::log2(kFFTSize)));

    if (fft_setup_) vDSP_destroy_fftsetup(fft_setup_);
    fft_setup_ = vDSP_create_fftsetup(log2n_, kFFTRadix2);
    if (!fft_setup_) throw std::runtime_error("MelLimiter: vDSP_create_fftsetup failed");

    n_freq_   = kFFTSize / 2 + 1;
    ola_scale_ = 1.f / (2.f * static_cast<float>(kFFTSize));

    // Pre-allocate per-bin gain and power-spectrum scratch so process() never
    // allocates.
    bin_gain_arr_.assign(n_freq_, 0.f);
    pwr_.assign(n_freq_, 0.f);

    // Hann window.
    window_.resize(kFFTSize);
    for (int n = 0; n < kFFTSize; ++n)
        window_[n] = 0.5f * (1.f - std::cos(2.f * kPi * n / kFFTSize));
    window_sq_.resize(kFFTSize);
    vDSP_vmul(window_.data(), 1, window_.data(), 1, window_sq_.data(), 1, kFFTSize);

    // Normalization so FFT-domain band energy maps to linear amplitude:
    // for a sine of amplitude A, total = A·sqrt(N·Σwindow²), so κ = 1/that.
    float wsq_sum = 0.f;
    for (int n = 0; n < kFFTSize; ++n) wsq_sum += window_sq_[n];
    level_scale_ = 1.f / std::sqrt(static_cast<float>(kFFTSize) * wsq_sum);

    // Per-channel state.
    const int ola_sz  = kFFTSize + kHopSize;
    const int dry_sz  = kLatency + kHopSize;
    for (auto& ch : ch_) {
        ch.in_ring.assign(kFFTSize, 0.f);
        ch.out_ring.assign(ola_sz, 0.f);
        ch.norm_ring.assign(ola_sz, 0.f);
        ch.dry_ring.assign(dry_sz, 0.f);
        ch.windowed.resize(kFFTSize, 0.f);
        ch.sp_re.resize(kFFTSize / 2, 0.f);
        ch.sp_im.resize(kFFTSize / 2, 0.f);
        ch.time_out.resize(kFFTSize, 0.f);
        ch.la_ring.assign(kBrickLA, 0.f);
    }

    // Brickwall ballistics: attack must complete within the lookahead so the
    // gain is already down when a peak reaches the output; release ~50 ms.
    brick_atk_ = std::exp(-1.f / (kBrickLA * 0.25f));
    brick_rel_ = std::exp(-1.f / (0.050f * sr_));

    build_mel_();
    reset();
}

void MelLimiter::reset() {
    for (auto& ch : ch_) {
        std::fill(ch.in_ring.begin(),    ch.in_ring.end(),    0.f);
        std::fill(ch.out_ring.begin(),   ch.out_ring.end(),   0.f);
        std::fill(ch.norm_ring.begin(),  ch.norm_ring.end(),  0.f);
        std::fill(ch.dry_ring.begin(),   ch.dry_ring.end(),   0.f);
        std::fill(ch.windowed.begin(),   ch.windowed.end(),   0.f);
        std::fill(ch.sp_re.begin(),      ch.sp_re.end(),      0.f);
        std::fill(ch.sp_im.begin(),      ch.sp_im.end(),      0.f);
        std::fill(ch.time_out.begin(),   ch.time_out.end(),   0.f);
        std::fill(ch.la_ring.begin(),    ch.la_ring.end(),    0.f);
        ch.in_fill = ch.samples_since = 0;
        ch.out_write = ch.out_read = ch.out_avail = 0;
        ch.dry_write = 0;
        ch.wet_z1 = 0.f;
    }
    band_gain_.fill(1.f);
    la_pos_     = 0;
    brick_gain_ = 1.f;
    dq_head_ = dq_tail_ = 0;
    brick_n_ = 0;
}

// ---------------------------------------------------------------------------
// Mel filterbank (HTK formula, triangular windows)
// ---------------------------------------------------------------------------

void MelLimiter::build_mel_() {
    constexpr float f_min = 20.f, f_max = 20000.f;
    const float mel_min = 2595.f * std::log10(1.f + f_min / 700.f);
    const float mel_max = 2595.f * std::log10(1.f + f_max / 700.f);

    std::vector<float> bin_pts(kNumBands + 2);
    for (int i = 0; i < kNumBands + 2; ++i) {
        float mel  = mel_min + (mel_max - mel_min) * i / (kNumBands + 1);
        float hz   = 700.f * (std::pow(10.f, mel / 2595.f) - 1.f);
        bin_pts[i] = std::clamp(hz * kFFTSize / static_cast<float>(sr_),
                                0.f, static_cast<float>(n_freq_ - 1));
    }

    band_to_bin_.assign(kNumBands * n_freq_, 0.f);
    for (int b = 0; b < kNumBands; ++b) {
        const float lo  = bin_pts[b];
        const float ctr = bin_pts[b + 1];
        const float hi  = bin_pts[b + 2];
        // Band centre in Hz (for the UI x-axis).
        band_center_hz_[b] = ctr * static_cast<float>(sr_) / kFFTSize;
        const float ls  = std::max(ctr - lo, 1e-6f);
        const float rs  = std::max(hi  - ctr, 1e-6f);
        for (int k = 0; k < n_freq_; ++k) {
            float f = static_cast<float>(k);
            float w = std::min((f - lo) / ls, (hi - f) / rs);
            band_to_bin_[b * n_freq_ + k] = std::max(0.f, w);
        }
    }

    bin_norm_.assign(n_freq_, 0.f);
    for (int b = 0; b < kNumBands; ++b)
        for (int k = 0; k < n_freq_; ++k)
            bin_norm_[k] += band_to_bin_[b * n_freq_ + k];

    // Sparse spans: first/last nonzero weight per band (triangles are
    // contiguous, so [start, start+len) covers every nonzero).
    for (int b = 0; b < kNumBands; ++b) {
        int lo = n_freq_, hi = -1;
        for (int k = 0; k < n_freq_; ++k)
            if (band_to_bin_[b * n_freq_ + k] > 0.f) { if (k < lo) lo = k; hi = k; }
        band_start_[b] = (hi >= lo) ? lo : 0;
        band_len_[b]   = (hi >= lo) ? (hi - lo + 1) : 0;
    }
    // Prenormalized weights and the uncovered-bin template (bins no triangle
    // reaches must pass through with gain 1, matching the dense path's
    // bin_norm_ ≤ 1e-6 branch).
    band_to_bin_nrm_.assign(kNumBands * n_freq_, 0.f);
    bin_gain_tmpl_.assign(n_freq_, 0.f);
    for (int k = 0; k < n_freq_; ++k) {
        if (bin_norm_[k] > 1e-6f) {
            for (int b = 0; b < kNumBands; ++b)
                band_to_bin_nrm_[b * n_freq_ + k] =
                    band_to_bin_[b * n_freq_ + k] / bin_norm_[k];
        } else {
            bin_gain_tmpl_[k] = 1.f;
        }
    }
}

// ---------------------------------------------------------------------------
// Per-band gain solver (water-filling + uniform blend)
// ---------------------------------------------------------------------------

void MelLimiter::solve_gains_(float* const* ch_sp_re,
                              float* const* ch_sp_im,
                              int n_ch,
                              const Params& p,
                              float* out_gains) const {
    // Compute per-band energy = max over channels (linked stereo).
    // Power spectrum once per channel, then one sparse dot product per band —
    // instead of re-deriving re²+im² for all 513 bins inside every band.
    std::array<float, kNumBands> band_level{};
    std::array<float, kNumBands> max_e_arr{};
    for (int c = 0; c < n_ch; ++c) {
        DSPSplitComplex sp{ch_sp_re[c], ch_sp_im[c]};
        vDSP_zvmags(&sp, 1, pwr_.data(), 1, kFFTSize / 2);
        pwr_[0]           = ch_sp_re[c][0] * ch_sp_re[c][0];  // DC
        pwr_[n_freq_ - 1] = ch_sp_im[c][0] * ch_sp_im[c][0];  // Nyquist (packed in im[0])
        for (int b = 0; b < kNumBands; ++b) {
            const int s = band_start_[b], n = band_len_[b];
            float e = 0.f;
            vDSP_dotpr(band_to_bin_.data() + b * n_freq_ + s, 1,
                       pwr_.data() + s, 1, &e, static_cast<vDSP_Length>(n));
            if (e > max_e_arr[b]) max_e_arr[b] = e;
        }
    }
    for (int b = 0; b < kNumBands; ++b) {
        band_level[b] = std::sqrt(max_e_arr[b]) * level_scale_;
        disp_level_[b] = band_level[b];   // display tap
    }

    // Total energy across bands.
    float sum_sq = 0.f;
    for (int n = 0; n < kNumBands; ++n) sum_sq += band_level[n] * band_level[n];
    const float total = std::sqrt(sum_sq);
    const float C     = p.ceiling_lin;

    if (total <= C + 1e-7f) {
        for (int n = 0; n < kNumBands; ++n) out_gains[n] = 1.f;
        return;
    }

    // Uniform solution: reduce all bands by the same ratio.
    const float g_uni = C / total;

    // Water-filling: find lambda such that
    //   sum_n min(1, lambda/L[n])^2 * L[n]^2 = C^2
    // Sort band levels ascending, scan for the cutoff k where bands 0..k stay
    // at g=1 and bands k+1..N-1 get g = lambda/L[n].
    std::array<float, kNumBands> sorted_L = band_level;
    std::sort(sorted_L.begin(), sorted_L.end());

    float lambda = g_uni; // fallback: uniform
    float accum  = 0.f;
    for (int k = 0; k < kNumBands - 1; ++k) {
        accum += sorted_L[k] * sorted_L[k];
        const float rem = C * C - accum;
        if (rem <= 0.f) { lambda = 0.f; break; }
        const float lam = std::sqrt(rem / (kNumBands - k - 1));
        if (lam >= sorted_L[k] && lam <= sorted_L[k + 1]) {
            lambda = lam;
            break;
        }
    }

    const float alpha = p.adaptive_gain;
    for (int n = 0; n < kNumBands; ++n) {
        const float g_wf = (band_level[n] > 1e-9f)
                         ? std::min(1.f, lambda / band_level[n])
                         : 1.f;
        out_gains[n] = std::clamp((1.f - alpha) * g_uni + alpha * g_wf, 0.f, 1.f);
    }
}

void MelLimiter::copy_display(float* levels_lin, float* gains_lin,
                              float* centers_hz) const {
    for (int b = 0; b < kNumBands; ++b) {
        levels_lin[b] = disp_level_[b];
        gains_lin[b]  = band_gain_[b];
        centers_hz[b] = band_center_hz_[b];
    }
}

// ---------------------------------------------------------------------------
// True-peak brickwall limiter (linked stereo, kBrickLA lookahead)
// ---------------------------------------------------------------------------
//
// The gain reacts to the loudest sample anywhere in the kBrickLA-sample
// lookahead window (a sliding-window maximum via a monotonic deque), so it has
// the full window to pre-duck before that peak reaches the output. A hard
// safety clip still guarantees |out| ≤ ceiling whenever the attack is
// deliberately slower than the window (Dynamic "loose" mode).
void MelLimiter::brickwall_(const float* in, float* out, int n_ch,
                            float ceiling) {
    // Magnitude of the newest (look-ahead) sample, linked across channels.
    float m = 0.f;
    for (int c = 0; c < n_ch; ++c) m = std::max(m, std::fabs(in[c]));

    // ── Sliding-window maximum over the last kBrickLA magnitudes ──
    const long long idx = brick_n_++;
    // Drop smaller values from the back (they can never be the max again).
    while (dq_head_ != dq_tail_) {
        const int back = (dq_tail_ - 1 + kDqCap) % kDqCap;
        if (dq_val_[back] <= m) dq_tail_ = back;
        else break;
    }
    dq_val_[dq_tail_] = m;
    dq_idx_[dq_tail_] = idx;
    dq_tail_ = (dq_tail_ + 1) % kDqCap;
    // Drop the front once it falls outside the window.
    while (dq_idx_[dq_head_] <= idx - kBrickLA)
        dq_head_ = (dq_head_ + 1) % kDqCap;

    const float wmax  = dq_val_[dq_head_];                 // loudest in window
    const float g_req = (wmax > ceiling && wmax > 1e-12f)
                      ? ceiling / wmax : 1.f;

    // Attack when ducking down, release when recovering.
    const float coef = (g_req < brick_gain_) ? brick_atk_ : brick_rel_;
    brick_gain_      = coef * brick_gain_ + (1.f - coef) * g_req;

    for (int c = 0; c < n_ch; ++c) {
        const float delayed = ch_[c].la_ring[la_pos_];
        ch_[c].la_ring[la_pos_] = in[c];
        float o = delayed * brick_gain_;
        out[c]  = std::clamp(o, -ceiling, ceiling);   // hard ceiling guarantee
    }
    la_pos_ = (la_pos_ + 1) % kBrickLA;
}

// ---------------------------------------------------------------------------
// Public process(): accumulate, hop, drain
// ---------------------------------------------------------------------------

void MelLimiter::process(float* l, float* r, int n_ch, int n_samples,
                         const Params& p) {
    float* ch_buf[2] = {l, r};
    const int dry_sz = static_cast<int>(ch_[0].dry_ring.size());

    // Ballistic time constants per hop (hop_ms ≈ 5.8 ms at 44100).
    const float hop_ms  = 1000.f * kHopSize / static_cast<float>(sr_);
    const float atk_ms  = 5.f;
    const float rel_ms  = 30.f + p.adaptive_speed * 370.f;
    const float atk_c   = std::exp(-hop_ms / atk_ms);
    const float rel_c   = std::exp(-hop_ms / rel_ms);

    // Brickwall ballistics. "Even" (toggle off): fixed tight attack + fast
    // 50 ms release. "Dynamic" (toggle on): the adaptive controls reshape it —
    //   adaptive_gain  → ATTACK CHARACTER: tight/clamped (fast, pre-ducks fully
    //                    within the lookahead → transparent) ↔ loose/punchy
    //                    (slow, lets transients leak to the safety clip → grittier).
    //   adaptive_speed → RELEASE: 50 → 400 ms (slow = breathing/pumping).
    // Attack is always bounded by the lookahead and the hard clip still
    // guarantees |out| ≤ ceiling, so "loose" trades transient clipping for punch.
    {
        const float rel_ms = p.adaptive_brickwall ? (50.f + p.adaptive_speed * 350.f) : 50.f;
        brick_rel_ = std::exp(-1.f / (rel_ms * 0.001f * static_cast<float>(sr_)));

        const float atk_samps = p.adaptive_brickwall
            ? kBrickLA * (0.15f + p.adaptive_gain * 1.05f)   // tight → loose
            : kBrickLA * 0.25f;                              // fixed tight
        brick_atk_ = std::exp(-1.f / atk_samps);
    }

    // Per-bin gain scratch — pre-allocated in init(); reused/cleared each hop.
    auto& bin_gain_arr = bin_gain_arr_;

    for (int i = 0; i < n_samples; ++i) {

        for (int c = 0; c < n_ch; ++c) {
            auto& ch = ch_[c];
            // Dry delay stores the raw (un-driven) input; the wet analysis path
            // is driven by p.drive_lin so the limiter is pushed into the ceiling.
            ch.dry_ring[ch.dry_write] = ch_buf[c][i];
            ch.in_ring[ch.in_fill]    = ch_buf[c][i] * p.drive_lin;
            ch.in_fill        = (ch.in_fill + 1)    % kFFTSize;
            ++ch.samples_since;
        }

        // Trigger hop based on channel 0 (both advance at the same rate).
        if (ch_[0].samples_since >= kHopSize) {
            for (int c = 0; c < n_ch; ++c) ch_[c].samples_since = 0;

            // ── Forward FFT for each channel so solve_gains_ can read spectra.
            for (int c = 0; c < n_ch; ++c) {
                auto& ch = ch_[c];
                const int tail = kFFTSize - ch.in_fill;
                vDSP_vmul(ch.in_ring.data() + ch.in_fill, 1, window_.data(),      1,
                          ch.windowed.data(),              1, static_cast<vDSP_Length>(tail));
                if (ch.in_fill > 0)
                    vDSP_vmul(ch.in_ring.data(), 1, window_.data() + tail, 1,
                              ch.windowed.data() + tail, 1, static_cast<vDSP_Length>(ch.in_fill));
                DSPSplitComplex sp{ch.sp_re.data(), ch.sp_im.data()};
                vDSP_ctoz(reinterpret_cast<DSPComplex*>(ch.windowed.data()), 2,
                          &sp, 1, kFFTSize / 2);
                vDSP_fft_zrip(fft_setup_, &sp, 1, log2n_, kFFTDirection_Forward);
            }

            // ── Solve per-band target gains.
            float* sp_re_ptrs[2] = {ch_[0].sp_re.data(), (n_ch > 1 ? ch_[1].sp_re.data() : nullptr)};
            float* sp_im_ptrs[2] = {ch_[0].sp_im.data(), (n_ch > 1 ? ch_[1].sp_im.data() : nullptr)};
            std::array<float, kNumBands> target_gains{};
            solve_gains_(sp_re_ptrs, sp_im_ptrs, n_ch, p, target_gains.data());

            // ── Time-smooth per-band gains.
            for (int n = 0; n < kNumBands; ++n) {
                const float tgt  = target_gains[n];
                const float prev = band_gain_[n];
                const float c    = (tgt < prev) ? atk_c : rel_c;
                band_gain_[n]    = c * prev + (1.f - c) * tgt;
            }

            // ── Map per-band gains to per-bin gains: seed uncovered bins from
            //    the template, then accumulate each band's prenormalized
            //    weights over its nonzero span only.
            std::copy(bin_gain_tmpl_.begin(), bin_gain_tmpl_.end(),
                      bin_gain_arr.begin());
            for (int b = 0; b < kNumBands; ++b) {
                const int s = band_start_[b], n = band_len_[b];
                vDSP_vsma(band_to_bin_nrm_.data() + b * n_freq_ + s, 1,
                          &band_gain_[b],
                          bin_gain_arr.data() + s, 1,
                          bin_gain_arr.data() + s, 1,
                          static_cast<vDSP_Length>(n));
            }

            // ── Apply gains + IFFT + OLA per channel, reusing the already-
            //    computed spectra (bin_gain_arr applied inside run_hop_
            //    directly to sp_re/sp_im, then IFFT).
            for (int c = 0; c < n_ch; ++c) {
                // run_hop_ expects FFT already done in ch.sp_re / ch.sp_im,
                // so skip the analysis step inside run_hop_ — but our current
                // run_hop_ always runs analysis. Use a small bridge: copy the
                // already-computed spectrum into ch.windowed so the analysis
                // inside run_hop_ produces the same data. Instead, just inline
                // the gain-apply + IFFT + OLA directly here.
                auto& ch = ch_[c];
                DSPSplitComplex sp{ch.sp_re.data(), ch.sp_im.data()};

                // Apply per-bin gain (DC and Nyquist are packed in element 0).
                sp.realp[0] *= bin_gain_arr[0];
                sp.imagp[0] *= bin_gain_arr[n_freq_ - 1];
                vDSP_vmul(sp.realp + 1, 1, bin_gain_arr.data() + 1, 1,
                          sp.realp + 1, 1, static_cast<vDSP_Length>(n_freq_ - 2));
                vDSP_vmul(sp.imagp + 1, 1, bin_gain_arr.data() + 1, 1,
                          sp.imagp + 1, 1, static_cast<vDSP_Length>(n_freq_ - 2));

                // Inverse FFT.
                vDSP_fft_zrip(fft_setup_, &sp, 1, log2n_, kFFTDirection_Inverse);
                vDSP_ztoc(&sp, 1,
                          reinterpret_cast<DSPComplex*>(ch.time_out.data()), 2, kFFTSize / 2);

                // Synthesis window + OLA.
                vDSP_vmul(ch.time_out.data(), 1, window_.data(), 1,
                          ch.windowed.data(), 1, kFFTSize);
                vDSP_vsmul(ch.windowed.data(), 1, &ola_scale_,
                           ch.windowed.data(), 1, kFFTSize);

                const int ola_sz = static_cast<int>(ch.out_ring.size());
                const int seg1   = std::min(kFFTSize, ola_sz - ch.out_write);
                const int seg2   = kFFTSize - seg1;
                vDSP_vadd(ch.windowed.data(),    1, ch.out_ring.data()  + ch.out_write, 1,
                          ch.out_ring.data()  + ch.out_write, 1, static_cast<vDSP_Length>(seg1));
                vDSP_vadd(window_sq_.data(),     1, ch.norm_ring.data() + ch.out_write, 1,
                          ch.norm_ring.data() + ch.out_write, 1, static_cast<vDSP_Length>(seg1));
                if (seg2 > 0) {
                    vDSP_vadd(ch.windowed.data()  + seg1, 1, ch.out_ring.data(),  1,
                              ch.out_ring.data(),  1, static_cast<vDSP_Length>(seg2));
                    vDSP_vadd(window_sq_.data() + seg1,   1, ch.norm_ring.data(), 1,
                              ch.norm_ring.data(), 1, static_cast<vDSP_Length>(seg2));
                }
                ch.out_write = (ch.out_write + kHopSize) % ola_sz;
                ch.out_avail += kHopSize;
            }

        }

        // ── Gather the aligned wet (spectrally-limited, driven) sample per ch.
        float wet_s[2] = {0.f, 0.f};
        for (int c = 0; c < n_ch; ++c) {
            auto& ch = ch_[c];

            float wet = 0.f;
            if (ch.out_avail > 0) {
                const int   rd   = ch.out_read;
                const float norm = ch.norm_ring[rd];
                wet              = (norm > 1e-8f) ? ch.out_ring[rd] / norm : 0.f;
                ch.out_ring[rd]  = 0.f;
                ch.norm_ring[rd] = 0.f;
                ch.out_read      = (rd + 1) % static_cast<int>(ch.out_ring.size());
                --ch.out_avail;
            }

            // Delay wet by one sample so the STFT path's group delay is exactly
            // kFFTSize (the brickwall below adds the remaining kBrickLA).
            const float wet_aligned = ch.wet_z1;
            ch.wet_z1               = wet;
            wet_s[c]                = wet_aligned;
        }

        // ── True-peak brickwall: caps |wet| ≤ ceiling, adds kBrickLA latency.
        float wet_out[2] = {0.f, 0.f};
        brickwall_(wet_s, wet_out, n_ch, p.ceiling_lin);

        // ── Output sample per channel: wet/dry blend.
        for (int c = 0; c < n_ch; ++c) {
            auto& ch = ch_[c];

            // Dry sample delayed kLatency samples (kFFTSize + kBrickLA).
            const int dry_read = (ch.dry_write - kLatency + dry_sz) % dry_sz;
            const float dry    = ch.dry_ring[dry_read];
            ch.dry_write       = (ch.dry_write + 1) % dry_sz;

            ch_buf[c][i] = (1.f - p.wet_mix) * dry + p.wet_mix * wet_out[c];
        }
    }
}

}  // namespace nablafx
