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

#include <stdio.h>
#include <cstring>

#include "common/Guest.h"

#include "thunkgen_guest_libEGL.inl"

extern "C" {
voidFunc* eglGetProcAddress(const char* procname) {
  // TODO: Fix this HACK
  return glXGetProcAddress((const GLubyte*)procname);
}
}

LOAD_LIB(libEGL)
