#pragma once
#include "pixel_unpack_core.h"

namespace vexa_pixel {
// Expand GL_BITMAP data to RGBA32F mask pixels (red is 0 or 1). Row zero is
// the bitmap's bottom row, matching GL unpack semantics. A draw operation
// must discard zero bits, not blend transparent fragments into the target.
inline bool bitmap_mask(const void* input, std::size_t available, std::size_t width, std::size_t height,
                        const Unpack& unpack, bool lsb_first, std::vector<float>& output) {
  if (unpack.alignment != 1 && unpack.alignment != 2 && unpack.alignment != 4 && unpack.alignment != 8) return false;
  if (!width || !height) { output.clear(); return true; }
  if (!input) return false;
  std::size_t bits, row_bytes, aligned, offset_rows, last_rows, last_bit, used, end, count;
  if (!checked_add(unpack.row_length ? unpack.row_length : width, 7, bits)) return false;
  row_bytes = bits / 8;
  if (!checked_add(row_bytes, unpack.alignment - 1, aligned)) return false;
  aligned &= ~(std::size_t(unpack.alignment) - 1);
  if (!checked_mul(unpack.skip_rows, aligned, offset_rows) || !checked_mul(height - 1, aligned, last_rows) ||
      !checked_add(unpack.skip_pixels, width - 1, last_bit) || !checked_add(last_bit / 8, 1, used) ||
      !checked_add(offset_rows, last_rows, end) || !checked_add(end, used, end) || end > available ||
      !checked_mul(width, height, count) || !checked_mul(count, 4, count)) return false;
  std::vector<float> result(count, 0.0f);
  const auto* src = static_cast<const std::uint8_t*>(input);
  for (std::size_t y = 0; y < height; ++y) for (std::size_t x = 0; x < width; ++x) {
    const std::size_t bit = unpack.skip_pixels + x;
    const unsigned shift = lsb_first ? unsigned(bit & 7) : 7 - unsigned(bit & 7);
    result[(y * width + x) * 4] = (src[offset_rows + y * aligned + bit / 8] >> shift) & 1;
  }
  output.swap(result); return true;
}
} // namespace vexa_pixel
