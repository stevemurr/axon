// Narrowband multiband exciter — clean harmonic generation with minimal
// intermodulation distortion (IMD).
//
// A single waveshaper applied to a WIDE band squares/cubes every pair of
// partials at once, producing difference tones across the whole band (broadband
// "noise"), which is especially audible in the highs. This stage instead splits
// the excite range into N CONTIGUOUS narrow bands and shapes each one
// independently, so the only IMD is *within* each narrow band — small
// differences that fall below the band and are removed by the wet high-pass.
// Measured on real program: a wide 3.5–16.5 kHz band gives in-band spectral
// flatness ~0.95 (near white noise); 5 narrow bands drop it to ~0.42 (cleaner
// than the source highs).
//
// Efficiency: the 4× oversampling (the expensive part) is done ONCE around the
// whole bank — up-FIR, then the N band-splits + shapers run in the oversampled
// domain, then one down-FIR — so the cost is ~one Exciter's worth of FIR plus a
// handful of cheap biquads, NOT N full oversampled exciters.
//
//   MultibandExciter mx;
//   mx.prepare(44100.0);
//   mx.configure(/*lo*/100, /*hi*/1000, /*bands*/5, /*character*/0.f, /*drive_db*/6.f);
//   mx.set_amount(0.4f);
//   mx.process(L, R, n, wet_tap);   // in place, stereo; wet_tap optional (mono)
//
// CLEAN polynomial shaper only (u² → pure 2nd, u³ → pure 3rd; no content above
// the 3rd → negligible aliasing at 4×). Parallel/loudness-gentle: dry passes at
// unity, delay-aligned to the wet's FIR group delay. Header-only, pure DSP.

#pragma once
#include <array>
#include <cmath>

namespace nablafx {

class MultibandExciter {
public:
    static constexpr int kOvs      = 4;
    static constexpr int kFirTaps  = 128;
    static constexpr int kFirPh    = kFirTaps / kOvs;                 // 32
    static constexpr int kDryDelay = (kFirTaps - 1 + kOvs / 2) / kOvs; // 32
    static constexpr int kMaxBands = 8;

    static_assert((kFirPh   & (kFirPh   - 1)) == 0, "kFirPh power of two");
    static_assert((kFirTaps & (kFirTaps - 1)) == 0, "kFirTaps power of two");
    static_assert((kDryDelay & (kDryDelay - 1)) == 0, "kDryDelay power of two");

    void prepare(double sample_rate) {
        sr_ = sample_rate > 0.0 ? sample_rate : 44100.0;
        build_fir_();
        reset();
        design_();
    }

    // lo/hi = excite range (Hz); n_bands = contiguous narrow bands across it;
    // character = 0 (pure even/2nd) .. 1 (pure odd/3rd); drive_db into the shaper.
    void configure(double lo_hz, double hi_hz, int n_bands,
                   float character, float drive_db) {
        lo_        = lo_hz;
        hi_        = hi_hz;
        nb_        = n_bands < 1 ? 1 : (n_bands > kMaxBands ? kMaxBands : n_bands);
        character_ = character < 0.f ? 0.f : (character > 1.f ? 1.f : character);
        drive_     = std::pow(10.0, drive_db / 20.0);
        design_();
    }

    void set_amount(float a) { amount_ = a < 0.f ? 0.f : (a > 1.f ? 1.f : a); }

    // Clear filter/delay STATE only — must NOT wipe the band coefficients
    // (a host reset() after activate would otherwise leave every band at
    // passthrough, since set_amount doesn't re-design and configure() runs only
    // once). Coefficients are (re)built by prepare()/configure() via design_().
    void reset() {
        for (auto& c : ch_) {
            c.up_hist.fill(0.0); c.up_idx = 0;
            c.dn_hist.fill(0.0); c.dn_idx = 0;
            c.dry_ring.fill(0.0); c.dry_idx = 0;
            for (auto& b : c.hpf) b.clear();
            for (auto& b : c.lpf) b.clear();
            c.wet_hpf.clear();
        }
    }

    // In place, stereo. amount == 0 → bit-identical bypass (and zero wet tap).
    void process(float* l, float* r, int n, float* wet_mono_out = nullptr) {
        if (amount_ <= 0.f) {
            if (wet_mono_out) for (int i = 0; i < n; ++i) wet_mono_out[i] = 0.f;
            return;
        }
        process_ch_(ch_[0], l, n, wet_mono_out);
        if (r) process_ch_(ch_[1], r, n, nullptr);
    }

    int latency_samples() const { return kDryDelay; }

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
        double process(double x) {
            double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void set(double nb0, double nb1, double nb2, double na1, double na2) {
            b0 = nb0; b1 = nb1; b2 = nb2; a1 = na1; a2 = na2;
        }
        void clear() { z1 = z2 = 0; }   // clear STATE only (keeps coefficients)
    };

    struct Channel {
        std::array<double, kFirPh>   up_hist{};
        int    up_idx = 0;
        std::array<double, kFirTaps> dn_hist{};
        int    dn_idx = 0;
        std::array<double, kDryDelay> dry_ring{};
        int    dry_idx = 0;
        // Per narrow band: HPF + LPF (contiguous band-pass), run at the
        // OVERSAMPLED rate (so the shaper that follows is anti-aliased).
        std::array<Biquad, kMaxBands> hpf{}, lpf{};
        Biquad wet_hpf{};   // strips sub-range junk from the summed wet (base rate)
    };

    // CLEAN monomial shaper: u² → pure 2nd, u³ → pure 3rd. Clamp bounds it; at a
    // musical drive on a narrow band it stays in the pure (un-clamped) region.
    double shape_(double bp) const {
        double u = drive_ * bp;
        if (u > 1.0) u = 1.0; else if (u < -1.0) u = -1.0;
        return (1.0 - character_) * (u * u) + character_ * (u * u * u);
    }

    void process_ch_(Channel& c, float* buf, int n, float* wet_mono_out) {
        for (int i = 0; i < n; ++i) {
            const double x = static_cast<double>(buf[i]);

            // Oversample the FULL input once (polyphase up-FIR).
            c.up_hist[c.up_idx] = x;
            c.up_idx = (c.up_idx + 1) & (kFirPh - 1);

            double wet = 0.0;
            for (int p = 0; p < kOvs; ++p) {
                double up = 0.0;
                for (int k = 0; k < kFirPh; ++k) {
                    const int tap   = p + k * kOvs;
                    const int h_idx = (c.up_idx + kFirPh - 1 - k) & (kFirPh - 1);
                    up += fir_[tap] * c.up_hist[h_idx];
                }
                // Split into narrow bands and shape EACH independently at the
                // oversampled rate, then sum — this is the IMD-killing step.
                double sh = 0.0;
                for (int b = 0; b < nb_; ++b) {
                    const double band = c.lpf[b].process(c.hpf[b].process(up));
                    sh += shape_(band);
                }

                c.dn_hist[c.dn_idx] = sh;
                c.dn_idx = (c.dn_idx + 1) & (kFirTaps - 1);

                if (p == kOvs - 1) {
                    double acc = 0.0;
                    for (int k = 0; k < kFirTaps; ++k) {
                        const int h_idx = (c.dn_idx + kFirTaps - 1 - k) & (kFirTaps - 1);
                        acc += fir_[k] * c.dn_hist[h_idx];
                    }
                    wet = acc / static_cast<double>(kOvs);
                }
            }

            const double wet_ac = c.wet_hpf.process(wet);
            const double dry_d  = c.dry_ring[c.dry_idx];
            c.dry_ring[c.dry_idx] = x;
            c.dry_idx = (c.dry_idx + 1) & (kDryDelay - 1);
            const double added = amount_ * wet_ac;
            buf[i] = static_cast<float>(dry_d + added);
            if (wet_mono_out) wet_mono_out[i] = static_cast<float>(added);
        }
    }

    // 2nd-order Butterworth HPF/LPF (RBJ, Q = 1/√2) at an arbitrary sample rate.
    static void hpf_coeffs(double fc, double sr, Biquad& b) {
        const double fcl = (fc > 1.0 && fc < 0.49 * sr) ? fc : 0.25 * sr;
        const double w0 = 2.0 * M_PI * fcl / sr, cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * 0.70710678), a0 = 1.0 + alpha;
        const double hb0 = (1.0 + cw) / 2.0 / a0, hb1 = -(1.0 + cw) / a0;
        const double ha1 = -2.0 * cw / a0, ha2 = (1.0 - alpha) / a0;
        b.set(hb0, hb1, hb0, ha1, ha2);
    }
    static void lpf_coeffs(double fc, double sr, Biquad& b) {
        const double fcl = (fc > 1.0 && fc < 0.49 * sr) ? fc : 0.49 * sr;
        const double w0 = 2.0 * M_PI * fcl / sr, cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * 0.70710678), a0 = 1.0 + alpha;
        b.set((1.0 - cw) / 2.0 / a0, (1.0 - cw) / a0, (1.0 - cw) / 2.0 / a0,
              -2.0 * cw / a0, (1.0 - alpha) / a0);
    }

    void design_() {
        const double os_sr = sr_ * kOvs;          // band-splits run oversampled
        for (int b = 0; b <= nb_; ++b) edge_[b] =
            lo_ * std::pow(hi_ / lo_, static_cast<double>(b) / nb_);
        for (auto& c : ch_) {
            // Each narrow band = one HPF (low edge) + one LPF (high edge),
            // 12 dB/oct — enough separation to keep cross-band IMD low.
            for (int b = 0; b < nb_; ++b) {
                hpf_coeffs(edge_[b],     os_sr, c.hpf[b]);
                lpf_coeffs(edge_[b + 1], os_sr, c.lpf[b]);
            }
            // Wet high-pass at the range's low edge, at the BASE rate (the wet is
            // downsampled before this) — removes demodulated sub-range junk.
            hpf_coeffs(lo_, sr_, c.wet_hpf);
        }
    }

    void build_fir_() {
        constexpr int N = kFirTaps;
        const double fc_norm = 0.5 / static_cast<double>(kOvs);
        const double center  = 0.5 * (N - 1);
        double sum = 0.0;
        for (int n = 0; n < N; ++n) {
            const double k = static_cast<double>(n) - center;
            const double s = (std::abs(k) < 1e-9)
                ? 2.0 * fc_norm
                : std::sin(2.0 * M_PI * fc_norm * k) / (M_PI * k);
            const double w = 0.5 * (1.0 - std::cos(2.0 * M_PI * n / (N - 1)));
            fir_[n] = s * w;
            sum += fir_[n];
        }
        const double scale = static_cast<double>(kOvs) / sum;
        for (double& v : fir_) v *= scale;
    }

    double sr_ = 44100.0;
    double lo_ = 100.0, hi_ = 1000.0;
    int    nb_ = 5;
    float  character_ = 0.f;
    double drive_ = std::pow(10.0, 6.0 / 20.0);
    float  amount_ = 0.f;

    std::array<double, kFirTaps>      fir_{};
    std::array<double, kMaxBands + 1> edge_{};
    std::array<Channel, 2>            ch_{};
};

}  // namespace nablafx
