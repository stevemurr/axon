// Micro-benchmark for MelLimiter (STFT multiband + brickwall).
//   c++ -O3 -std=c++17 -I src bench/bench_mel_limiter.cpp src/mel_limiter.cpp \
//       -framework Accelerate -o bench/bench_mel_limiter
#include "../src/mel_limiter.hpp"
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
using Clock = std::chrono::steady_clock;
static double bench(int blk,int iters){
  const int SR=48000,N=SR; std::mt19937 rng(4); std::uniform_real_distribution<float> d(-0.7f,0.7f);
  std::vector<float> L(N),R(N); for(int i=0;i<N;++i){L[i]=d(rng);R[i]=d(rng);}
  nablafx::MelLimiter ml; ml.init(SR);
  nablafx::MelLimiter::Params p; p.ceiling_lin=0.5f; p.drive_lin=2.0f; p.adaptive_gain=0.5f; p.wet_mix=1.f;
  auto a=L,b=R; for(int i=0;i+blk<=N;i+=blk) ml.process(&a[i],&b[i],2,blk,p);
  volatile float s=0; auto t0=Clock::now();
  for(int it=0;it<iters;++it){a=L;b=R;for(int i=0;i+blk<=N;i+=blk)ml.process(&a[i],&b[i],2,blk,p);s+=a[0];}
  auto t1=Clock::now();(void)s; return std::chrono::duration<double,std::nano>(t1-t0).count()/((double)(N/blk*blk)*iters);
}
int main(){
  double budget=1e9/48000.0*128.0;
  std::printf("== MelLimiter throughput (stereo, drive 2x ceiling 0.5) ==\n");
  for(int blk:{64,128,512}){double ns=bench(blk,15);
    std::printf("  block=%-4d %.2f ns/sample (%.2f%% of 128-blk budget)\n",blk,ns,ns*128/budget*100);}
  return 0;
}
