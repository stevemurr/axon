// Micro-benchmark for SpectralMaskEq (STFT min-phase mask EQ).
//   c++ -O3 -std=c++17 -I src bench/bench_spectral_mask_eq.cpp \
//       -framework Accelerate -o bench/bench_spectral_mask_eq
#include "../src/spectral_mask_eq.hpp"
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
using Clock = std::chrono::steady_clock;

static nablafx::SpectralMaskEqParams cfg() {
    nablafx::SpectralMaskEqParams p;
    p.sample_rate=44100; p.block_size=128; p.num_control_params=24;
    p.n_fft=2048; p.hop=512; p.n_bands=24;
    p.min_gain_db=-12.f; p.max_gain_db=12.f; p.f_min=40.f; p.f_max=18000.f;
    return p;
}
// scenario: static mask (controller output constant) -- the common mastering
// steady state. Also a "moving" mask scenario (params change every block).
static double run(bool moving, int seconds) {
    auto p = cfg();
    nablafx::SpectralMaskEq eq; eq.reset(p);
    const int N = 44100*seconds, blk = p.block_size;
    std::mt19937 rng(3); std::uniform_real_distribution<float> d(-0.4f,0.4f);
    std::vector<float> in(N), out(N);
    for (int i=0;i<N;++i) in[i]=d(rng);
    std::vector<float> bands(p.n_bands, 0.5f);
    // converge first
    for (int i=0;i+blk<=N;i+=blk){ eq.set_params(bands.data(),p.n_bands); eq.process(&in[i],&out[i],blk);}
    volatile float sink=0;
    auto t0=Clock::now();
    int tick=0;
    for (int i=0;i+blk<=N;i+=blk){
        if (moving){ for(int b=0;b<p.n_bands;++b) bands[b]=0.5f+0.2f*std::sin(0.01f*(tick*p.n_bands+b)); ++tick; }
        eq.set_params(bands.data(),p.n_bands);
        eq.process(&in[i],&out[i],blk);
        sink+=out[i];
    }
    auto t1=Clock::now(); (void)sink;
    double ns=std::chrono::duration<double,std::nano>(t1-t0).count();
    return ns/((double)(N/blk*blk));
}
int main(){
    double budget = 1e9/44100.0*128.0;
    double s = run(false,4), m = run(true,4);
    std::printf("== SpectralMaskEq (n_fft=2048 hop=512, 128-blk + set_params/blk) ==\n");
    std::printf("  static mask : %.2f ns/sample (%.1f%% of 128-blk budget)\n", s, s*128/budget*100);
    std::printf("  moving mask : %.2f ns/sample (%.1f%% of 128-blk budget)\n", m, m*128/budget*100);
    return 0;
}
