// HARMONICS two-knob tool validation. Replicates the plugin's two-CLEAN-exciter
// wiring (Warmth: 100–1000 Hz even; Presence: 3500–16500 Hz even+odd, drive 6 dB)
// and checks: bypass at 0; Warmth adds low-mid 2nd with highs ~untouched;
// Presence adds high content with low-mids ~untouched; monotonic vs knob.
//   c++ -O3 -std=c++17 -I src bench/sonic/harness_harmonics.cpp -o bench/sonic/harness_harmonics
#include "../../src/multiband_exciter.hpp"
#include "analysis.hpp"
#include "signals.hpp"
#include <cstdio>
using namespace sonic;
constexpr double SR = 48000.0;

// Mirror the plugin: two CLEAN exciters in series.
static std::vector<float> harmonics(const std::vector<float>& in, float warm, float pres) {
    std::vector<float> out = in;
    nablafx::MultibandExciter w; w.prepare(SR); w.configure(100.0, 1000.0, 5, 0.0f, 6.f); w.set_amount(warm);
    w.process(out.data(), nullptr, (int)out.size());
    nablafx::MultibandExciter p; p.prepare(SR); p.configure(3500.0, 16500.0, 5, 0.5f, 6.f); p.set_amount(pres);
    p.process(out.data(), nullptr, (int)out.size());
    return out;
}

int main() {
    const int N = 1 << 16;
    // low-mid probe at 400 Hz (2nd -> 800), high probe at 6 kHz (2nd/3rd -> 12/18k)
    auto lo = sine(400.0, 0.3, N, SR);
    auto hi = sine(6000.0, 0.3, N, SR);

    std::printf("HARMONICS validation (SR=%.0f)\n", SR);

    // bypass
    auto b = harmonics(lo, 0.f, 0.f);
    double byp = 0; for (int i = 0; i < N; ++i) byp = std::max(byp, (double)std::fabs(b[i] - lo[i]));
    std::printf("  bypass (0,0): max|out-in| = %.2e  %s\n", byp, byp == 0 ? "(bit-identical)" : "");

    // Warmth on low-mid tone: 2nd should rise; presence band should stay ~flat.
    std::printf("\n  Warmth on 400 Hz tone — 2nd harmonic (800 Hz) vs knob:\n");
    double prev = -300;
    for (float k : {0.f, 0.25f, 0.5f, 1.0f}) {
        auto y = harmonics(lo, k, 0.f);
        double h2 = harmonic_db(y, 400.0, 2, SR);
        std::printf("    warm=%.2f  h2=%.1f dB %s\n", k, h2, (k > 0 && h2 > prev) ? "(rising)" : "");
        prev = h2;
    }
    // Presence band untouched by Warmth (probe 12 kHz region on a 6k tone w/ warmth only)
    {
        auto y = harmonics(hi, 1.0f, 0.f);
        double h2hi = harmonic_db(y, 6000.0, 2, SR);
        std::printf("    [cross-talk] warmth-only on 6k tone: high 2nd = %.1f dB (want low)\n", h2hi);
    }

    // Presence on high tone: 2nd+3rd should rise.
    std::printf("\n  Presence on 6 kHz tone — 2nd(12k)/3rd(18k) vs knob:\n");
    double pprev = -300;
    for (float k : {0.f, 0.25f, 0.5f, 1.0f}) {
        auto y = harmonics(hi, 0.f, k);
        double h2 = harmonic_db(y, 6000.0, 2, SR);
        double h3 = harmonic_db(y, 6000.0, 3, SR);
        std::printf("    pres=%.2f  h2=%.1f h3=%.1f dB %s\n", k, h2, h3, (k > 0 && h2 > pprev) ? "(rising)" : "");
        pprev = h2;
    }
    // Low-mids untouched by Presence (warmth 2nd on 400 tone with presence only)
    {
        auto y = harmonics(lo, 0.f, 1.0f);
        double h2lo = harmonic_db(y, 400.0, 2, SR);
        std::printf("    [cross-talk] presence-only on 400 tone: low 2nd = %.1f dB (want low)\n", h2lo);
    }
    return 0;
}
