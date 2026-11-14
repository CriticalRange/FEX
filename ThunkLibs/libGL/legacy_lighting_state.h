#pragma once

// Independent desktop fixed-function state. This component intentionally does
// not claim to implement fixed-function drawing: the caller must feed this
// state into its shader and must maintain one instance per GL context.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace vexa_legacy_lighting {
constexpr uint32_t no_error = 0, invalid_enum = 0x0500, invalid_value = 0x0501;
constexpr uint32_t ambient = 0x1200, diffuse = 0x1201, specular = 0x1202;
constexpr uint32_t position = 0x1203, spot_direction = 0x1204, spot_exponent = 0x1205, spot_cutoff = 0x1206;
constexpr uint32_t constant_attenuation = 0x1207, linear_attenuation = 0x1208, quadratic_attenuation = 0x1209;
constexpr uint32_t emission = 0x1600, shininess = 0x1601, ambient_diffuse = 0x1602, color_indexes = 0x1603;
constexpr uint32_t front = 0x0404, back = 0x0405, front_and_back = 0x0408;
constexpr uint32_t fog_index = 0x0b61, fog_density = 0x0b62, fog_start = 0x0b63, fog_end = 0x0b64;
constexpr uint32_t fog_mode = 0x0b65, fog_color = 0x0b66, linear = 0x2601, exp = 0x0800, exp2 = 0x0801;
constexpr uint32_t light_model_local_viewer = 0x0b51, light_model_two_side = 0x0b52, light_model_ambient = 0x0b53;
constexpr uint32_t texture_gen_mode = 0x2500, object_plane = 0x2501, eye_plane = 0x2502;
constexpr uint32_t eye_linear = 0x2400, object_linear = 0x2401, sphere_map = 0x2402;
constexpr uint32_t coord_s = 0x2000, coord_t = 0x2001, coord_r = 0x2002, coord_q = 0x2003;
constexpr uint32_t texture_env = 0x2300, texture_env_mode = 0x2200, texture_env_color = 0x2201;
constexpr uint32_t modulate = 0x2100, decal = 0x2101, blend = 0x0be2, replace = 0x1e01, add = 0x0104;
using Vec4 = std::array<float, 4>;
using Mat4 = std::array<double, 16>; // OpenGL column-major storage

inline Mat4 identity() { return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }
inline Vec4 vector4(const float *p) { return {p[0], p[1], p[2], p[3]}; }
inline Vec4 clamped4(const float *p) {
  return {std::clamp(p[0], 0.f, 1.f), std::clamp(p[1], 0.f, 1.f),
          std::clamp(p[2], 0.f, 1.f), std::clamp(p[3], 0.f, 1.f)};
}
inline Vec4 transform(const Mat4 &m, const Vec4 &v) {
  Vec4 out{};
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r] += static_cast<float>(m[c * 4 + r] * v[c]);
  return out;
}
// Inverse-transpose transforms a plane equation or normal into eye space.
// Singular matrices have no inverse, so callers must not overwrite old state.
inline bool inverse_transpose(const Mat4 &m, const Vec4 &v, Vec4 &out) {
  double a[4][8]{};
  for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
    a[r][c] = m[c * 4 + r]; a[r][c + 4] = (r == c);
  }
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
    if (a[pivot][c] == 0) return false;
    if (pivot != c) for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[pivot][j]);
    const double factor = a[c][c]; for (double &x : a[c]) x /= factor;
    for (int r = 0; r < 4; ++r) if (r != c) {
      const double f = a[r][c]; for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
    }
  }
  Vec4 result{};
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) result[r] += static_cast<float>(a[c][r + 4] * v[c]);
  out = result; return true;
}

struct Fog {
  Vec4 color{0,0,0,0}; float index = 0, density = 1, start = 0, end = 1; uint32_t mode = exp;
  uint32_t set(uint32_t pname, const float *p) {
    if (!p) return invalid_value;
    switch (pname) {
      case fog_color: color = clamped4(p); return no_error;
      case fog_index: index = *p; return no_error;
      case fog_density: if (*p < 0) return invalid_value; density = *p; return no_error;
      case fog_start: start = *p; return no_error;
      case fog_end: end = *p; return no_error;
      case fog_mode: {
        if (*p != static_cast<float>(linear) && *p != static_cast<float>(exp) && *p != static_cast<float>(exp2)) return invalid_enum;
        mode = static_cast<uint32_t>(*p); return no_error;
      }
      default: return invalid_enum;
    }
  }
  uint32_t get(uint32_t pname, float *p, unsigned &count) const {
    if (!p) { count=0; return invalid_value; }
    count=1;
    switch (pname) {
      case fog_color: count=4; std::copy(color.begin(),color.end(),p); break;
      case fog_index: *p=index; break; case fog_density: *p=density; break;
      case fog_start: *p=start; break; case fog_end: *p=end; break;
      case fog_mode: *p=static_cast<float>(mode); break;
      default: count=0; return invalid_enum;
    }
    return no_error;
  }
};
struct Light {
  Vec4 ambient_value{0,0,0,1}, diffuse_value{0,0,0,1}, specular_value{0,0,0,1};
  Vec4 position_value{0,0,1,0}, direction_value{0,0,-1,0};
  float exponent = 0, cutoff = 180, constant = 1, linear_factor = 0, quadratic = 0;
  uint32_t set(uint32_t pname, const float *p, const Mat4 &model) {
    if (!p) return invalid_value;
    switch (pname) {
      case ambient: ambient_value = vector4(p); return no_error;
      case diffuse: diffuse_value = vector4(p); return no_error;
      case specular: specular_value = vector4(p); return no_error;
      case position: position_value = transform(model, vector4(p)); return no_error;
      case spot_direction: {
        const Vec4 result = transform(model, {p[0],p[1],p[2],0});
        direction_value = {result[0],result[1],result[2],0}; return no_error;
      }
      case spot_exponent: if (*p < 0 || *p > 128) return invalid_value; exponent = *p; return no_error;
      case spot_cutoff: if ((*p < 0 || *p > 90) && *p != 180) return invalid_value; cutoff = *p; return no_error;
      case constant_attenuation: if (*p < 0) return invalid_value; constant = *p; return no_error;
      case linear_attenuation: if (*p < 0) return invalid_value; linear_factor = *p; return no_error;
      case quadratic_attenuation: if (*p < 0) return invalid_value; quadratic = *p; return no_error;
      default: return invalid_enum;
    }
  }
  const float *get(uint32_t pname, unsigned &count) const {
    count = 1;
    switch (pname) {
      case ambient: count=4; return ambient_value.data(); case diffuse: count=4; return diffuse_value.data();
      case specular: count=4; return specular_value.data(); case position: count=4; return position_value.data();
      case spot_direction: count=3; return direction_value.data(); case spot_exponent: return &exponent;
      case spot_cutoff: return &cutoff; case constant_attenuation: return &constant;
      case linear_attenuation: return &linear_factor; case quadratic_attenuation: return &quadratic;
      default: count=0; return nullptr;
    }
  }
};
struct Material {
  Vec4 ambient_value{.2f,.2f,.2f,1}, diffuse_value{.8f,.8f,.8f,1};
  Vec4 specular_value{0,0,0,1}, emission_value{0,0,0,1};
  std::array<float,3> indexes{0,1,1}; float shiny=0;
  uint32_t set(uint32_t pname, const float *p) {
    if (!p) return invalid_value;
    switch (pname) {
      case ambient: ambient_value=vector4(p); return no_error;
      case diffuse: diffuse_value=vector4(p); return no_error;
      case specular: specular_value=vector4(p); return no_error;
      case emission: emission_value=vector4(p); return no_error;
      case ambient_diffuse: ambient_value=diffuse_value=vector4(p); return no_error;
      case shininess: if (*p < 0 || *p > 128) return invalid_value; shiny=*p; return no_error;
      case color_indexes: indexes={p[0],p[1],p[2]}; return no_error;
      default: return invalid_enum;
    }
  }
  const float *get(uint32_t pname, unsigned &count) const {
    count=4;
    switch (pname) {
      case ambient: return ambient_value.data(); case diffuse: return diffuse_value.data();
      case specular: return specular_value.data(); case emission: return emission_value.data();
      case shininess: count=1; return &shiny; case color_indexes: count=3; return indexes.data();
      default: count=0; return nullptr;
    }
  }
};
struct TexGen {
  uint32_t mode = eye_linear;
  Vec4 object{}, eye{};
  uint32_t set(unsigned coordinate, uint32_t pname, const float *p, const Mat4 &model) {
    if (!p) return invalid_value;
    switch (pname) {
      case texture_gen_mode:
        if (*p != float(eye_linear) && *p != float(object_linear) &&
            !(*p == float(sphere_map) && coordinate < 2)) return invalid_enum;
        mode=static_cast<uint32_t>(*p); return no_error;
      case object_plane: object=vector4(p); return no_error;
      case eye_plane: { Vec4 transformed{}; if (!inverse_transpose(model, vector4(p), transformed)) return invalid_value;
                        eye=transformed; return no_error; }
      default: return invalid_enum;
    }
  }
  uint32_t get(uint32_t pname, float *p, unsigned &count) const {
    if (!p) { count=0; return invalid_value; }
    if (pname == texture_gen_mode) { *p=static_cast<float>(mode); count=1; return no_error; }
    if (pname == object_plane || pname == eye_plane) {
      const auto &plane = pname == object_plane ? object : eye;
      std::copy(plane.begin(),plane.end(),p); count=4; return no_error;
    }
    count=0; return invalid_enum;
  }
};
struct TexEnv {
  uint32_t mode=modulate; Vec4 color{0,0,0,0};
  uint32_t set(uint32_t target, uint32_t pname, const float *p) {
    if (target != texture_env) return invalid_enum;
    if (!p) return invalid_value;
    if (pname == texture_env_color) { color=clamped4(p); return no_error; }
    if (pname != texture_env_mode) return invalid_enum;
    if (*p != float(modulate) && *p != float(decal) && *p != float(blend) && *p != float(replace) && *p != float(add)) return invalid_enum;
    mode=static_cast<uint32_t>(*p); return no_error;
  }
  uint32_t get(uint32_t target, uint32_t pname, float *p, unsigned &count) const {
    if (target != texture_env) { count=0; return invalid_enum; }
    if (!p) { count=0; return invalid_value; }
    if (pname == texture_env_mode) { *p=static_cast<float>(mode); count=1; return no_error; }
    if (pname == texture_env_color) { std::copy(color.begin(),color.end(),p); count=4; return no_error; }
    count=0; return invalid_enum;
  }
};
struct State {
  Fog fog; std::array<Light,8> lights{}; std::array<Material,2> materials{};
  Vec4 model_ambient{.2f,.2f,.2f,1}; bool local_viewer=false, two_side=false;
  std::array<std::array<TexGen,4>,8> texgen{}; std::array<TexEnv,8> texenv{};
  bool fog_enabled=false, lighting_enabled=false, color_material_enabled=false, normalize_enabled=false;
  std::array<bool,8> light_enabled{};
  std::array<bool,8> texture_2d_enabled{};
  std::array<std::array<bool,4>,8> texgen_enabled{};
  uint32_t color_material_face=front_and_back, color_material_mode=ambient_diffuse;
  State() {
    lights[0].diffuse_value=lights[0].specular_value={1,1,1,1};
    for (auto &unit : texgen) { unit[0].object=unit[0].eye={1,0,0,0}; unit[1].object=unit[1].eye={0,1,0,0}; }
  }
  uint32_t set_material(uint32_t face, uint32_t pname, const float *p) {
    if (face != front && face != back && face != front_and_back) return invalid_enum;
    Material f=materials[0], b=materials[1];
    if (face != back) { auto err=f.set(pname,p); if (err) return err; }
    if (face != front) { auto err=b.set(pname,p); if (err) return err; }
    materials[0]=f; materials[1]=b; return no_error;
  }
  uint32_t set_light_model(uint32_t pname, const float *p) {
    if (!p) return invalid_value;
    switch (pname) {
      case light_model_ambient: model_ambient=vector4(p); return no_error;
      case light_model_local_viewer: local_viewer=(*p != 0); return no_error;
      case light_model_two_side: two_side=(*p != 0); return no_error;
      default: return invalid_enum;
    }
  }
  uint32_t set_color_material(uint32_t face, uint32_t mode) {
    if (face != front && face != back && face != front_and_back) return invalid_enum;
    if (mode != emission && mode != ambient && mode != diffuse && mode != specular && mode != ambient_diffuse) return invalid_enum;
    color_material_face=face; color_material_mode=mode; return no_error;
  }
  uint32_t update_color_material(const float *color) {
    return set_material(color_material_face, color_material_mode, color);
  }
};
} // namespace vexa_legacy_lighting
