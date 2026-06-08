// Unit tests for AdaptiveEqController (the productionized C1→C2 cascade).
// Asserts the properties the harness A/B measured: stability, adaptivity (it
// does NOT mode-collapse — distinct material → distinct curves), correct tonal
// direction, determinism, and the depth=0 bypass. assert()-based (-UNDEBUG).
//
//   c++ -O2 -std=c++17 -I src tests/test_adaptive_eq.cpp -framework Accelerate -o test_adaptive_eq
#include "../src/adaptive_eq.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using nablafx::AdaptiveEqController;
using nablafx::SpectralMaskEqParams;

static SpectralMaskEqParams cfg() {
    SpectralMaskEqParams p;
    p.sample_rate = 44100; p.block_size = 128; p.num_control_params = 24;
    p.n_fft = 2048; p.hop = 512; p.n_bands = 24;
    p.min_gain_db = -12.f; p.max_gain_db = 12.f; p.f_min = 40.f; p.f_max = 18000.f;
    return p;
}

// White noise, seedable.
static std::vector<float> white(int n, uint32_t seed) {
    std::mt19937 rng(seed); std::uniform_real_distribution<float> d(-0.3f, 0.3f);
    std::vector<float> o(n); for (auto& v : o) v = d(rng); return o;
}
// Dark (LF-heavy): leaky integral of white. Bright (HF-heavy): first difference.
static std::vector<float> dark(int n, uint32_t seed) {
    auto w = white(n, seed); std::vector<float> o(n); double acc = 0;
    for (int i = 0; i < n; ++i) { acc = 0.995 * acc + w[i]; o[i] = (float)acc; }
    double pk = 1e-9; for (float v : o) pk = std::max(pk, (double)std::fabs(v));
    for (auto& v : o) v = (float)(0.3 * v / pk); return o;
}
static std::vector<float> bright(int n, uint32_t seed) {
    auto w = white(n, seed); std::vector<float> o(n); float prev = 0;
    for (int i = 0; i < n; ++i) { o[i] = 0.6f * (w[i] - prev); prev = w[i]; } return o;
}

// Drive the controller over a signal (observe + target every block, so the
// emitted-curve smoother converges) and return the final per-band dB curve.
static std::vector<float> curve_db(AdaptiveEqController& c, const std::vector<float>& x, const SpectralMaskEqParams& p) {
    c.reset(p);
    std::vector<float> db(p.n_bands, 0.f);
    for (int i = 0; i + p.block_size <= (int)x.size(); i += p.block_size) {
        c.observe(&x[i], p.block_size);
        c.target_db(db.data(), p.n_bands);
    }
    return db;
}

static double l2(const std::vector<float>& a, const std::vector<float>& b) {
    double s = 0; for (size_t i = 0; i < a.size(); ++i) { double d = a[i] - b[i]; s += d * d; } return std::sqrt(s);
}
static double meanr(const std::vector<float>& v, int lo, int hi) {
    double s = 0; for (int i = lo; i < hi; ++i) s += v[i]; return s / (hi - lo);
}

int main() {
    const auto p = cfg();
    const int n = p.sample_rate * 3;  // 3 s
    int checks = 0;

    // 1) STABILITY: finite, within the renderer span; bands in [0,1].
    {
        AdaptiveEqController c;
        auto db = curve_db(c, white(n, 1), p);
        for (int b = 0; b < p.n_bands; ++b) {
            assert(std::isfinite(db[b]));
            assert(db[b] >= p.min_gain_db - 1e-3f && db[b] <= p.max_gain_db + 1e-3f);
        }
        std::vector<float> bands(p.n_bands);
        c.target_bands(bands.data(), p.n_bands);
        for (int b = 0; b < p.n_bands; ++b) assert(bands[b] >= 0.f && bands[b] <= 1.f);
        ++checks;
    }

    // 2) ADAPTIVITY (no mode collapse): spectrally-distinct material → distinct
    //    curves. A collapsed controller would output ~the same curve for both.
    std::vector<float> d_db, b_db;
    {
        AdaptiveEqController cd, cb;
        d_db = curve_db(cd, dark(n, 2), p);
        b_db = curve_db(cb, bright(n, 3), p);
        const double dist = l2(d_db, b_db);
        std::printf("adaptivity ||dark-bright|| = %.2f dB\n", dist);
        assert(dist > 3.0);   // several dB apart — clearly not collapsed
        ++checks;
    }

    // 3) TONAL DIRECTION: dark material (LF-heavy) ends up tilted UP toward HF
    //    relative to bright material (the solver pulls each toward the target).
    {
        const double dark_tilt   = meanr(d_db, p.n_bands - 3, p.n_bands) - meanr(d_db, 0, 3);
        const double bright_tilt = meanr(b_db, p.n_bands - 3, p.n_bands) - meanr(b_db, 0, 3);
        std::printf("tilt(top-bottom): dark=%.2f bright=%.2f dB\n", dark_tilt, bright_tilt);
        assert(dark_tilt > bright_tilt);   // dark corrected upward more than bright
        ++checks;
    }

    // 4) DETERMINISM: same input → identical curve.
    {
        AdaptiveEqController c1, c2;
        auto a = curve_db(c1, dark(n, 7), p);
        auto b = curve_db(c2, dark(n, 7), p);
        assert(l2(a, b) < 1e-5);
        ++checks;
    }

    // 5) DEPTH=0 BYPASS: target_bands(depth01=0) → flat 0 dB ⇒ all bands == 0.5.
    {
        AdaptiveEqController c; c.reset(p);
        auto x = bright(n, 9);
        std::vector<float> bands(p.n_bands);
        for (int i = 0; i + p.block_size <= (int)x.size(); i += p.block_size) {
            c.observe(&x[i], p.block_size);
            c.target_bands(bands.data(), p.n_bands, /*depth01=*/0.0f);
        }
        for (int b = 0; b < p.n_bands; ++b) assert(std::fabs(bands[b] - 0.5f) < 1e-4f);
        ++checks;
    }

    std::printf("test_adaptive_eq: %d checks passed\n", checks);
    return 0;
}
