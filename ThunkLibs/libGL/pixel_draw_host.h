#pragma once

// A restricted GLES 3 color-pixel blit building block. This is NOT a complete
// glDrawPixels implementation: in particular it cannot run an application's
// fragment program, perform pixel texturing or handle depth/stencil pixels.
// Callers must reject those cases and provide already unpacked RGBA float data.
#include <GLES3/gl3.h>
#include <cmath>

namespace vexa_pixel {
inline GLuint compile_shader(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  if (!shader) return 0;
  glShaderSource(shader, 1, &source, nullptr); glCompileShader(shader);
  GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) { glDeleteShader(shader); return 0; }
  return shader;
}

// Caller supplies the window-space raster position, depth and pixel zoom.
// All pointers refer to process memory; no GPU readback is involved. The
// framebuffer, viewport, blend/depth/stencil/scissor/color masks remain bound.
inline bool draw_rgba_float(const float* rgba, int width, int height,
                            float x, float y, float depth, float zoom_x, float zoom_y,
                            const float* bitmap_color = nullptr) {
  if (!rgba || width <= 0 || height <= 0) return false;
  if (zoom_x == 0 || zoom_y == 0) return true;
  GLint program = 0, vao = 0, active = 0, texture = 0, sampler = 0, unpack_buffer = 0;
  GLint align = 0, row_length = 0, skip_rows = 0, skip_pixels = 0, viewport[4]{};
  GLfloat range[2]{};
  glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  // Replacing a user fragment program would silently change glDrawPixels semantics.
  if (program != 0) return false;
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
  glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
  glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
  glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
  glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
  glGetIntegerv(GL_VIEWPORT, viewport);
  glGetFloatv(GL_DEPTH_RANGE, range);
  if (viewport[2] <= 0 || viewport[3] <= 0 ||
      !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(depth) ||
      !std::isfinite(zoom_x) || !std::isfinite(zoom_y)) return false;
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
  glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
  const GLboolean cull = glIsEnabled(GL_CULL_FACE);
  static const char* vs = R"(#version 300 es
precision highp float;
uniform vec4 rect;
uniform vec4 viewport;
uniform float ndcz;
out vec2 uv;
void main() {
  vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
  vec2 pos = rect.xy + p * rect.zw;
  gl_Position = vec4((pos - viewport.xy) * 2.0 / viewport.zw - 1.0, ndcz, 1.0);
  uv = p;
})";
  static const char* fs = R"(#version 300 es
precision highp float;
uniform highp sampler2D image;
uniform bool bitmap;
uniform vec4 bitmap_color;
in vec2 uv;
out vec4 color;
void main() {
  vec4 sample_value = texture(image, uv);
  if (bitmap) { if (sample_value.r < 0.5) discard; color = bitmap_color; }
  else color = sample_value;
}
)";
  GLuint vertex = compile_shader(GL_VERTEX_SHADER, vs), fragment = compile_shader(GL_FRAGMENT_SHADER, fs);
  GLuint draw_program = 0, draw_texture = 0, draw_vao = 0;
  bool ok = false;
  if (vertex && fragment) {
    draw_program = glCreateProgram();
    glAttachShader(draw_program, vertex); glAttachShader(draw_program, fragment); glLinkProgram(draw_program);
    GLint linked = 0; glGetProgramiv(draw_program, GL_LINK_STATUS, &linked);
    if (linked) {
      glGenTextures(1, &draw_texture); glGenVertexArrays(1, &draw_vao);
      if (draw_texture && draw_vao) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0); glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glBindTexture(GL_TEXTURE_2D, draw_texture); glBindSampler(0, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, rgba);
        glUseProgram(draw_program); glBindVertexArray(draw_vao);
        glUniform1i(glGetUniformLocation(draw_program, "image"), 0);
        glUniform1i(glGetUniformLocation(draw_program, "bitmap"), bitmap_color ? 1 : 0);
        if (bitmap_color) glUniform4fv(glGetUniformLocation(draw_program, "bitmap_color"), 1, bitmap_color);
        glUniform4f(glGetUniformLocation(draw_program, "rect"), x, y, width * zoom_x, height * zoom_y);
        glUniform4f(glGetUniformLocation(draw_program, "viewport"), viewport[0], viewport[1], viewport[2], viewport[3]);
        const float ndcz = range[0] == range[1] ? 0.0f : (2 * depth - range[0] - range[1]) / (range[1] - range[0]);
        glUniform1f(glGetUniformLocation(draw_program, "ndcz"), ndcz);
        if (cull) glDisable(GL_CULL_FACE);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (cull) glEnable(GL_CULL_FACE);
        ok = true;
      }
    }
  }
  glUseProgram(program); glBindVertexArray(vao);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer);
  glPixelStorei(GL_UNPACK_ALIGNMENT, align); glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows); glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
  glBindTexture(GL_TEXTURE_2D, texture); glBindSampler(0, sampler); glActiveTexture(active);
  if (draw_vao) glDeleteVertexArrays(1, &draw_vao);
  if (draw_texture) glDeleteTextures(1, &draw_texture);
  if (draw_program) glDeleteProgram(draw_program);
  if (vertex) glDeleteShader(vertex);
  if (fragment) glDeleteShader(fragment);
  return ok;
}
} // namespace vexa_pixel
