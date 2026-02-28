/*
$info$
tags: thunklibs|EGL
$end_info$
*/

#include <cstdio>
#include <cstdint>
#include <dlfcn.h>
#include <type_traits>

#include <EGL/egl.h>

#include "common/Host.h"
#include <dlfcn.h>

#include "thunkgen_host_libEGL.inl"

template<typename T>
static T DecodeNativeWindowHandle(uint64_t win) {
  if constexpr (std::is_pointer_v<T>) {
    return reinterpret_cast<T>(static_cast<uintptr_t>(win));
  } else {
    return static_cast<T>(win);
  }
}

static EGLSurface fexfn_impl_libEGL_FEX_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, uint64_t win,
                                                               const EGLint* attrib_list) {
  return ::eglCreateWindowSurface(dpy, config, DecodeNativeWindowHandle<EGLNativeWindowType>(win), attrib_list);
}

EXPORTS(libEGL)
