#pragma once
// Requires `vexa_ff::State::lighting`, legacy_lighting_state.h and the guest
// thunk's existing VEXA_GL_GUEST_EXPORT / VEXA_CompatError bridge. Include this
// once after the existing guest GL declarations. Replace the former no-op
// glLightfv, glMaterialfv and glTexEnv* definitions before inclusion.
// Rendering must consume this state before these entry points are exposed.
#include <array>
#include <climits>
#include <cmath>
#include <limits>

namespace vexa_lighting_guest {
using namespace vexa_legacy_lighting;
using Value = std::array<float,4>;
inline void report(uint32_t error) { if (error) fexfn_pack_VEXA_CompatError(error); }
inline Mat4 model() {
  Mat4 m{}; for (int i=0; i<16; ++i) m[i]=vexa_ff::S().model[i]; return m;
}
inline Value values(const GLfloat *p, unsigned n) {
  Value v{}; for (unsigned i=0; i<n; ++i) v[i]=p[i]; return v;
}
inline float normalized(GLint n) { return std::max(-1.f, static_cast<float>(double(n)/2147483647.0)); }
inline Value values(const GLint *p, unsigned n, bool normalize) {
  Value v{}; for (unsigned i=0; i<n; ++i) v[i]=normalize ? normalized(p[i]) : static_cast<float>(p[i]); return v;
}
inline Value values(const GLdouble *p, unsigned n) {
  Value v{}; for (unsigned i=0; i<n; ++i) v[i]=static_cast<float>(p[i]); return v;
}
inline GLint integer(float v, bool normalize=false) {
  if (normalize) v=std::clamp(v,-1.f,1.f)*static_cast<float>(INT_MAX);
  if (std::isnan(v)) return 0;
  if (double(v)>=double(INT_MAX)) return INT_MAX;
  if (double(v)<=double(INT_MIN)) return INT_MIN;
  return static_cast<GLint>(std::round(v));
}
inline unsigned light_count(GLenum pname) {
  switch(pname) { case ambient: case diffuse: case specular: case position: return 4;
    case spot_direction: return 3; case spot_exponent: case spot_cutoff: case constant_attenuation:
    case linear_attenuation: case quadratic_attenuation: return 1; default: return 0; }
}
inline unsigned material_count(GLenum pname) {
  switch(pname) { case ambient: case diffuse: case specular: case emission: case ambient_diffuse: return 4;
    case shininess: return 1; case color_indexes: return 3; default: return 0; }
}
inline unsigned fog_count(GLenum pname) {
  switch(pname) { case fog_color: return 4; case fog_mode: case fog_density: case fog_start:
    case fog_end: case fog_index: return 1; default: return 0; }
}
inline unsigned model_count(GLenum pname) {
  switch(pname) { case light_model_ambient: return 4; case light_model_local_viewer: case light_model_two_side: return 1; default: return 0; }
}
inline int tex_unit() {
  GLint unit=0; glGetIntegerv(0x84e0, &unit); unit-=0x84c0;
  return unit>=0 && unit<8 ? unit : -1;
}
inline int coord(GLenum c) { return c>=coord_s && c<=coord_q ? static_cast<int>(c-coord_s) : -1; }
inline void queue_fog(GLenum pname, Value v) {
  vexa_ff::run_or_record_callback([pname,v] { report(vexa_ff::S().lighting.fog.set(pname,v.data())); });
}
inline void queue_light(GLenum light, GLenum pname, Value v) {
  if (light<0x4000 || light>=0x4008) { report(invalid_enum); return; }
  vexa_ff::run_or_record_callback([light,pname,v] { report(vexa_ff::S().lighting.lights[light-0x4000].set(pname,v.data(),model())); });
}
inline void queue_light_model(GLenum pname, Value v) {
  vexa_ff::run_or_record_callback([pname,v] { report(vexa_ff::S().lighting.set_light_model(pname,v.data())); });
}
inline void queue_material(GLenum face, GLenum pname, Value v) {
  vexa_ff::run_or_record_callback([face,pname,v] {
    report(vexa_ff::S().lighting.set_material(face,pname,v.data()));
  });
}
inline void queue_texgen(GLenum coordinate, GLenum pname, Value v) {
  const int c=coord(coordinate); if (c<0) { report(invalid_enum); return; }
  vexa_ff::run_or_record_callback([c,pname,v] {
    const int unit=tex_unit(); if (unit<0) { report(invalid_value); return; }
    report(vexa_ff::S().lighting.texgen[unit][c].set(c,pname,v.data(),model()));
  });
}
inline void queue_texenv(GLenum target, GLenum pname, Value v) {
  vexa_ff::run_or_record_callback([target,pname,v] {
    const int unit=tex_unit(); if (unit<0) { report(invalid_value); return; }
    report(vexa_ff::S().lighting.texenv[unit].set(target,pname,v.data()));
  });
}
inline bool set_cap(GLenum cap, bool enabled) {
  if (cap != 0x0de1 && cap != 0x0b60 && cap != 0x0b50 && cap != 0x0b57 && cap != 0x0ba1 &&
      !(cap>=0x4000 && cap<0x4008) && !(cap>=0x0c60 && cap<=0x0c63)) return false;
  vexa_ff::run_or_record_callback([cap,enabled] {
    auto &state=vexa_ff::S().lighting;
    if (cap==0x0de1) { int unit=tex_unit(); if (unit>=0) state.texture_2d_enabled[unit]=enabled; }
    else if (cap==0x0b60) state.fog_enabled=enabled;
    else if (cap==0x0b50) state.lighting_enabled=enabled;
    else if (cap==0x0b57) state.color_material_enabled=enabled;
    else if (cap==0x0ba1) state.normalize_enabled=enabled;
    else if (cap>=0x4000 && cap<0x4008) state.light_enabled[cap-0x4000]=enabled;
    else { int unit=tex_unit(); if (unit>=0) state.texgen_enabled[unit][cap-0x0c60]=enabled; }
  });
  return true;
}
inline bool get_cap(GLenum cap, GLboolean &result) {
  auto &state=vexa_ff::S().lighting; bool enabled;
  if (cap==0x0de1) { int unit=tex_unit(); enabled=unit>=0 && state.texture_2d_enabled[unit]; }
  else if (cap==0x0b60) enabled=state.fog_enabled;
  else if (cap==0x0b50) enabled=state.lighting_enabled;
  else if (cap==0x0b57) enabled=state.color_material_enabled;
  else if (cap==0x0ba1) enabled=state.normalize_enabled;
  else if (cap>=0x4000 && cap<0x4008) enabled=state.light_enabled[cap-0x4000];
  else if (cap>=0x0c60 && cap<=0x0c63) { int unit=tex_unit(); enabled=unit>=0 && state.texgen_enabled[unit][cap-0x0c60]; }
  else return false;
  result=enabled ? GL_TRUE : GL_FALSE; return true;
}
inline void current_color(const float *v) {
  auto &state=vexa_ff::S().lighting;
  if (state.color_material_enabled) report(state.update_color_material(v));
}
} // namespace vexa_lighting_guest

VEXA_GL_GUEST_EXPORT void glFogfv(GLenum pname, const GLfloat *p) {
  const auto n=vexa_lighting_guest::fog_count(pname); if (!n) { vexa_lighting_guest::report(0x0500); return; }
  if (p) vexa_lighting_guest::queue_fog(pname,vexa_lighting_guest::values(p,n));
}
VEXA_GL_GUEST_EXPORT void glFogf(GLenum pname, GLfloat v) {
  if (vexa_lighting_guest::fog_count(pname)!=1) { vexa_lighting_guest::report(0x0500); return; } glFogfv(pname,&v);
}
VEXA_GL_GUEST_EXPORT void glFogiv(GLenum pname, const GLint *p) {
  const auto n=vexa_lighting_guest::fog_count(pname); if (!n) { vexa_lighting_guest::report(0x0500); return; }
  if (p) vexa_lighting_guest::queue_fog(pname,vexa_lighting_guest::values(p,n,pname==vexa_legacy_lighting::fog_color));
}
VEXA_GL_GUEST_EXPORT void glFogi(GLenum pname, GLint v) {
  if (vexa_lighting_guest::fog_count(pname)!=1) { vexa_lighting_guest::report(0x0500); return; } glFogiv(pname,&v);
}
VEXA_GL_GUEST_EXPORT void glLightfv(GLenum light, GLenum pname, const GLfloat *p) {
  const auto n=vexa_lighting_guest::light_count(pname); if (!n) { vexa_lighting_guest::report(0x0500); return; }
  if (p) vexa_lighting_guest::queue_light(light,pname,vexa_lighting_guest::values(p,n));
}
VEXA_GL_GUEST_EXPORT void glLightf(GLenum light, GLenum pname, GLfloat v) {
  if (vexa_lighting_guest::light_count(pname)!=1) { vexa_lighting_guest::report(0x0500); return; } glLightfv(light,pname,&v);
}
VEXA_GL_GUEST_EXPORT void glLightiv(GLenum light, GLenum pname, const GLint *p) {
  const auto n=vexa_lighting_guest::light_count(pname); if (!n) { vexa_lighting_guest::report(0x0500); return; }
  if (p) vexa_lighting_guest::queue_light(light,pname,vexa_lighting_guest::values(p,n,pname>=0x1200 && pname<=0x1202));
}
VEXA_GL_GUEST_EXPORT void glLighti(GLenum light, GLenum pname, GLint v) {
  if (vexa_lighting_guest::light_count(pname)!=1) { vexa_lighting_guest::report(0x0500); return; } glLightiv(light,pname,&v);
}
VEXA_GL_GUEST_EXPORT void glGetLightfv(GLenum light, GLenum pname, GLfloat *p) {
  if (!p) return; if (light<0x4000 || light>=0x4008) { vexa_lighting_guest::report(0x0500); return; }
  unsigned n=0; auto v=vexa_ff::S().lighting.lights[light-0x4000].get(pname,n);
  if (!v) { vexa_lighting_guest::report(0x0500); return; } std::copy_n(v,n,p);
}
VEXA_GL_GUEST_EXPORT void glGetLightiv(GLenum light, GLenum pname, GLint *p) {
  if (!p) return; if (light<0x4000 || light>=0x4008) { vexa_lighting_guest::report(0x0500); return; }
  unsigned n=0; auto v=vexa_ff::S().lighting.lights[light-0x4000].get(pname,n);
  if (!v) { vexa_lighting_guest::report(0x0500); return; }
  for(unsigned i=0;i<n;++i) p[i]=vexa_lighting_guest::integer(v[i],pname>=0x1200 && pname<=0x1202);
}
VEXA_GL_GUEST_EXPORT void glLightModelfv(GLenum pname, const GLfloat *p) {
  const auto n=vexa_lighting_guest::model_count(pname); if (!n) { vexa_lighting_guest::report(0x0500); return; }
  if (p) vexa_lighting_guest::queue_light_model(pname,vexa_lighting_guest::values(p,n));
}
VEXA_GL_GUEST_EXPORT void glLightModelf(GLenum pname, GLfloat v) {
  if (vexa_lighting_guest::model_count(pname)!=1) { vexa_lighting_guest::report(0x0500); return; } glLightModelfv(pname,&v);
}
VEXA_GL_GUEST_EXPORT void glLightModeliv(GLenum pname, const GLint *p) {
  const auto n=vexa_lighting_guest::model_count(pname); if (!n) { vexa_lighting_guest::report(0x0500); return; }
  if (p) vexa_lighting_guest::queue_light_model(pname,vexa_lighting_guest::values(p,n,pname==vexa_legacy_lighting::light_model_ambient));
}
VEXA_GL_GUEST_EXPORT void glLightModeli(GLenum pname, GLint v) {
  if (vexa_lighting_guest::model_count(pname)!=1) { vexa_lighting_guest::report(0x0500); return; } glLightModeliv(pname,&v);
}
VEXA_GL_GUEST_EXPORT void glMaterialfv(GLenum face, GLenum pname, const GLfloat *p) {
  const auto n=vexa_lighting_guest::material_count(pname); if (!n) { vexa_lighting_guest::report(0x0500); return; }
  if (p) vexa_lighting_guest::queue_material(face,pname,vexa_lighting_guest::values(p,n));
}
VEXA_GL_GUEST_EXPORT void glMaterialf(GLenum face, GLenum pname, GLfloat v) {
  if (vexa_lighting_guest::material_count(pname)!=1) { vexa_lighting_guest::report(0x0500); return; } glMaterialfv(face,pname,&v);
}
VEXA_GL_GUEST_EXPORT void glMaterialiv(GLenum face, GLenum pname, const GLint *p) {
  const auto n=vexa_lighting_guest::material_count(pname); if (!n) { vexa_lighting_guest::report(0x0500); return; }
  const bool norm=pname==0x1200 || pname==0x1201 || pname==0x1202 || pname==0x1600 || pname==0x1602;
  if (p) vexa_lighting_guest::queue_material(face,pname,vexa_lighting_guest::values(p,n,norm));
}
VEXA_GL_GUEST_EXPORT void glMateriali(GLenum face, GLenum pname, GLint v) {
  if (vexa_lighting_guest::material_count(pname)!=1) { vexa_lighting_guest::report(0x0500); return; } glMaterialiv(face,pname,&v);
}
VEXA_GL_GUEST_EXPORT void glGetMaterialfv(GLenum face, GLenum pname, GLfloat *p) {
  if (!p) return; if (face!=0x0404 && face!=0x0405) { vexa_lighting_guest::report(0x0500); return; }
  unsigned n=0; auto v=vexa_ff::S().lighting.materials[face==0x0404 ? 0 : 1].get(pname,n);
  if (!v) { vexa_lighting_guest::report(0x0500); return; } std::copy_n(v,n,p);
}
VEXA_GL_GUEST_EXPORT void glGetMaterialiv(GLenum face, GLenum pname, GLint *p) {
  if (!p) return; if (face!=0x0404 && face!=0x0405) { vexa_lighting_guest::report(0x0500); return; }
  unsigned n=0; auto v=vexa_ff::S().lighting.materials[face==0x0404 ? 0 : 1].get(pname,n);
  if (!v) { vexa_lighting_guest::report(0x0500); return; }
  bool norm=pname==0x1200 || pname==0x1201 || pname==0x1202 || pname==0x1600;
  for(unsigned i=0;i<n;++i) p[i]=vexa_lighting_guest::integer(v[i],norm);
}
VEXA_GL_GUEST_EXPORT void glColorMaterial(GLenum face, GLenum mode) {
  vexa_ff::run_or_record_callback([face,mode] { vexa_lighting_guest::report(vexa_ff::S().lighting.set_color_material(face,mode)); });
}
VEXA_GL_GUEST_EXPORT void glTexGenfv(GLenum coordinate, GLenum pname, const GLfloat *p) {
  const unsigned n=pname==0x2500 ? 1 : (pname==0x2501 || pname==0x2502 ? 4 : 0);
  if (!n) { vexa_lighting_guest::report(0x0500); return; } if(p) vexa_lighting_guest::queue_texgen(coordinate,pname,vexa_lighting_guest::values(p,n));
}
VEXA_GL_GUEST_EXPORT void glTexGenf(GLenum coordinate, GLenum pname, GLfloat v) {
  if (pname!=0x2500) { vexa_lighting_guest::report(0x0500); return; } glTexGenfv(coordinate,pname,&v);
}
VEXA_GL_GUEST_EXPORT void glTexGendv(GLenum coordinate, GLenum pname, const GLdouble *p) {
  const unsigned n=pname==0x2500 ? 1 : (pname==0x2501 || pname==0x2502 ? 4 : 0);
  if (!n) { vexa_lighting_guest::report(0x0500); return; } if(p) vexa_lighting_guest::queue_texgen(coordinate,pname,vexa_lighting_guest::values(p,n));
}
VEXA_GL_GUEST_EXPORT void glTexGend(GLenum coordinate, GLenum pname, GLdouble v) {
  if (pname!=0x2500) { vexa_lighting_guest::report(0x0500); return; } glTexGendv(coordinate,pname,&v);
}
VEXA_GL_GUEST_EXPORT void glTexGeniv(GLenum coordinate, GLenum pname, const GLint *p) {
  const unsigned n=pname==0x2500 ? 1 : (pname==0x2501 || pname==0x2502 ? 4 : 0);
  if (!n) { vexa_lighting_guest::report(0x0500); return; } if(p) vexa_lighting_guest::queue_texgen(coordinate,pname,vexa_lighting_guest::values(p,n,false));
}
VEXA_GL_GUEST_EXPORT void glTexGeni(GLenum coordinate, GLenum pname, GLint v) {
  if (pname!=0x2500) { vexa_lighting_guest::report(0x0500); return; } glTexGeniv(coordinate,pname,&v);
}
namespace vexa_lighting_guest {
inline bool get_texgen(GLenum c, GLenum pname, float *v, unsigned &n) {
  int i=coord(c), unit=tex_unit(); if(i<0 || unit<0) { report(invalid_enum); return false; }
  const auto error=vexa_ff::S().lighting.texgen[unit][i].get(pname,v,n); report(error); return !error;
}
}
VEXA_GL_GUEST_EXPORT void glGetTexGenfv(GLenum c, GLenum pname, GLfloat *p) {
  if (!p) return; float v[4]{}; unsigned n=0; if(vexa_lighting_guest::get_texgen(c,pname,v,n)) std::copy_n(v,n,p);
}
VEXA_GL_GUEST_EXPORT void glGetTexGendv(GLenum c, GLenum pname, GLdouble *p) {
  if (!p) return; float v[4]{}; unsigned n=0; if(vexa_lighting_guest::get_texgen(c,pname,v,n)) for(unsigned i=0;i<n;++i) p[i]=v[i];
}
VEXA_GL_GUEST_EXPORT void glGetTexGeniv(GLenum c, GLenum pname, GLint *p) {
  if (!p) return; float v[4]{}; unsigned n=0; if(vexa_lighting_guest::get_texgen(c,pname,v,n)) for(unsigned i=0;i<n;++i) p[i]=vexa_lighting_guest::integer(v[i]);
}
VEXA_GL_GUEST_EXPORT void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *p) {
  const unsigned n=pname==0x2200 ? 1 : (pname==0x2201 ? 4 : 0); if (!n) { vexa_lighting_guest::report(0x0500); return; }
  if (p) vexa_lighting_guest::queue_texenv(target,pname,vexa_lighting_guest::values(p,n));
}
VEXA_GL_GUEST_EXPORT void glTexEnvf(GLenum target, GLenum pname, GLfloat v) {
  if (pname!=0x2200) { vexa_lighting_guest::report(0x0500); return; } glTexEnvfv(target,pname,&v);
}
VEXA_GL_GUEST_EXPORT void glTexEnviv(GLenum target, GLenum pname, const GLint *p) {
  const unsigned n=pname==0x2200 ? 1 : (pname==0x2201 ? 4 : 0); if (!n) { vexa_lighting_guest::report(0x0500); return; }
  if (p) vexa_lighting_guest::queue_texenv(target,pname,vexa_lighting_guest::values(p,n,pname==0x2201));
}
VEXA_GL_GUEST_EXPORT void glTexEnvi(GLenum target, GLenum pname, GLint v) {
  if (pname!=0x2200) { vexa_lighting_guest::report(0x0500); return; } glTexEnviv(target,pname,&v);
}
namespace vexa_lighting_guest {
inline bool get_texenv(GLenum target, GLenum pname, float *v, unsigned &n) {
  int unit=tex_unit(); if(unit<0) { report(invalid_enum); return false; }
  const auto error=vexa_ff::S().lighting.texenv[unit].get(target,pname,v,n); report(error); return !error;
}
}
VEXA_GL_GUEST_EXPORT void glGetTexEnvfv(GLenum target, GLenum pname, GLfloat *p) {
  if (!p) return; float v[4]{}; unsigned n=0; if(vexa_lighting_guest::get_texenv(target,pname,v,n)) std::copy_n(v,n,p);
}
VEXA_GL_GUEST_EXPORT void glGetTexEnviv(GLenum target, GLenum pname, GLint *p) {
  if (!p) return; float v[4]{}; unsigned n=0; if(vexa_lighting_guest::get_texenv(target,pname,v,n)) for(unsigned i=0;i<n;++i) p[i]=vexa_lighting_guest::integer(v[i],pname==0x2201);
}
