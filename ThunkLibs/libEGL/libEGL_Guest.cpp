/*
$info$
tags: thunklibs|EGL
desc: Depends on glXGetProcAddress thunk
$end_info$
*/

#include <GL/glx.h>
#include <EGL/egl.h>

#include <cstdint>
#include <stdio.h>
#include <cstring>
#include <type_traits>

#include "common/Guest.h"

#include "thunkgen_guest_libEGL.inl"

typedef void voidFunc();

template<typename T>
static uint64_t EncodeNativeWindowHandle(T win) {
  if constexpr (std::is_pointer_v<T>) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(win));
  } else {
    return static_cast<uint64_t>(win);
  }
}

extern "C" {
voidFunc* eglGetProcAddress(const char* procname) {
  // TODO: Fix this HACK
  return glXGetProcAddress((const GLubyte*)procname);
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint* attrib_list) {
  return fexfn_pack_FEX_eglCreateWindowSurface(dpy, config, EncodeNativeWindowHandle(win), attrib_list);
}
}

LOAD_LIB(libEGL)
