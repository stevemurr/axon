// Micro-benchmark for the M/S Widener (header-only DSP).
//   c++ -O3 -std=c++17 -I src bench/bench_widener.cpp -o bench/bench_widener
#include "../src/widener.hpp"
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
using Clock=std::chrono::steady_clock;
static double bench(int blk,int iters,float width,float air){
  int SR=48000,N=SR; std::mt19937 rng(8); std::uniform_real_distribution<float> d(-0.4f,0.4f);
  std::vector<float> L(N),R(N); for(int i=0;i<N;++i){L[i]=d(rng);R[i]=d(rng);}
  nablafx::Widener w; w.prepare(SR); w.set_params(width,250.f,air);
  auto a=L,b=R; for(int i=0;i+blk<=N;i+=blk)w.process(&a[i],&b[i],blk);
  volatile float s=0; auto t0=Clock::now();
  for(int it=0;it<iters;++it){a=L;b=R;for(int i=0;i+blk<=N;i+=blk)w.process(&a[i],&b[i],blk);s+=a[0];}
  auto t1=Clock::now();(void)s; return std::chrono::duration<double,std::nano>(t1-t0).count()/((double)(N/blk*blk)*iters);
}
int main(){
  double budget=1e9/48000.0*128.0;
  std::printf("== Widener throughput (stereo, width 1.4 air 0.2) ==\n");
  for(int blk:{64,128,512}){double ns=bench(blk,40,1.4f,0.2f);
    std::printf("  block=%-4d %.2f ns/sample (%.2f%% of 128-blk budget)\n",blk,ns,ns*128/budget*100);}
  std::printf("  bypass (w=1,air=0): %.2f ns/sample\n", bench(128,40,1.0f,0.0f));
  return 0;
}
