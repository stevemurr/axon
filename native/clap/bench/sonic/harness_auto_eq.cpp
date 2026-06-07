// Auto-EQ sonic A/B harness. Drives a renderer with a fixed per-band target
// curve and measures magnitude-match / transient preservation / musical noise /
// latency / reconstruction / CPU. See docs/sonic_harness_spec.md.
//
//   c++ -O3 -std=c++17 -I src bench/sonic/harness_auto_eq.cpp \
//       -framework Accelerate -o bench/sonic/harness_auto_eq
#include "../../src/spectral_mask_eq.hpp"
#include "../../src/iir_filterbank_eq.hpp"
#include "analysis.hpp"
#include "signals.hpp"

#include <chrono>
#include <cstdio>
#include <string>

using namespace sonic;
using Clock = std::chrono::steady_clock;

constexpr double SR = 44100.0;
constexpr int    NB = 24;
constexpr double MIN_DB = -12.0, MAX_DB = 12.0;   // matches cfg span

static nablafx::SpectralMaskEqParams cfg() {
    nablafx::SpectralMaskEqParams p;
    p.sample_rate = (int)SR; p.block_size = 128; p.num_control_params = NB;
    p.n_fft = 2048; p.hop = 512; p.n_bands = NB;
    p.min_gain_db = (float)MIN_DB; p.max_gain_db = (float)MAX_DB;
    p.f_min = 40.f; p.f_max = 18000.f;
    return p;
}

// Mel band-center frequencies (HTK), matching SpectralMaskEq::build_mel_.
static std::vector<double> mel_centers() {
    const double mmin = 2595.0 * std::log10(1.0 + 40.0 / 700.0);
    const double mmax = 2595.0 * std::log10(1.0 + 18000.0 / 700.0);
    std::vector<double> c(NB);
    for (int b = 0; b < NB; ++b) {
        const double mel = mmin + (mmax - mmin) * (b + 1) / (NB + 1);
        c[b] = 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
    }
    return c;
}

// dB -> sigmoid band value (inverse of (min + g*span)).
static float db_to_band(double db) {
    double g = (db - MIN_DB) / (MAX_DB - MIN_DB);
    return (float)std::clamp(g, 0.0, 1.0);
}

// Target curves (analytic, in dB at frequency f).
static double curve_tilt(double f)  { return 3.0 - 6.0 * (std::log2(std::max(f, 20.0) / 80.0) / std::log2(10000.0 / 80.0)); }
static double curve_notch(double f) { double o = std::log2(std::max(f,20.0)/3000.0); return -6.0 * std::exp(-0.5 * (o*o) / (0.25*0.25)); }
static double curve_flat(double)    { return 0.0; }

static std::vector<float> bands_for(const std::function<double(double)>& curve) {
    auto c = mel_centers(); std::vector<float> b(NB);
    for (int i = 0; i < NB; ++i) b[i] = db_to_band(curve(c[i]));
    return b;
}

// DUT: fresh SpectralMaskEq, set_params every block to converge the smoother.
static DUT make_autoeq(const std::vector<float>& bands) {
    return [bands](const float* in, float* out, int n) {
        auto eq = std::make_shared<nablafx::SpectralMaskEq>();
        eq->reset(cfg());
        const int B = 128;
        for (int i = 0; i < n; i += B) {
            const int m = std::min(B, n - i);
            eq->set_params(bands.data(), NB);
            eq->process(in + i, out + i, (std::size_t)m);
        }
    };
}

static DUT make_autoeq_iir(const std::vector<float>& bands) {
    return [bands](const float* in, float* out, int n) {
        auto eq = std::make_shared<nablafx::IirFilterbankEq>();
        eq->reset(cfg());
        const int B = 128;
        for (int i = 0; i < n; i += B) {
            const int m = std::min(B, n - i);
            eq->set_params(bands.data(), NB);
            eq->process(in + i, out + i, (std::size_t)m);
        }
    };
}

template <class EQ>
static double cpu_ns_per_sample(const std::vector<float>& bands) {
    EQ eq; eq.reset(cfg());
    const int n = (int)SR; auto pink = pink_like(n, 0.3, 9);
    std::vector<float> out(n);
    for (int i = 0; i + 128 <= n; i += 128) { eq.set_params(bands.data(), NB); eq.process(&pink[i], &out[i], 128); }
    volatile float s = 0; auto t0 = Clock::now();
    for (int it = 0; it < 20; ++it) for (int i = 0; i + 128 <= n; i += 128) { eq.set_params(bands.data(), NB); eq.process(&pink[i], &out[i], 128); s += out[i]; }
    auto t1 = Clock::now(); (void)s;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / ((double)(n / 128 * 128) * 20);
}

static void run_suite(const std::string& name,
                      const std::function<DUT(const std::vector<float>&)>& factory,
                      int reported_latency,
                      const std::function<double()>& cpu_fn) {
    std::printf("\n== %s ==\n", name.c_str());

    // 1) magnitude-match vs each analytic target
    struct C { const char* nm; std::function<double(double)> fn; };
    C curves[] = {{"tilt", curve_tilt}, {"notch", curve_notch}};
    for (auto& cv : curves) {
        DUT dut = factory(bands_for(cv.fn));
        double err2 = 0, emax = 0; int cnt = 0;
        for (double f = 40.0; f <= 16000.0; f *= std::pow(2.0, 1.0 / 12.0)) {
            const double got = gain_db_at(dut, f, SR);
            const double want = cv.fn(f);
            const double e = got - want;
            err2 += e * e; emax = std::max(emax, std::fabs(e)); ++cnt;
        }
        std::printf("  match[%-5s]  RMS=%.2f dB  max=%.2f dB\n", cv.nm, std::sqrt(err2 / cnt), emax);
        std::printf("KV %s match_%s_rms=%.3f match_%s_max=%.3f\n", name.c_str(), cv.nm, std::sqrt(err2/cnt), cv.nm, emax);
    }

    // 2) transient preservation (click train) under an ACTIVE curve: both
    //    renderers apply the same tilt, so the one that smears less keeps a
    //    higher output crest (flat would be transparent and uninformative).
    {
        DUT dut = factory(bands_for(curve_tilt));
        const int n = 1 << 15; auto in = click_train(n, 2048, 0.7);
        std::vector<float> out(n); dut(in.data(), out.data(), n);
        const double ci = crest_db(in.data(), n, n / 4), co = crest_db(out.data(), n, n / 4);
        std::printf("  transient crest  in=%.1f out=%.1f dB  (Δ=%.1f)\n", ci, co, co - ci);
        std::printf("KV %s crest_in=%.2f crest_out=%.2f crest_delta=%.2f\n", name.c_str(), ci, co, co - ci);
    }

    // 3) musical noise: static mask + steady tone -> output modulation depth
    {
        DUT dut = factory(bands_for(curve_tilt));
        const int n = (int)SR; auto in = sine(1000.0, 0.3, n, SR);
        std::vector<float> out(n); dut(in.data(), out.data(), n);
        const double md = modulation_depth_db(out.data(), n, SR, 5.0, n / 3);
        std::printf("  musical-noise (1k, static)  modulation = %.1f dB\n", md);
        std::printf("KV %s modulation=%.2f\n", name.c_str(), md);
    }

    // 4) latency (measured) + reported
    {
        DUT dut = factory(bands_for(curve_flat));
        const int lat = measured_latency(dut, 1 << 14, 4096);
        std::printf("  latency measured=%d  reported=%d samples (%.1f ms)\n", lat, reported_latency, 1000.0 * reported_latency / SR);
        std::printf("KV %s latency_meas=%d latency_rep=%d\n", name.c_str(), lat, reported_latency);
    }

    // 5) reconstruction null (flat mask)
    {
        DUT dut = factory(bands_for(curve_flat));
        const int n = 1 << 15; auto in = pink_like(n, 0.3, 5);
        std::vector<float> out(n); dut(in.data(), out.data(), n);
        const double nr = null_residual_db(in, out, 3000);
        std::printf("  flat-mask null residual    = %.1f dBFS\n", nr);
        std::printf("KV %s null=%.2f\n", name.c_str(), nr);
    }

    // 6) CPU
    {
        const double cpu = cpu_fn();
        std::printf("  CPU                        = %.1f ns/sample\n", cpu);
        std::printf("KV %s cpu=%.1f\n", name.c_str(), cpu);
    }
}

int main() {
    std::printf("Auto-EQ sonic harness — SR=%.0f, %d bands, span [%.0f,%.0f] dB\n", SR, NB, MIN_DB, MAX_DB);
    run_suite("baseline (STFT min-phase mask)", make_autoeq, /*reported*/2048,
              [] { return cpu_ns_per_sample<nablafx::SpectralMaskEq>(bands_for(curve_tilt)); });
    run_suite("candidate (min-phase IIR bank)", make_autoeq_iir, /*reported*/0,
              [] { return cpu_ns_per_sample<nablafx::IirFilterbankEq>(bands_for(curve_tilt)); });
    return 0;
}
