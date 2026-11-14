#pragma once
#include "legacy_lighting_state.h"
#include <algorithm>
#include <cstdint>
#include <type_traits>

// Fixed-layout packet for a custom thunk. This is intentionally limited to
// lighting/fog and the primary 2D texture unit. Other texture targets and
// multitexture combinations are outside this fixed-function draw path.
namespace vexa_legacy_lighting {
struct WireMaterial { float ambient[4], diffuse[4], specular[4], emission[4], shininess; };
struct WireLight {
  float ambient[4], diffuse[4], specular[4], position[4], direction[3];
  float exponent, cutoff, attenuation[3];
  int32_t enabled;
};
struct Wire {
  uint32_t version, byte_size;
  float modelview[16], normal_matrix[9];
  int32_t lighting_enabled, normalize_enabled, two_sided, local_viewer;
  float global_ambient[4];
  WireMaterial material[2]; WireLight light[8];
  int32_t fog_enabled, fog_mode;
  float fog_color[4], fog_density, fog_start, fog_end;
  int32_t texture_enabled, texture_env_mode;
  float texture_env_color[4], texture_matrix[16];
};
static_assert(sizeof(float)==4 && sizeof(uint32_t)==4 && sizeof(int32_t)==4);
static_assert(std::is_standard_layout_v<Wire> && std::is_trivially_copyable_v<Wire>);
static_assert(sizeof(WireMaterial)==68 && sizeof(WireLight)==100 && sizeof(Wire)==1200,
              "wire must have the same fixed layout for x86, i386 and arm64");
inline bool build_wire(const State &state, const Mat4 &model, Wire &out, const float *texture_matrix=nullptr) {
  Wire result{}; result.version=2; result.byte_size=sizeof(Wire);
  for (int i=0;i<16;++i) result.modelview[i]=static_cast<float>(model[i]);
  for (int c=0;c<3;++c) {
    Vec4 axis{}; axis[c]=1.f; Vec4 transformed{};
    if (!inverse_transpose(model,axis,transformed)) return false;
    for (int r=0;r<3;++r) result.normal_matrix[c*3+r]=transformed[r];
  }
  result.lighting_enabled=state.lighting_enabled; result.normalize_enabled=state.normalize_enabled;
  result.two_sided=state.two_side; result.local_viewer=state.local_viewer;
  std::copy(state.model_ambient.begin(),state.model_ambient.end(),result.global_ambient);
  for (unsigned i=0;i<2;++i) {
    const auto &s=state.materials[i]; auto &d=result.material[i];
    std::copy(s.ambient_value.begin(),s.ambient_value.end(),d.ambient);
    std::copy(s.diffuse_value.begin(),s.diffuse_value.end(),d.diffuse);
    std::copy(s.specular_value.begin(),s.specular_value.end(),d.specular);
    std::copy(s.emission_value.begin(),s.emission_value.end(),d.emission); d.shininess=s.shiny;
  }
  for (unsigned i=0;i<8;++i) {
    const auto &s=state.lights[i]; auto &d=result.light[i];
    std::copy(s.ambient_value.begin(),s.ambient_value.end(),d.ambient);
    std::copy(s.diffuse_value.begin(),s.diffuse_value.end(),d.diffuse);
    std::copy(s.specular_value.begin(),s.specular_value.end(),d.specular);
    std::copy(s.position_value.begin(),s.position_value.end(),d.position);
    std::copy_n(s.direction_value.begin(),3,d.direction);
    d.exponent=s.exponent; d.cutoff=s.cutoff; d.attenuation[0]=s.constant;
    d.attenuation[1]=s.linear_factor; d.attenuation[2]=s.quadratic; d.enabled=state.light_enabled[i];
  }
  result.fog_enabled=state.fog_enabled; result.fog_mode=state.fog.mode;
  std::copy(state.fog.color.begin(),state.fog.color.end(),result.fog_color);
  result.fog_density=state.fog.density; result.fog_start=state.fog.start; result.fog_end=state.fog.end;
  result.texture_enabled=state.texture_2d_enabled[0];
  result.texture_env_mode=state.texenv[0].mode;
  std::copy(state.texenv[0].color.begin(),state.texenv[0].color.end(),result.texture_env_color);
  for(int i=0;i<16;++i) result.texture_matrix[i]=texture_matrix ? texture_matrix[i] : (i%5==0 ? 1.f : 0.f);
  out=result; return true;
}
inline bool valid_wire(const Wire &wire) { return wire.version==2 && wire.byte_size==sizeof(Wire); }
} // namespace vexa_legacy_lighting
