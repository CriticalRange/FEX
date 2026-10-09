/*
$info$
tags: thunklibs|EGL
$end_info$
*/

#include <cstdio>
#include <dlfcn.h>

#include <EGL/egl.h>

#include "common/Host.h"
#if defined(BUILD_ANDROID)
#include "common/AndroidWindow.h"
#endif
#include <dlfcn.h>

#if defined(BUILD_ANDROID)
static auto fexfn_impl_libEGL_eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context) -> EGLBoolean {
  EGLBoolean ok = eglMakeCurrent(display, draw, read, context);
  if (ok != EGL_TRUE || draw == EGL_NO_SURFACE || context == EGL_NO_CONTEXT) {
    return ok;
  }
  using ViewportFn = void (*)(EGLint, EGLint, EGLint, EGLint);
  auto viewport = reinterpret_cast<ViewportFn>(eglGetProcAddress("glViewport"));
  ANativeWindow* window = VexaAcquireRuntimeWindow(0);
  if (!viewport || !window) {
    if (window) {
      ANativeWindow_release(window);
    }
    return ok;
  }
  int surface_w = ANativeWindow_getWidth(window);
  int surface_h = ANativeWindow_getHeight(window);
  ANativeWindow_release(window);
  if (surface_w > 0 && surface_h > 0) {
    EGLint view_h = surface_h / 2;
    EGLint bottom = surface_h - view_h - surface_h / 10;
    if (bottom < surface_h / 3) {
      bottom = surface_h / 3;
    }
    if (bottom + view_h > surface_h) {
      view_h = surface_h - bottom;
    }
    viewport(0, bottom, surface_w, view_h);
    std::fprintf(stderr, "[VEXA][EGL] default viewport 0,%d %dx%d on %dx%d\n",
                 bottom, surface_w, view_h, surface_w, surface_h);
  }
  return ok;
}

static auto fexfn_impl_libEGL_eglGetDisplay(EGLNativeDisplayType) -> EGLDisplay {
  EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  std::fprintf(stderr, "[VEXA][EGL] eglGetDisplay -> %p\n", display);
  return display;
}

// Android exposes GLES configs only. Mesa eglgears asks for EGL_OPENGL_BIT.
static constexpr EGLint kVexaEs3Bit = 0x00000040;

static const EGLint* VexaRewriteEglAttribs(const EGLint* attribs, EGLint* storage, int storage_count, bool* rewrote) {
  *rewrote = false;
  if (!attribs) {
    return attribs;
  }
  int index = 0;
  while (index + 1 < storage_count && attribs[index] != EGL_NONE) {
    storage[index] = attribs[index];
    storage[index + 1] = attribs[index + 1];
    if (attribs[index] == EGL_RENDERABLE_TYPE && (attribs[index + 1] & EGL_OPENGL_BIT)) {
      storage[index + 1] = (attribs[index + 1] & ~EGL_OPENGL_BIT) | EGL_OPENGL_ES2_BIT | kVexaEs3Bit;
      *rewrote = true;
    }
    index += 2;
  }
  storage[index] = EGL_NONE;
  return *rewrote ? storage : attribs;
}

static auto fexfn_impl_libEGL_eglBindAPI(EGLenum api) -> EGLBoolean {
  EGLenum host_api = api == EGL_OPENGL_API ? EGL_OPENGL_ES_API : api;
  EGLBoolean ok = eglBindAPI(host_api);
  std::fprintf(stderr, "[VEXA][EGL] eglBindAPI 0x%x -> 0x%x (%d)\n", api, host_api, ok);
  return ok;
}

static auto fexfn_impl_libEGL_eglChooseConfig(EGLDisplay display, const EGLint* attribs, EGLConfig* configs, EGLint config_size, EGLint* num_config) -> EGLBoolean {
  EGLint rewritten[48];
  bool rewrote = false;
  const EGLint* use = VexaRewriteEglAttribs(attribs, rewritten, 48, &rewrote);
  if (rewrote) {
    std::fprintf(stderr, "[VEXA][EGL] eglChooseConfig rewrote OPENGL_BIT to ES2|ES3\n");
  }
  EGLBoolean ok = eglChooseConfig(display, use, configs, config_size, num_config);
  if (ok == EGL_TRUE && num_config && *num_config > 0) {
    std::fprintf(stderr, "[VEXA][EGL] eglChooseConfig -> %d\n", *num_config);
    return ok;
  }
  static const EGLint fallback[] = {
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, kVexaEs3Bit,
    EGL_NONE
  };
  std::fprintf(stderr, "[VEXA][EGL] eglChooseConfig fallback after num=%d err=0x%x\n",
               num_config ? *num_config : -1, eglGetError());
  return eglChooseConfig(display, fallback, configs, config_size, num_config);
}

static auto fexfn_impl_libEGL_eglCreateContext(EGLDisplay display, EGLConfig config, EGLContext share, const EGLint* attribs) -> EGLContext {
  bool has_version = false;
  if (attribs) {
    for (int index = 0; attribs[index] != EGL_NONE && index < 32; index += 2) {
      if (attribs[index] == EGL_CONTEXT_CLIENT_VERSION) {
        has_version = true;
      }
    }
  }
  const EGLint es3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  const EGLint* use = has_version ? attribs : es3;
  if (!has_version) {
    std::fprintf(stderr, "[VEXA][EGL] eglCreateContext forcing ES 3\n");
  }
  EGLContext context = eglCreateContext(display, config, share, use);
  if (context == EGL_NO_CONTEXT && !has_version) {
    const EGLint es2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    std::fprintf(stderr, "[VEXA][EGL] eglCreateContext ES3 failed 0x%x\n", eglGetError());
    context = eglCreateContext(display, config, share, es2);
  }
  std::fprintf(stderr, "[VEXA][EGL] eglCreateContext -> %p\n", context);
  return context;
}

static auto fexfn_impl_libEGL_eglCreateWindowSurface(EGLDisplay display, EGLConfig config, EGLNativeWindowType, const EGLint* attribs) -> EGLSurface {
  ANativeWindow* window = VexaAcquireRuntimeWindow(2000);
  if (!window) {
    return EGL_NO_SURFACE;
  }
  EGLint format = 0;
  if (eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &format) == EGL_TRUE && format != 0) {
    ANativeWindow_setBuffersGeometry(window, 0, 0, format);
  }
  EGLSurface surface = eglCreateWindowSurface(display, config, window, attribs);
  if (surface == EGL_NO_SURFACE) {
    std::fprintf(stderr, "[VEXA][EGL] eglCreateWindowSurface failed 0x%x\n", eglGetError());
    ANativeWindow_release(window);
    return EGL_NO_SURFACE;
  }
  std::fprintf(stderr, "[VEXA][EGL] window surface %p\n", surface);
  return surface;
}
#endif

#include "thunkgen_host_libEGL.inl"

EXPORTS(libEGL)
