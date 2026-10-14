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
static void VexaGL_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
                                  GLsizei width, GLsizei height, GLenum format, GLenum type, const void* data) {
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

static void VexaGL_glDeleteTextures(GLsizei count, const GLuint* textures) {
  if (count > 0 && textures) {
    std::lock_guard lock(VexaUnorm16Mutex);
    for (GLsizei i = 0; i < count; ++i) VexaUnorm16Textures.erase(textures[i]);
  }
  ::glDeleteTextures(count, textures);
}
