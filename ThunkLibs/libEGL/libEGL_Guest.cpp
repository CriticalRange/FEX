/*
$info$
tags: thunklibs|EGL
desc: Depends on glXGetProcAddress thunk
$end_info$
*/

typedef void voidFunc();

// VEXA_FIXES: Android NDK sysroot has no GLX headers; keep the guest bridge by
// forward-declaring glXGetProcAddress and using GLES types for GLubyte.
#ifdef BUILD_ANDROID
#include <GLES3/gl3.h>
extern "C" voidFunc* glXGetProcAddress(const GLubyte* procname);
#else
#include <GL/glx.h>
#endif
#include <EGL/egl.h>
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/eglext.h>

#include <stdio.h>
#include <cstring>

#include "common/Guest.h"

#include "thunkgen_guest_libEGL.inl"

extern "C" {
const char* eglQueryString(EGLDisplay, EGLint name) {
  switch (name) {
    case EGL_VERSION: return "1.5 VEXA";
    case EGL_VENDOR: return "VEXA";
    case EGL_CLIENT_APIS: return "OpenGL_ES";
    case EGL_EXTENSIONS: return "EGL_KHR_image EGL_KHR_gl_texture_2D_image";
    default: return "";
  }
}

EGLSurface eglCreatePixmapSurface(EGLDisplay, EGLConfig, EGLNativePixmapType, const EGLint*) {
  fprintf(stderr, "[VEXA][EGL] eglCreatePixmapSurface unsupported\n");
  return EGL_NO_SURFACE;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint*) {
  return EGL_NO_SURFACE;
}

EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay, EGLenum, EGLClientBuffer, EGLConfig, const EGLint*) {
  return EGL_NO_SURFACE;
}

EGLBoolean eglCopyBuffers(EGLDisplay, EGLSurface, EGLNativePixmapType) {
  return EGL_FALSE;
}

EGLBoolean eglGetConfigs(EGLDisplay, EGLConfig* configs, EGLint config_size, EGLint* num_config) {
  if (num_config) {
    *num_config = 0;
  }
  (void)configs;
  (void)config_size;
  return EGL_TRUE;
}

EGLBoolean eglQueryContext(EGLDisplay, EGLContext, EGLint, EGLint* value) {
  if (value) {
    *value = 0;
  }
  return EGL_TRUE;
}

EGLBoolean eglWaitGL(void) { return EGL_TRUE; }
EGLBoolean eglWaitNative(EGLint) { return EGL_TRUE; }
EGLBoolean eglWaitClient(void) { return EGL_TRUE; }
EGLBoolean eglBindTexImage(EGLDisplay, EGLSurface, EGLint) { return EGL_FALSE; }
EGLBoolean eglReleaseTexImage(EGLDisplay, EGLSurface, EGLint) { return EGL_FALSE; }
EGLenum eglQueryAPI(void) { return EGL_OPENGL_ES_API; }
EGLBoolean eglReleaseThread(void) { return EGL_TRUE; }

EGLSync eglCreateSync(EGLDisplay, EGLenum, const EGLAttrib*) { return EGL_NO_SYNC; }
EGLBoolean eglDestroySync(EGLDisplay, EGLSync) { return EGL_TRUE; }
EGLint eglClientWaitSync(EGLDisplay, EGLSync, EGLint, EGLTime) { return EGL_CONDITION_SATISFIED; }
EGLBoolean eglGetSyncAttrib(EGLDisplay, EGLSync, EGLint, EGLAttrib* value) {
  if (value) {
    *value = 0;
  }
  return EGL_TRUE;
}
EGLBoolean eglWaitSync(EGLDisplay, EGLSync, EGLint) { return EGL_TRUE; }
EGLImage eglCreateImage(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLAttrib*) { return EGL_NO_IMAGE; }
EGLBoolean eglDestroyImage(EGLDisplay, EGLImage) { return EGL_TRUE; }

EGLDisplay eglGetPlatformDisplay(EGLenum, void*, const EGLAttrib*) { return EGL_NO_DISPLAY; }
EGLDisplay eglGetPlatformDisplayEXT(EGLenum, void*, const EGLint*) { return EGL_NO_DISPLAY; }
EGLSurface eglCreatePlatformWindowSurface(EGLDisplay, EGLConfig, void*, const EGLAttrib*) { return EGL_NO_SURFACE; }
EGLSurface eglCreatePlatformPixmapSurface(EGLDisplay, EGLConfig, void*, const EGLAttrib*) { return EGL_NO_SURFACE; }
EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay, EGLConfig, void*, const EGLint*) { return EGL_NO_SURFACE; }
EGLSurface eglCreatePlatformPixmapSurfaceEXT(EGLDisplay, EGLConfig, void*, const EGLint*) { return EGL_NO_SURFACE; }

voidFunc* eglGetProcAddress(const char* procname) {
  if (!procname) {
    return nullptr;
  }
  if (std::strcmp(procname, "eglQueryString") == 0) return reinterpret_cast<voidFunc*>(eglQueryString);
  if (std::strcmp(procname, "eglGetConfigAttrib") == 0) return reinterpret_cast<voidFunc*>(eglGetConfigAttrib);
  if (std::strcmp(procname, "eglCreatePixmapSurface") == 0) return reinterpret_cast<voidFunc*>(eglCreatePixmapSurface);
  if (std::strcmp(procname, "eglSwapInterval") == 0) return reinterpret_cast<voidFunc*>(eglSwapInterval);
  if (std::strcmp(procname, "eglGetPlatformDisplayEXT") == 0) return reinterpret_cast<voidFunc*>(eglGetPlatformDisplayEXT);
  if (std::strcmp(procname, "eglGetPlatformDisplay") == 0) return reinterpret_cast<voidFunc*>(eglGetPlatformDisplay);
  return glXGetProcAddress(reinterpret_cast<const GLubyte*>(procname));
}
}

LOAD_LIB(libEGL)
