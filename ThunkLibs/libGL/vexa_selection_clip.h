#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

// Clip-space selection helpers. The caller is responsible for model/view and
// projection, user clip planes, primitive assembly, and front/back culling.
namespace vexa_select {
struct ClipVertex { double x,y,z,w; };
inline double planeDistance(const ClipVertex& v, unsigned plane) {
  switch(plane) {
    case 0: return v.w+v.x; case 1: return v.w-v.x;
    case 2: return v.w+v.y; case 3: return v.w-v.y;
    case 4: return v.w+v.z; default: return v.w-v.z;
  }
}
inline ClipVertex interpolate(const ClipVertex& a,const ClipVertex& b,double t) {
  return {a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t,a.w+(b.w-a.w)*t};
}
inline bool clipPoint(const ClipVertex& p) {
  if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||!std::isfinite(p.w)) return false;
  for(unsigned plane=0;plane<6;++plane) if(planeDistance(p,plane)<0) return false;
  return p.w>0;
}
inline bool clipLine(ClipVertex& a,ClipVertex& b) {
  for(unsigned plane=0;plane<6;++plane) {
    const double da=planeDistance(a,plane), db=planeDistance(b,plane);
    if(!std::isfinite(da)||!std::isfinite(db) || (da<0 && db<0)) return false;
    if(da<0) a=interpolate(a,b,da/(da-db));
    else if(db<0) b=interpolate(a,b,da/(da-db));
  }
  return a.w>0 && b.w>0;
}
inline std::vector<ClipVertex> clipPolygon(std::vector<ClipVertex> vertices) {
  for(unsigned plane=0;plane<6 && !vertices.empty();++plane) {
    std::vector<ClipVertex> out;
    auto prev=vertices.back(); double dp=planeDistance(prev,plane);
    if(!std::isfinite(dp)) return {};
    for(const auto& current:vertices) {
      const double dc=planeDistance(current,plane); if(!std::isfinite(dc)) return {};
      if((dp<0)!=(dc<0)) out.push_back(interpolate(prev,current,dp/(dp-dc)));
      if(dc>=0) out.push_back(current);
      prev=current;dp=dc;
    }
    vertices=std::move(out);
  }
  vertices.erase(std::remove_if(vertices.begin(),vertices.end(),[](const ClipVertex& v){return v.w<=0;}),vertices.end());
  return vertices;
}
inline bool depthInterval(const std::vector<ClipVertex>& vertices,double depth_near,double depth_far,double& low,double& high) {
  if(vertices.empty()) return false;
  low=1;high=0;
  for(const auto& vertex:vertices) {
    if(vertex.w<=0) return false;
    const double z=depth_near+(depth_far-depth_near)*(vertex.z/vertex.w+1)*0.5;
    if(!std::isfinite(z)) return false;
    low=std::min(low,z); high=std::max(high,z);
  }
  return true;
}
}
