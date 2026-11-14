#pragma once

// Selection/feedback bookkeeping. Draw integration must feed actual clipped
// primitives and transformed window coordinates into this state. Calling hit()
// is not itself proof that a primitive survived clipping.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace vexa_select {
constexpr unsigned kInvalidEnum=0x0500, kInvalidValue=0x0501, kInvalidOperation=0x0502;
constexpr unsigned kStackOverflow=0x0503, kStackUnderflow=0x0504;
constexpr unsigned kRender=0x1c00, kFeedback=0x1c01, kSelect=0x1c02;
constexpr unsigned kPassThroughToken=0x0700;
constexpr size_t kMaxNameDepth=64;
struct FeedbackVertex {
  std::array<float,4> position{}; // window x/y/z and clip w
  std::array<float,4> color{};
  std::array<float,4> texture{};
  float index=0;
};
struct State {
  unsigned mode=kRender, feedback_type=0;
  uint32_t* select_buffer=nullptr;
  float* feedback_buffer=nullptr;
  int select_size=0, feedback_size=0;
  bool select_set=false, feedback_set=false, overflow=false, hit_pending=false;
  int used=0, hits=0;
  double zmin=1, zmax=0;
  std::vector<uint32_t> names;

  unsigned selectBuffer(int size, uint32_t* buffer) {
    if(mode==kSelect) return kInvalidOperation;
    if(size<0) return kInvalidValue;
    select_size=size; select_buffer=buffer; select_set=true; return 0;
  }
  unsigned feedbackBuffer(int size, unsigned type, float* buffer) {
    if(mode==kFeedback) return kInvalidOperation;
    if(size<0) return kInvalidValue;
    if(type<0x0600 || type>0x0604) return kInvalidEnum;
    feedback_size=size; feedback_type=type; feedback_buffer=buffer; feedback_set=true; return 0;
  }
  static uint32_t depth(double value) {
    value=std::max(0.0,std::min(1.0,value));
    return static_cast<uint32_t>(std::round(value*double(std::numeric_limits<uint32_t>::max())));
  }
  void flushHit() {
    if(!hit_pending) return;
    auto write=[&](uint32_t value) {
      if(used<select_size && select_buffer) select_buffer[used++]=value;
      else overflow=true;
    };
    write(static_cast<uint32_t>(names.size())); write(depth(zmin)); write(depth(zmax));
    for(auto name:names) write(name);
    ++hits; hit_pending=false; zmin=1; zmax=0;
  }
  void hit(double minimum, double maximum) {
    if(mode!=kSelect) return;
    if(!std::isfinite(minimum) || !std::isfinite(maximum)) return;
    if(minimum>maximum) std::swap(minimum,maximum);
    hit_pending=true; zmin=std::min(zmin,minimum); zmax=std::max(zmax,maximum);
  }
  unsigned renderMode(unsigned next, int& result) {
    if(next!=kRender && next!=kFeedback && next!=kSelect) return kInvalidEnum;
    if(next==kSelect && !select_set) return kInvalidOperation;
    if(next==kFeedback && !feedback_set) return kInvalidOperation;
    if(mode==kSelect) {flushHit();result=overflow ? -1 : hits;}
    else if(mode==kFeedback) result=overflow ? -1 : used;
    else result=0;
    mode=next; used=hits=0; overflow=hit_pending=false;zmin=1;zmax=0; names.clear();
    return 0;
  }
  unsigned initNames() {
    if(mode==kSelect) {flushHit(); names.clear();} return 0;
  }
  unsigned pushName(uint32_t name) {
    if(mode!=kSelect) return 0;
    if(names.size()>=kMaxNameDepth) return kStackOverflow;
    flushHit(); names.push_back(name); return 0;
  }
  unsigned popName() {
    if(mode!=kSelect) return 0;
    if(names.empty()) return kStackUnderflow;
    flushHit(); names.pop_back(); return 0;
  }
  unsigned loadName(uint32_t name) {
    if(mode!=kSelect) return 0;
    if(names.empty()) return kInvalidOperation;
    flushHit(); names.back()=name; return 0;
  }
  void feedbackValue(float value) {
    if(mode!=kFeedback) return;
    if(used>=feedback_size || !feedback_buffer) overflow=true;
    else feedback_buffer[used]=value;
    if(used<std::numeric_limits<int>::max()) ++used; else overflow=true;
  }
  void passThrough(float token) {
    if(mode==kFeedback) {feedbackValue(static_cast<float>(kPassThroughToken));feedbackValue(token);}
  }
  void feedbackVertex(const FeedbackVertex& vertex, bool color_index) {
    if(mode!=kFeedback) return;
    feedbackValue(vertex.position[0]);feedbackValue(vertex.position[1]);
    if(feedback_type>=0x0601) feedbackValue(vertex.position[2]);
    if(feedback_type==0x0604) feedbackValue(vertex.position[3]);
    if(feedback_type>=0x0602) {
      if(color_index) feedbackValue(vertex.index);
      else for(float component:vertex.color) feedbackValue(component);
    }
    if(feedback_type>=0x0603) for(float component:vertex.texture) feedbackValue(component);
  }
  void feedbackPrimitive(unsigned token,const FeedbackVertex* vertices,size_t count,bool color_index) {
    if(mode!=kFeedback) return;
    feedbackValue(static_cast<float>(token));
    if(token==0x0703) feedbackValue(static_cast<float>(count)); // GL_POLYGON_TOKEN
    for(size_t i=0;i<count;++i) feedbackVertex(vertices[i],color_index);
  }
};
}
