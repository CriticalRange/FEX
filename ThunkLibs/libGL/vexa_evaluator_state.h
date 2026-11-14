#pragma once

// OpenGL 1.x evaluator mathematics and map storage. This deliberately has no
// GL entrypoints: the caller must attach it to each GL context and display-list
// state before exposing the evaluator API.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace vexa_eval {
constexpr unsigned kMap1 = 0x0d90, kMap2 = 0x0db0;
constexpr unsigned kInvalidEnum = 0x0500, kInvalidValue = 0x0501;
constexpr int kMaxOrder = 30;
inline int width(unsigned slot) {
  constexpr int widths[9] = {4, 1, 3, 1, 2, 3, 4, 3, 4};
  return slot < 9 ? widths[slot] : 0;
}
inline int target_slot(unsigned target, int dimensions) {
  const unsigned base = dimensions == 1 ? kMap1 : kMap2;
  return target >= base && target < base + 9 ? static_cast<int>(target - base) : -1;
}
struct Map {
  int components = 0, uorder = 1, vorder = 1;
  double u1 = 0, u2 = 1, v1 = 0, v2 = 1;
  std::vector<double> points;
};
struct Grid {
  int un = 1, vn = 1;
  double u1 = 0, u2 = 1, v1 = 0, v2 = 1;
};
struct State {
  std::array<Map, 9> map1, map2;
  std::array<bool, 9> enabled1{}, enabled2{};
  bool auto_normal = false;
  Grid grid1, grid2;

  State() {
    for (unsigned i = 0; i < 9; ++i) {
      for (Map* map : {&map1[i], &map2[i]}) {
        map->components = width(i);
        map->points.assign(map->components, 0.0);
        if (i == 0) map->points = {1, 1, 1, 1};
        if (i == 1) map->points = {1};
        if (i == 2) map->points = {0, 0, 1};
        if (i == 6 || i == 8) map->points[3] = 1;
      }
    }
  }

  template<typename T>
  unsigned set1(unsigned target, double u1, double u2, int stride, int order, const T* points) {
    const int slot = target_slot(target, 1);
    if (slot < 0) return kInvalidEnum;
    const int n = width(slot);
    if (u1 == u2 || stride < n || order < 1 || order > kMaxOrder || !points) return kInvalidValue;
    Map result; result.components=n; result.u1=u1; result.u2=u2; result.uorder=order;
    result.points.resize(static_cast<size_t>(order)*n);
    for (int u=0; u<order; ++u) for (int c=0; c<n; ++c)
      result.points[static_cast<size_t>(u)*n+c] = points[static_cast<size_t>(u)*stride+c];
    map1[slot]=std::move(result);
    return 0;
  }
  template<typename T>
  unsigned set2(unsigned target, double u1, double u2, int ustride, int uorder,
                double v1, double v2, int vstride, int vorder, const T* points) {
    const int slot=target_slot(target, 2);
    if (slot < 0) return kInvalidEnum;
    const int n=width(slot);
    if (u1==u2 || v1==v2 || ustride<n || vstride<n || uorder<1 || vorder<1 ||
        uorder>kMaxOrder || vorder>kMaxOrder || !points) return kInvalidValue;
    Map result; result.components=n; result.u1=u1; result.u2=u2; result.uorder=uorder;
    result.v1=v1; result.v2=v2; result.vorder=vorder;
    result.points.resize(static_cast<size_t>(uorder)*vorder*n);
    for (int u=0; u<uorder; ++u) for (int v=0; v<vorder; ++v) for (int c=0; c<n; ++c)
      result.points[(static_cast<size_t>(u)*vorder+v)*n+c] =
        points[static_cast<size_t>(u)*ustride+static_cast<size_t>(v)*vstride+c];
    map2[slot]=std::move(result);
    return 0;
  }
  unsigned set_grid1(int un, double u1, double u2) {
    if (un < 1) return kInvalidValue;
    grid1.un=un; grid1.u1=u1; grid1.u2=u2; return 0;
  }
  unsigned set_grid2(int un, double u1, double u2, int vn, double v1, double v2) {
    if (un < 1 || vn < 1) return kInvalidValue;
    grid2.un=un; grid2.u1=u1; grid2.u2=u2;
    grid2.vn=vn; grid2.v1=v1; grid2.v2=v2; return 0;
  }
  unsigned query(unsigned target, unsigned name, std::vector<double>& values) const {
    int slot=target_slot(target,1); int dimensions=1;
    if (slot<0) { slot=target_slot(target,2); dimensions=2; }
    if (slot<0) return kInvalidEnum;
    const Map& map=dimensions==1 ? map1[slot] : map2[slot];
    switch (name) {
      case 0x0a00: values=map.points; break; // GL_COEFF
      case 0x0a01: values=dimensions==1 ? std::vector<double>{double(map.uorder)} :
                                          std::vector<double>{double(map.uorder),double(map.vorder)}; break;
      case 0x0a02: values=dimensions==1 ? std::vector<double>{map.u1,map.u2} :
                                          std::vector<double>{map.u1,map.u2,map.v1,map.v2}; break;
      default: return kInvalidEnum;
    }
    return 0;
  }
};

// de Casteljau interpolation works for values outside [0,1] as required by GL.
inline std::array<double, 4> curve(const double* points, int order, int n, double t) {
  std::array<double, kMaxOrder*4> work{};
  for (int i=0; i<order; ++i) for (int c=0; c<n; ++c) work[i*4+c]=points[i*n+c];
  for (int degree=order-1; degree>0; --degree)
    for (int i=0; i<degree; ++i) for (int c=0; c<n; ++c)
      work[i*4+c]=(1-t)*work[i*4+c]+t*work[(i+1)*4+c];
  return {work[0], work[1], work[2], work[3]};
}
inline std::array<double, 4> evaluate1(const Map& map, double u) {
  return curve(map.points.data(), map.uorder, map.components, (u-map.u1)/(map.u2-map.u1));
}
inline std::array<double, 4> evaluate2(const Map& map, double u, double v) {
  std::array<double, kMaxOrder*4> rows{};
  const double tv=(v-map.v1)/(map.v2-map.v1);
  for (int i=0; i<map.uorder; ++i) {
    const auto value=curve(map.points.data()+static_cast<size_t>(i)*map.vorder*map.components,
                           map.vorder, map.components, tv);
    for (int c=0; c<map.components; ++c) rows[i*map.components+c]=value[c];
  }
  return curve(rows.data(), map.uorder, map.components, (u-map.u1)/(map.u2-map.u1));
}
inline std::array<double, 4> derivative2(const Map& map, double u, double v, bool along_u) {
  const int order=along_u ? map.uorder : map.vorder;
  if (order==1) return {};
  Map d=map;
  if (along_u) --d.uorder; else --d.vorder;
  d.points.resize(static_cast<size_t>(d.uorder)*d.vorder*d.components);
  const double scale=(order-1)/(along_u ? map.u2-map.u1 : map.v2-map.v1);
  for (int i=0; i<d.uorder; ++i) for (int j=0; j<d.vorder; ++j) for (int c=0; c<d.components; ++c) {
    const auto first=(static_cast<size_t>(i)*map.vorder+j)*map.components+c;
    const auto second=(static_cast<size_t>(i+(along_u?1:0))*map.vorder+j+(along_u?0:1))*map.components+c;
    d.points[(static_cast<size_t>(i)*d.vorder+j)*d.components+c]=(map.points[second]-map.points[first])*scale;
  }
  return evaluate2(d,u,v);
}
inline std::array<double, 3> automatic_normal(const Map& map, double u, double v) {
  auto du=derivative2(map,u,v,true), dv=derivative2(map,u,v,false);
  if (map.components==4) {
    const auto p=evaluate2(map,u,v);
    for (int i=0; i<3; ++i) { du[i]=du[i]*p[3]-du[3]*p[i]; dv[i]=dv[i]*p[3]-dv[3]*p[i]; }
  }
  std::array<double,3> n={du[1]*dv[2]-du[2]*dv[1],du[2]*dv[0]-du[0]*dv[2],du[0]*dv[1]-du[1]*dv[0]};
  const double length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
  if (length>0) for (double& c : n) c/=length;
  return n;
}

// Callback must implement color(v), index(v), normal(v), texcoord(v,n),
// vertex(v,n). Non-vertex attributes precede the vertex as required by GL.
template<class Sink>
void emit1(const State& state, double u, Sink& sink) {
  const auto& e=state.enabled1; const auto& m=state.map1;
  if(e[0]) sink.color(evaluate1(m[0],u));
  if(e[1]) sink.index(evaluate1(m[1],u)[0]);
  if(e[2]) {const auto n=evaluate1(m[2],u);sink.normal(std::array<double,3>{n[0],n[1],n[2]});}
  for(int i=6;i>=3;--i) if(e[i]) {sink.texcoord(evaluate1(m[i],u),width(i));break;}
  if(e[8]) sink.vertex(evaluate1(m[8],u),4);
  else if(e[7]) sink.vertex(evaluate1(m[7],u),3);
}
template<class Sink>
void emit2(const State& state, double u, double v, Sink& sink) {
  const auto& e=state.enabled2; const auto& m=state.map2;
  if(e[0]) sink.color(evaluate2(m[0],u,v));
  if(e[1]) sink.index(evaluate2(m[1],u,v)[0]);
  const int vertex=e[8]?8:(e[7]?7:-1);
  if(state.auto_normal && vertex>=0) sink.normal(automatic_normal(m[vertex],u,v));
  else if(e[2]) {const auto n=evaluate2(m[2],u,v);sink.normal(std::array<double,3>{n[0],n[1],n[2]});}
  for(int i=6;i>=3;--i) if(e[i]) {sink.texcoord(evaluate2(m[i],u,v),width(i));break;}
  if(vertex>=0) sink.vertex(evaluate2(m[vertex],u,v),width(vertex));
}

// Callbacks begin/end use desktop GL primitive enums. This layer does not
// change evaluator enable state, model-view state, or context ownership.
template<class Sink>
unsigned mesh1(const State& state, unsigned mode, int i1, int i2, Sink& sink) {
  if(mode!=0x1b00 && mode!=0x1b01) return kInvalidEnum; // POINT / LINE
  sink.begin(mode==0x1b00 ? 0x0000 : 0x0003); // POINTS / LINE_STRIP
  for(int64_t i=i1;i<=i2;++i) {
    const double u=state.grid1.u1+(state.grid1.u2-state.grid1.u1)*i/state.grid1.un;
    emit1(state,u,sink);
  }
  sink.end(); return 0;
}
template<class Sink>
unsigned mesh2(const State& state, unsigned mode, int i1, int i2, int j1, int j2, Sink& sink) {
  if(mode!=0x1b00 && mode!=0x1b01 && mode!=0x1b02) return kInvalidEnum;
  const auto u=[&](int64_t i) {return state.grid2.u1+(state.grid2.u2-state.grid2.u1)*i/state.grid2.un;};
  const auto v=[&](int64_t j) {return state.grid2.v1+(state.grid2.v2-state.grid2.v1)*j/state.grid2.vn;};
  if(mode==0x1b00) {
    sink.begin(0x0000);
    for(int64_t j=j1;j<=j2;++j) for(int64_t i=i1;i<=i2;++i) emit2(state,u(i),v(j),sink);
    sink.end();
  } else if(mode==0x1b01) {
    for(int64_t j=j1;j<=j2;++j) {sink.begin(0x0003);for(int64_t i=i1;i<=i2;++i) emit2(state,u(i),v(j),sink);sink.end();}
    for(int64_t i=i1;i<=i2;++i) {sink.begin(0x0003);for(int64_t j=j1;j<=j2;++j) emit2(state,u(i),v(j),sink);sink.end();}
  } else {
    for(int64_t j=j1;j<j2;++j) {
      sink.begin(0x0008); // GL_QUAD_STRIP
      for(int64_t i=i1;i<=i2;++i) {emit2(state,u(i),v(j),sink);emit2(state,u(i),v(j+1),sink);}
      sink.end();
    }
  }
  return 0;
}
} // namespace vexa_eval
