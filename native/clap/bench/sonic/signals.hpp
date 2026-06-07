// Reference-signal generators for the sonic harness. Deterministic.
#pragma once
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace sonic {

inline std::vector<float> sine(double f, double amp, int n, double sr, double phase = 0.0) {
    std::vector<float> o(n);
    for (int i = 0; i < n; ++i) o[i] = (float)(amp * std::sin(2.0 * kPi * f * i / sr + phase));
    return o;
}

inline std::vector<float> twin_tone(double f1, double f2, double a1, double a2, int n, double sr) {
    std::vector<float> o(n);
    for (int i = 0; i < n; ++i)
        o[i] = (float)(a1 * std::sin(2.0 * kPi * f1 * i / sr) + a2 * std::sin(2.0 * kPi * f2 * i / sr));
    return o;
}

// White noise band-limited by a simple 1-pole LPF cascade (rough), seedable.
inline std::vector<float> pink_like(int n, double amp, uint32_t seed = 1) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-1.f, 1.f);
    std::vector<float> o(n);
    double b0 = 0, b1 = 0, b2 = 0;  // Paul Kellet pink filter (subset)
    for (int i = 0; i < n; ++i) {
        double w = d(rng);
        b0 = 0.99765 * b0 + w * 0.0990460;
        b1 = 0.96300 * b1 + w * 0.2965164;
        b2 = 0.57000 * b2 + w * 1.0526913;
        o[i] = (float)(amp * (b0 + b1 + b2 + w * 0.1848) * 0.2);
    }
    return o;
}

// Click train: unit impulses every `period` samples (transient probe).
inline std::vector<float> click_train(int n, int period, double amp = 0.7) {
    std::vector<float> o(n, 0.f);
    for (int i = 0; i < n; i += period) o[i] = (float)amp;
    return o;
}

}  // namespace sonic
