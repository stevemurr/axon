// Unit tests for AutoGain (level-matched bypass integrator).
//   g++ -O2 -std=c++17 -I src tests/test_auto_gain.cpp -o tests/test_auto_gain \
//       && tests/test_auto_gain

#include "../src/auto_gain.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

// Feed-forward model: out_lufs is the REAL (uncompensated) output, i.e.
// in + proc, independent of the gain we apply. The smoother should converge so
// the monitoring gain g ≈ −proc (delivered = real + g = input).
double converge(double in_lufs, double proc_db, int iters, bool enabled = true) {
    nablafx::AutoGain ag;
    for (int i = 0; i < iters; ++i)
        ag.process(enabled, (float)in_lufs, (float)(in_lufs + proc_db), 0.f);
    return ag.gain_db();
}

void test_converges_to_match() {
    // Chain made it +6 dB louder → auto gain should settle near -6 dB.
    double g = converge(-14.0, +6.0, 4000);
    double out = -14.0 + 6.0 + g;
    std::fprintf(stderr, "[match+6]  g=%.2f dB  out=%.2f LUFS (want ~-14)\n", g, out);
    assert(std::fabs(g - (-6.0)) < 0.5);
    assert(std::fabs(out - (-14.0)) < 0.5);
    std::fprintf(stderr, "[match+6]  PASS\n");
}

void test_converges_quieter() {
    // Chain made it 4 dB quieter → auto gain should push +4 dB.
    double g = converge(-20.0, -4.0, 4000);
    std::fprintf(stderr, "[match-4]  g=%.2f dB (want ~+4)\n", g);
    assert(std::fabs(g - 4.0) < 0.5);
    std::fprintf(stderr, "[match-4]  PASS\n");
}

void test_disabled_relaxes_to_unity() {
    nablafx::AutoGain ag;
    // Wind it up while enabled…
    for (int i = 0; i < 4000; ++i) ag.process(true, -14.f, -14.f + 6.f, 0.f);
    assert(std::fabs(ag.gain_db() - (-6.0)) < 0.5);
    // …then disable: must relax to 0 dB (unity).
    for (int i = 0; i < 4000; ++i) ag.process(false, -14.f, -14.f, 0.f);
    std::fprintf(stderr, "[disable]  g=%.3f dB (want 0)\n", ag.gain_db());
    assert(std::fabs(ag.gain_db()) < 1e-3);
    std::fprintf(stderr, "[disable]  PASS\n");
}

void test_silence_gated() {
    nablafx::AutoGain ag;
    // Both below the floor → must not adapt (stays at unity).
    for (int i = 0; i < 2000; ++i) ag.process(true, -120.f, -120.f, 0.f);
    std::fprintf(stderr, "[silence]  g=%.3f dB (want 0)\n", ag.gain_db());
    assert(ag.gain_db() == 0.f);
    std::fprintf(stderr, "[silence]  PASS\n");
}

// Feed-forward: a Drive jump must be cancelled in the SAME block, before the
// slow LUFS loop reacts — so the monitoring level doesn't bump.
void test_feed_forward_instant() {
    nablafx::AutoGain ag;
    // Settle matched with no drive (out == in).
    for (int i = 0; i < 4000; ++i) ag.process(true, -14.f, -14.f, 0.f);
    const double before = ag.gain_db();              // ≈ 0
    // Drive jumps +6 dB. The output meter hasn't moved yet (still -14), but we
    // feed the known +6 forward → gain must drop ~6 dB instantly.
    ag.process(true, -14.f, -14.f, 6.f);
    const double after = ag.gain_db();
    std::fprintf(stderr, "[ff]       g %.2f → %.2f dB on a +6 dB drive jump (want ~-6)\n",
                before, after);
    assert(after < before - 5.5);                    // ~6 dB drop in one block
    std::fprintf(stderr, "[ff]       PASS\n");
}

void test_clamped() {
    // Absurd +60 dB processing → gain clamps at -24 dB, never runs away.
    double g = converge(-10.0, +60.0, 8000);
    std::fprintf(stderr, "[clamp]    g=%.2f dB (want -24)\n", g);
    assert(g <= -24.0 + 1e-3 && g >= -24.0 - 1e-3);
    std::fprintf(stderr, "[clamp]    PASS\n");
}

}  // namespace

int main() {
    test_converges_to_match();
    test_converges_quieter();
    test_disabled_relaxes_to_unity();
    test_silence_gated();
    test_feed_forward_instant();
    test_clamped();
    std::fprintf(stderr, "ALL AUTO-GAIN TESTS PASSED\n");
    return 0;
}
