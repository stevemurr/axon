// Transparent, mastering-grade STEREO WIDENER — a frequency-dependent Mid/Side
// "shuffler" (Blumlein/Gerzon). It decodes to Mid/Side, applies a FREQUENCY-
// DEPENDENT gain to the SIDE only (more width in the mids/highs, none in the
// lows), then re-encodes.
//
// Why it is transparent / mono-compatible BY CONSTRUCTION:
//   M = 0.5(L+R),  S = 0.5(L-R).  We only ever scale the side; the mid is
//   untouched. Re-encoding L = M + S', R = M - S' gives a mono sum
//   L + R = 2·M regardless of what we do to the side — so EVERY width change
//   cancels perfectly in mono. Phase-coherent (gain applied to filtered side
//   components, not a delay/all-pass trick), zero latency (IIR), CPU-cheap.
//
// Algorithm (in place, stereo):
//   M     = 0.5(L+R)
//   S     = 0.5(L-R)
//   S_hi  = HPF(S, low_hz)        // 2nd-order Butterworth high-pass on the side
//   S_lo  = S - S_hi              // exact complement → width==1 stays identity
//   S_air = HPF(S, kAirHz)        // extra high-frequency width band (~6 kHz)
//   S'    = S_lo + width·S_hi + air·S_air
//   L = M + S',  R = M - S'
//
//   * At width==1, air==0  →  S' == S_lo + S_hi == S → BIT-IDENTICAL passthrough.
//   * Mono input (r == nullptr) → S == 0, nothing to widen → no-op.
//   * ONE set of side biquads (filters run on the single side signal).
//
// Header-only, pure DSP in namespace nablafx, no CLAP/ORT deps — unit-testable
// standalone (cf. bass_mono.hpp, exciter.hpp).
//
//   Widener w;
//   w.prepare(44100.0);
//   w.set_params(/*width*/1.4f, /*low_hz*/250.f, /*air*/0.2f);
//   w.process(L, R, n);     // in place, stereo
//   w.reset();
//
// Latency: zero — all paths are IIR with no compensating delay.

#pragma once
#include <cmath>

namespace nablafx {

class Widener {
public:
    static constexpr double kAirHz = 6000.0;   // fixed crossover for the "Air" band

    void prepare(double sample_rate) {
        sr_ = sample_rate > 0.0 ? sample_rate : 44100.0;
        reset();      // clear filter state AND invalidate cached cutoffs
        design_();    // build valid coeffs so the first process() is band-limited
        // After design_(), the cached cutoffs are valid (== designed values), so a
        // first set_params() with the same low_hz won't redundantly re-design, and
        // a different one will. reset() (below) re-invalidates if the host calls it.
    }

    // width  : side gain ABOVE low_hz. 1.0 = neutral/identity, 0 = mono, 2 = 2× side.
    // low_hz : width low crossover — width applies above this; below stays neutral.
    // air    : extra high-frequency side width above ~kAirHz. 0 = none.
    void set_params(float width, float low_hz, float air) {
        width_ = width < 0.f ? 0.f : (width > 4.f ? 4.f : width);
        air_   = air   < 0.f ? 0.f : (air   > 4.f ? 4.f : air);
        if (low_hz != low_hz_) { low_hz_ = low_hz; design_low_(); }
    }

    void reset() {
        hp_lo1_.clear(); hp_lo2_.clear();
        hp_air1_.clear(); hp_air2_.clear();
        // Invalidate the cached low crossover so the NEXT set_params() re-designs.
        // CLAP calls reset() AFTER activate/prepare; default-constructing Biquads
        // would otherwise leave the filters at passthrough until a knob moved
        // (the exciter-class bug). -1 = stale sentinel. The Air filter has a FIXED
        // cutoff, so it is re-designed unconditionally in design_() / prepare().
        low_hz_ = -1.f;
        design_air_();   // Air cutoff is fixed; keep its coeffs valid after reset.
    }

    // In place, stereo. At width==1 && air==0 the output is bit-identical to in.
    void process(float* l, float* r, int n) {
        if (!r) return;   // mono: side == 0, nothing to widen.
        // Neutral early-out → exact passthrough (and skips the filters entirely).
        if (width_ == 1.f && air_ == 0.f) return;
        const double w   = static_cast<double>(width_);
        const double air = static_cast<double>(air_);
        for (int i = 0; i < n; ++i) {
            const double m = 0.5 * (static_cast<double>(l[i]) + r[i]);
            const double s = 0.5 * (static_cast<double>(l[i]) - r[i]);
            const double s_hi  = hp_lo2_.process(hp_lo1_.process(s));   // side above low_hz
            const double s_lo  = s - s_hi;                              // exact complement
            const double s_air = hp_air2_.process(hp_air1_.process(s)); // side above ~6 kHz
            const double s_out = s_lo + w * s_hi + air * s_air;
            l[i] = static_cast<float>(m + s_out);
            r[i] = static_cast<float>(m - s_out);
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

    void design_() { design_low_(); design_air_(); }

    // 2nd-order Butterworth high-pass (RBJ cookbook), Q = 1/√2, two cascaded
    // sections (24 dB/oct LR4) for a clean split between the widened band and the
    // untouched lows. Helper writes into the two supplied biquads.
    void design_hpf_(double fc_req, double fallback, Biquad& s1, Biquad& s2) {
        const double fc = (fc_req > 1.0 && fc_req < 0.49 * sr_) ? fc_req : fallback;
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
        s1.set(b0, b1, b2, a1, a2);
        s2.set(b0, b1, b2, a1, a2);
    }

    void design_low_() { design_hpf_((double)low_hz_, 250.0, hp_lo1_, hp_lo2_); }
    void design_air_() { design_hpf_(kAirHz,          6000.0, hp_air1_, hp_air2_); }

    double sr_     = 44100.0;
    float  width_  = 1.f;
    float  air_    = 0.f;
    float  low_hz_ = -1.f;   // stale sentinel → first set_params() designs
    Biquad hp_lo1_{}, hp_lo2_{};     // side high-pass at low_hz (the widen band)
    Biquad hp_air1_{}, hp_air2_{};   // side high-pass at kAirHz (the air band)
};

}  // namespace nablafx
