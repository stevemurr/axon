// gui_math.hpp
// The functions behind the editor's graphs, kept next to the DSP they describe
// and free of any UI or platform code, so what is drawn can be (and is, see
// tests/test_gui_math.cpp) checked against what the audio actually does.
//
// Header-only, namespace nablafx::guimath.

#pragma once
#include <algorithm>
#include <cmath>
#include <complex>

#include "bass_mono.hpp"
#include "biquad.hpp"
#include "reverb.hpp"
#include "widener.hpp"

namespace nablafx {
namespace guimath {

// Complex frequency response of one biquad at `hz`.
inline std::complex<double> biquad_response(const BiquadTDF2& q, double hz, double sr) {
    const double w = 2.0 * M_PI * hz / sr;
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
    return (q.b0 + q.b1 * z1 + q.b2 * z2) / (1.0 + q.a1 * z1 + q.a2 * z2);
}
inline double to_db(double magnitude) { return 20.0 * std::log10(std::max(magnitude, 1e-9)); }

// The 4th-order Linkwitz-Riley high-pass both BassMono and Widener put on the
// side channel, designed exactly as they design it.
inline std::complex<double> lr4_highpass(double fc, double fallback, double hz, double sr) {
    const double f = (fc > 1.0 && fc < 0.49 * sr) ? fc : fallback;
    BiquadTDF2 q;
    rbj_butterworth_hpf(f, sr, q);
    const auto h = biquad_response(q, hz, sr);
    return h * h;
}

// BassMono: what the side channel is left with, in dB (the mid, and so the
// mono sum, is untouched at every frequency: that is a flat 0 dB).
inline double bass_mono_side_db(double hz, double cutoff_hz, double sr = 48000.0) {
    return to_db(std::abs(lr4_highpass(cutoff_hz, 250.0, hz, sr)));
}

// Widener: the gain on the side channel in dB. S' = S + (width - 1) * S_hi + air * S_air,
// so the gain is |1 + (width - 1) H_low + air H_air| with H the two high-passes.
inline double widener_side_db(double hz, double width, double low_hz, double air, double sr = 48000.0) {
    width = std::clamp(width, 0.0, 4.0);
    air = std::clamp(air, 0.0, 4.0);
    const auto lo = lr4_highpass(low_hz, 250.0, hz, sr);
    const auto hi = lr4_highpass(Widener::kAirHz, 6000.0, hz, sr);
    return to_db(std::abs(1.0 + (width - 1.0) * lo + air * hi));
}

}  // namespace guimath
}  // namespace nablafx
