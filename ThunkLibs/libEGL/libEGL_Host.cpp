/*
$info$
tags: thunklibs|EGL
$end_info$
*/

#include <cstdio>
#include <dlfcn.h>

#include <EGL/egl.h>

#include "common/Host.h"
#include <dlfcn.h>

#ifdef BUILD_ANDROID
// VEXA_FIXES: Android EGL thunkgen can serialize native window handles as
// guest-sized integers while host EGL expects EGLNativeWindowType pointer form.
template<>
struct host_layout<EGLNativeWindowType> {
  EGLNativeWindowType data;

  explicit host_layout(const guest_layout<uint64_t>& from)
    : data {reinterpret_cast<EGLNativeWindowType>(uintptr_t {from.data})} {}

  explicit host_layout(const guest_layout<uint32_t>& from)
    : data {reinterpret_cast<EGLNativeWindowType>(uintptr_t {from.data})} {}
};
#endif

#include "thunkgen_host_libEGL.inl"

EXPORTS(libEGL)
