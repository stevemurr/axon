// Exciter sonic A/B harness. Measures, at MATCHED excitation (drive calibrated
// so 2nd-harmonic level is equal), the aliasing / IMD / harmonic purity / CPU of
// each shaper. See docs/sonic_harness_spec.md.
//
//   c++ -O3 -std=c++17 -I src bench/sonic/harness_exciter.cpp -o bench/sonic/harness_exciter
#include "../../src/exciter.hpp"
#include "analysis.hpp"
#include "signals.hpp"

#include <chrono>
#include <cstdio>
#include <string>

using namespace sonic;
using Clock = std::chrono::steady_clock;

constexpr double SR = 48000.0;
constexpr double kTargetH2 = -24.0;   // dB rel fundamental — the matched "effect"
constexpr double kCharacter = 0.0;    // even-dominant (2nd) for the comparison

// Fresh-instance DUT (no state carry between measurements).
static DUT make_exciter(double drive_db) {
    return [drive_db](const float* in, float* out, int n) {
        nablafx::Exciter ex; ex.prepare(SR);
        ex.set_params(/*amount*/1.0f, /*freq*/3000.f, (float)drive_db,
                      (float)kCharacter, /*tame*/19000.f);
        std::copy(in, in + n, out);
        ex.process(out, nullptr, n);
    };
}

static double cpu_ns_per_sample(double drive_db) {
    const int n = (int)SR;
    auto pink = pink_like(n, 0.3, 7);
    nablafx::Exciter ex; ex.prepare(SR);
    ex.set_params(1.0f, 3000.f, (float)drive_db, (float)kCharacter, 19000.f);
    std::vector<float> a = pink;
    for (int i = 0; i + 256 <= n; i += 256) ex.process(&a[i], nullptr, 256);  // warm
    volatile float s = 0; auto t0 = Clock::now();
    for (int it = 0; it < 30; ++it) { a = pink; for (int i = 0; i + 256 <= n; i += 256) ex.process(&a[i], nullptr, 256); s += a[0]; }
    auto t1 = Clock::now(); (void)s;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / ((double)(n / 256 * 256) * 30);
}

static void run_suite(const std::string& name,
                      const std::function<DUT(double)>& factory) {
    // 1) match the effect: calibrate drive so h2 == kTargetH2 at f0 = 4 kHz.
    const double drive = calibrate_drive(factory, 4000.0, SR, kTargetH2);

    auto process = [&](const std::vector<float>& in) {
        std::vector<float> out(in.size());
        factory(drive)(in.data(), out.data(), (int)in.size());
        return out;
    };

    const int N = 1 << 16;
    // confirm the match
    auto s4 = sine(4000.0, 0.25, N, SR); auto y4 = process(s4);
    const double h2 = harmonic_db(y4, 4000.0, 2, SR);
    const double h3 = harmonic_db(y4, 4000.0, 3, SR);
    const double h4 = harmonic_db(y4, 4000.0, 4, SR);
    const double h5 = harmonic_db(y4, 4000.0, 5, SR);
    const double h6 = harmonic_db(y4, 4000.0, 6, SR);
    const double thd4 = thd(y4, 4000.0, SR).percent;

    // alias at high f0
    auto y9  = process(sine(9000.0,  0.25, N, SR));
    auto y12 = process(sine(12000.0, 0.25, N, SR));
    const double a9  = alias_over_fund_db(y9,  9000.0,  SR);
    const double a12 = alias_over_fund_db(y12, 12000.0, SR);

    // IMD
    auto ycc = process(twin_tone(19000.0, 20000.0, 0.2, 0.2, N, SR));
    const double imd_cc = imd_ccif_db(ycc, 19000.0, 20000.0, SR);
    auto ysm = process(twin_tone(60.0, 7000.0, 0.32, 0.08, N, SR));  // 4:1
    const double imd_sm = imd_smpte_db(ysm, 60.0, 7000.0, SR);

    const double cpu = cpu_ns_per_sample(drive);

    std::printf("\n== %s ==\n", name.c_str());
    std::printf("  calibrated drive_db        = %.2f  (target h2 = %.1f dB)\n", drive, kTargetH2);
    std::printf("  harmonics @4k  h2=%.1f h3=%.1f h4=%.1f h5=%.1f h6=%.1f dB\n", h2, h3, h4, h5, h6);
    std::printf("  THD @4k                    = %.4f %%\n", thd4);
    std::printf("  alias/fund @9k             = %.1f dB\n", a9);
    std::printf("  alias/fund @12k            = %.1f dB\n", a12);
    std::printf("  IMD CCIF (19k+20k)         = %.1f dB\n", imd_cc);
    std::printf("  IMD SMPTE (60+7k 4:1)      = %.1f dB\n", imd_sm);
    std::printf("  CPU                        = %.1f ns/sample\n", cpu);
    std::printf("KV %s drive=%.3f h2=%.2f h4=%.2f thd=%.4f alias9=%.2f alias12=%.2f imdcc=%.2f imdsm=%.2f cpu=%.1f\n",
                name.c_str(), drive, h2, h4, thd4, a9, a12, imd_cc, imd_sm, cpu);
}

int main() {
    std::printf("Exciter sonic harness — SR=%.0f, matched 2nd-harmonic = %.1f dB, character=%.2f\n",
                SR, kTargetH2, kCharacter);
    run_suite("baseline (biased-tanh)", make_exciter);
    // Candidate (Chebyshev) appended here once implemented:
    // run_suite("candidate (chebyshev)", make_exciter_cheby);
    return 0;
}
