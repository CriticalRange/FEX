#include <common/GeneratorInterface.h>

#include <EGL/egl.h>

template<typename>
struct fex_gen_type {};

struct ANativeWindow;
struct egl_native_pixmap_t;
template<>
struct fex_gen_type<ANativeWindow> : fexgen::opaque_type {};
template<>
struct fex_gen_type<egl_native_pixmap_t> : fexgen::opaque_type {};

template<auto>
struct fex_gen_config {
  unsigned version = 1;
};

// Function, parameter index, parameter type [optional]
template<auto, int, typename = void>
struct fex_gen_param {};

template<>
struct fex_gen_config<eglBindAPI> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<eglChooseConfig> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<eglDestroyContext> {};
template<>
struct fex_gen_config<eglDestroySurface> {};
template<>
struct fex_gen_config<eglInitialize> {};
template<>
struct fex_gen_config<eglMakeCurrent> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<eglQuerySurface> {};
template<>
struct fex_gen_config<eglSurfaceAttrib> {};
template<>
struct fex_gen_config<eglSwapBuffers> {};
template<>
struct fex_gen_config<eglSwapInterval> {};
template<>
struct fex_gen_config<eglTerminate> {};
template<>
struct fex_gen_config<eglGetError> {};
template<>
struct fex_gen_config<eglCreateContext> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<eglCreateWindowSurface> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<eglGetCurrentContext> {};
template<>
struct fex_gen_config<eglGetCurrentDisplay> {};
template<>
struct fex_gen_config<eglGetCurrentSurface> {};

// EGLNativeDisplayType is a pointer to opaque data (wl_display/(X)Display/...)
template<>
struct fex_gen_config<eglGetDisplay> : fexgen::custom_host_impl {};
template<>
struct fex_gen_param<eglGetDisplay, 0, EGLNativeDisplayType> : fexgen::assume_compatible_data_layout {};

// es2gears and glmark2-es2 resolve this at load or through dlsym.
template<>
struct fex_gen_config<eglGetConfigAttrib> {};
