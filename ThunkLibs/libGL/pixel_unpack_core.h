#pragma once

// CPU-side unpack/transfer building block for glDrawPixels. It neither uploads
// to the GPU nor reads GPU pixels. Unsupported formats are rejected explicitly.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace vexa_pixel {
enum class Format { Alpha, Luminance, LuminanceAlpha, RGB, RGBA, BGRA };
enum class Type { U8, U16, F32 };
struct Unpack {
  unsigned alignment = 4;
  std::size_t row_length = 0, skip_rows = 0, skip_pixels = 0;
  bool swap_bytes = false;
};
struct Transfer {
  std::array<float, 4> scale{1, 1, 1, 1}, bias{0, 0, 0, 0};
  bool map_color = false;
  std::array<std::vector<float>, 4> maps{};
};

inline bool checked_mul(std::size_t a, std::size_t b, std::size_t& out) {
  if (b && a > std::numeric_limits<std::size_t>::max() / b) return false;
  out = a * b; return true;
}
inline bool checked_add(std::size_t a, std::size_t b, std::size_t& out) {
  if (a > std::numeric_limits<std::size_t>::max() - b) return false;
  out = a + b; return true;
}
inline float component(const std::uint8_t* src, Type type, bool swap) {
  if (type == Type::U8) return *src / 255.0f;
  const unsigned size = type == Type::U16 ? 2 : 4;
  std::uint8_t bytes[4]{};
  for (unsigned i = 0; i < size; ++i) bytes[i] = src[swap ? size - 1 - i : i];
  if (type == Type::U16) { std::uint16_t v; std::memcpy(&v, bytes, 2); return v / 65535.0f; }
  float v; std::memcpy(&v, bytes, 4); return v;
}

// On success emits width*height*4 floats in bottom-to-top GL row order.
// `available` describes safely readable input bytes (callers must calculate
// this from guest address validation); insufficient memory never gets read.
inline bool rgba32f(const void* input, std::size_t available, std::size_t width, std::size_t height,
                    Format format, Type type, const Unpack& unpack, const Transfer& transfer,
                    std::vector<float>& output) {
  if (unpack.alignment != 1 && unpack.alignment != 2 && unpack.alignment != 4 && unpack.alignment != 8) return false;
  const unsigned channels = format == Format::Alpha || format == Format::Luminance ? 1 :
                            format == Format::LuminanceAlpha ? 2 : format == Format::RGB ? 3 : 4;
  const unsigned bytes = type == Type::U8 ? 1 : type == Type::U16 ? 2 : 4;
  if (!width || !height) { output.clear(); return true; }
  if (!input) return false;
  if (transfer.map_color)
    for (const auto& map : transfer.maps) if (map.empty()) return false;
  std::size_t pixel_bytes, row_bytes, aligned, offset_rows, offset_pixels, start, last_rows, used, total;
  if (!checked_mul(channels, bytes, pixel_bytes) ||
      !checked_mul(unpack.row_length ? unpack.row_length : width, pixel_bytes, row_bytes) ||
      !checked_add(row_bytes, unpack.alignment - 1, aligned)) return false;
  aligned &= ~(std::size_t(unpack.alignment) - 1);
  if (!checked_mul(unpack.skip_rows, aligned, offset_rows) || !checked_mul(unpack.skip_pixels, pixel_bytes, offset_pixels) ||
      !checked_add(offset_rows, offset_pixels, start) || !checked_mul(height - 1, aligned, last_rows) ||
      !checked_mul(width, pixel_bytes, used) || !checked_add(start, last_rows, total) ||
      !checked_add(total, used, total) || total > available) return false;
  std::size_t count;
  if (!checked_mul(width, height, count) || !checked_mul(count, 4, count)) return false;
  std::vector<float> result(count);
  auto* src = static_cast<const std::uint8_t*>(input);
  for (std::size_t y = 0; y < height; ++y) for (std::size_t x = 0; x < width; ++x) {
    const auto* p = src + start + y * aligned + x * pixel_bytes;
    float c[4] = {0, 0, 0, 1};
    float v[4]{};
    for (unsigned i = 0; i < channels; ++i) v[i] = component(p + i * bytes, type, unpack.swap_bytes);
    switch (format) {
      case Format::Alpha: c[3] = v[0]; break;
      case Format::Luminance: c[0] = c[1] = c[2] = v[0]; break;
      case Format::LuminanceAlpha: c[0] = c[1] = c[2] = v[0]; c[3] = v[1]; break;
      case Format::RGB: c[0] = v[0]; c[1] = v[1]; c[2] = v[2]; break;
      case Format::RGBA: std::copy_n(v, 4, c); break;
      case Format::BGRA: c[0] = v[2]; c[1] = v[1]; c[2] = v[0]; c[3] = v[3]; break;
    }
    for (unsigned i = 0; i < 4; ++i) {
      c[i] = std::clamp(c[i] * transfer.scale[i] + transfer.bias[i], 0.0f, 1.0f);
      if (transfer.map_color) {
        if (!std::isfinite(c[i])) return false;
        const auto& map = transfer.maps[i];
        const auto index = static_cast<std::size_t>(std::floor(c[i] * static_cast<float>(map.size() - 1) + 0.5f));
        c[i] = std::clamp(map[index], 0.0f, 1.0f);
      }
      result[(y * width + x) * 4 + i] = c[i];
    }
  }
  output.swap(result);
  return true;
}
} // namespace vexa_pixel
