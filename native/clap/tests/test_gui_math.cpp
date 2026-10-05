// The editor draws the stages from the functions in gui_math.hpp; this checks them against what the DSP does.
//   g++ -O2 -std=c++17 -I src tests/test_gui_math.cpp -o tests/test_gui_math && tests/test_gui_math

#include "../src/gui_math.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kSR = 48000.0;

// Gain of a stereo-processing function for a side-only sine at `hz`: left = +s, right = -s, so mid = 0 and the
// output side is the output left. Returns the measured side gain in dB over the second half of the run.
template <class Process>
double measured_side_db(double hz, Process process) {
    const int n = 1 << 16;
    std::vector<float> l(n), r(n);
    for (int i = 0; i < n; ++i) { const float s = static_cast<float>(.25 * std::sin(2 * kPi * hz * i / kSR)); l[i] = s; r[i] = -s; }
    process(l.data(), r.data(), n);
    double in = 0, out = 0;
    for (int i = n / 2; i < n; ++i) { const double s = .25 * std::sin(2 * kPi * hz * i / kSR); in += s * s; out += static_cast<double>(l[i]) * l[i]; }
    return 10 * std::log10(out / in);
}

void bass_mono_curve_is_what_it_does() {
    double worst = 0;
    for (double cutoff : {80., 225., 400.}) {
        nablafx::BassMono bm; bm.prepare(kSR); bm.set_cutoff(static_cast<float>(cutoff));
        for (double hz : {30., 60., 120., 225., 450., 900., 3000., 9000.}) {
            nablafx::BassMono m; m.prepare(kSR); m.set_cutoff(static_cast<float>(cutoff));
            const double measured = measured_side_db(hz, [&](float* l, float* r, int n) { m.process(l, r, n); });
            const double drawn = nablafx::guimath::bass_mono_side_db(hz, cutoff, kSR);
            if (drawn > -50) worst = std::max(worst, std::abs(measured - drawn));
        }
    }
    std::printf("bass mono: side gain drawn vs measured, worst %.3f dB\n", worst);
    assert(worst < .15);
}

void widener_curve_is_what_it_does() {
    double worst = 0;
    for (double width : {0., .6, 1.38, 2.}) for (double low : {120., 250., 600.}) for (double air : {0., .5, 1.}) {
        for (double hz : {40., 150., 300., 800., 2500., 6000., 12000.}) {
            nablafx::Widener w; w.prepare(kSR); w.set_params(static_cast<float>(width), static_cast<float>(low), static_cast<float>(air));
            const double measured = measured_side_db(hz, [&](float* l, float* r, int n) { w.process(l, r, n); });
            const double drawn = nablafx::guimath::widener_side_db(hz, width, low, air, kSR);
            if (drawn > -50) worst = std::max(worst, std::abs(measured - drawn));
        }
    }
    std::printf("widener: side gain drawn vs measured, worst %.3f dB\n", worst);
    assert(worst < .15);
}

// Schroeder backward integration: the time the energy takes to fall from -5 dB to -35 dB, doubled.
double decay_time(const std::vector<float>& ir, double rate) {
    std::vector<double> edc(ir.size());
    double sum = 0;
    for (size_t i = ir.size(); i-- > 0;) { sum += static_cast<double>(ir[i]) * ir[i]; edc[i] = sum; }
    auto at = [&](double db) { for (size_t i = 0; i < edc.size(); ++i) if (10 * std::log10(edc[i] / edc[0]) < db) return static_cast<double>(i) / rate; return -1.; };
    return 2 * (at(-35) - at(-5));
}

void reverb_decay_is_what_it_does() {
    for (double size : {.1, .5, .9}) for (double damp : {2000., 7000., 18000.}) {
        nablafx::Reverb rv; rv.prepare(kSR); rv.set_params(1.f, static_cast<float>(size), 1.f, static_cast<float>(damp), 20.f);
        const int n = static_cast<int>(kSR * 6);
        std::vector<float> l(n, 0.f), r(n, 0.f);
        l[0] = r[0] = 1.f;
        rv.process(l.data(), r.data(), n);
        l[0] -= 1.f;                                                    // the dry
        for (double hz : {250., 1500., 5000.}) {
            const double w = 2 * kPi * hz / kSR, alpha = std::sin(w) / 12., a0 = 1 + alpha;
            const double b0 = alpha / a0, b2 = -alpha / a0, a1 = -2 * std::cos(w) / a0, a2 = (1 - alpha) / a0;
            std::vector<float> band(n);
            double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            for (int i = 0; i < n; ++i) { const double x = l[i], y = b0 * x + b2 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = x; y2 = y1; y1 = y; band[i] = static_cast<float>(y); }
            const double measured = decay_time(band, kSR), drawn = nablafx::Reverb::rt60_at(size, damp, hz, kSR);
            std::printf("reverb size %.1f damp %5.0f at %4.0f Hz: measured %.2f s, drawn %.2f s\n", size, damp, hz, measured, drawn);
            assert(measured > .6 * drawn && measured < 1.6 * drawn);
        }
    }
    // At a frequency the damping does not reach, the tail lasts the Size time.
    assert(std::abs(nablafx::Reverb::rt60_at(.5, 18000, 100, kSR) / nablafx::Reverb::rt60_seconds(.5) - 1) < .1);
    assert(nablafx::Reverb::rt60_at(.5, 2000, 8000, kSR) < .5 * nablafx::Reverb::rt60_at(.5, 2000, 100, kSR));
}
}  // namespace

int main() {
    bass_mono_curve_is_what_it_does();
    widener_curve_is_what_it_does();
    reverb_decay_is_what_it_does();
    std::printf("gui_math: bass mono, widener and reverb curves match the DSP\n");
    return 0;
}
