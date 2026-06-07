// Bass mono-maker: collapses the stereo image to mono below a cutoff frequency
// while leaving everything above it untouched. A staple mastering move — keeps
// low end tight/centred (vinyl-safe, translates on club systems) without
// narrowing the mids/highs.
//
// Method: mid/side decompose, high-pass the SIDE channel (so only above-cutoff
// side content survives), recombine. Below the cutoff the side → 0, so L ≈ R.
// The MID is never touched, so the mono sum (L+R) is preserved exactly at all
// frequencies — the process only removes low-frequency *width*.
//
// Filter: 4th-order Linkwitz-Riley high-pass on the side (two cascaded 2nd-order
// Butterworth biquads, 24 dB/oct). IIR — negligible latency.
//
//   BassMono bm;
//   bm.prepare(44100.0);
//   bm.set_cutoff(250.f);
//   bm.process(L, R, n);     // in place, stereo

#pragma once
#include <cmath>

namespace nablafx {

class BassMono {
public:
    void prepare(double sample_rate) {
        sr_ = sample_rate > 0.0 ? sample_rate : 44100.0;
        reset();
        design_();   // build coeffs for the current cutoff
    }

    // Recompute coefficients only when the cutoff actually changes.
    void set_cutoff(float hz) {
        if (hz == cutoff_) return;
        cutoff_ = hz;
        design_();
    }

    void reset() { hp1_.clear(); hp2_.clear(); }

    // In-place stereo. Below the cutoff the image collapses to mono; the mono
    // sum (L+R) is preserved exactly. No-op semantics if called with garbage.
    void process(float* l, float* r, int n) {
        for (int i = 0; i < n; ++i) {
            const double m = 0.5 * (static_cast<double>(l[i]) + r[i]);
            const double s = 0.5 * (static_cast<double>(l[i]) - r[i]);
            const double sh = hp2_.process(hp1_.process(s));   // side, HPF'd
            l[i] = static_cast<float>(m + sh);
            r[i] = static_cast<float>(m - sh);
        }
    }

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
        void clear() { z1 = z2 = 0; }
        void set(double nb0, double nb1, double nb2, double na1, double na2) {
            b0 = nb0; b1 = nb1; b2 = nb2; a1 = na1; a2 = na2;
        }
    };

    // 2nd-order Butterworth high-pass (RBJ cookbook), Q = 1/√2.
    void design_() {
        const double fc = (cutoff_ > 1.0 && cutoff_ < 0.49 * sr_) ? cutoff_ : 250.0;
        const double w0 = 2.0 * M_PI * fc / sr_;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double Q  = 0.70710678;
        const double alpha = sw / (2.0 * Q);
        const double a0 = 1.0 + alpha;
        const double b0 = (1.0 + cw) / 2.0 / a0;
        const double b1 = -(1.0 + cw)     / a0;
        const double b2 = (1.0 + cw) / 2.0 / a0;
        const double a1 = -2.0 * cw       / a0;
        const double a2 = (1.0 - alpha)   / a0;
        // LR4 = two identical Butterworth sections cascaded (24 dB/oct).
        hp1_.set(b0, b1, b2, a1, a2);
        hp2_.set(b0, b1, b2, a1, a2);
    }

    double sr_     = 44100.0;
    float  cutoff_ = 250.f;
    Biquad hp1_{}, hp2_{};
};

}  // namespace nablafx
