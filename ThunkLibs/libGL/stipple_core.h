#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <vector>
namespace vexa_stipple {
struct State {
  bool line=false, polygon=false;
  int factor=1;
  unsigned pattern=0xffff;
  std::array<unsigned char,128> mask=[] {std::array<unsigned char,128> a; a.fill(255);return a;}();
};
// Split aliased line segments in window space; preserve perspective interpolation.
inline std::vector<float> lines(const std::vector<float>& input,unsigned mode,const float* mvp,
                                int width,int height,int factor,unsigned pattern) {
  std::vector<float> out; const size_t n=input.size()/15; double phase=0;
  auto segment=[&](size_t ia,size_t ib) {
    const float* a=input.data()+ia*15; const float* b=input.data()+ib*15;
    double ca[4]{},cb[4]{};
    for(int r=0;r<4;++r)for(int c=0;c<4;++c){ca[r]+=mvp[c*4+r]*a[c];cb[r]+=mvp[c*4+r]*b[c];}
    double lo=0,hi=1;
    for(int p=0;p<6;++p){int axis=p/2;double da=ca[3]+(p%2?-ca[axis]:ca[axis]);double db=cb[3]+(p%2?-cb[axis]:cb[axis]);
      if(da<0&&db<0)return; if(da<0)lo=std::max(lo,da/(da-db));if(db<0)hi=std::min(hi,da/(da-db));}
    if(lo>hi)return;
    double ac[4],bc[4];for(int r=0;r<4;++r){ac[r]=ca[r]+(cb[r]-ca[r])*lo;bc[r]=ca[r]+(cb[r]-ca[r])*hi;}
    if(ac[3]<=0||bc[3]<=0)return;
    double length=std::max(std::abs((bc[0]/bc[3]-ac[0]/ac[3])*width*.5),std::abs((bc[1]/bc[3]-ac[1]/ac[3])*height*.5));
    if(!std::isfinite(length)||length<=0)return;
    auto emit=[&](double s){double t=(s*ac[3])/((1-s)*bc[3]+s*ac[3]);t=lo+(hi-lo)*t;for(int k=0;k<15;++k)out.push_back(float(a[k]+(b[k]-a[k])*t));};
    if(pattern==0xffff || pattern==0){if(pattern){emit(0);emit(1);}phase=std::fmod(phase+length,double(16*factor));return;}
    for(double at=0;at<length;){double next=std::min(length,at+factor-std::fmod(phase+at,double(factor)));
      if(next<=at)break;
      if(pattern&(1u<<(unsigned(std::floor((phase+at)/factor))%16))){emit(at/length);emit(next/length);} at=next;}
    phase=std::fmod(phase+length,double(16*factor));
  };
  if(mode==1)for(size_t i=1;i<n;i+=2){phase=0;segment(i-1,i);}
  else {for(size_t i=1;i<n;++i)segment(i-1,i);if(mode==2&&n>1)segment(n-1,0);}
  return out;
}
}
