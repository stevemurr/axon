// Micro-benchmark for TruePeakCeiling (4x oversampled true-peak limiter).
//   c++ -O3 -std=c++17 -I src bench/bench_true_peak.cpp src/true_peak_ceiling.cpp -o bench/bench_true_peak
#include "../src/true_peak_ceiling.hpp"
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
using Clock=std::chrono::steady_clock;
static double bench(int blk,int iters){
  int SR=48000,N=SR; std::mt19937 rng(6); std::uniform_real_distribution<float> d(-1.2f,1.2f);
  std::vector<float> X(N); for(int i=0;i<N;++i)X[i]=d(rng);
  nablafx::TruePeakCeiling tp; tp.reset(SR);
  std::vector<float> o(N);
  for(int i=0;i+blk<=N;i+=blk) tp.process(&X[i],&o[i],blk);
  volatile float s=0; auto t0=Clock::now();
  for(int it=0;it<iters;++it){for(int i=0;i+blk<=N;i+=blk)tp.process(&X[i],&o[i],blk);s+=o[0];}
  auto t1=Clock::now();(void)s; return std::chrono::duration<double,std::nano>(t1-t0).count()/((double)(N/blk*blk)*iters);
}
int main(){
  double budget=1e9/48000.0*128.0;
  std::printf("== TruePeakCeiling throughput (mono/channel) ==\n");
  for(int blk:{64,128,512}){double ns=bench(blk,40);
    std::printf("  block=%-4d %.2f ns/sample/ch (stereo %.2f%% of 128-blk budget)\n",blk,ns,2*ns*128/budget*100);}
  return 0;
}
