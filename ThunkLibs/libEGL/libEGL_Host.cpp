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

namespace {
constexpr uint64_t kThunkEGLCreateWindowSurface = 0x45474C000001ULL;
}

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
  HyMobileTraceThunkEvent("eglCreateWindowSurface", HYMOBILE_THUNK_EVENT_ENTER, kThunkEGLCreateWindowSurface,
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(dpy)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(config)),
                          win,
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(attrib_list)));
  if (win == 0) {
    HyMobileAbortThunkNullTarget("eglCreateWindowSurface", "native window handle is null", kThunkEGLCreateWindowSurface,
                                 static_cast<uint64_t>(reinterpret_cast<uintptr_t>(dpy)),
                                 static_cast<uint64_t>(reinterpret_cast<uintptr_t>(config)));
  }
  auto result = ::eglCreateWindowSurface(dpy, config, DecodeNativeWindowHandle<EGLNativeWindowType>(win), attrib_list);
  HyMobileTraceThunkEvent("eglCreateWindowSurface", HYMOBILE_THUNK_EVENT_EXIT, kThunkEGLCreateWindowSurface,
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)),
                          static_cast<uint64_t>(eglGetError()),
                          win);
  return result;
}

EXPORTS(libEGL)
