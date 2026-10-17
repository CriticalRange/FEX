// Desktop GL_UNPACK_SWAP_BYTES / LSB_FIRST and the compressed-block pixel
// stores are not in GLES. Remember the byte swap; drop the rest so Mali does
// not raise INVALID_ENUM and abort the upload that follows.
static int g_vexa_unpack_swap_bytes = 0;

static bool VexaIgnorePixelStore(GLenum pname, GLint param) {
  switch (pname) {
    case 0x0CF0:  // GL_UNPACK_SWAP_BYTES
      g_vexa_unpack_swap_bytes = param;
      return true;
    case 0x0CF1:  // GL_UNPACK_LSB_FIRST
    case 0x0D00:  // GL_PACK_SWAP_BYTES
    case 0x0D01:  // GL_PACK_LSB_FIRST
    case 0x9127:  // GL_UNPACK_COMPRESSED_BLOCK_WIDTH
    case 0x9128:  // GL_UNPACK_COMPRESSED_BLOCK_HEIGHT
    case 0x9129:  // GL_UNPACK_COMPRESSED_BLOCK_DEPTH
    case 0x912A:  // GL_UNPACK_COMPRESSED_BLOCK_SIZE
    case 0x912B:  // GL_PACK_COMPRESSED_BLOCK_WIDTH
    case 0x912C:  // GL_PACK_COMPRESSED_BLOCK_HEIGHT
    case 0x912D:  // GL_PACK_COMPRESSED_BLOCK_DEPTH
    case 0x912E:  // GL_PACK_COMPRESSED_BLOCK_SIZE
      return true;
    default:
      return false;
  }
}

static void VexaRebindUploadThunks();

static void VexaGL_glPixelStorei(GLenum pname, GLint param) {
  VexaRebindUploadThunks();
  if (VexaIgnorePixelStore(pname, param)) {
    static int logs = 0;
    if (logs++ < 8) {
      fprintf(stderr, "[VEXA][TEX] ignore glPixelStorei pname=0x%x param=%d\n", pname, param);
    }
    return;
  }
  ::glPixelStorei(pname, param);
}

// ETS2 uploads packed images as GL_BGR + 565 and GL_BGRA + 5551. GLES accepts
// only GL_RGB / GL_RGBA for those types, and has no GL_BGR at all.
static bool VexaDesktopPackedFormat(GLenum format, GLenum type) {
  // GLES only accepts GL_RGB with 565 and GL_RGBA with 5551/4444. ETS2 also
  // passes GL_BGR / GL_BGRA, and GL_RGBA + UNSIGNED_INT_8_8_8_8_REV (0x8367),
  // which Mali rejects even though the little-endian bytes are already RGBA.
  if (type == 0x8367 || type == 0x8035) return true;
  if (type == 0x8363) return format != GL_RGB;
  if (type == 0x8034 || type == 0x8033) return format != GL_RGBA;
  if (format == 0x80E0) return true;  // GL_BGR
  if (format == 0x80E1 && type == GL_UNSIGNED_BYTE) return true; // GL_BGRA
  return false;
}

static GLenum VexaDesktopInternalFormat(GLint internalformat, GLenum format, GLenum type) {
  if (internalformat == 0x80E0) {  // GL_BGR
    return type == 0x8363 ? 0x8D62 /* GL_RGB565 */ : GL_RGB8;
  }
  if (internalformat == 0x80E1) {  // GL_BGRA
    if (type == 0x8034) return 0x8057;  // GL_RGB5_A1
    if (type == 0x8033) return GL_RGBA4;
    return GL_RGBA8;
  }
  return static_cast<GLenum>(internalformat);
}

static uint16_t VexaSwap16(uint16_t value) {
  return static_cast<uint16_t>((value >> 8) | (value << 8));
}

// Repack one pixel into the GLES channel order. 565/5551/4444 stay packed.
static void VexaRepackPixel(GLenum format, GLenum type, const uint8_t* src, uint8_t* dst) {
  if (type == GL_UNSIGNED_BYTE && format == 0x80E1) {
    dst[0] = src[2];
    dst[1] = src[1];
    dst[2] = src[0];
    dst[3] = src[3];
    return;
  }
  if (type == GL_UNSIGNED_BYTE && format == 0x80E0) {
    dst[0] = src[2];
    dst[1] = src[1];
    dst[2] = src[0];
    return;
  }
  uint16_t value = 0;
  std::memcpy(&value, src, 2);
  if (g_vexa_unpack_swap_bytes) value = VexaSwap16(value);
  uint16_t out = value;
  if (type == 0x8363) {  // GL_UNSIGNED_SHORT_5_6_5, GL_BGR -> GL_RGB
    const uint16_t b = (value >> 11) & 0x1F;
    const uint16_t g = (value >> 5) & 0x3F;
    const uint16_t r = value & 0x1F;
    out = static_cast<uint16_t>((r << 11) | (g << 5) | b);
  } else if (type == 0x8034) {  // GL_UNSIGNED_SHORT_5_5_5_1, GL_BGRA -> GL_RGBA
    const uint16_t b = (value >> 11) & 0x1F;
    const uint16_t g = (value >> 6) & 0x1F;
    const uint16_t r = (value >> 1) & 0x1F;
    const uint16_t a = value & 0x1;
    out = static_cast<uint16_t>((r << 11) | (g << 6) | (b << 1) | a);
  } else if (type == 0x8033) {  // GL_UNSIGNED_SHORT_4_4_4_4, GL_BGRA -> GL_RGBA
    const uint16_t b = (value >> 12) & 0xF;
    const uint16_t g = (value >> 8) & 0xF;
    const uint16_t r = (value >> 4) & 0xF;
    const uint16_t a = value & 0xF;
    out = static_cast<uint16_t>((r << 12) | (g << 8) | (b << 4) | a);
  }
  std::memcpy(dst, &out, 2);
}

static GLenum VexaDesktopUploadFormat(GLenum format, GLenum type) {
  if (type == 0x8363) return GL_RGB;
  if (type == 0x8034 || type == 0x8033) return GL_RGBA;
  return format == 0x80E0 ? GL_RGB : GL_RGBA;
}

static bool VexaDesktopNeedsChannelSwap(GLenum format) {
  return format == 0x80E0 || format == 0x80E1;
}

static int VexaDesktopPixelBytes(GLenum format, GLenum type) {
  if (type == 0x8367 || type == 0x8035) return 4;
  if (type == GL_UNSIGNED_BYTE) return format == 0x80E0 ? 3 : 4;
  return 2;
}

static GLenum VexaDesktopUploadType(GLenum type) {
  if (type == 0x8367 || type == 0x8035) return GL_UNSIGNED_BYTE;
  return type;
}

// Convert a 2D rectangle and upload it. image_stride_rows is the source slice
// pitch in rows (0 = tightly packed height). Returns false if this format is
// not one we rewrite.
static bool VexaUploadDesktopImage2D(bool allocate, GLenum target, GLint level, GLint internalformat,
                                     GLint x, GLint y, GLsizei width, GLsizei height, GLint border,
                                     GLenum format, GLenum type, const void* data) {
  if (!VexaDesktopPackedFormat(format, type)) return false;
  GLenum upload_format = VexaDesktopUploadFormat(format, type);
  const GLenum upload_type = VexaDesktopUploadType(type);
  const bool swap_channels = VexaDesktopNeedsChannelSwap(format) || type == 0x8035;
  const GLenum storage = VexaDesktopInternalFormat(internalformat, format, type);
  bool rgb_destination = false;
  if (format == 0x80E1 && type == GL_UNSIGNED_BYTE) {
    GLint destination = storage;
    if (!allocate) ::glGetTexLevelParameteriv(target, level, GL_TEXTURE_INTERNAL_FORMAT, &destination);
    rgb_destination = destination == GL_RGB8 || destination == GL_SRGB8 || destination == 0x8D62;
    if (rgb_destination) upload_format = GL_RGB;
  }
  GLint source_pbo = 0;
  ::glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &source_pbo);
  // A null pointer is byte offset zero when an unpack buffer is bound.
  if ((!data && !source_pbo) || width <= 0 || height <= 0) {
    if (allocate) {
      ::glTexImage2D(target, level, storage, width, height, border, upload_format, upload_type, nullptr);
    }
    return true;
  }
  GLint row = 0, align = 4, skip_x = 0, skip_y = 0;
  ::glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
  ::glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
  ::glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_x);
  ::glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_y);
  if (align <= 0) align = 4;
  const int pixel_bytes = VexaDesktopPixelBytes(format, type);
  const int row_pixels = row > 0 ? row : width;
  const size_t raw_stride = static_cast<size_t>(row_pixels) * pixel_bytes;
  const size_t stride = (raw_stride + static_cast<size_t>(align) - 1) / static_cast<size_t>(align) * static_cast<size_t>(align);
  const size_t first = static_cast<size_t>(skip_y) * stride + static_cast<size_t>(skip_x) * pixel_bytes;
  const size_t length = first + static_cast<size_t>(height - 1) * stride + static_cast<size_t>(width) * pixel_bytes;
  if (length > static_cast<size_t>(INT32_MAX)) return true;
  VexaBCSource source(data, static_cast<GLsizei>(length));
  if (!source.ptr) return true;
  const int output_bytes = rgb_destination ? 3 : pixel_bytes;
  std::vector<uint8_t> converted(static_cast<size_t>(width) * height * output_bytes);
  for (GLsizei yy = 0; yy < height; ++yy) {
    const uint8_t* src = source.ptr + first + static_cast<size_t>(yy) * stride;
    uint8_t* dst = converted.data() + static_cast<size_t>(yy) * width * output_bytes;
    for (GLsizei xx = 0; xx < width; ++xx) {
      const uint8_t* pix = src + static_cast<size_t>(xx) * pixel_bytes;
      uint8_t* out = dst + static_cast<size_t>(xx) * output_bytes;
      if (rgb_destination) {
        out[0] = pix[2]; out[1] = pix[1]; out[2] = pix[0];
      } else if (type == 0x8035 && pixel_bytes == 4) {
        out[0] = pix[3];
        out[1] = pix[2];
        out[2] = pix[1];
        out[3] = pix[0];
      } else if (swap_channels) {
        VexaRepackPixel(format, type, pix, out);
      } else {
        std::memcpy(out, pix, pixel_bytes);
      }
    }
  }
  VexaUnpackScope unpack;
  if (allocate) {
    ::glTexImage2D(target, level, storage, width, height, border, upload_format, upload_type, converted.data());
  } else {
    ::glTexSubImage2D(target, level, x, y, width, height, upload_format, upload_type, converted.data());
  }
  static int logs = 0;
  if (logs++ < 8) {
    fprintf(stderr, "[VEXA][TEX] %s fmt=0x%x type=0x%x %dx%d -> fmt=0x%x\n",
            allocate ? "teximage" : "texsub", format, type, width, height, upload_format);
  }
  return true;
}

// GLES without EXT_texture_norm16: float32 represents every 16-bit normalized
// input to float accuracy. Upload conversion honours row/skip state and PBOs.
static std::mutex VexaUnorm16Mutex;
static std::unordered_map<GLuint, GLenum> VexaUnorm16Textures;
static void VexaNoteUnorm16Storage(GLenum target, GLenum format) {
  if (target != GL_TEXTURE_2D) return;
  GLint texture = 0;
  ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
  std::lock_guard lock(VexaUnorm16Mutex);
  VexaUnorm16Textures.erase(texture);
  if (VexaUnorm16Storage(format) != format) VexaUnorm16Textures[texture] = format;
}
static void VexaGL_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                                GLint border, GLenum format, GLenum type, const void* data) {
  static int logs = 0;
  if (logs++ < 8) {
    fprintf(stderr, "[VEXA][TEX] image2d internal=0x%x fmt=0x%x type=0x%x %dx%d\n",
            internalformat, format, type, width, height);
  }
  if (VexaUploadDesktopImage2D(true, target, level, internalformat, 0, 0, width, height, border, format, type, data)) {
    return;
  }
  ::glTexImage2D(target, level, internalformat, width, height, border, format, type, data);
}

static void VexaGL_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
                                  GLsizei width, GLsizei height, GLenum format, GLenum type, const void* data) {
  VexaUploadTrace trace(format, type, width, height);
  if (type == 0x8363 || type == 0x8034 || type == 0x8033) {
    static int packed_logs = 0;
    if (packed_logs++ < 6) {
      fprintf(stderr, "[VEXA][TEX] hit sub fmt=0x%x type=0x%x %dx%d\n", format, type, width, height);
    }
  }
  if (VexaUploadDesktopImage2D(false, target, level, 0, x, y, width, height, 0, format, type, data)) {
    return;
  }
  GLenum original = 0;
  if (target == GL_TEXTURE_2D) {
    GLint texture = 0;
    ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    std::lock_guard lock(VexaUnorm16Mutex);
    const auto it = VexaUnorm16Textures.find(texture);
    if (it != VexaUnorm16Textures.end()) original = it->second;
  }
  const int channels = format == GL_RED ? 1 : format == GL_RG ? 2 : format == GL_RGBA ? 4 : 0;
  if (!original || !channels || (type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_BYTE) || width <= 0 || height <= 0) {
    ::glTexSubImage2D(target, level, x, y, width, height, format, type, data);
    return;
  }
  GLint row = 0, align = 4, skip_x = 0, skip_y = 0;
  ::glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
  ::glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
  ::glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_x);
  ::glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_y);
  const size_t bytes = type == GL_UNSIGNED_SHORT ? 2 : 1;
  const size_t raw_stride = static_cast<size_t>(row ? row : width) * channels * bytes;
  const size_t stride = (raw_stride + align - 1) / align * align;
  const size_t first = static_cast<size_t>(skip_y) * stride + static_cast<size_t>(skip_x) * channels * bytes;
  const size_t length = first + static_cast<size_t>(height - 1) * stride + static_cast<size_t>(width) * channels * bytes;
  if (length > INT32_MAX) return;
  VexaBCSource source(data, static_cast<GLsizei>(length));
  if (!source.ptr) return;
  std::vector<float> converted(static_cast<size_t>(width) * height * channels);
  for (GLsizei yy = 0; yy < height; ++yy) {
    const uint8_t* src = source.ptr + first + static_cast<size_t>(yy) * stride;
    for (size_t n = 0; n < static_cast<size_t>(width) * channels; ++n) {
      uint16_t sample = src[n];
      if (bytes == 2) memcpy(&sample, src + n * 2, 2);
      converted[static_cast<size_t>(yy) * width * channels + n] = sample / (bytes == 2 ? 65535.0f : 255.0f);
    }
  }
  VexaUnpackScope unpack;
  ::glTexSubImage2D(target, level, x, y, width, height, format, GL_FLOAT, converted.data());
  static unsigned logs = 0;
  if (logs++ < 8) fprintf(stderr, "[VEXA][UNORM16] upload %dx%d channels=%d type=%x -> normalized float\n", width, height, channels, type);
}

static bool VexaConvertDesktopVolume(GLenum format, GLenum type, GLsizei width, GLsizei height, GLsizei depth,
                                    const void* data, std::vector<uint8_t>* converted) {
  if (!VexaDesktopPackedFormat(format, type)) return false;
  if (!data || width <= 0 || height <= 0 || depth <= 0) return true;
  GLint row = 0, align = 4, skip_x = 0, skip_y = 0, image_h = 0, skip_images = 0;
  ::glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
  ::glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
  ::glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_x);
  ::glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_y);
  ::glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &image_h);
  ::glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &skip_images);
  if (align <= 0) align = 4;
  const int pixel_bytes = VexaDesktopPixelBytes(format, type);
  const int row_pixels = row > 0 ? row : width;
  const int slice_rows = image_h > 0 ? image_h : height;
  const size_t raw_stride = static_cast<size_t>(row_pixels) * pixel_bytes;
  const size_t stride = (raw_stride + static_cast<size_t>(align) - 1) / static_cast<size_t>(align) * static_cast<size_t>(align);
  const size_t slice_stride = stride * static_cast<size_t>(slice_rows);
  const size_t first = static_cast<size_t>(skip_images) * slice_stride + static_cast<size_t>(skip_y) * stride +
                       static_cast<size_t>(skip_x) * pixel_bytes;
  const size_t length = first + static_cast<size_t>(depth - 1) * slice_stride +
                        static_cast<size_t>(height - 1) * stride + static_cast<size_t>(width) * pixel_bytes;
  if (length > static_cast<size_t>(INT32_MAX)) return true;
  VexaBCSource source(data, static_cast<GLsizei>(length));
  if (!source.ptr) return true;
  converted->resize(static_cast<size_t>(width) * height * depth * pixel_bytes);
  for (GLsizei z = 0; z < depth; ++z) {
    for (GLsizei yy = 0; yy < height; ++yy) {
      const uint8_t* src = source.ptr + first + static_cast<size_t>(z) * slice_stride + static_cast<size_t>(yy) * stride;
      uint8_t* dst = converted->data() + (static_cast<size_t>(z) * height + yy) * width * pixel_bytes;
      const bool swap_channels = VexaDesktopNeedsChannelSwap(format);
      for (GLsizei xx = 0; xx < width; ++xx) {
        const uint8_t* pix = src + static_cast<size_t>(xx) * pixel_bytes;
        uint8_t* out = dst + static_cast<size_t>(xx) * pixel_bytes;
        if (swap_channels) VexaRepackPixel(format, type, pix, out);
        else std::memcpy(out, pix, pixel_bytes);
      }
    }
  }
  return true;
}

static void VexaGL_glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                                GLsizei depth, GLint border, GLenum format, GLenum type, const void* data) {
  std::vector<uint8_t> converted;
  if (VexaConvertDesktopVolume(format, type, width, height, depth, data, &converted)) {
    VexaUnpackScope unpack;
    ::glTexImage3D(target, level, VexaDesktopInternalFormat(internalformat, format, type), width, height, depth, border,
                   VexaDesktopUploadFormat(format, type), type, data ? converted.data() : nullptr);
    return;
  }
  ::glTexImage3D(target, level, internalformat, width, height, depth, border, format, type, data);
}

static void VexaGL_glTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei width, GLsizei height,
                                  GLsizei depth, GLenum format, GLenum type, const void* data) {
  std::vector<uint8_t> converted;
  if (VexaConvertDesktopVolume(format, type, width, height, depth, data, &converted)) {
    VexaUnpackScope unpack;
    ::glTexSubImage3D(target, level, x, y, z, width, height, depth, VexaDesktopUploadFormat(format, type), type,
                      converted.data());
    return;
  }
  ::glTexSubImage3D(target, level, x, y, z, width, height, depth, format, type, data);
}

static void VexaGL_glDeleteTextures(GLsizei count, const GLuint* textures) {
  if (count > 0 && textures) {
    std::lock_guard lock(VexaUnorm16Mutex);
    for (GLsizei i = 0; i < count; ++i) VexaUnorm16Textures.erase(textures[i]);
  }
  ::glDeleteTextures(count, textures);
}
