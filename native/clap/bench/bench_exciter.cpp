// Micro-benchmark for the Exciter (4x oversampled waveshaper, 128-tap FIRs).
//   c++ -O3 -std=c++17 -I src bench/bench_exciter.cpp -o bench/bench_exciter
#include "../src/exciter.hpp"
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
using Clock = std::chrono::steady_clock;

static double bench(int block, int iters) {
    const int SR = 48000, N = SR; // 1 s
    std::mt19937 rng(7); std::uniform_real_distribution<float> d(-0.5f, 0.5f);
    std::vector<float> L(N), R(N);
    for (int i = 0; i < N; ++i) { L[i] = d(rng); R[i] = d(rng); }
    nablafx::Exciter ex; ex.prepare(SR);
    ex.set_params(/*amount*/0.5f, /*freq*/3000.f, /*drive*/12.f, /*char*/0.5f, /*tame*/18000.f);
    auto a = L, b = R;
    for (int i = 0; i + block <= N; i += block) ex.process(&a[i], &b[i], block);
    volatile float sink = 0.f;
    auto t0 = Clock::now();
    for (int it = 0; it < iters; ++it) {
        a = L; b = R;
        for (int i = 0; i + block <= N; i += block) ex.process(&a[i], &b[i], block);
        sink += a[0];
    }
    auto t1 = Clock::now(); (void)sink;
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    double samples = (double)((N / block) * block) * iters;
    return ns / samples;
}
int main() {
    std::printf("== Exciter throughput (stereo, amount 0.5, drive 12 dB) ==\n");
    for (int blk : {64, 128, 512}) {
        double nsps = bench(blk, 30);
        std::printf("  block=%-4d  %.2f ns/sample  (%.1f%% of 128-blk budget @48k)\n",
                    blk, nsps, nsps * 128.0 / (1e9/48000.0*128.0) * 100.0);
    }
    return 0;
}
