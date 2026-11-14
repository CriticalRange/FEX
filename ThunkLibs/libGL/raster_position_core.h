#pragma once

// Pure raster-position mathematics. This header does not own a GL context or
// draw pixels; callers must provide state from the currently bound context.
#include <array>
#include <cstddef>

namespace vexa_raster {
using Vec4 = std::array<double, 4>;
using Mat4 = std::array<double, 16>; // OpenGL column-major storage.

struct Input {
  Mat4 modelview{};
  Mat4 projection{};
  std::array<int, 4> viewport{};
  std::array<double, 2> depth_range{0.0, 1.0};
  std::array<Vec4, 6> clip_planes{}; // Plane equations in eye coordinates.
  std::array<bool, 6> clip_enabled{};
  Vec4 color{1.0, 1.0, 1.0, 1.0};
  Vec4 texcoord{0.0, 0.0, 0.0, 1.0};
};

struct Position {
  bool valid = true;
  Vec4 window{0.0, 0.0, 0.0, 1.0};
  Vec4 color{1.0, 1.0, 1.0, 1.0};
  Vec4 texcoord{0.0, 0.0, 0.0, 1.0};
};

inline Vec4 transform(const Mat4& matrix, const Vec4& v) {
  Vec4 result{};
  for (std::size_t row = 0; row != 4; ++row)
    for (std::size_t col = 0; col != 4; ++col)
      result[row] += matrix[col * 4 + row] * v[col];
  return result;
}

// Returns false for a clipped position and only changes the validity flag in
// that case. This preserves the previous position/color/texture coordinates.
inline bool raster_pos(Position& state, const Input& input, const Vec4& object) {
  const Vec4 eye = transform(input.modelview, object);
  for (std::size_t i = 0; i != input.clip_planes.size(); ++i) {
    if (!input.clip_enabled[i]) continue;
    double distance = 0.0;
    for (std::size_t j = 0; j != 4; ++j) distance += input.clip_planes[i][j] * eye[j];
    if (distance < 0.0) { state.valid = false; return false; }
  }
  const Vec4 clip = transform(input.projection, eye);
  const double w = clip[3];
  if (w <= 0.0 || clip[0] < -w || clip[0] > w || clip[1] < -w || clip[1] > w ||
      clip[2] < -w || clip[2] > w) {
    state.valid = false;
    return false;
  }
  const double inv_w = 1.0 / w;
  state.window[0] = input.viewport[0] + (clip[0] * inv_w + 1.0) * input.viewport[2] * 0.5;
  state.window[1] = input.viewport[1] + (clip[1] * inv_w + 1.0) * input.viewport[3] * 0.5;
  state.window[2] = (input.depth_range[1] - input.depth_range[0]) * clip[2] * inv_w * 0.5 +
                    (input.depth_range[1] + input.depth_range[0]) * 0.5;
  state.window[3] = clip[3];
  state.color = input.color;
  state.texcoord = input.texcoord;
  state.valid = true;
  return true;
}
} // namespace vexa_raster
