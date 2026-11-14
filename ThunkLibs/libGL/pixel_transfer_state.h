#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace vexa_pixel {
struct PixelTransferState {
  bool map_color = false, map_stencil = false;
  int index_shift = 0, index_offset = 0;
  std::array<float,4> scale{1,1,1,1}, bias{0,0,0,0};
  float depth_scale = 1, depth_bias = 0, zoom_x = 1, zoom_y = 1;
  std::array<std::vector<float>,10> maps;
  PixelTransferState() { for (auto& map : maps) map.push_back(0); }
};
inline int nearest_int(double value) {
  if (!std::isfinite(value)) return 0;
  value = std::round(value);
  return static_cast<int>(std::clamp(value, static_cast<double>(std::numeric_limits<int>::min()),
                                     static_cast<double>(std::numeric_limits<int>::max())));
}
inline bool transfer_value(PixelTransferState& s, unsigned pname, double value) {
  switch(pname) {
    case 0x0D10: s.map_color = value != 0; return true;
    case 0x0D11: s.map_stencil = value != 0; return true;
    case 0x0D12: s.index_shift = nearest_int(value); return true;
    case 0x0D13: s.index_offset = nearest_int(value); return true;
    case 0x0D14: s.scale[0] = value; return true;
    case 0x0D15: s.bias[0] = value; return true;
    case 0x0D18: s.scale[1] = value; return true;
    case 0x0D19: s.bias[1] = value; return true;
    case 0x0D1A: s.scale[2] = value; return true;
    case 0x0D1B: s.bias[2] = value; return true;
    case 0x0D1C: s.scale[3] = value; return true;
    case 0x0D1D: s.bias[3] = value; return true;
    case 0x0D1E: s.depth_scale = value; return true;
    case 0x0D1F: s.depth_bias = value; return true;
    default: return false;
  }
}
inline bool query_transfer(const PixelTransferState& s, unsigned pname, double& value) {
  switch(pname) {
    case 0x0D10: value=s.map_color; return true; case 0x0D11: value=s.map_stencil; return true;
    case 0x0D12: value=s.index_shift; return true; case 0x0D13: value=s.index_offset; return true;
    case 0x0D14: value=s.scale[0]; return true; case 0x0D15: value=s.bias[0]; return true;
    case 0x0D16: value=s.zoom_x; return true; case 0x0D17: value=s.zoom_y; return true;
    case 0x0D18: value=s.scale[1]; return true; case 0x0D19: value=s.bias[1]; return true;
    case 0x0D1A: value=s.scale[2]; return true; case 0x0D1B: value=s.bias[2]; return true;
    case 0x0D1C: value=s.scale[3]; return true; case 0x0D1D: value=s.bias[3]; return true;
    case 0x0D1E: value=s.depth_scale; return true; case 0x0D1F: value=s.depth_bias; return true;
    default: return false;
  }
}
inline int map_index(unsigned map) { return map >= 0x0C70 && map <= 0x0C79 ? int(map - 0x0C70) : -1; }
inline bool map_size_valid(int index, int size) {
  // The exposed maximum is 256. Index-addressed tables require power of two;
  // component-addressed tables have no power-of-two requirement.
  return index >= 0 && index < 10 && size > 0 && size <= 256 &&
         (index >= 6 || (size & (size - 1)) == 0);
}
inline bool query_map_size(const PixelTransferState& s, unsigned pname, double& result) {
  if (pname < 0x0CB0 || pname > 0x0CB9) return false;
  result = static_cast<double>(s.maps[pname - 0x0CB0].size()); return true;
}
} // namespace vexa_pixel
