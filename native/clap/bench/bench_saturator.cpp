// Micro-benchmark for the parked Saturator's RationalA nonlinearity.
//   c++ -O3 -std=c++17 -I src bench/bench_saturator.cpp -o bench/bench_saturator
#include "../src/rational_a.hpp"
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
using Clock=std::chrono::steady_clock;
int main(){
  // Real coefficients from weights/axon_bundle/saturator/plugin_meta.json.
  std::vector<float> num={5.014246085011109e-07f,3.4951839447021484f,-9.277632102566713e-07f,3.858473539352417f,-7.840228022359952e-08f,0.9341245889663696f,-2.7553147674552747e-07f};
  std::vector<float> den={1.658664345741272f,2.483762741088867f,1.050411581993103f,-8.716301067579479e-07f,2.0789926052093506f};
  nablafx::RationalA r; r.reset(num,den);
  int SR=48000,N=SR; std::mt19937 rng(2); std::uniform_real_distribution<float> d(-0.6f,0.6f);
  std::vector<float> X(N),o(N); for(int i=0;i<N;++i)X[i]=d(rng);
  for(int blk:{64,128,512}){
    for(int i=0;i+blk<=N;i+=blk)r.process(&X[i],&o[i],blk);
    volatile float s=0; auto t0=Clock::now();
    for(int it=0;it<60;++it){for(int i=0;i+blk<=N;i+=blk)r.process(&X[i],&o[i],blk);s+=o[0];}
    auto t1=Clock::now();(void)s;
    double ns=std::chrono::duration<double,std::nano>(t1-t0).count()/((double)(N/blk*blk)*60);
    printf("  block=%-4d %.2f ns/sample/ch (base rate, no oversampling)\n",blk,ns);
  }
  return 0;
}
