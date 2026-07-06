// HTK mel scale — the one hz↔mel formula every mel consumer shares.
//
// The formula appears in the Auto-EQ controller, both Auto-EQ renderers and
// the MelLimiter (float variants). The double-precision band-CENTER
// computation below is the verbatim move of adaptive_eq_detail::mel_centers,
// now also used by IirFilterbankEq. Instantiated at float the templates emit
// the exact same float ops as the loops they replace, so renders stay
// byte-identical. (bench/ and tests/ keep their own copies on purpose — they
// are independent oracles.)

#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace nablafx {

template <typename T>
inline T hz_to_mel(T f) { return T(2595) * std::log10(T(1) + f / T(700)); }

template <typename T>
inline T mel_to_hz(T m) { return T(700) * (std::pow(T(10), m / T(2595)) - T(1)); }

// HTK-mel band centers (double): n_bands centers strictly inside (f_min,
// f_max), each clamped to nyq_clamp_hz. Matches SpectralMaskEq::build_mel_'s
// center points so the controller's bands line up with the renderers'.
inline std::vector<double> mel_band_centers_htk(int n_bands, double f_min,
                                                double f_max, double nyq_clamp_hz) {
    const double mmin = hz_to_mel(f_min);
    const double mmax = hz_to_mel(f_max);
    std::vector<double> out(n_bands);
    for (int b = 0; b < n_bands; ++b) {
        const double mel = mmin + (mmax - mmin) * (b + 1) / (n_bands + 1);
        out[b] = std::min(mel_to_hz(mel), nyq_clamp_hz);
    }
    return out;
}

}  // namespace nablafx
