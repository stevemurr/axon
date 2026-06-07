// Micro-benchmark for BassMono. Header-only DSP, compiles standalone.
//   c++ -O3 -std=c++17 -I src bench/bench_bass_mono.cpp -o bench/bench_bass_mono
//
// Reports ns/sample for the hot path on representative input, plus the
// denormal-stall scenario: a low-frequency burst followed by long silence,
// which drives the side-channel IIR states into the denormal range. We time
// that with hardware denormal-flush (FTZ) OFF and ON to quantify the stall.

#include "../src/bass_mono.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#if defined(__aarch64__)
static inline void set_ftz(bool on) {
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    if (on) fpcr |= (1ULL << 24); else fpcr &= ~(1ULL << 24); // FZ bit
    __asm__ volatile("msr fpcr, %0" :: "r"(fpcr));
}
#else
static inline void set_ftz(bool) {}
#endif

using Clock = std::chrono::steady_clock;

static double bench(std::vector<float>& L, std::vector<float>& R, int block,
                    int iters, float cutoff) {
    const int N = (int)L.size();
    nablafx::BassMono bm; bm.prepare(48000.0); bm.set_cutoff(cutoff);
    // warm
    for (int i = 0; i + block <= N; i += block) bm.process(&L[i], &R[i], block);
    volatile float sink = 0.f;
    auto t0 = Clock::now();
    for (int it = 0; it < iters; ++it)
        for (int i = 0; i + block <= N; i += block) {
            bm.process(&L[i], &R[i], block);
            sink += L[i];
        }
    auto t1 = Clock::now();
    (void)sink;
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    double samples = (double)((N / block) * block) * iters;
    return ns / samples; // ns per sample
}

int main() {
    const int SR = 48000;
    const int N = SR * 4; // 4 s
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> d(-0.5f, 0.5f);

    // White-noise stereo (decorrelated -> real side content).
    std::vector<float> wl(N), wr(N);
    for (int i = 0; i < N; ++i) { wl[i] = d(rng); wr[i] = d(rng); }

    std::printf("== Normal throughput (white noise, cutoff 250 Hz) ==\n");
    for (int blk : {64, 128, 512}) {
        auto a = wl, b = wr;
        double nsps = bench(a, b, blk, 50, 250.f);
        std::printf("  block=%-4d  %.3f ns/sample\n", blk, nsps);
    }

    // Denormal scenario: 0.2 s hard-panned 50 Hz burst, then silence.
    auto make_burst = [&](std::vector<float>& l, std::vector<float>& r) {
        l.assign(N, 0.f); r.assign(N, 0.f);
        int burst = SR / 5;
        for (int i = 0; i < burst; ++i)
            l[i] = 0.5f * std::sin(2.0 * M_PI * 50.0 * i / SR); // all in L
    };
    std::vector<float> bl, br;

    std::printf("\n== Denormal-stall scenario (50 Hz burst -> silence) ==\n");
    for (bool ftz : {false, true}) {
        make_burst(bl, br);
        set_ftz(ftz);
        double nsps = bench(bl, br, 128, 50, 250.f);
        set_ftz(false);
        std::printf("  FTZ=%-3s  %.3f ns/sample\n", ftz ? "on" : "off", nsps);
    }
    return 0;
}
