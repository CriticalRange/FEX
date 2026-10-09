#include <GLES3/gl3.h>

#include <cstdio>

#include "common/AndroidWindow.h"
#include "common/Host.h"

static void fexfn_impl_libGLESv2_glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
  static bool logged = false;
  static int surface_w = 0;
  static int surface_h = 0;
  if (surface_w <= 0 || surface_h <= 0) {
    ANativeWindow* window = VexaAcquireRuntimeWindow(0);
    if (window) {
      surface_w = ANativeWindow_getWidth(window);
      surface_h = ANativeWindow_getHeight(window);
      ANativeWindow_release(window);
    }
  }
  if (surface_w > 0 && surface_h > 0 && width > 0 && height > 0) {
    GLsizei side = width;
    if (side < surface_w / 2) {
      side = surface_w;
    }
    if (side > surface_w) {
      side = surface_w;
    }
    GLsizei view_h = static_cast<GLsizei>((static_cast<int64_t>(side) * height) / width);
    if (view_h > surface_h / 2) {
      view_h = surface_h / 2;
      side = static_cast<GLsizei>((static_cast<int64_t>(view_h) * width) / height);
    }
    // The GameActivity log panel covers the bottom of the buffer. GL's origin is there.
    GLint bottom = surface_h - view_h - surface_h / 10;
    if (bottom < surface_h / 3) {
      bottom = surface_h / 3;
    }
    if (bottom + view_h > surface_h) {
      view_h = surface_h - bottom;
    }
    glViewport(0, bottom, side, view_h);
    if (!logged) {
      logged = true;
      std::fprintf(stderr, "[VEXA][GLES] glViewport %d,%d %dx%d -> 0,%d %dx%d on %dx%d\n",
                   x, y, width, height, bottom, side, view_h, surface_w, surface_h);
    }
    return;
  }
  glViewport(x, y, width, height);
}

#include "thunkgen_host_libGLESv2.inl"

EXPORTS(libGLESv2)
