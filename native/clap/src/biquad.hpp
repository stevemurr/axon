// Shared biquad building blocks (transposed direct form II, double precision).
//
// The exact struct that was copy-pasted into bass_mono / widener / reverb /
// iir_filterbank_eq / ssl_channel_eq (and, with per-channel state, meter /
// lufs_leveler). Extracted verbatim so every module shares ONE definition:
// process() bodies are token-identical to the originals, so codegen and FP
// results are unchanged (renders stay byte-identical).
//
// Per-module extras deliberately stay AT THEIR SITES: SslBiquad's mag()
// (complex-based) and IirFilterbankEq's mag_db() (trig-based) are different
// magnitude evaluations and must not be merged.

#pragma once
#include <cmath>

namespace nablafx {

// Mono biquad. set() takes already-a0-normalized coefficients.
struct BiquadTDF2 {
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

// Stereo-state variant: ONE coefficient set, separate left/right state
// registers, so a shared filter can process a linked stereo pair without
// cross-talk (LoudnessMeter, LufsLeveler K-weighting).
struct BiquadTDF2Stereo {
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

// 2nd-order Butterworth high-pass (RBJ cookbook), Q = 1/√2 — the exact
// expression sequence shared by bass_mono / widener / reverb. `fc` must
// already be validated: each caller keeps its own fallback/clamp policy at
// the call site (they intentionally differ).
inline void rbj_butterworth_hpf(double fc, double sr, BiquadTDF2& out) {
    const double w0 = 2.0 * M_PI * fc / sr;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double Q  = 0.70710678;
    const double alpha = sw / (2.0 * Q);
    const double a0 = 1.0 + alpha;
    const double b0 = (1.0 + cw) / 2.0 / a0;
    const double b1 = -(1.0 + cw)     / a0;
    const double b2 = (1.0 + cw) / 2.0 / a0;
    const double a1 = -2.0 * cw       / a0;
    const double a2 = (1.0 - alpha)   / a0;
    out.set(b0, b1, b2, a1, a2);
}

}  // namespace nablafx
