#pragma once
#include "vexa_selection_state.h"
#include "raster_position_core.h"

namespace vexa_select {
struct Vertex {
  vexa_raster::Vec4 clip{}, eye{}, color{}, texture{};
};
inline Vertex mix(const Vertex& a, const Vertex& b, double t) {
  Vertex out;
  for (unsigned i=0;i<4;++i) {
    out.clip[i]=a.clip[i]+t*(b.clip[i]-a.clip[i]);
    out.eye[i]=a.eye[i]+t*(b.eye[i]-a.eye[i]);
    out.color[i]=a.color[i]+t*(b.color[i]-a.color[i]);
    out.texture[i]=a.texture[i]+t*(b.texture[i]-a.texture[i]);
  }
  return out;
}
inline double distance(const Vertex& v, unsigned plane, const vexa_raster::Input& input) {
  if (plane<6) return v.clip[3]+((plane&1)?-1:1)*v.clip[plane/2];
  if (!input.clip_enabled[plane-6]) return 1;
  double d=0; for(unsigned i=0;i<4;++i)d+=v.eye[i]*input.clip_planes[plane-6][i];
  return d;
}
inline std::vector<Vertex> clip(std::vector<Vertex> vertices, const vexa_raster::Input& input) {
  const auto size=vertices.size();
  for(unsigned plane=0;plane<12 && !vertices.empty();++plane) {
    if(size<=2) {
      double a=distance(vertices[0],plane,input), b=distance(vertices.back(),plane,input);
      if(!std::isfinite(a)||!std::isfinite(b)||(a<0&&b<0)) return {};
      if(a<0)vertices[0]=mix(vertices[0],vertices.back(),a/(a-b));
      else if(b<0)vertices.back()=mix(vertices[0],vertices.back(),a/(a-b));
    } else {
      std::vector<Vertex> output;
      auto prev=vertices.back(); double a=distance(prev,plane,input);
      for(const auto& current:vertices) {
        const double b=distance(current,plane,input);
        if(!std::isfinite(a)||!std::isfinite(b))return {};
        if((a<0)!=(b<0))output.push_back(mix(prev,current,a/(a-b)));
        if(b>=0)output.push_back(current);
        prev=current;a=b;
      }
      vertices=std::move(output);
    }
  }
  for(const auto& v:vertices)if(v.clip[3]<=0)return {};
  return vertices;
}
inline FeedbackVertex window(const Vertex& vertex, const vexa_raster::Input& input) {
  FeedbackVertex out;
  const double w=vertex.clip[3];
  out.position={float(input.viewport[0]+(vertex.clip[0]/w+1)*input.viewport[2]*.5),
    float(input.viewport[1]+(vertex.clip[1]/w+1)*input.viewport[3]*.5),
    float(input.depth_range[0]+(vertex.clip[2]/w+1)*(input.depth_range[1]-input.depth_range[0])*.5),float(w)};
  std::copy(vertex.color.begin(),vertex.color.end(),out.color.begin());
  std::copy(vertex.texture.begin(),vertex.texture.end(),out.texture.begin());
  return out;
}
inline void draw(State& state,const std::vector<float>& data,unsigned mode,const vexa_raster::Input& input,
                 bool cull=false,unsigned cullFace=0x0405,unsigned frontFace=0x0901) {
  const size_t count=data.size()/15;
  auto vertex=[&](size_t n) {
    Vertex out; const auto* p=data.data()+n*15;
    out.eye=vexa_raster::transform(input.modelview,{p[0],p[1],p[2],p[3]});
    out.clip=vexa_raster::transform(input.projection,out.eye);
    std::copy_n(p+7,4,out.color.begin());std::copy_n(p+11,4,out.texture.begin());return out;
  };
  auto emit=[&](std::vector<Vertex> polygon,unsigned token) {
    polygon=clip(std::move(polygon),input);if(polygon.empty())return;
    std::vector<FeedbackVertex> values;for(const auto& p:polygon)values.push_back(window(p,input));
    if(values.size()>=3 && cull) {
      double area=0;for(size_t i=0;i<values.size();++i){const auto&a=values[i].position;const auto&b=values[(i+1)%values.size()].position;area+=a[0]*b[1]-b[0]*a[1];}
      const bool front=(area>=0)==(frontFace==0x0901);
      if(cullFace==0x0408||(front&&cullFace==0x0404)||(!front&&cullFace==0x0405))return;
    }
    if(state.mode==kSelect){double low=1,high=0;for(const auto&v:values){low=std::min(low,double(v.position[2]));high=std::max(high,double(v.position[2]));}state.hit(low,high);}
    else state.feedbackPrimitive(token,values.data(),values.size(),false);
  };
  if(mode==0){for(size_t i=0;i<count;++i)emit({vertex(i)},0x0701);}
  else if(mode==1){for(size_t i=0;i+1<count;i+=2)emit({vertex(i),vertex(i+1)},0x0707);}
  else if(mode==2||mode==3){for(size_t i=1;i<count;++i)emit({vertex(i-1),vertex(i)},i==1?0x0707:0x0702);if(mode==2&&count>1)emit({vertex(count-1),vertex(0)},0x0702);}
  else if(mode==4){for(size_t i=0;i+2<count;i+=3)emit({vertex(i),vertex(i+1),vertex(i+2)},0x0703);}
  else if(mode==5){for(size_t i=2;i<count;++i)emit({vertex(i-2+(i&1)),vertex(i-1-(i&1)),vertex(i)},0x0703);}
  else if(mode==6){for(size_t i=2;i<count;++i)emit({vertex(0),vertex(i-1),vertex(i)},0x0703);}
  else if(mode==7){for(size_t i=0;i+3<count;i+=4)emit({vertex(i),vertex(i+1),vertex(i+2),vertex(i+3)},0x0703);}
  else if(mode==8){for(size_t i=0;i+3<count;i+=2)emit({vertex(i),vertex(i+1),vertex(i+3),vertex(i+2)},0x0703);}
  else if(mode==9&&count>=3){std::vector<Vertex> polygon;for(size_t i=0;i<count;++i)polygon.push_back(vertex(i));emit(std::move(polygon),0x0703);}
}
}
