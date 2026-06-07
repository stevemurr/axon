// Micro-benchmark for the FDN Reverb (header-only DSP).
//   c++ -O3 -std=c++17 -I src bench/bench_reverb.cpp -o bench/bench_reverb
#include "../src/reverb.hpp"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>
using Clock = std::chrono::steady_clock;
#if defined(__aarch64__)
static inline void set_ftz(bool on){uint64_t f;__asm__ volatile("mrs %0,fpcr":"=r"(f));
  if(on)f|=(1ULL<<24);else f&=~(1ULL<<24);__asm__ volatile("msr fpcr,%0"::"r"(f));}
#else
static inline void set_ftz(bool){}
#endif
static double bench(std::vector<float>&L,std::vector<float>&R,int blk,int iters){
  const int N=(int)L.size();
  nablafx::Reverb rv; rv.prepare(48000.0); rv.set_params(0.3f,0.5f,0.8f,7000.f,250.f);
  auto a=L,b=R;
  for(int i=0;i+blk<=N;i+=blk) rv.process(&a[i],&b[i],blk);
  volatile float s=0; auto t0=Clock::now();
  for(int it=0;it<iters;++it){a=L;b=R;for(int i=0;i+blk<=N;i+=blk)rv.process(&a[i],&b[i],blk);s+=a[0];}
  auto t1=Clock::now();(void)s;
  return std::chrono::duration<double,std::nano>(t1-t0).count()/((double)(N/blk*blk)*iters);
}
int main(){
  const int SR=48000,N=SR*2; std::mt19937 rng(5); std::uniform_real_distribution<float> d(-0.4f,0.4f);
  std::vector<float> L(N),R(N); for(int i=0;i<N;++i){L[i]=d(rng);R[i]=d(rng);}
  double budget=1e9/48000.0*128.0;
  std::printf("== Reverb throughput (stereo, mix 0.3 size 0.5) ==\n");
  for(int blk:{64,128,512}){double ns=bench(L,R,blk,20);
    std::printf("  block=%-4d %.2f ns/sample (%.1f%% of 128-blk budget)\n",blk,ns,ns*128/budget*100);}
  // denormal tail: 0.2s burst then silence
  std::vector<float> bl(N,0.f),br(N,0.f); int burst=SR/5;
  for(int i=0;i<burst;++i){bl[i]=0.4f*std::sin(2.0*M_PI*200.0*i/SR);br[i]=0.4f*std::sin(2.0*M_PI*200.0*i/SR);}
  std::printf("\n== Denormal-tail (burst -> silence) ==\n");
  for(bool f:{false,true}){auto x=bl,y=br;set_ftz(f);double ns=bench(x,y,128,20);set_ftz(false);
    std::printf("  FTZ=%-3s %.2f ns/sample\n",f?"on":"off",ns);}
  return 0;
}
