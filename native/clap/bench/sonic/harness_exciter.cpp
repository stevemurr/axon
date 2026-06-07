// Exciter sonic A/B harness. Measures, at MATCHED excitation (drive calibrated
// so 2nd-harmonic level is equal), the aliasing / IMD / harmonic purity / CPU of
// each shaper, swept over Character. See docs/sonic_harness_spec.md.
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
constexpr double kTargetTHD = 6.0;    // %% THD — the matched "effect" (total excitement)

using Shaper = nablafx::Exciter::Shaper;

static std::function<DUT(double)> factory(double character, Shaper shaper) {
    return [character, shaper](double drive_db) -> DUT {
        return [character, shaper, drive_db](const float* in, float* out, int n) {
            nablafx::Exciter ex; ex.prepare(SR); ex.set_shaper(shaper);
            ex.set_params(/*amount*/1.0f, /*freq*/3000.f, (float)drive_db,
                          (float)character, /*tame*/19000.f);
            std::copy(in, in + n, out);
            ex.process(out, nullptr, n);
        };
    };
}

static double cpu_ns_per_sample(double character, Shaper shaper, double drive_db) {
    const int n = (int)SR; auto pink = pink_like(n, 0.3, 7);
    nablafx::Exciter ex; ex.prepare(SR); ex.set_shaper(shaper);
    ex.set_params(1.0f, 3000.f, (float)drive_db, (float)character, 19000.f);
    std::vector<float> a = pink;
    for (int i = 0; i + 256 <= n; i += 256) ex.process(&a[i], nullptr, 256);
    volatile float s = 0; auto t0 = Clock::now();
    for (int it = 0; it < 30; ++it) { a = pink; for (int i = 0; i + 256 <= n; i += 256) ex.process(&a[i], nullptr, 256); s += a[0]; }
    auto t1 = Clock::now(); (void)s;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / ((double)(n / 256 * 256) * 30);
}

struct Row { double drive, h2, h3, h4, h5, h6, thd, a9, a12, imdcc, imdsm, cpu; };

static Row measure(double character, Shaper shaper) {
    auto fac = factory(character, shaper);
    const double drive = calibrate_drive(fac, 4000.0, SR, kTargetTHD);
    auto run = [&](const std::vector<float>& in) { std::vector<float> o(in.size()); fac(drive)(in.data(), o.data(), (int)in.size()); return o; };
    const int N = 1 << 16;
    auto y4 = run(sine(4000.0, 0.25, N, SR));
    auto y9 = run(sine(9000.0, 0.25, N, SR));
    auto y12 = run(sine(12000.0, 0.25, N, SR));
    auto ycc = run(twin_tone(19000.0, 20000.0, 0.2, 0.2, N, SR));
    auto ysm = run(twin_tone(60.0, 7000.0, 0.32, 0.08, N, SR));
    return {drive,
            harmonic_db(y4,4000,2,SR), harmonic_db(y4,4000,3,SR), harmonic_db(y4,4000,4,SR),
            harmonic_db(y4,4000,5,SR), harmonic_db(y4,4000,6,SR), thd(y4,4000,SR).percent,
            alias_over_fund_db(y9,9000,SR), alias_over_fund_db(y12,12000,SR),
            imd_ccif_db(ycc,19000,20000,SR), imd_smpte_db(ysm,60,7000,SR),
            cpu_ns_per_sample(character, shaper, drive)};
}

static void compare(double character) {
    Row b = measure(character, Shaper::BiasedTanh);
    Row p = measure(character, Shaper::Polynomial);
    std::printf("\n================ Character = %.2f  (matched THD = %.1f%%%%) ================\n", character, kTargetTHD);
    std::printf("  %-26s %12s %12s %12s\n", "metric", "tanh", "poly", "Δ (poly-tanh)");
    auto line = [&](const char* m, double bt, double pt, const char* unit, bool lowergood) {
        const double d = pt - bt;
        const char* flag = (lowergood ? (d < -0.5 ? " <-- better" : "") : "");
        std::printf("  %-26s %10.2f%2s %10.2f%2s %10.2f   %s\n", m, bt, unit, pt, unit, d, flag);
    };
    line("drive_db (calibrated)", b.drive, p.drive, "", false);
    line("h2 @4k", b.h2, p.h2, "dB", false);
    line("h3 @4k", b.h3, p.h3, "dB", false);
    line("h4 @4k (purity)", b.h4, p.h4, "dB", true);
    line("h5 @4k (purity)", b.h5, p.h5, "dB", true);
    line("h6 @4k (purity)", b.h6, p.h6, "dB", true);
    line("THD @4k", b.thd, p.thd, "%", false);
    line("alias/fund @9k", b.a9, p.a9, "dB", true);
    line("alias/fund @12k", b.a12, p.a12, "dB", true);
    line("IMD CCIF (19k+20k)", b.imdcc, p.imdcc, "dB", true);
    line("IMD SMPTE (60+7k)", b.imdsm, p.imdsm, "dB", true);
    line("CPU", b.cpu, p.cpu, "ns", true);
}

int main() {
    std::printf("Exciter A/B — SR=%.0f, matched THD=%.1f%%. 'lower better' = aliasing/IMD/purity.\n",
                SR, kTargetTHD);
    for (double c : {0.0, 0.5, 1.0}) compare(c);
    return 0;
}
