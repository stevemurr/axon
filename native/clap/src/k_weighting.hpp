// BS.1770-4 K-weighting — the constants + rate selection shared by the
// production LoudnessMeter and the (test-only) LufsLeveler.
//
// Coefficients from ITU-R BS.1770-4 Annex 1, bilinear-transformed to 48 kHz
// (reference) and 44.1 kHz; values match the libebur128 reference
// implementation. Other rates fall back to a proportional z-plane warp from
// 48 kHz — not exact per the spec (which designs in continuous time) but
// acceptable: LUFS integrates over seconds, smoothing the error.
//
// ONLY the constants/selection live here. Each unit keeps its own biquad
// state/application and its own sub-block/ring integration machinery, so
// test_meter's LoudnessMeter-vs-LufsLeveler cross-check stays meaningful.

#pragma once
#include <cmath>

namespace nablafx {

// One K-weighting stage pair: pre-filter (high-shelf) + RLB (2nd-order HPF),
// each a0-normalized (b0, b1, b2, a1, a2).
struct KCoeffs {
    double pre_b0, pre_b1, pre_b2;
    double pre_a1, pre_a2;
    double rlb_b0, rlb_b1, rlb_b2;
    double rlb_a1, rlb_a2;
};

inline constexpr KCoeffs kKCoeffs48000{
    1.53512485958697,   -2.69169618940638,   1.19839281085285,
    -1.69065929318241,   0.73248077421585,
    1.0,                 -2.0,                1.0,
    -1.99004745483398,   0.99007225036621,
};

inline constexpr KCoeffs kKCoeffs44100{
    1.5308412300503478, -2.6509799000031379, 1.1690790340624427,
    -1.6636551132560902, 0.7125954280732254,
    1.0,                 -2.0,                1.0,
    -1.9891696736297957, 0.9891959257876969,
};

// Exact tables for 44.1 / 48 kHz; any other rate gets the proportional
// z-plane warp of the 48 kHz reference (pole/zero angles scaled by 48000/sr).
// Callers keep their own sample-rate guards (they intentionally differ).
inline KCoeffs k_weighting_coeffs(double sr) {
    if (std::abs(sr - 44100.0) < 0.5) return kKCoeffs44100;
    if (std::abs(sr - 48000.0) < 0.5) return kKCoeffs48000;
    const double s = 48000.0 / sr;
    KCoeffs c = kKCoeffs48000;      // b0 terms stay; the rest scale by s / s·s
    c.pre_b1 = kKCoeffs48000.pre_b1 * s;
    c.pre_b2 = kKCoeffs48000.pre_b2 * s * s;
    c.pre_a1 = kKCoeffs48000.pre_a1 * s;
    c.pre_a2 = kKCoeffs48000.pre_a2 * s * s;
    c.rlb_b1 = kKCoeffs48000.rlb_b1 * s;
    c.rlb_b2 = kKCoeffs48000.rlb_b2 * s * s;
    c.rlb_a1 = kKCoeffs48000.rlb_a1 * s;
    c.rlb_a2 = kKCoeffs48000.rlb_a2 * s * s;
    return c;
}

// BS.1770: L_k = -0.691 + 10·log10(mean-square).
inline double lufs_from_ms(double ms) {
    if (ms <= 0.0) return -120.0;
    return -0.691 + 10.0 * std::log10(ms);
}

}  // namespace nablafx
