// Unit tests for LoudnessMeter.
//
// Build from native/clap/:
//   g++ -O2 -std=c++17 -I src tests/test_meter.cpp src/meter.cpp \
//       src/lufs_leveler.cpp -o tests/test_meter && tests/test_meter

#include "../src/meter.hpp"
#include "../src/lufs_leveler.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

constexpr int    kSR = 44100;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> make_sine(double f, double a, int n) {
    std::vector<float> o(n);
    for (int i = 0; i < n; ++i) o[i] = (float)(a * std::sin(2.0 * kPi * f * i / kSR));
    return o;
}
std::vector<float> make_pink(int n, double amp, uint32_t seed = 7) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    std::vector<float> o(n); double acc = 0;
    for (int i = 0; i < n; ++i) { acc = 0.995 * acc + 0.05 * nd(rng); o[i] = (float)acc; }
    double pk = 0; for (float v : o) pk = std::max(pk, std::fabs((double)v));
    if (pk > 1e-9) for (float& v : o) v = (float)(v * amp / pk);
    return o;
}

// ---------------------------------------------------------------------------
// Test 1: absolute LUFS matches the trusted LufsLeveler on the same signal
// ---------------------------------------------------------------------------
void test_lufs_matches_leveler() {
    const int N = 5 * kSR;

    // Mono.
    {
        auto sig = make_pink(N, 0.5, 1);
        nablafx::LoudnessMeter m; m.reset(kSR);
        m.process(sig.data(), nullptr, 1, N);

        nablafx::LufsLeveler lv; lv.reset(kSR, -14.0);
        std::vector<float> tmp(sig);
        lv.process(sig.data(), tmp.data(), N);   // measures input LUFS

        double meter = m.readout().lufs_s;
        double ref   = lv.last_measured_lufs();
        std::fprintf(stderr, "[lufs/mono]   meter=%.2f  leveler=%.2f LUFS\n", meter, ref);
        assert(std::fabs(meter - ref) < 0.5);
    }
    // Stereo (decorrelated channels).
    {
        auto l = make_pink(N, 0.5, 2);
        auto r = make_pink(N, 0.5, 3);
        nablafx::LoudnessMeter m; m.reset(kSR);
        m.process(l.data(), r.data(), 2, N);

        nablafx::LufsLeveler lv; lv.reset(kSR, -14.0);
        std::vector<float> lo(l), ro(r);
        lv.process_linked(l.data(), r.data(), lo.data(), ro.data(), N);

        double meter = m.readout().lufs_s;
        double ref   = lv.last_measured_lufs();
        std::fprintf(stderr, "[lufs/stereo] meter=%.2f  leveler=%.2f LUFS\n", meter, ref);
        assert(std::fabs(meter - ref) < 0.5);
    }
    std::fprintf(stderr, "[lufs]        PASS\n");
}

// ---------------------------------------------------------------------------
// Test 2: scaling — ×2 amplitude raises LUFS / RMS / peak by ~6.02 dB
// ---------------------------------------------------------------------------
void test_scaling() {
    const int N = 5 * kSR;
    auto run = [&](double amp) {
        auto s = make_pink(N, amp, 9);
        nablafx::LoudnessMeter m; m.reset(kSR);
        m.process(s.data(), nullptr, 1, N);
        return m.readout();
    };
    auto a = run(0.25);
    auto b = run(0.50);
    std::fprintf(stderr,
        "[scale]       lufs %.2f->%.2f (d=%.2f)  rms %.2f->%.2f (d=%.2f)  peak %.2f->%.2f (d=%.2f)\n",
        a.lufs_s, b.lufs_s, b.lufs_s - a.lufs_s,
        a.rms_db, b.rms_db, b.rms_db - a.rms_db,
        a.peak_db, b.peak_db, b.peak_db - a.peak_db);
    assert(std::fabs((b.lufs_s - a.lufs_s) - 6.02) < 0.2);
    assert(std::fabs((b.rms_db - a.rms_db) - 6.02) < 0.2);
    assert(std::fabs((b.peak_db - a.peak_db) - 6.02) < 0.2);
    std::fprintf(stderr, "[scale]       PASS\n");
}

// ---------------------------------------------------------------------------
// Test 3: peak & RMS absolute values on a known sine
// ---------------------------------------------------------------------------
void test_peak_rms_sine() {
    const int N = 2 * kSR;
    const double amp = 0.5;            // -6.02 dBFS peak
    auto s = make_sine(1000.0, amp, N);
    nablafx::LoudnessMeter m; m.reset(kSR);
    m.process(s.data(), nullptr, 1, N);
    auto r = m.readout();

    const double want_peak = 20.0 * std::log10(amp);            // -6.02
    const double want_rms  = 20.0 * std::log10(amp / std::sqrt(2.0)); // -9.03
    std::fprintf(stderr, "[sine]        peak=%.2f (want %.2f)  rms=%.2f (want %.2f)\n",
                r.peak_db, want_peak, r.rms_db, want_rms);
    assert(std::fabs(r.peak_db - want_peak) < 0.1);
    assert(std::fabs(r.rms_db  - want_rms)  < 0.2);
    std::fprintf(stderr, "[sine]        PASS\n");
}

// ---------------------------------------------------------------------------
// Test 4: silence reads very low everywhere
// ---------------------------------------------------------------------------
void test_silence() {
    const int N = 4 * kSR;
    std::vector<float> z(N, 0.f);
    nablafx::LoudnessMeter m; m.reset(kSR);
    m.process(z.data(), nullptr, 1, N);
    auto r = m.readout();
    std::fprintf(stderr, "[silence]     lufs=%.1f rms=%.1f peak=%.1f\n",
                r.lufs_s, r.rms_db, r.peak_db);
    assert(r.lufs_s  < -70.f);
    assert(r.rms_db  < -100.f);
    assert(r.peak_db < -100.f);
    std::fprintf(stderr, "[silence]     PASS\n");
}

// ---------------------------------------------------------------------------
// Test 5: a -14 LUFS target lands in range (sanity for the mastering use case)
// ---------------------------------------------------------------------------
void test_target_minus14() {
    const int N = 6 * kSR;
    // Use the leveler to drive pink noise to -14 LUFS, then meter its output.
    auto src = make_pink(N, 0.7, 5);
    nablafx::LufsLeveler lv; lv.reset(kSR, -14.0);
    std::vector<float> out(N);
    lv.process(src.data(), out.data(), N);

    nablafx::LoudnessMeter m; m.reset(kSR);
    m.process(out.data(), nullptr, 1, N);
    double lufs = m.readout().lufs_s;
    std::fprintf(stderr, "[target]      leveled-to-14 reads %.2f LUFS\n", lufs);
    assert(std::fabs(lufs - (-14.0)) < 1.5);
    std::fprintf(stderr, "[target]      PASS\n");
}

}  // namespace

int main() {
    test_lufs_matches_leveler();
    test_scaling();
    test_peak_rms_sine();
    test_silence();
    test_target_minus14();
    std::fprintf(stderr, "ALL METER TESTS PASSED\n");
    return 0;
}
