#pragma once

// A value-capturing adapter for evaluator output. The GL renderer must retain
// these attributes per vertex; updating the current state for each evaluated
// vertex is incorrect because glEvalCoord must not change current attributes.
#include "vexa_evaluator_state.h"
#include <array>
#include <functional>
#include <vector>

namespace vexa_eval {
struct Vertex {
  std::array<double,4> position{0,0,0,1};
  std::array<double,3> normal{0,0,1};
  std::array<double,4> color{1,1,1,1};
  std::array<double,4> texcoord{0,0,0,1};
  double index=1;
};
struct Primitive {
  unsigned mode=0;
  std::vector<Vertex> vertices;
};

// Instantiate with snapshots of the current attributes each time an
// EvalCoord or EvalMesh command executes (not when a display list compiles).
class Capture {
public:
  explicit Capture(const Vertex& current) : base_(current), next_(current) {}
  void begin(unsigned mode) { in_primitive_=true; primitive_.mode=mode; primitive_.vertices.clear(); }
  void end() { primitives_.push_back(std::move(primitive_)); primitive_=Primitive{}; in_primitive_=false; }
  void color(const std::array<double,4>& value) { next_.color=value; }
  void index(double value) { next_.index=value; }
  void normal(const std::array<double,3>& value) { next_.normal=value; }
  void texcoord(const std::array<double,4>& value,int width) {
    next_.texcoord={0,0,0,1};
    for(int i=0;i<width && i<4;++i) next_.texcoord[i]=value[i];
  }
  void vertex(const std::array<double,4>& value,int width) {
    next_.position={0,0,0,1};
    for(int i=0;i<width && i<4;++i) next_.position[i]=value[i];
    if(in_primitive_) primitive_.vertices.push_back(next_);
    else standalone_.push_back(next_);
    next_=base_;
  }
  const std::vector<Vertex>& standalone() const { return standalone_; }
  const std::vector<Primitive>& primitives() const { return primitives_; }
private:
  Vertex base_, next_;
  Primitive primitive_;
  bool in_primitive_=false;
  std::vector<Vertex> standalone_;
  std::vector<Primitive> primitives_;
};
} // namespace vexa_eval
