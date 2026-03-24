/*
$info$
tags: thunklibs|SDL3
desc: Android SDL3 guest thunk with local GL proc lookup and local SDL error/log helpers
$end_info$
*/

#define GL_GLEXT_PROTOTYPES 1

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_version.h>

#if defined(BUILD_ANDROID) && __has_include(<android/log.h>)
#include <android/log.h>
#define VEXA_HAS_ANDROID_LOG 1
#else
#define VEXA_HAS_ANDROID_LOG 0
#endif

#undef GL_ARB_viewport_array
#include "../libGL/glcorearb.h"
// VEXA_FIXES: Reuse the same Android GL proc allowlist adopted by libGL thunk
// so SDL_GL_GetProcAddress follows identical bridge coverage.
#include "../libGL/android_gl_proc_list.h"

extern "C" {
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
}

#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/Guest.h"

#if defined(BUILD_ANDROID)
static int VexaSDL3GuestVfprintf(FILE* stream, const char* fmt, va_list args) {
  va_list copy;
  va_copy(copy, args);
  const int rc = vfprintf(stream, fmt, args);
  if (stream == stderr) {
#if VEXA_HAS_ANDROID_LOG
    __android_log_vprint(ANDROID_LOG_ERROR, "Vexa-SDL3Thunk", fmt, copy);
#endif
  }
  va_end(copy);
  return rc;
}

static int VexaSDL3GuestFprintf(FILE* stream, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const int rc = VexaSDL3GuestVfprintf(stream, fmt, args);
  va_end(args);
  return rc;
}

#define fprintf(stream, ...) VexaSDL3GuestFprintf(stream, __VA_ARGS__)
#define vfprintf(stream, fmt, args) VexaSDL3GuestVfprintf(stream, fmt, args)
#endif

#include "thunkgen_guest_libSDL3.inl"

namespace {
// VEXA_FIXES: Keep SDL thunk GL proc diagnostics in app-internal artifacts storage.
constexpr const char* SDL3GLProcTracePath = "/data/user/0/com.critical.vexaemulator/files/artifacts/gl_proc_trace.log";

thread_local std::string LastError;
thread_local std::string KeyNameBuffer;
std::atomic<SDL_InitFlags> InitializedSubsystems {};
std::atomic<Uint16> CurrentModState {SDL_KMOD_NONE};
std::atomic<int> WindowWidth {0};
std::atomic<int> WindowHeight {0};
std::atomic<int> WindowPosX {0};
std::atomic<int> WindowPosY {0};
std::atomic<int> WindowMinWidth {0};
std::atomic<int> WindowMinHeight {0};
std::atomic<int> WindowMaxWidth {0};
std::atomic<int> WindowMaxHeight {0};
std::atomic<uintptr_t> PrimaryWindowPointer {0};
std::atomic<uintptr_t> ParentWindowPointer {0};
std::atomic<Uint64> WindowFlagsState {0};
std::mutex WindowStateMutex;
std::mutex WindowTitleMutex;
std::mutex SyntheticEventQueueMutex;
std::deque<SDL_Event> SyntheticEventQueue;
// VEXA_FIXES: Default synthetic window title should match Vexa branding.
std::string WindowTitle {"Vexa SDL3"};
SDL_DisplayMode FullscreenWindowMode {};
bool HasFullscreenWindowMode = false;

constexpr SDL_DisplayID PrimaryDisplayId = 1;
constexpr SDL_WindowID PrimaryWindowId = 1;
constexpr size_t GLAttributeCount = static_cast<size_t>(SDL_GL_EGL_PLATFORM) + 1;

std::array<int, GLAttributeCount> GLAttributes {};
std::atomic<bool> GLAttributesInitialized {false};
std::atomic<uintptr_t> CurrentGLWindowPointer {0};
std::atomic<uintptr_t> CurrentGLContextPointer {0};
std::atomic<int> CurrentSwapInterval {0};
std::atomic<bool> GLLibraryLoaded {true};
std::atomic<uint32_t> SDLValidationLogBudget {192};

struct SDLPropertyValue {
  SDL_PropertyType Type {SDL_PROPERTY_TYPE_INVALID};
  void* PointerValue {};
  std::string StringValue {};
  Sint64 NumberValue {};
  float FloatValue {};
  bool BooleanValue {};
};

std::mutex PropertiesMutex;
std::unordered_map<SDL_PropertiesID, std::unordered_map<std::string, SDLPropertyValue>> PropertiesById;
std::atomic<SDL_PropertiesID> NextPropertiesId {1};
std::atomic<SDL_PropertiesID> GlobalPropertiesId {0};
std::atomic<SDL_PropertiesID> PrimaryWindowPropertiesId {0};
std::atomic<SDL_PropertiesID> PrimaryDisplayPropertiesId {0};

struct KeyLookupEntry {
  std::string_view Name;
  SDL_Keycode Keycode;
  SDL_Scancode Scancode;
};

constexpr KeyLookupEntry KeyLookupTable[] = {
  {"Return", SDLK_RETURN, SDL_SCANCODE_RETURN},
  {"Escape", SDLK_ESCAPE, SDL_SCANCODE_ESCAPE},
  {"Backspace", SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE},
  {"Tab", SDLK_TAB, SDL_SCANCODE_TAB},
  {"Space", SDLK_SPACE, SDL_SCANCODE_SPACE},
  {"Delete", SDLK_DELETE, SDL_SCANCODE_DELETE},
  {"Up", SDLK_UP, SDL_SCANCODE_UP},
  {"Down", SDLK_DOWN, SDL_SCANCODE_DOWN},
  {"Left", SDLK_LEFT, SDL_SCANCODE_LEFT},
  {"Right", SDLK_RIGHT, SDL_SCANCODE_RIGHT},
  {"F1", SDLK_F1, SDL_SCANCODE_F1},
  {"F2", SDLK_F2, SDL_SCANCODE_F2},
  {"F3", SDLK_F3, SDL_SCANCODE_F3},
  {"F4", SDLK_F4, SDL_SCANCODE_F4},
  {"F5", SDLK_F5, SDL_SCANCODE_F5},
  {"F6", SDLK_F6, SDL_SCANCODE_F6},
  {"F7", SDLK_F7, SDL_SCANCODE_F7},
  {"F8", SDLK_F8, SDL_SCANCODE_F8},
  {"F9", SDLK_F9, SDL_SCANCODE_F9},
  {"F10", SDLK_F10, SDL_SCANCODE_F10},
  {"F11", SDLK_F11, SDL_SCANCODE_F11},
  {"F12", SDLK_F12, SDL_SCANCODE_F12},
  {"Left Shift", SDLK_LSHIFT, SDL_SCANCODE_LSHIFT},
  {"Right Shift", SDLK_RSHIFT, SDL_SCANCODE_RSHIFT},
  {"Left Ctrl", SDLK_LCTRL, SDL_SCANCODE_LCTRL},
  {"Right Ctrl", SDLK_RCTRL, SDL_SCANCODE_RCTRL},
  {"Left Alt", SDLK_LALT, SDL_SCANCODE_LALT},
  {"Right Alt", SDLK_RALT, SDL_SCANCODE_RALT},
};

bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  for (size_t i = 0; i < lhs.size(); ++i) {
    const unsigned char left = static_cast<unsigned char>(lhs[i]);
    const unsigned char right = static_cast<unsigned char>(rhs[i]);
    if (std::tolower(left) != std::tolower(right)) {
      return false;
    }
  }

  return true;
}

const KeyLookupEntry* FindByKeycode(SDL_Keycode keycode) {
  for (const auto& entry : KeyLookupTable) {
    if (entry.Keycode == keycode) {
      return &entry;
    }
  }
  return nullptr;
}

const KeyLookupEntry* FindByScancode(SDL_Scancode scancode) {
  for (const auto& entry : KeyLookupTable) {
    if (entry.Scancode == scancode) {
      return &entry;
    }
  }
  return nullptr;
}

const KeyLookupEntry* FindByName(std::string_view name) {
  for (const auto& entry : KeyLookupTable) {
    if (EqualsIgnoreCase(name, entry.Name)) {
      return &entry;
    }
  }
  return nullptr;
}

void UpdateFallbackWindowSize(int width, int height) {
  if (width > 0) {
    WindowWidth.store(width, std::memory_order_release);
  }
  if (height > 0) {
    WindowHeight.store(height, std::memory_order_release);
  }
}

void GetFallbackWindowSize(int* width, int* height) {
  int w = WindowWidth.load(std::memory_order_acquire);
  int h = WindowHeight.load(std::memory_order_acquire);

  if (width) {
    *width = w;
  }
  if (height) {
    *height = h;
  }
}

bool RefreshFallbackWindowSizeFromHost(SDL_Window* window) {
  SDL_Window* target = window;
  if (!target) {
    const uintptr_t primary = PrimaryWindowPointer.load(std::memory_order_acquire);
    target = reinterpret_cast<SDL_Window*>(primary);
  }

  if (!target) {
    return false;
  }

  int host_width = 0;
  int host_height = 0;
  const bool success = fexfn_pack_FEX_SDL_GetWindowSize(target, &host_width, &host_height) != 0;
  if (!success || host_width <= 0 || host_height <= 0) {
    return false;
  }

  UpdateFallbackWindowSize(host_width, host_height);
  return true;
}

bool IsKnownDisplay(SDL_DisplayID display_id) {
  return display_id == 0 || display_id == PrimaryDisplayId;
}

SDL_DisplayMode BuildFallbackDisplayMode() {
  int width {};
  int height {};
  GetFallbackWindowSize(&width, &height);

  SDL_DisplayMode mode {};
  mode.displayID = PrimaryDisplayId;
  mode.format = SDL_PIXELFORMAT_UNKNOWN;
  mode.w = width;
  mode.h = height;
  mode.pixel_density = 1.0f;
  mode.refresh_rate = 60.0f;
  mode.refresh_rate_numerator = 60;
  mode.refresh_rate_denominator = 1;
  mode.internal = nullptr;
  return mode;
}

bool IsValidGLAttr(SDL_GLAttr attr) {
  const int index = static_cast<int>(attr);
  return index >= 0 && index < static_cast<int>(GLAttributeCount);
}

// VEXA_FIXES: Keep parameter-validation logs rate-limited so frequent caller
// mistakes remain visible without flooding runtime logs.
void LogSDLValidationFailure(const char* function_name, const char* detail) {
  uint32_t remaining = SDLValidationLogBudget.load(std::memory_order_acquire);
  while (remaining > 0 &&
         !SDLValidationLogBudget.compare_exchange_weak(
             remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  if (remaining == 0) {
    return;
  }

  fprintf(stderr, "[SDL3 thunk][validation] %s: %s\n",
               function_name ? function_name : "<null>",
               detail ? detail : "unknown");
}

void EnsureGLAttributesInitialized() {
  if (GLAttributesInitialized.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  GLAttributes[SDL_GL_RED_SIZE] = 8;
  GLAttributes[SDL_GL_GREEN_SIZE] = 8;
  GLAttributes[SDL_GL_BLUE_SIZE] = 8;
  GLAttributes[SDL_GL_ALPHA_SIZE] = 8;
  GLAttributes[SDL_GL_DOUBLEBUFFER] = 1;
  GLAttributes[SDL_GL_DEPTH_SIZE] = 16;
  GLAttributes[SDL_GL_CONTEXT_MAJOR_VERSION] = 3;
  GLAttributes[SDL_GL_CONTEXT_MINOR_VERSION] = 0;
  GLAttributes[SDL_GL_CONTEXT_PROFILE_MASK] = SDL_GL_CONTEXT_PROFILE_ES;
}

void ClearCurrentGLStateIfMatching(SDL_GLContext context) {
  if (!context) {
    return;
  }

  const uintptr_t target = reinterpret_cast<uintptr_t>(context);
  if (CurrentGLContextPointer.load(std::memory_order_acquire) == target) {
    CurrentGLContextPointer.store(0, std::memory_order_release);
    CurrentGLWindowPointer.store(0, std::memory_order_release);
  }
}

bool ShouldTraceGLProcName(std::string_view name) {
  return name.rfind("gl", 0) == 0 ||
         name.rfind("egl", 0) == 0 ||
         name.rfind("SDL_GL_", 0) == 0;
}

void TraceGLProcLookup(const char* proc, const void* resolved, const char* source) {
  if (!proc) {
    return;
  }

  const std::string_view name {proc};
  if (!ShouldTraceGLProcName(name)) {
    return;
  }

  static std::atomic<uint32_t> budget {256};
  uint32_t remaining = budget.load(std::memory_order_acquire);
  while (remaining > 0 &&
         !budget.compare_exchange_weak(remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  if (remaining == 0) {
    return;
  }

  fprintf(stderr, "[SDL3 thunk] SDL_GL_GetProcAddress('%s') => %p via %s\n",
               proc,
               resolved,
               source ? source : "unknown");

  if (FILE* f = std::fopen(SDL3GLProcTracePath, "a")) {
    fprintf(f, "[SDL3 thunk] SDL_GL_GetProcAddress('%s') => %p via %s\n",
                 proc,
                 resolved,
                 source ? source : "unknown");
    std::fclose(f);
  }
}

// VEXA_FIXES: Resolve glXGetProcAddress lazily so loading libSDL3-guest.so
// doesn't require libGL symbols to be pre-exported in the current namespace.
void* SDL3ResolveGLProc(const GLubyte* procname) {
  const char* procname_str = reinterpret_cast<const char*>(procname);
  if (!procname_str || !*procname_str) {
    return nullptr;
  }

  using GLXGetProcAddressFn = void* (*)(const GLubyte*);
  static GLXGetProcAddressFn glx_get_proc = nullptr;
  static bool resolver_initialized = false;
  static bool resolver_missing_logged = false;
  static void* gl_handle = nullptr;
  static const char* gl_handle_name = nullptr;
  static bool gl_dlopen_failures_logged = false;
  static bool glx_dlsym_failure_logged = false;
  static bool gl_symbol_dlsym_failure_logged = false;

  if (!resolver_initialized) {
    resolver_initialized = true;
    glx_get_proc = reinterpret_cast<GLXGetProcAddressFn>(dlsym(RTLD_DEFAULT, "glXGetProcAddress"));
    if (!glx_get_proc) {
      glx_get_proc = reinterpret_cast<GLXGetProcAddressFn>(dlsym(RTLD_DEFAULT, "glXGetProcAddressARB"));
    }

    // Prefer explicitly loading the VEXA GL guest thunk so proc resolution
    // does not depend on global namespace ordering.
    if (!glx_get_proc) {
      constexpr const char* GLHandleCandidates[] = {
        "/data/user/0/com.critical.vexaemulator/files/thunks/guest/libGL-guest.so",
        "/data/data/com.critical.vexaemulator/files/thunks/guest/libGL-guest.so",
        "libGL-guest.so",
        "libGL.so.1",
        "libGL.so",
      };

      for (const char* candidate : GLHandleCandidates) {
        dlerror();
        void* handle = dlopen(candidate, RTLD_NOW | RTLD_GLOBAL);
        if (handle) {
          gl_handle = handle;
          gl_handle_name = candidate;
          fprintf(stderr, "[SDL3 thunk] gl resolver handle loaded: %s => %p\n", candidate, handle);
          break;
        }

        if (!gl_dlopen_failures_logged) {
          const char* err = dlerror();
          fprintf(stderr,
                  "[SDL3 thunk] dlopen('%s') failed while probing GL resolver: %s\n",
                  candidate,
                  err ? err : "<no dlerror>");
          gl_dlopen_failures_logged = true;
        }
      }

      if (gl_handle) {
        dlerror();
        glx_get_proc = reinterpret_cast<GLXGetProcAddressFn>(dlsym(gl_handle, "glXGetProcAddress"));
        if (!glx_get_proc) {
          if (!glx_dlsym_failure_logged) {
            const char* err = dlerror();
            fprintf(stderr,
                    "[SDL3 thunk] dlsym(%s,'glXGetProcAddress') failed: %s\n",
                    gl_handle_name ? gl_handle_name : "<unknown>",
                    err ? err : "<no dlerror>");
          }
          dlerror();
          glx_get_proc = reinterpret_cast<GLXGetProcAddressFn>(dlsym(gl_handle, "glXGetProcAddressARB"));
          if (!glx_get_proc && !glx_dlsym_failure_logged) {
            const char* err = dlerror();
            fprintf(stderr,
                    "[SDL3 thunk] dlsym(%s,'glXGetProcAddressARB') failed: %s\n",
                    gl_handle_name ? gl_handle_name : "<unknown>",
                    err ? err : "<no dlerror>");
            glx_dlsym_failure_logged = true;
          }
        }
      }
    }
  }

  if (!glx_get_proc) {
    if (!resolver_missing_logged) {
      resolver_missing_logged = true;
      fprintf(stderr, "[SDL3 thunk] missing glXGetProcAddress resolver while looking up '%s'\n", procname_str);
    }

    // Fallback path: resolve direct exports when glXGetProcAddress bridge
    // is not available yet in this namespace.
    if (void* direct = dlsym(RTLD_DEFAULT, procname_str)) {
      fprintf(stderr, "[SDL3 thunk] direct dlsym fallback('%s') => %p via RTLD_DEFAULT\n",
                   procname_str, direct);
      return direct;
    }

    if (gl_handle) {
      dlerror();
      if (void* direct = dlsym(gl_handle, procname_str)) {
        fprintf(stderr, "[SDL3 thunk] direct dlsym fallback('%s') => %p via libGL handle\n",
                     procname_str, direct);
        return direct;
      }
      if (!gl_symbol_dlsym_failure_logged) {
        const char* err = dlerror();
        fprintf(stderr,
                "[SDL3 thunk] dlsym(%s,'%s') failed in direct fallback: %s\n",
                gl_handle_name ? gl_handle_name : "<unknown>",
                procname_str,
                err ? err : "<no dlerror>");
        gl_symbol_dlsym_failure_logged = true;
      }
    }
    return nullptr;
  }

  if (void* resolved = glx_get_proc(procname)) {
    return resolved;
  }

  // Secondary fallback even when resolver exists but returns null.
  if (void* direct = dlsym(RTLD_DEFAULT, procname_str)) {
    fprintf(stderr, "[SDL3 thunk] secondary dlsym fallback('%s') => %p via RTLD_DEFAULT\n",
                 procname_str, direct);
    return direct;
  }
  if (gl_handle) {
    dlerror();
    if (void* direct = dlsym(gl_handle, procname_str)) {
      fprintf(stderr, "[SDL3 thunk] secondary dlsym fallback('%s') => %p via libGL handle\n",
                   procname_str, direct);
      return direct;
    }
    if (!gl_symbol_dlsym_failure_logged) {
      const char* err = dlerror();
      fprintf(stderr,
              "[SDL3 thunk] dlsym(%s,'%s') failed in secondary fallback: %s\n",
              gl_handle_name ? gl_handle_name : "<unknown>",
              procname_str,
              err ? err : "<no dlerror>");
      gl_symbol_dlsym_failure_logged = true;
    }
  }
  return nullptr;
}

// Android GLES drivers frequently expose desktop-style query APIs through EXT
// names only. Provide a small alias map so SDL_GL_GetProcAddress can resolve
// them without returning null to the guest.
std::array<const char*, 4> GetAndroidGLProcAliases(std::string_view proc_name) {
  if (proc_name == "glQueryCounter") {
    return {"glQueryCounterEXT", nullptr, nullptr, nullptr};
  }
  if (proc_name == "glGetQueryObjectui64v") {
    return {"glGetQueryObjectui64vEXT", nullptr, nullptr, nullptr};
  }
  if (proc_name == "glGenQueries") {
    return {"glGenQueriesEXT", nullptr, nullptr, nullptr};
  }
  if (proc_name == "glDeleteQueries") {
    return {"glDeleteQueriesEXT", nullptr, nullptr, nullptr};
  }
  if (proc_name == "glBeginQuery") {
    return {"glBeginQueryEXT", nullptr, nullptr, nullptr};
  }
  if (proc_name == "glEndQuery") {
    return {"glEndQueryEXT", nullptr, nullptr, nullptr};
  }
  if (proc_name == "glGetQueryObjectuiv") {
    return {"glGetQueryObjectuivEXT", nullptr, nullptr, nullptr};
  }
  return {nullptr, nullptr, nullptr, nullptr};
}

using GLClearDepthfFn = void (*)(GLfloat);
using GLClearStencilFn = void (*)(GLint);
using GLDepthRangefFn = void (*)(GLfloat, GLfloat);
using GLPolygonModeFn = void (*)(GLenum, GLenum);
using GLTexImage2DFn = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
using GLTexImage2DMultisampleFn = void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean);
using GLTexStorage2DMultisampleFn = void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean);
using GLDrawBufferFn = void (*)(GLenum);
using GLDrawBuffersFn = void (*)(GLsizei, const GLenum*);
using GLGetBufferSubDataFn = void (*)(GLenum, GLintptr, GLsizeiptr, void*);
using GLMapBufferRangeFn = void* (*)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
using GLUnmapBufferFn = GLboolean (*)(GLenum);
using GLVertexAttribI2iFn = void (*)(GLuint, GLint, GLint);
using GLVertexAttribI4iFn = void (*)(GLuint, GLint, GLint, GLint, GLint);
using GLGetFramebufferAttachmentParameterivFn = void (*)(GLenum, GLenum, GLenum, GLint*);
using GLGetTexLevelParameterivFn = void (*)(GLenum, GLint, GLenum, GLint*);
using GLGetIntegervFn = void (*)(GLenum, GLint*);
using GLGenFramebuffersFn = void (*)(GLsizei, GLuint*);
using GLDeleteFramebuffersFn = void (*)(GLsizei, const GLuint*);
using GLBindFramebufferFn = void (*)(GLenum, GLuint);
using GLFramebufferTexture2DFn = void (*)(GLenum, GLenum, GLenum, GLuint, GLint);
using GLCheckFramebufferStatusFn = GLenum (*)(GLenum);
using GLReadPixelsFn = void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);

std::atomic<GLClearDepthfFn> CachedGLClearDepthf {nullptr};
std::atomic<GLClearStencilFn> CachedGLClearStencil {nullptr};
std::atomic<GLDepthRangefFn> CachedGLDepthRangef {nullptr};
std::atomic<GLPolygonModeFn> CachedGLPolygonMode {nullptr};
std::atomic<bool> CachedGLPolygonModeResolved {false};
std::atomic<GLTexImage2DFn> CachedGLTexImage2D {nullptr};
std::atomic<GLTexImage2DMultisampleFn> CachedGLTexImage2DMultisample {nullptr};
std::atomic<bool> CachedGLTexImage2DMultisampleResolved {false};
std::atomic<GLTexStorage2DMultisampleFn> CachedGLTexStorage2DMultisample {nullptr};
std::atomic<GLDrawBufferFn> CachedGLDrawBuffer {nullptr};
std::atomic<bool> CachedGLDrawBufferResolved {false};
std::atomic<GLDrawBuffersFn> CachedGLDrawBuffers {nullptr};
std::atomic<GLGetBufferSubDataFn> CachedGLGetBufferSubData {nullptr};
std::atomic<bool> CachedGLGetBufferSubDataResolved {false};
std::atomic<GLMapBufferRangeFn> CachedGLMapBufferRange {nullptr};
std::atomic<GLUnmapBufferFn> CachedGLUnmapBuffer {nullptr};
std::atomic<GLVertexAttribI2iFn> CachedGLVertexAttribI2i {nullptr};
std::atomic<bool> CachedGLVertexAttribI2iResolved {false};
std::atomic<GLVertexAttribI4iFn> CachedGLVertexAttribI4i {nullptr};
std::atomic<GLGetFramebufferAttachmentParameterivFn> CachedGLGetFramebufferAttachmentParameteriv {nullptr};
std::atomic<bool> CachedGLGetFramebufferAttachmentParameterivResolved {false};
std::atomic<GLGetTexLevelParameterivFn> CachedGLGetTexLevelParameteriv {nullptr};
std::atomic<GLGetIntegervFn> CachedGLGetIntegerv {nullptr};
std::atomic<GLGenFramebuffersFn> CachedGLGenFramebuffers {nullptr};
std::atomic<GLDeleteFramebuffersFn> CachedGLDeleteFramebuffers {nullptr};
std::atomic<GLBindFramebufferFn> CachedGLBindFramebuffer {nullptr};
std::atomic<GLFramebufferTexture2DFn> CachedGLFramebufferTexture2D {nullptr};
std::atomic<GLCheckFramebufferStatusFn> CachedGLCheckFramebufferStatus {nullptr};
std::atomic<GLReadPixelsFn> CachedGLReadPixels {nullptr};

GLClearDepthfFn ResolveGLClearDepthf() {
  if (auto fn = CachedGLClearDepthf.load(std::memory_order_acquire)) {
    return fn;
  }

  auto fn = reinterpret_cast<GLClearDepthfFn>(SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glClearDepthf")));
  CachedGLClearDepthf.store(fn, std::memory_order_release);
  return fn;
}

GLClearStencilFn ResolveGLClearStencil() {
  if (auto fn = CachedGLClearStencil.load(std::memory_order_acquire)) {
    return fn;
  }

  auto fn = reinterpret_cast<GLClearStencilFn>(
      SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glClearStencil")));
  CachedGLClearStencil.store(fn, std::memory_order_release);
  return fn;
}

GLDepthRangefFn ResolveGLDepthRangef() {
  if (auto fn = CachedGLDepthRangef.load(std::memory_order_acquire)) {
    return fn;
  }

  auto fn = reinterpret_cast<GLDepthRangefFn>(SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glDepthRangef")));
  CachedGLDepthRangef.store(fn, std::memory_order_release);
  return fn;
}

GLTexImage2DFn ResolveGLTexImage2D() {
  if (auto fn = CachedGLTexImage2D.load(std::memory_order_acquire)) {
    return fn;
  }

  auto fn = reinterpret_cast<GLTexImage2DFn>(SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glTexImage2D")));
  CachedGLTexImage2D.store(fn, std::memory_order_release);
  return fn;
}

GLTexImage2DMultisampleFn ResolveGLTexImage2DMultisample() {
  if (CachedGLTexImage2DMultisampleResolved.load(std::memory_order_acquire)) {
    return CachedGLTexImage2DMultisample.load(std::memory_order_acquire);
  }

  GLTexImage2DMultisampleFn fn = reinterpret_cast<GLTexImage2DMultisampleFn>(
      SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glTexImage2DMultisample")));
  if (!fn) {
    fn = reinterpret_cast<GLTexImage2DMultisampleFn>(
        SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glTexImage2DMultisampleEXT")));
  }

  CachedGLTexImage2DMultisample.store(fn, std::memory_order_release);
  CachedGLTexImage2DMultisampleResolved.store(true, std::memory_order_release);
  return fn;
}

GLTexStorage2DMultisampleFn ResolveGLTexStorage2DMultisample() {
  if (auto fn = CachedGLTexStorage2DMultisample.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLTexStorage2DMultisampleFn>(
      SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glTexStorage2DMultisample")));
  CachedGLTexStorage2DMultisample.store(fn, std::memory_order_release);
  return fn;
}

GLDrawBufferFn ResolveGLDrawBuffer() {
  if (CachedGLDrawBufferResolved.load(std::memory_order_acquire)) {
    return CachedGLDrawBuffer.load(std::memory_order_acquire);
  }

  GLDrawBufferFn fn =
      reinterpret_cast<GLDrawBufferFn>(SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glDrawBuffer")));
  if (!fn) {
    fn = reinterpret_cast<GLDrawBufferFn>(SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glDrawBufferEXT")));
  }

  CachedGLDrawBuffer.store(fn, std::memory_order_release);
  CachedGLDrawBufferResolved.store(true, std::memory_order_release);
  return fn;
}

GLDrawBuffersFn ResolveGLDrawBuffers() {
  if (auto fn = CachedGLDrawBuffers.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLDrawBuffersFn>(SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glDrawBuffers")));
  CachedGLDrawBuffers.store(fn, std::memory_order_release);
  return fn;
}

GLGetBufferSubDataFn ResolveGLGetBufferSubData() {
  if (CachedGLGetBufferSubDataResolved.load(std::memory_order_acquire)) {
    return CachedGLGetBufferSubData.load(std::memory_order_acquire);
  }

  GLGetBufferSubDataFn fn = reinterpret_cast<GLGetBufferSubDataFn>(
      SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glGetBufferSubData")));
  if (!fn) {
    fn = reinterpret_cast<GLGetBufferSubDataFn>(
        SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glGetBufferSubDataARB")));
  }

  CachedGLGetBufferSubData.store(fn, std::memory_order_release);
  CachedGLGetBufferSubDataResolved.store(true, std::memory_order_release);
  return fn;
}

GLMapBufferRangeFn ResolveGLMapBufferRange() {
  if (auto fn = CachedGLMapBufferRange.load(std::memory_order_acquire)) {
    return fn;
  }

  GLMapBufferRangeFn fn = reinterpret_cast<GLMapBufferRangeFn>(
      SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glMapBufferRange")));
  if (!fn) {
    fn = reinterpret_cast<GLMapBufferRangeFn>(
        SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glMapBufferRangeEXT")));
  }

  CachedGLMapBufferRange.store(fn, std::memory_order_release);
  return fn;
}

GLUnmapBufferFn ResolveGLUnmapBuffer() {
  if (auto fn = CachedGLUnmapBuffer.load(std::memory_order_acquire)) {
    return fn;
  }

  GLUnmapBufferFn fn =
      reinterpret_cast<GLUnmapBufferFn>(SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glUnmapBuffer")));
  if (!fn) {
    fn = reinterpret_cast<GLUnmapBufferFn>(SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glUnmapBufferOES")));
  }

  CachedGLUnmapBuffer.store(fn, std::memory_order_release);
  return fn;
}

GLVertexAttribI2iFn ResolveGLVertexAttribI2i() {
  if (CachedGLVertexAttribI2iResolved.load(std::memory_order_acquire)) {
    return CachedGLVertexAttribI2i.load(std::memory_order_acquire);
  }

  GLVertexAttribI2iFn fn = reinterpret_cast<GLVertexAttribI2iFn>(
      SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glVertexAttribI2i")));
  if (!fn) {
    fn = reinterpret_cast<GLVertexAttribI2iFn>(
        SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glVertexAttribI2iEXT")));
  }

  CachedGLVertexAttribI2i.store(fn, std::memory_order_release);
  CachedGLVertexAttribI2iResolved.store(true, std::memory_order_release);
  return fn;
}

GLVertexAttribI4iFn ResolveGLVertexAttribI4i() {
  if (auto fn = CachedGLVertexAttribI4i.load(std::memory_order_acquire)) {
    return fn;
  }

  GLVertexAttribI4iFn fn = reinterpret_cast<GLVertexAttribI4iFn>(
      SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glVertexAttribI4i")));
  if (!fn) {
    fn = reinterpret_cast<GLVertexAttribI4iFn>(
        SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glVertexAttribI4iEXT")));
  }

  CachedGLVertexAttribI4i.store(fn, std::memory_order_release);
  return fn;
}

GLGetFramebufferAttachmentParameterivFn ResolveGLGetFramebufferAttachmentParameteriv() {
  if (CachedGLGetFramebufferAttachmentParameterivResolved.load(std::memory_order_acquire)) {
    return CachedGLGetFramebufferAttachmentParameteriv.load(std::memory_order_acquire);
  }

  GLGetFramebufferAttachmentParameterivFn fn = reinterpret_cast<GLGetFramebufferAttachmentParameterivFn>(
      SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glGetFramebufferAttachmentParameteriv")));
  if (!fn) {
    fn = reinterpret_cast<GLGetFramebufferAttachmentParameterivFn>(
        SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glGetFramebufferAttachmentParameterivEXT")));
  }

  CachedGLGetFramebufferAttachmentParameteriv.store(fn, std::memory_order_release);
  CachedGLGetFramebufferAttachmentParameterivResolved.store(true, std::memory_order_release);
  return fn;
}

GLGetTexLevelParameterivFn ResolveGLGetTexLevelParameteriv() {
  if (auto fn = CachedGLGetTexLevelParameteriv.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLGetTexLevelParameterivFn>(
      SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glGetTexLevelParameteriv")));
  CachedGLGetTexLevelParameteriv.store(fn, std::memory_order_release);
  return fn;
}

GLGetIntegervFn ResolveGLGetIntegerv() {
  if (auto fn = CachedGLGetIntegerv.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLGetIntegervFn>(SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glGetIntegerv")));
  CachedGLGetIntegerv.store(fn, std::memory_order_release);
  return fn;
}

GLGenFramebuffersFn ResolveGLGenFramebuffers() {
  if (auto fn = CachedGLGenFramebuffers.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLGenFramebuffersFn>(SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glGenFramebuffers")));
  CachedGLGenFramebuffers.store(fn, std::memory_order_release);
  return fn;
}

GLDeleteFramebuffersFn ResolveGLDeleteFramebuffers() {
  if (auto fn = CachedGLDeleteFramebuffers.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLDeleteFramebuffersFn>(SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glDeleteFramebuffers")));
  CachedGLDeleteFramebuffers.store(fn, std::memory_order_release);
  return fn;
}

GLBindFramebufferFn ResolveGLBindFramebuffer() {
  if (auto fn = CachedGLBindFramebuffer.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLBindFramebufferFn>(SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glBindFramebuffer")));
  CachedGLBindFramebuffer.store(fn, std::memory_order_release);
  return fn;
}

GLFramebufferTexture2DFn ResolveGLFramebufferTexture2D() {
  if (auto fn = CachedGLFramebufferTexture2D.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLFramebufferTexture2DFn>(
      SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glFramebufferTexture2D")));
  CachedGLFramebufferTexture2D.store(fn, std::memory_order_release);
  return fn;
}

GLCheckFramebufferStatusFn ResolveGLCheckFramebufferStatus() {
  if (auto fn = CachedGLCheckFramebufferStatus.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLCheckFramebufferStatusFn>(
      SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glCheckFramebufferStatus")));
  CachedGLCheckFramebufferStatus.store(fn, std::memory_order_release);
  return fn;
}

GLReadPixelsFn ResolveGLReadPixels() {
  if (auto fn = CachedGLReadPixels.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLReadPixelsFn>(SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glReadPixels")));
  CachedGLReadPixels.store(fn, std::memory_order_release);
  return fn;
}

bool GetTexImageBindingInfo(GLenum target, GLenum* bind_pname, GLenum* textarget) {
  switch (target) {
    case GL_TEXTURE_2D:
      *bind_pname = GL_TEXTURE_BINDING_2D;
      *textarget = GL_TEXTURE_2D;
      return true;
    case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
      *bind_pname = GL_TEXTURE_BINDING_CUBE_MAP;
      *textarget = target;
      return true;
    case GL_TEXTURE_1D:
    case GL_PROXY_TEXTURE_1D:
      *bind_pname = GL_TEXTURE_BINDING_2D;
      *textarget = GL_TEXTURE_2D;
      return true;
    default:
      return false;
  }
}

void GLCompatClearDepth(GLdouble depth) {
  if (auto fn = ResolveGLClearDepthf()) {
    fn(static_cast<GLfloat>(depth));
    return;
  }

  static std::atomic<uint32_t> warn_budget {1};
  uint32_t remaining = warn_budget.load(std::memory_order_acquire);
  while (remaining > 0 &&
         !warn_budget.compare_exchange_weak(remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  if (remaining > 0) {
    fprintf(stderr, "[SDL3 thunk] glClearDepth fallback: missing glClearDepthf, keeping no-op\n");
  }
}

#ifndef GL_FRAMEBUFFER_DEFAULT
#define GL_FRAMEBUFFER_DEFAULT 0x8218
#endif

void GLCompatClearStencil(GLint s) {
  if (auto fn = ResolveGLClearStencil()) {
    fn(s);
    return;
  }

  static std::atomic<uint32_t> warn_budget {1};
  uint32_t remaining = warn_budget.load(std::memory_order_acquire);
  while (remaining > 0 &&
         !warn_budget.compare_exchange_weak(remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  if (remaining > 0) {
    fprintf(stderr, "[SDL3 thunk] glClearStencil fallback: missing glClearStencil, keeping no-op\n");
  }
}

void GLCompatDepthRange(GLdouble z_near, GLdouble z_far) {
  if (auto fn = ResolveGLDepthRangef()) {
    fn(static_cast<GLfloat>(z_near), static_cast<GLfloat>(z_far));
  }
}

GLPolygonModeFn ResolveGLPolygonMode() {
  if (CachedGLPolygonModeResolved.load(std::memory_order_acquire)) {
    return CachedGLPolygonMode.load(std::memory_order_acquire);
  }

  GLPolygonModeFn fn = reinterpret_cast<GLPolygonModeFn>(SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glPolygonMode")));
  if (!fn) {
    fn = reinterpret_cast<GLPolygonModeFn>(SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glPolygonModeEXT")));
  }
  if (!fn) {
    fn = reinterpret_cast<GLPolygonModeFn>(SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glPolygonModeNV")));
  }

  CachedGLPolygonMode.store(fn, std::memory_order_release);
  CachedGLPolygonModeResolved.store(true, std::memory_order_release);
  return fn;
}

void GLCompatPolygonMode(GLenum face, GLenum mode) {
  if (auto fn = ResolveGLPolygonMode()) {
    fn(face, mode);
    return;
  }

  // GLES has no polygon rasterization mode toggle; fill is default behavior.
  // NOTE: This fallback can cause visual differences/artifacts versus desktop GL
  // when the title expects line/point polygon modes.
  if (mode == GL_FILL) {
    return;
  }

  static std::atomic<uint32_t> warn_budget {16};
  uint32_t remaining = warn_budget.load(std::memory_order_acquire);
  while (remaining > 0 &&
         !warn_budget.compare_exchange_weak(remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  if (remaining == 0) {
    return;
  }

  fprintf(stderr, "[SDL3 thunk] glPolygonMode fallback ignored unsupported mode=0x%x\n", static_cast<unsigned>(mode));
}

void GLCompatTexImage1D(GLenum target,
                        GLint level,
                        GLint internalformat,
                        GLsizei width,
                        GLint border,
                        GLenum format,
                        GLenum type,
                        const void* pixels) {
  if (auto fn = ResolveGLTexImage2D()) {
    GLenum target_2d = target;
    if (target == GL_TEXTURE_1D || target == GL_PROXY_TEXTURE_1D) {
      target_2d = GL_TEXTURE_2D;
    }

    // Compat note: 1D textures are emulated as 2D textures with height=1.
    // This can cause rendering differences in titles that rely on strict
    // desktop 1D texture behavior.
    fn(target_2d, level, internalformat, width, 1, border, format, type, pixels);
  }
}

void GLCompatTexImage2DMultisample(GLenum target,
                                   GLsizei samples,
                                   GLint internalformat,
                                   GLsizei width,
                                   GLsizei height,
                                   GLboolean fixedsamplelocations) {
  if (auto fn = ResolveGLTexImage2DMultisample()) {
    fn(target, samples, static_cast<GLenum>(internalformat), width, height, fixedsamplelocations);
    return;
  }

  if (auto fn = ResolveGLTexStorage2DMultisample()) {
    // Compat fallback: immutable multisample storage path used when desktop
    // glTexImage2DMultisample entry points are missing on GLES drivers.
    fn(target, samples, static_cast<GLenum>(internalformat), width, height, fixedsamplelocations);
  }
}

void GLCompatDrawBuffer(GLenum buf) {
  if (auto fn = ResolveGLDrawBuffer()) {
    fn(buf);
    return;
  }

  if (auto fn = ResolveGLDrawBuffers()) {
    GLenum mapped = buf;
    switch (buf) {
      case GL_FRONT:
      case GL_FRONT_LEFT:
      case GL_FRONT_RIGHT:
      case GL_BACK_LEFT:
      case GL_BACK_RIGHT:
      case GL_LEFT:
      case GL_RIGHT:
        mapped = GL_BACK;
        break;
      default:
        break;
    }
    fn(1, &mapped);
    return;
  }

  // Last-resort fallback for default framebuffer semantics on GLES.
  if (buf == GL_BACK || buf == GL_NONE) {
    return;
  }

  static std::atomic<uint32_t> warn_budget {16};
  uint32_t remaining = warn_budget.load(std::memory_order_acquire);
  while (remaining > 0 &&
         !warn_budget.compare_exchange_weak(remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  if (remaining == 0) {
    return;
  }

  fprintf(stderr, "[SDL3 thunk] glDrawBuffer fallback ignored unsupported buf=0x%x\n", static_cast<unsigned>(buf));
}

void GLCompatGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* data) {
  if (!data || size <= 0) {
    return;
  }

  if (auto fn = ResolveGLGetBufferSubData()) {
    fn(target, offset, size, data);
    return;
  }

  const auto map_buffer_range = ResolveGLMapBufferRange();
  const auto unmap_buffer = ResolveGLUnmapBuffer();
  if (!map_buffer_range || !unmap_buffer) {
    return;
  }

  // Compat path for GLES: map read-only range and copy requested bytes.
  void* mapped = map_buffer_range(target, offset, size, GL_MAP_READ_BIT);
  if (!mapped) {
    return;
  }

  std::memcpy(data, mapped, static_cast<size_t>(size));
  unmap_buffer(target);
}

void GLCompatVertexAttribI2i(GLuint index, GLint x, GLint y) {
  if (auto fn = ResolveGLVertexAttribI2i()) {
    fn(index, x, y);
    return;
  }

  if (auto fn = ResolveGLVertexAttribI4i()) {
    // Compat fallback: widen ivec2 constant attribute assignment to ivec4.
    fn(index, x, y, 0, 1);
    return;
  }

  static std::atomic<uint32_t> warn_budget {16};
  uint32_t remaining = warn_budget.load(std::memory_order_acquire);
  while (remaining > 0 &&
         !warn_budget.compare_exchange_weak(remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  if (remaining == 0) {
    return;
  }

  fprintf(stderr, "[SDL3 thunk] glVertexAttribI2i fallback unavailable for index=%u\n", index);
}

void GLCompatGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint* params) {
  if (!params) {
    return;
  }

  if (auto fn = ResolveGLGetFramebufferAttachmentParameteriv()) {
    fn(target, attachment, pname, params);
    return;
  }

  auto get_integerv = ResolveGLGetIntegerv();

  GLint bound_fbo = -1;
  if (get_integerv) {
    GLenum binding = GL_DRAW_FRAMEBUFFER_BINDING;
#ifdef GL_FRAMEBUFFER_BINDING
    if (target == GL_FRAMEBUFFER) {
      binding = GL_FRAMEBUFFER_BINDING;
    } else
#endif
    if (target == GL_READ_FRAMEBUFFER) {
      binding = GL_READ_FRAMEBUFFER_BINDING;
    }
    get_integerv(binding, &bound_fbo);
  }

  const bool default_fb = (bound_fbo == 0);
  const bool depth_stencil_attachment =
      attachment == GL_DEPTH_ATTACHMENT ||
      attachment == GL_STENCIL_ATTACHMENT ||
      attachment == GL_DEPTH_STENCIL_ATTACHMENT;
  const bool color_attachment = (attachment != GL_NONE) && !depth_stencil_attachment;

  switch (pname) {
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:
      if (!color_attachment && !depth_stencil_attachment) {
        *params = GL_NONE;
      } else {
        *params = default_fb ? GL_FRAMEBUFFER_DEFAULT : GL_RENDERBUFFER;
      }
      return;

    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:
      *params = default_fb ? 0 : ((color_attachment || depth_stencil_attachment) ? 1 : 0);
      return;

    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER:
      *params = 0;
      return;

    case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE:
      if (get_integerv && default_fb) {
        get_integerv(GL_RED_BITS, params);
      } else {
        *params = color_attachment ? 8 : 0;
      }
      return;

    case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:
      if (get_integerv && default_fb) {
        get_integerv(GL_GREEN_BITS, params);
      } else {
        *params = color_attachment ? 8 : 0;
      }
      return;

    case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE:
      if (get_integerv && default_fb) {
        get_integerv(GL_BLUE_BITS, params);
      } else {
        *params = color_attachment ? 8 : 0;
      }
      return;

    case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:
      if (get_integerv && default_fb) {
        get_integerv(GL_ALPHA_BITS, params);
      } else {
        *params = color_attachment ? 8 : 0;
      }
      return;

    case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE:
      if (get_integerv && default_fb) {
        get_integerv(GL_DEPTH_BITS, params);
      } else {
        *params = 24;
      }
      return;

    case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE:
      if (get_integerv && default_fb) {
        get_integerv(GL_STENCIL_BITS, params);
      } else {
        *params = 8;
      }
      return;

    case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING:
      *params = GL_LINEAR;
      return;

    case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:
      *params = GL_UNSIGNED_NORMALIZED;
      return;

    default:
      *params = 0;
      return;
  }
}

void GLCompatGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* pixels) {
  if (!pixels) {
    return;
  }

  const auto get_tex_level_param = ResolveGLGetTexLevelParameteriv();
  const auto get_integerv = ResolveGLGetIntegerv();
  const auto gen_framebuffers = ResolveGLGenFramebuffers();
  const auto delete_framebuffers = ResolveGLDeleteFramebuffers();
  const auto bind_framebuffer = ResolveGLBindFramebuffer();
  const auto framebuffer_texture_2d = ResolveGLFramebufferTexture2D();
  const auto check_framebuffer_status = ResolveGLCheckFramebufferStatus();
  const auto read_pixels = ResolveGLReadPixels();

  if (!get_tex_level_param || !get_integerv || !gen_framebuffers || !delete_framebuffers ||
      !bind_framebuffer || !framebuffer_texture_2d || !check_framebuffer_status || !read_pixels) {
    return;
  }

  GLenum bind_pname {};
  GLenum textarget {};
  if (!GetTexImageBindingInfo(target, &bind_pname, &textarget)) {
    return;
  }

  GLint texture = 0;
  get_integerv(bind_pname, &texture);
  if (texture == 0) {
    return;
  }

  GLint width = 0;
  GLint height = 1;
  get_tex_level_param(target, level, GL_TEXTURE_WIDTH, &width);
  if (width <= 0) {
    return;
  }

  if (target != GL_TEXTURE_1D && target != GL_PROXY_TEXTURE_1D) {
    get_tex_level_param(target, level, GL_TEXTURE_HEIGHT, &height);
    if (height <= 0) {
      return;
    }
  }

  GLint prev_read_fbo = 0;
  GLint prev_draw_fbo = 0;
  get_integerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
  get_integerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);

  GLuint fbo = 0;
  gen_framebuffers(1, &fbo);
  if (fbo == 0) {
    return;
  }

  // Compat path: emulate glGetTexImage via FBO attachment + glReadPixels.
  // This is close to desktop semantics but may still differ for some formats.
  bind_framebuffer(GL_READ_FRAMEBUFFER, fbo);
  bind_framebuffer(GL_DRAW_FRAMEBUFFER, fbo);
  framebuffer_texture_2d(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, textarget, static_cast<GLuint>(texture), level);

  if (check_framebuffer_status(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
    read_pixels(0, 0, width, height, format, type, pixels);
  }

  bind_framebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read_fbo));
  bind_framebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prev_draw_fbo));
  delete_framebuffers(1, &fbo);
}

std::unordered_map<std::string, SDLPropertyValue>* FindPropertiesLocked(SDL_PropertiesID props) {
  auto it = PropertiesById.find(props);
  return it != PropertiesById.end() ? &it->second : nullptr;
}

SDL_PropertiesID CreatePropertiesLocked() {
  SDL_PropertiesID id = NextPropertiesId.fetch_add(1, std::memory_order_acq_rel);
  if (id == 0) {
    id = NextPropertiesId.fetch_add(1, std::memory_order_acq_rel);
  }
  PropertiesById.try_emplace(id);
  return id;
}

SDL_PropertiesID EnsureGlobalProperties() {
  SDL_PropertiesID id = GlobalPropertiesId.load(std::memory_order_acquire);
  if (id != 0) {
    return id;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  id = GlobalPropertiesId.load(std::memory_order_acquire);
  if (id == 0 || PropertiesById.find(id) == PropertiesById.end()) {
    id = CreatePropertiesLocked();
    GlobalPropertiesId.store(id, std::memory_order_release);
  }
  return id;
}

SDL_PropertiesID EnsurePrimaryDisplayProperties() {
  SDL_PropertiesID id = PrimaryDisplayPropertiesId.load(std::memory_order_acquire);
  if (id != 0) {
    return id;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  id = PrimaryDisplayPropertiesId.load(std::memory_order_acquire);
  if (id == 0 || PropertiesById.find(id) == PropertiesById.end()) {
    id = CreatePropertiesLocked();
    PrimaryDisplayPropertiesId.store(id, std::memory_order_release);
  }

  auto* props = FindPropertiesLocked(id);
  if (props) {
    SDLPropertyValue hdr_enabled {};
    hdr_enabled.Type = SDL_PROPERTY_TYPE_BOOLEAN;
    hdr_enabled.BooleanValue = false;
    (*props)[SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN] = hdr_enabled;
  }

  return id;
}

SDL_PropertiesID EnsurePrimaryWindowProperties(SDL_Window* window) {
  if (!window) {
    return 0;
  }

  SDL_PropertiesID id = PrimaryWindowPropertiesId.load(std::memory_order_acquire);
  std::lock_guard<std::mutex> guard(PropertiesMutex);
  if (id == 0 || PropertiesById.find(id) == PropertiesById.end()) {
    id = CreatePropertiesLocked();
    PrimaryWindowPropertiesId.store(id, std::memory_order_release);
  }

  auto* props = FindPropertiesLocked(id);
  if (props) {
    SDLPropertyValue window_pointer {};
    window_pointer.Type = SDL_PROPERTY_TYPE_POINTER;
    window_pointer.PointerValue = window;
    (*props)[SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER] = window_pointer;

    SDLPropertyValue window_id {};
    window_id.Type = SDL_PROPERTY_TYPE_NUMBER;
    window_id.NumberValue = PrimaryWindowId;
    (*props)["SDL.window.id"] = window_id;

    SDLPropertyValue hdr_enabled {};
    hdr_enabled.Type = SDL_PROPERTY_TYPE_BOOLEAN;
    hdr_enabled.BooleanValue = false;
    (*props)[SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN] = hdr_enabled;

    SDLPropertyValue sdr_white_level {};
    sdr_white_level.Type = SDL_PROPERTY_TYPE_FLOAT;
    sdr_white_level.FloatValue = 1.0f;
    (*props)[SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT] = sdr_white_level;

    SDLPropertyValue hdr_headroom {};
    hdr_headroom.Type = SDL_PROPERTY_TYPE_FLOAT;
    hdr_headroom.FloatValue = 1.0f;
    (*props)[SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT] = hdr_headroom;
  }

  return id;
}

void ClearPrimaryWindowPropertiesIfMatching(SDL_Window* window) {
  SDL_PropertiesID id = PrimaryWindowPropertiesId.load(std::memory_order_acquire);
  if (id == 0) {
    return;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  auto* props = FindPropertiesLocked(id);
  if (!props) {
    PrimaryWindowPropertiesId.store(0, std::memory_order_release);
    return;
  }

  auto it = props->find(SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER);
  if (it != props->end() && it->second.Type == SDL_PROPERTY_TYPE_POINTER && it->second.PointerValue == window) {
    PropertiesById.erase(id);
    PrimaryWindowPropertiesId.store(0, std::memory_order_release);
  }
}

void UpdateWindowFlags(SDL_WindowFlags set_bits, SDL_WindowFlags clear_bits) {
  SDL_WindowFlags flags = WindowFlagsState.load(std::memory_order_acquire);
  flags |= set_bits;
  flags &= ~clear_bits;
  WindowFlagsState.store(flags, std::memory_order_release);
}

Uint64 GetTimestampNS() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void QueueSyntheticEvent(const SDL_Event& event) {
  std::scoped_lock lock {SyntheticEventQueueMutex};
  SyntheticEventQueue.push_back(event);
}

void QueueSyntheticWindowEvent(SDL_EventType type, Sint32 data1 = 0, Sint32 data2 = 0) {
  SDL_Event event {};
  event.window.type = type;
  event.window.timestamp = GetTimestampNS();
  event.window.windowID = PrimaryWindowId;
  event.window.data1 = data1;
  event.window.data2 = data2;
  QueueSyntheticEvent(event);
}

void QueueSyntheticWindowShownEvents() {
  int w = 0;
  int h = 0;
  GetFallbackWindowSize(&w, &h);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_SHOWN);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_DISPLAY_CHANGED, static_cast<Sint32>(PrimaryDisplayId), 0);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_RESIZED, w, h);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, w, h);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_EXPOSED);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_MOUSE_ENTER);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
}

void SetWindowVisibleFocused(bool visible) {
  if (visible) {
    UpdateWindowFlags(SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS,
                      SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED);
  } else {
    UpdateWindowFlags(SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED,
                      SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
  }
}

constexpr SDL_InitFlags NormalizeInitFlags(SDL_InitFlags flags) {
  if ((flags & SDL_INIT_GAMEPAD) != 0) {
    flags |= SDL_INIT_JOYSTICK;
  }
  if ((flags & (SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_SENSOR | SDL_INIT_CAMERA)) != 0) {
    flags |= SDL_INIT_EVENTS;
  }
  return flags;
}

bool SetErrorFromVA(const char* fmt, va_list ap) {
  std::array<char, 1024> buffer {};
  va_list ap_copy;
  va_copy(ap_copy, ap);
  vsnprintf(buffer.data(), buffer.size(), fmt, ap_copy);
  va_end(ap_copy);

  LastError = buffer.data();
  fprintf(stderr, "SDL3: %s\n", LastError.c_str());
  return false;
}

struct GLProcEntry {
  std::string_view Name;
  SDL_FunctionPointer Function;
};

#define SDL3_GL_PROC_ENTRY(name) {#name, nullptr},
const GLProcEntry GLProcTable[] = {
  FEX_ANDROID_GL_PROC_LIST(SDL3_GL_PROC_ENTRY)
};
#undef SDL3_GL_PROC_ENTRY
} // namespace

extern "C" bool SDL_Init(SDL_InitFlags flags) {
  fprintf(stderr, "[SDL3 thunk] SDL_Init(flags=0x%x)\n", static_cast<unsigned>(flags));
  const SDL_InitFlags normalized = NormalizeInitFlags(flags);
  const SDL_InitFlags previous = InitializedSubsystems.fetch_or(normalized, std::memory_order_acq_rel);
  if (previous == 0) {
    const bool success = fexfn_pack_FEX_SDL_Init(normalized) != 0;
    fprintf(stderr,
                 "[SDL3 thunk] SDL_Init => %d normalized=0x%x previous=0x%x\n",
                 success ? 1 : 0,
                 static_cast<unsigned>(normalized),
                 static_cast<unsigned>(previous));
    return success;
  }
  fprintf(stderr,
               "[SDL3 thunk] SDL_Init => 1 (already initialized previous=0x%x normalized=0x%x)\n",
               static_cast<unsigned>(previous),
               static_cast<unsigned>(normalized));
  return true;
}

extern "C" bool SDL_InitSubSystem(SDL_InitFlags flags) {
  fprintf(stderr, "[SDL3 thunk] SDL_InitSubSystem(flags=0x%x)\n", static_cast<unsigned>(flags));
  return SDL_Init(flags);
}

extern "C" void SDL_Quit() {
  InitializedSubsystems.store(0, std::memory_order_release);
  {
    std::scoped_lock lock {SyntheticEventQueueMutex};
    SyntheticEventQueue.clear();
  }
  fexfn_pack_FEX_SDL_Quit();
}

extern "C" void SDL_QuitSubSystem(SDL_InitFlags flags) {
  const SDL_InitFlags normalized = NormalizeInitFlags(flags);

  SDL_InitFlags current = InitializedSubsystems.load(std::memory_order_acquire);
  while (true) {
    const SDL_InitFlags remaining = current & ~normalized;
    if (InitializedSubsystems.compare_exchange_weak(current, remaining, std::memory_order_acq_rel, std::memory_order_acquire)) {
      if (current != 0 && remaining == 0) {
        fexfn_pack_FEX_SDL_Quit();
      }
      return;
    }
  }
}

extern "C" SDL_InitFlags SDL_WasInit(SDL_InitFlags flags) {
  const SDL_InitFlags current = InitializedSubsystems.load(std::memory_order_acquire);
  return flags == 0 ? current : (current & flags);
}

extern "C" SDL_Window* SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags) {
  fprintf(stderr,
               "[SDL3 thunk] SDL_CreateWindow(title=%s w=%d h=%d flags=0x%llx)\n",
               title ? title : "(null)",
               w,
               h,
               static_cast<unsigned long long>(flags));
  SDL_Window* window = fexfn_pack_FEX_SDL_CreateWindow(title, w, h, flags);
  fprintf(stderr, "[SDL3 thunk] SDL_CreateWindow => %p\n", window);
  if (window) {
    UpdateFallbackWindowSize(w, h);
    WindowPosX.store(0, std::memory_order_release);
    WindowPosY.store(0, std::memory_order_release);
    WindowMinWidth.store(0, std::memory_order_release);
    WindowMinHeight.store(0, std::memory_order_release);
    WindowMaxWidth.store(0, std::memory_order_release);
    WindowMaxHeight.store(0, std::memory_order_release);
    PrimaryWindowPointer.store(reinterpret_cast<uintptr_t>(window), std::memory_order_release);
    ParentWindowPointer.store(0, std::memory_order_release);
    SDL_WindowFlags initial_flags = flags;
    if ((initial_flags & SDL_WINDOW_HIDDEN) == 0) {
      initial_flags |= (SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
      initial_flags &= ~(SDL_WINDOW_OCCLUDED | SDL_WINDOW_MINIMIZED);
    } else {
      initial_flags &= ~(SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
    }
    WindowFlagsState.store(initial_flags, std::memory_order_release);
    {
      std::lock_guard<std::mutex> guard(WindowStateMutex);
      HasFullscreenWindowMode = false;
      FullscreenWindowMode = {};
    }
    if ((initial_flags & SDL_WINDOW_HIDDEN) == 0) {
      QueueSyntheticWindowShownEvents();
    }
    EnsurePrimaryWindowProperties(window);
  }
  return window;
}

extern "C" void SDL_DestroyWindow(SDL_Window* window) {
  if (window) {
    const uintptr_t current = PrimaryWindowPointer.load(std::memory_order_acquire);
    if (current == reinterpret_cast<uintptr_t>(window)) {
      PrimaryWindowPointer.store(0, std::memory_order_release);
    }
    ClearPrimaryWindowPropertiesIfMatching(window);
  }
  ParentWindowPointer.store(0, std::memory_order_release);
  WindowFlagsState.store(0, std::memory_order_release);
  WindowPosX.store(0, std::memory_order_release);
  WindowPosY.store(0, std::memory_order_release);
  WindowMinWidth.store(0, std::memory_order_release);
  WindowMinHeight.store(0, std::memory_order_release);
  WindowMaxWidth.store(0, std::memory_order_release);
  WindowMaxHeight.store(0, std::memory_order_release);
  {
    std::lock_guard<std::mutex> guard(WindowStateMutex);
    HasFullscreenWindowMode = false;
    FullscreenWindowMode = {};
  }
  fexfn_pack_FEX_SDL_DestroyWindow(window);
}

extern "C" bool SDL_GetWindowSize(SDL_Window* window, int* w, int* h) {
  fprintf(stderr, "[SDL3 thunk] SDL_GetWindowSize(window=%p)\n", window);
  const bool success = fexfn_pack_FEX_SDL_GetWindowSize(window, w, h) != 0;
  fprintf(stderr,
               "[SDL3 thunk] SDL_GetWindowSize => %d w=%d h=%d\n",
               success ? 1 : 0,
               (success && w) ? *w : -1,
               (success && h) ? *h : -1);
  if (success && w && h) {
    UpdateFallbackWindowSize(*w, *h);
    return true;
  }

  if (window) {
    GetFallbackWindowSize(w, h);
    return true;
  }
  return false;
}

extern "C" SDL_DisplayID* SDL_GetDisplays(int* count) {
  fprintf(stderr, "[SDL3 thunk] SDL_GetDisplays(count_ptr=%p)\n", count);
  if (count) {
    *count = 1;
  }

  auto* displays = static_cast<SDL_DisplayID*>(std::malloc(sizeof(SDL_DisplayID) * 2));
  if (!displays) {
    return nullptr;
  }

  displays[0] = PrimaryDisplayId;
  displays[1] = 0;
  fprintf(stderr,
               "[SDL3 thunk] SDL_GetDisplays => displays=%p count=%d first=%u\n",
               displays,
               count ? *count : 1,
               static_cast<unsigned>(displays[0]));
  return displays;
}

extern "C" SDL_DisplayID SDL_GetPrimaryDisplay() {
  return PrimaryDisplayId;
}

extern "C" SDL_PropertiesID SDL_GetDisplayProperties(SDL_DisplayID display_id) {
  return IsKnownDisplay(display_id) ? EnsurePrimaryDisplayProperties() : 0;
}

extern "C" const char* SDL_GetDisplayName(SDL_DisplayID display_id) {
  return IsKnownDisplay(display_id) ? "Android Display" : nullptr;
}

extern "C" bool SDL_GetDisplayBounds(SDL_DisplayID display_id, SDL_Rect* rect) {
  if (!rect || !IsKnownDisplay(display_id)) {
    return false;
  }

  RefreshFallbackWindowSizeFromHost(nullptr);
  rect->x = 0;
  rect->y = 0;
  GetFallbackWindowSize(&rect->w, &rect->h);
  return true;
}

extern "C" bool SDL_GetDisplayUsableBounds(SDL_DisplayID display_id, SDL_Rect* rect) {
  return SDL_GetDisplayBounds(display_id, rect);
}

extern "C" float SDL_GetDisplayContentScale(SDL_DisplayID display_id) {
  if (!IsKnownDisplay(display_id)) {
    return 0.0f;
  }

  const uintptr_t window_ptr = PrimaryWindowPointer.load(std::memory_order_acquire);
  if (!window_ptr) {
    return 1.0f;
  }

  const float scale = SDL_GetWindowDisplayScale(reinterpret_cast<SDL_Window*>(window_ptr));
  return scale > 0.0f ? scale : 1.0f;
}

extern "C" const SDL_DisplayMode* SDL_GetDesktopDisplayMode(SDL_DisplayID display_id) {
  static SDL_DisplayMode mode {};
  if (!IsKnownDisplay(display_id)) {
    return nullptr;
  }

  RefreshFallbackWindowSizeFromHost(nullptr);
  mode = BuildFallbackDisplayMode();
  return &mode;
}

extern "C" const SDL_DisplayMode* SDL_GetCurrentDisplayMode(SDL_DisplayID display_id) {
  fprintf(stderr, "[SDL3 thunk] SDL_GetCurrentDisplayMode(display=%u)\n",
               static_cast<unsigned>(display_id));
  return SDL_GetDesktopDisplayMode(display_id);
}

extern "C" SDL_DisplayID SDL_GetDisplayForPoint(const SDL_Point*) {
  return PrimaryDisplayId;
}

extern "C" SDL_DisplayID SDL_GetDisplayForRect(const SDL_Rect*) {
  return PrimaryDisplayId;
}

extern "C" SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window*) {
  return PrimaryDisplayId;
}

extern "C" float SDL_GetWindowPixelDensity(SDL_Window* window) {
  SDL_Window* target = window;
  if (!target) {
    const uintptr_t window_ptr = PrimaryWindowPointer.load(std::memory_order_acquire);
    target = reinterpret_cast<SDL_Window*>(window_ptr);
  }
  if (!target) {
    return 0.0f;
  }

  int logical_w = 0;
  int logical_h = 0;
  if (!SDL_GetWindowSize(target, &logical_w, &logical_h) || logical_w <= 0 || logical_h <= 0) {
    return 0.0f;
  }

  int pixel_w = 0;
  int pixel_h = 0;
  if (!SDL_GetWindowSizeInPixels(target, &pixel_w, &pixel_h) || pixel_w <= 0 || pixel_h <= 0) {
    return 0.0f;
  }

  const float scale_x = static_cast<float>(pixel_w) / static_cast<float>(logical_w);
  const float scale_y = static_cast<float>(pixel_h) / static_cast<float>(logical_h);
  return scale_x > scale_y ? scale_x : scale_y;
}

extern "C" float SDL_GetWindowDisplayScale(SDL_Window* window) {
  const float density = SDL_GetWindowPixelDensity(window);
  return density > 0.0f ? density : 1.0f;
}

extern "C" SDL_WindowID SDL_GetWindowID(SDL_Window* window) {
  if (!window) {
    return 0;
  }

  return PrimaryWindowId;
}

extern "C" SDL_Window* SDL_GetWindowFromID(SDL_WindowID id) {
  if (id != PrimaryWindowId) {
    return nullptr;
  }

  const uintptr_t window_ptr = PrimaryWindowPointer.load(std::memory_order_acquire);
  return reinterpret_cast<SDL_Window*>(window_ptr);
}

extern "C" SDL_PropertiesID SDL_GetWindowProperties(SDL_Window* window) {
  return EnsurePrimaryWindowProperties(window);
}

extern "C" SDL_WindowFlags SDL_GetWindowFlags(SDL_Window* window) {
  return window ? WindowFlagsState.load(std::memory_order_acquire) : 0;
}

extern "C" bool SDL_SetWindowTitle(SDL_Window* window, const char* title) {
  if (!window) {
    return false;
  }

  std::lock_guard<std::mutex> guard(WindowTitleMutex);
  WindowTitle = title ? title : "";
  return true;
}

extern "C" const char* SDL_GetWindowTitle(SDL_Window* window) {
  if (!window) {
    return "";
  }

  std::lock_guard<std::mutex> guard(WindowTitleMutex);
  return WindowTitle.c_str();
}

extern "C" bool SDL_SetWindowPosition(SDL_Window* window, int x, int y) {
  if (!window) {
    return false;
  }
  WindowPosX.store(x, std::memory_order_release);
  WindowPosY.store(y, std::memory_order_release);
  return true;
}

extern "C" bool SDL_GetWindowPosition(SDL_Window* window, int* x, int* y) {
  if (!window) {
    return false;
  }
  if (x) {
    *x = WindowPosX.load(std::memory_order_acquire);
  }
  if (y) {
    *y = WindowPosY.load(std::memory_order_acquire);
  }
  return true;
}

extern "C" bool SDL_SetWindowSize(SDL_Window* window, int w, int h) {
  if (!window || w <= 0 || h <= 0) {
    return false;
  }

  const int min_w = WindowMinWidth.load(std::memory_order_acquire);
  const int min_h = WindowMinHeight.load(std::memory_order_acquire);
  const int max_w = WindowMaxWidth.load(std::memory_order_acquire);
  const int max_h = WindowMaxHeight.load(std::memory_order_acquire);

  int clamped_w = w;
  int clamped_h = h;
  if (min_w > 0 && clamped_w < min_w) {
    clamped_w = min_w;
  }
  if (min_h > 0 && clamped_h < min_h) {
    clamped_h = min_h;
  }
  if (max_w > 0 && clamped_w > max_w) {
    clamped_w = max_w;
  }
  if (max_h > 0 && clamped_h > max_h) {
    clamped_h = max_h;
  }

  UpdateFallbackWindowSize(clamped_w, clamped_h);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_RESIZED, clamped_w, clamped_h);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, clamped_w, clamped_h);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_EXPOSED);
  return true;
}

extern "C" bool SDL_GetWindowSizeInPixels(SDL_Window* window, int* w, int* h) {
  fprintf(stderr, "[SDL3 thunk] SDL_GetWindowSizeInPixels(window=%p)\n", window);
  if (!window) {
    return false;
  }

  RefreshFallbackWindowSizeFromHost(window);
  GetFallbackWindowSize(w, h);
  fprintf(stderr,
               "[SDL3 thunk] SDL_GetWindowSizeInPixels => w=%d h=%d\n",
               w ? *w : -1,
               h ? *h : -1);
  return true;
}

extern "C" bool SDL_SetWindowMinimumSize(SDL_Window* window, int min_w, int min_h) {
  if (!window || min_w < 0 || min_h < 0) {
    return false;
  }
  WindowMinWidth.store(min_w, std::memory_order_release);
  WindowMinHeight.store(min_h, std::memory_order_release);
  return true;
}

extern "C" bool SDL_GetWindowMinimumSize(SDL_Window* window, int* w, int* h) {
  if (!window) {
    return false;
  }
  if (w) {
    *w = WindowMinWidth.load(std::memory_order_acquire);
  }
  if (h) {
    *h = WindowMinHeight.load(std::memory_order_acquire);
  }
  return true;
}

extern "C" bool SDL_SetWindowMaximumSize(SDL_Window* window, int max_w, int max_h) {
  if (!window || max_w < 0 || max_h < 0) {
    return false;
  }
  WindowMaxWidth.store(max_w, std::memory_order_release);
  WindowMaxHeight.store(max_h, std::memory_order_release);
  return true;
}

extern "C" bool SDL_GetWindowMaximumSize(SDL_Window* window, int* w, int* h) {
  if (!window) {
    return false;
  }
  if (w) {
    *w = WindowMaxWidth.load(std::memory_order_acquire);
  }
  if (h) {
    *h = WindowMaxHeight.load(std::memory_order_acquire);
  }
  return true;
}

extern "C" bool SDL_SetWindowBordered(SDL_Window* window, bool bordered) {
  if (!window) {
    return false;
  }
  if (bordered) {
    UpdateWindowFlags(0, SDL_WINDOW_BORDERLESS);
  } else {
    UpdateWindowFlags(SDL_WINDOW_BORDERLESS, 0);
  }
  return true;
}

extern "C" bool SDL_SetWindowResizable(SDL_Window* window, bool resizable) {
  if (!window) {
    return false;
  }
  if (resizable) {
    UpdateWindowFlags(SDL_WINDOW_RESIZABLE, 0);
  } else {
    UpdateWindowFlags(0, SDL_WINDOW_RESIZABLE);
  }
  return true;
}

extern "C" bool SDL_SetWindowAlwaysOnTop(SDL_Window* window, bool on_top) {
  if (!window) {
    return false;
  }
  if (on_top) {
    UpdateWindowFlags(SDL_WINDOW_ALWAYS_ON_TOP, 0);
  } else {
    UpdateWindowFlags(0, SDL_WINDOW_ALWAYS_ON_TOP);
  }
  return true;
}

extern "C" bool SDL_SetWindowFullscreenMode(SDL_Window* window, const SDL_DisplayMode* mode) {
  if (!window) {
    return false;
  }

  std::lock_guard<std::mutex> guard(WindowStateMutex);
  if (mode) {
    FullscreenWindowMode = *mode;
    HasFullscreenWindowMode = true;
  } else {
    FullscreenWindowMode = {};
    HasFullscreenWindowMode = false;
  }
  return true;
}

extern "C" const SDL_DisplayMode* SDL_GetWindowFullscreenMode(SDL_Window* window) {
  if (!window) {
    return nullptr;
  }

  std::lock_guard<std::mutex> guard(WindowStateMutex);
  return HasFullscreenWindowMode ? &FullscreenWindowMode : nullptr;
}

extern "C" bool SDL_SetWindowFullscreen(SDL_Window* window, bool fullscreen) {
  if (!window) {
    return false;
  }

  if (fullscreen) {
    UpdateWindowFlags(SDL_WINDOW_FULLSCREEN, SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_HIDDEN);
  } else {
    UpdateWindowFlags(0, SDL_WINDOW_FULLSCREEN);
  }
  return true;
}

extern "C" bool SDL_SyncWindow(SDL_Window* window) {
  return window != nullptr;
}

extern "C" bool SDL_ShowWindow(SDL_Window* window) {
  fprintf(stderr, "[SDL3 thunk] SDL_ShowWindow(window=%p)\n", window);
  if (!window) {
    return false;
  }
  SetWindowVisibleFocused(true);
  QueueSyntheticWindowShownEvents();
  return true;
}

extern "C" bool SDL_HideWindow(SDL_Window* window) {
  fprintf(stderr, "[SDL3 thunk] SDL_HideWindow(window=%p)\n", window);
  if (!window) {
    return false;
  }
  SetWindowVisibleFocused(false);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_HIDDEN);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_FOCUS_LOST);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_MOUSE_LEAVE);
  return true;
}

extern "C" bool SDL_MaximizeWindow(SDL_Window* window) {
  if (!window) {
    return false;
  }
  UpdateWindowFlags(SDL_WINDOW_MAXIMIZED, SDL_WINDOW_MINIMIZED);
  return true;
}

extern "C" bool SDL_MinimizeWindow(SDL_Window* window) {
  if (!window) {
    return false;
  }
  UpdateWindowFlags(SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED,
                    SDL_WINDOW_MAXIMIZED | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_MINIMIZED);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_FOCUS_LOST);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_MOUSE_LEAVE);
  return true;
}

extern "C" bool SDL_RestoreWindow(SDL_Window* window) {
  if (!window) {
    return false;
  }
  UpdateWindowFlags(SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS,
                    SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_RESTORED);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_EXPOSED);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_MOUSE_ENTER);
  QueueSyntheticWindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
  return true;
}

extern "C" SDL_Window* SDL_GetWindowParent(SDL_Window* window) {
  if (!window) {
    return nullptr;
  }
  return reinterpret_cast<SDL_Window*>(ParentWindowPointer.load(std::memory_order_acquire));
}

extern "C" bool SDL_SetWindowParent(SDL_Window* window, SDL_Window* parent) {
  if (!window) {
    return false;
  }
  ParentWindowPointer.store(reinterpret_cast<uintptr_t>(parent), std::memory_order_release);
  return true;
}

extern "C" bool SDL_SetWindowModal(SDL_Window* window, bool modal) {
  if (!window) {
    return false;
  }

  if (modal) {
    UpdateWindowFlags(SDL_WINDOW_MODAL, 0);
  } else {
    UpdateWindowFlags(0, SDL_WINDOW_MODAL);
  }
  return true;
}

extern "C" bool SDL_SetWindowFocusable(SDL_Window* window, bool focusable) {
  if (!window) {
    return false;
  }

  if (focusable) {
    UpdateWindowFlags(0, SDL_WINDOW_NOT_FOCUSABLE);
  } else {
    UpdateWindowFlags(SDL_WINDOW_NOT_FOCUSABLE, 0);
  }
  return true;
}

extern "C" SDL_PropertiesID SDL_GetGlobalProperties() {
  return EnsureGlobalProperties();
}

extern "C" SDL_PropertiesID SDL_CreateProperties() {
  std::lock_guard<std::mutex> guard(PropertiesMutex);
  return CreatePropertiesLocked();
}

extern "C" bool SDL_CopyProperties(SDL_PropertiesID src, SDL_PropertiesID dst) {
  std::lock_guard<std::mutex> guard(PropertiesMutex);
  auto* source = FindPropertiesLocked(src);
  auto* destination = FindPropertiesLocked(dst);
  if (!source || !destination) {
    return false;
  }

  for (const auto& [name, value] : *source) {
    (*destination)[name] = value;
  }
  return true;
}

extern "C" bool SDL_LockProperties(SDL_PropertiesID props) {
  std::lock_guard<std::mutex> guard(PropertiesMutex);
  return FindPropertiesLocked(props) != nullptr;
}

extern "C" void SDL_UnlockProperties(SDL_PropertiesID) {
}

extern "C" bool SDL_SetPointerProperty(SDL_PropertiesID props, const char* name, void* value) {
  if (props == 0 || !name) {
    return false;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  auto* table = FindPropertiesLocked(props);
  if (!table) {
    return false;
  }

  if (!value) {
    table->erase(name);
    return true;
  }

  SDLPropertyValue property {};
  property.Type = SDL_PROPERTY_TYPE_POINTER;
  property.PointerValue = value;
  (*table)[name] = std::move(property);
  return true;
}

extern "C" bool SDL_SetPointerPropertyWithCleanup(
  SDL_PropertiesID props, const char* name, void* value, SDL_CleanupPropertyCallback cleanup, void* userdata) {
  const bool success = SDL_SetPointerProperty(props, name, value);
  if (!success && cleanup && value) {
    cleanup(userdata, value);
  }
  return success;
}

extern "C" bool SDL_SetStringProperty(SDL_PropertiesID props, const char* name, const char* value) {
  if (props == 0 || !name) {
    return false;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  auto* table = FindPropertiesLocked(props);
  if (!table) {
    return false;
  }

  if (!value) {
    table->erase(name);
    return true;
  }

  SDLPropertyValue property {};
  property.Type = SDL_PROPERTY_TYPE_STRING;
  property.StringValue = value;
  (*table)[name] = std::move(property);
  return true;
}

extern "C" bool SDL_SetNumberProperty(SDL_PropertiesID props, const char* name, Sint64 value) {
  if (props == 0 || !name) {
    return false;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  auto* table = FindPropertiesLocked(props);
  if (!table) {
    return false;
  }

  SDLPropertyValue property {};
  property.Type = SDL_PROPERTY_TYPE_NUMBER;
  property.NumberValue = value;
  (*table)[name] = property;
  return true;
}

extern "C" bool SDL_SetFloatProperty(SDL_PropertiesID props, const char* name, float value) {
  if (props == 0 || !name) {
    return false;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  auto* table = FindPropertiesLocked(props);
  if (!table) {
    return false;
  }

  SDLPropertyValue property {};
  property.Type = SDL_PROPERTY_TYPE_FLOAT;
  property.FloatValue = value;
  (*table)[name] = property;
  return true;
}

extern "C" bool SDL_SetBooleanProperty(SDL_PropertiesID props, const char* name, bool value) {
  if (props == 0 || !name) {
    return false;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  auto* table = FindPropertiesLocked(props);
  if (!table) {
    return false;
  }

  SDLPropertyValue property {};
  property.Type = SDL_PROPERTY_TYPE_BOOLEAN;
  property.BooleanValue = value;
  (*table)[name] = property;
  return true;
}

extern "C" bool SDL_HasProperty(SDL_PropertiesID props, const char* name) {
  if (props == 0 || !name) {
    return false;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  auto* table = FindPropertiesLocked(props);
  if (!table) {
    return false;
  }
  return table->find(name) != table->end();
}

extern "C" SDL_PropertyType SDL_GetPropertyType(SDL_PropertiesID props, const char* name) {
  if (props == 0 || !name) {
    return SDL_PROPERTY_TYPE_INVALID;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  auto* table = FindPropertiesLocked(props);
  if (!table) {
    return SDL_PROPERTY_TYPE_INVALID;
  }

  auto it = table->find(name);
  return it != table->end() ? it->second.Type : SDL_PROPERTY_TYPE_INVALID;
}

extern "C" void* SDL_GetPointerProperty(SDL_PropertiesID props, const char* name, void* default_value) {
  if (props == 0 || !name) {
    return default_value;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  auto* table = FindPropertiesLocked(props);
  if (!table) {
    return default_value;
  }

  auto it = table->find(name);
  if (it == table->end() || it->second.Type != SDL_PROPERTY_TYPE_POINTER) {
    return default_value;
  }
  return it->second.PointerValue;
}

extern "C" const char* SDL_GetStringProperty(SDL_PropertiesID props, const char* name, const char* default_value) {
  if (props == 0 || !name) {
    return default_value;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  auto* table = FindPropertiesLocked(props);
  if (!table) {
    return default_value;
  }

  auto it = table->find(name);
  if (it == table->end() || it->second.Type != SDL_PROPERTY_TYPE_STRING) {
    return default_value;
  }
  return it->second.StringValue.c_str();
}

extern "C" Sint64 SDL_GetNumberProperty(SDL_PropertiesID props, const char* name, Sint64 default_value) {
  if (props == 0 || !name) {
    return default_value;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  auto* table = FindPropertiesLocked(props);
  if (!table) {
    return default_value;
  }

  auto it = table->find(name);
  if (it == table->end() || it->second.Type != SDL_PROPERTY_TYPE_NUMBER) {
    return default_value;
  }
  return it->second.NumberValue;
}

extern "C" float SDL_GetFloatProperty(SDL_PropertiesID props, const char* name, float default_value) {
  if (props == 0 || !name) {
    return default_value;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  auto* table = FindPropertiesLocked(props);
  if (!table) {
    return default_value;
  }

  auto it = table->find(name);
  if (it == table->end() || it->second.Type != SDL_PROPERTY_TYPE_FLOAT) {
    return default_value;
  }
  return it->second.FloatValue;
}

extern "C" bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char* name, bool default_value) {
  if (props == 0 || !name) {
    return default_value;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  auto* table = FindPropertiesLocked(props);
  if (!table) {
    return default_value;
  }

  auto it = table->find(name);
  if (it == table->end() || it->second.Type != SDL_PROPERTY_TYPE_BOOLEAN) {
    return default_value;
  }
  return it->second.BooleanValue;
}

extern "C" bool SDL_ClearProperty(SDL_PropertiesID props, const char* name) {
  if (props == 0 || !name) {
    return false;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  auto* table = FindPropertiesLocked(props);
  if (!table) {
    return false;
  }

  table->erase(name);
  return true;
}

extern "C" bool SDL_EnumerateProperties(
  SDL_PropertiesID props, SDL_EnumeratePropertiesCallback callback, void* userdata) {
  if (props == 0 || !callback) {
    return false;
  }

  std::vector<std::string> names;
  {
    std::lock_guard<std::mutex> guard(PropertiesMutex);
    auto* table = FindPropertiesLocked(props);
    if (!table) {
      return false;
    }

    names.reserve(table->size());
    for (const auto& [name, value] : *table) {
      (void)value;
      names.emplace_back(name);
    }
  }

  for (const auto& name : names) {
    callback(userdata, props, name.c_str());
  }
  return true;
}

extern "C" void SDL_DestroyProperties(SDL_PropertiesID props) {
  if (props == 0) {
    return;
  }

  std::lock_guard<std::mutex> guard(PropertiesMutex);
  PropertiesById.erase(props);
  if (GlobalPropertiesId.load(std::memory_order_acquire) == props) {
    GlobalPropertiesId.store(0, std::memory_order_release);
  }
  if (PrimaryWindowPropertiesId.load(std::memory_order_acquire) == props) {
    PrimaryWindowPropertiesId.store(0, std::memory_order_release);
  }
  if (PrimaryDisplayPropertiesId.load(std::memory_order_acquire) == props) {
    PrimaryDisplayPropertiesId.store(0, std::memory_order_release);
  }
}

extern "C" SDL_GLContext SDL_GL_CreateContext(SDL_Window* window) {
  fprintf(stderr, "[SDL3 thunk] SDL_GL_CreateContext(window=%p)\n", window);
  SDL_GLContext context = fexfn_pack_FEX_SDL_GL_CreateContext(window);
  fprintf(stderr, "[SDL3 thunk] SDL_GL_CreateContext => %p\n", context);
  if (context) {
    CurrentGLContextPointer.store(reinterpret_cast<uintptr_t>(context), std::memory_order_release);
    CurrentGLWindowPointer.store(reinterpret_cast<uintptr_t>(window), std::memory_order_release);
    GLLibraryLoaded.store(true, std::memory_order_release);
  }
  return context;
}

extern "C" bool SDL_GL_DestroyContext(SDL_GLContext context) {
  fprintf(stderr, "[SDL3 thunk] SDL_GL_DestroyContext(context=%p)\n", context);
  fexfn_pack_FEX_SDL_GL_DeleteContext(context);
  ClearCurrentGLStateIfMatching(context);
  return true;
}

extern "C" void SDL_GL_DeleteContext(SDL_GLContext context) {
  (void)SDL_GL_DestroyContext(context);
}

extern "C" bool SDL_GL_LoadLibrary(const char*) {
  fprintf(stderr, "[SDL3 thunk] SDL_GL_LoadLibrary()\n");
  GLLibraryLoaded.store(true, std::memory_order_release);
  return true;
}

extern "C" void SDL_GL_UnloadLibrary() {
  fprintf(stderr, "[SDL3 thunk] SDL_GL_UnloadLibrary()\n");
  GLLibraryLoaded.store(false, std::memory_order_release);
}

extern "C" bool SDL_GL_ExtensionSupported(const char* extension) {
  fprintf(stderr, "[SDL3 thunk] SDL_GL_ExtensionSupported('%s')\n", extension ? extension : "<null>");
  if (!extension || extension[0] == '\0') {
    return false;
  }
  if (!GLLibraryLoaded.load(std::memory_order_acquire)) {
    return false;
  }

  using GLGetStringFn = const GLubyte* (*)(GLenum);
  auto gl_get_string = reinterpret_cast<GLGetStringFn>(SDL_GL_GetProcAddress("glGetString"));
  if (!gl_get_string) {
    TraceGLProcLookup("glGetString", nullptr, "SDL_GL_ExtensionSupported(missing)");
    return false;
  }

  const char* all_extensions = reinterpret_cast<const char*>(gl_get_string(GL_EXTENSIONS));
  if (!all_extensions || all_extensions[0] == '\0') {
    return false;
  }

  const size_t ext_len = std::strlen(extension);
  const char* search = all_extensions;
  while (search) {
    const char* match = std::strstr(search, extension);
    if (!match) {
      break;
    }

    const bool has_left_boundary = (match == all_extensions) || (match[-1] == ' ');
    const char right = match[ext_len];
    const bool has_right_boundary = (right == '\0') || (right == ' ');
    if (has_left_boundary && has_right_boundary) {
      return true;
    }

    search = match + ext_len;
  }
  return false;
}

extern "C" void SDL_GL_ResetAttributes() {
  GLAttributesInitialized.store(false, std::memory_order_release);
  EnsureGLAttributesInitialized();
}

extern "C" bool SDL_GL_MakeCurrent(SDL_Window* window, SDL_GLContext context) {
  fprintf(stderr, "[SDL3 thunk] SDL_GL_MakeCurrent(window=%p context=%p)\n", window, context);
  if (!window && !context) {
    const bool success = fexfn_pack_FEX_SDL_GL_MakeCurrent(nullptr, nullptr) != 0;
    fprintf(stderr, "[SDL3 thunk] SDL_GL_MakeCurrent(NULL,NULL) => %d\n", success ? 1 : 0);
    if (success) {
      CurrentGLWindowPointer.store(0, std::memory_order_release);
      CurrentGLContextPointer.store(0, std::memory_order_release);
    }
    return success;
  }

  if (!window || !context) {
    LogSDLValidationFailure("SDL_GL_MakeCurrent", "window/context must be both null or both non-null");
    return SDL_SetError("SDL_GL_MakeCurrent requires both window and context");
  }

  const bool success = fexfn_pack_FEX_SDL_GL_MakeCurrent(window, context) != 0;
  fprintf(stderr, "[SDL3 thunk] SDL_GL_MakeCurrent => %d\n", success ? 1 : 0);
  if (success) {
    CurrentGLWindowPointer.store(reinterpret_cast<uintptr_t>(window), std::memory_order_release);
    CurrentGLContextPointer.store(reinterpret_cast<uintptr_t>(context), std::memory_order_release);
  }
  return success;
}

extern "C" SDL_Window* SDL_GL_GetCurrentWindow() {
  return reinterpret_cast<SDL_Window*>(CurrentGLWindowPointer.load(std::memory_order_acquire));
}

extern "C" SDL_GLContext SDL_GL_GetCurrentContext() {
  return reinterpret_cast<SDL_GLContext>(CurrentGLContextPointer.load(std::memory_order_acquire));
}

extern "C" bool SDL_GL_SetAttribute(SDL_GLAttr attr, int value) {
  EnsureGLAttributesInitialized();
  fprintf(stderr, "[SDL3 thunk] SDL_GL_SetAttribute(attr=%d value=%d)\n", static_cast<int>(attr), value);
  if (!IsValidGLAttr(attr)) {
    LogSDLValidationFailure("SDL_GL_SetAttribute", "invalid SDL_GLAttr value");
    SDL_SetError("SDL_GL_SetAttribute invalid attr=%d", static_cast<int>(attr));
    return false;
  }

  GLAttributes[static_cast<size_t>(attr)] = value;
  return true;
}

extern "C" bool SDL_GL_GetAttribute(SDL_GLAttr attr, int* value) {
  EnsureGLAttributesInitialized();
  if (!value) {
    LogSDLValidationFailure("SDL_GL_GetAttribute", "value output pointer is null");
    SDL_SetError("SDL_GL_GetAttribute output pointer is null");
    return false;
  }
  if (!IsValidGLAttr(attr)) {
    LogSDLValidationFailure("SDL_GL_GetAttribute", "invalid SDL_GLAttr value");
    SDL_SetError("SDL_GL_GetAttribute invalid attr=%d", static_cast<int>(attr));
    return false;
  }

  // VEXA_FIXES: Desktop clients often treat a 0 double-buffer report as a
  // hard capability mismatch and stop rendering, even though Android presents
  // via eglSwapBuffers.
  if (attr == SDL_GL_DOUBLEBUFFER) {
    *value = 1;
    fprintf(stderr, "[SDL3 thunk] SDL_GL_GetAttribute(attr=%d) => %d (forced)\n",
                 static_cast<int>(attr), *value);
    return true;
  }

  *value = GLAttributes[static_cast<size_t>(attr)];
  fprintf(stderr, "[SDL3 thunk] SDL_GL_GetAttribute(attr=%d) => %d\n", static_cast<int>(attr), *value);
  return true;
}

extern "C" bool SDL_GL_SetSwapInterval(int interval) {
  const bool success = fexfn_pack_FEX_SDL_GL_SetSwapInterval(interval) != 0;
  fprintf(stderr, "[SDL3 thunk] SDL_GL_SetSwapInterval(%d) => %d\n", interval, success ? 1 : 0);
  if (success) {
    CurrentSwapInterval.store(interval, std::memory_order_release);
  }
  return success;
}

extern "C" bool SDL_GL_GetSwapInterval(int* interval) {
  if (!interval) {
    return false;
  }
  *interval = CurrentSwapInterval.load(std::memory_order_acquire);
  fprintf(stderr, "[SDL3 thunk] SDL_GL_GetSwapInterval() => %d\n", *interval);
  return true;
}

extern "C" bool SDL_GL_SwapWindow(SDL_Window* window) {
  if (!window) {
    LogSDLValidationFailure("SDL_GL_SwapWindow", "window is null");
    SDL_SetError("SDL_GL_SwapWindow window is null");
    return false;
  }
  const bool success = fexfn_pack_FEX_SDL_GL_SwapWindow(window) != 0;
  fprintf(stderr, "[SDL3 thunk] SDL_GL_SwapWindow(window=%p) => %d\n", window, success ? 1 : 0);
  return success;
}

extern "C" SDL_FunctionPointer SDL_GL_GetProcAddress(const char* proc) {
  if (!proc) {
    LogSDLValidationFailure("SDL_GL_GetProcAddress", "proc name pointer is null");
    return nullptr;
  }
  if (!*proc) {
    LogSDLValidationFailure("SDL_GL_GetProcAddress", "proc name is empty");
    return nullptr;
  }

  const std::string_view proc_name {proc};

  auto ReturnResolved = [&](void* resolved, const char* source) -> SDL_FunctionPointer {
    TraceGLProcLookup(proc, resolved, source);
    return reinterpret_cast<SDL_FunctionPointer>(resolved);
  };

  // VEXA_FIXES: Lookup pipeline order:
  // 1) shader bridge + thunk self exports, 2) static GL proc table + Android aliases,
  // 3) dynamic fallback (non-GL symbols only).
  // Force runtime proc lookup for shader source so Android can route through
  // libGL-host's custom shader translation path when available.
  if (proc_name == "glShaderSource" || proc_name == "glShaderSourceARB") {
    if (void* resolved = SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>(proc))) {
      return ReturnResolved(resolved, "glXGetProcAddress(force-shadersource)");
    }
  }

  for (const auto& entry : GLProcTable) {
    if (entry.Name == proc) {
      if (entry.Function) {
        return ReturnResolved(reinterpret_cast<void*>(entry.Function), "table");
      }
      break;
    }
  }

  if (proc_name == "SDL_GL_GetProcAddress") {
    return ReturnResolved(reinterpret_cast<void*>(SDL_GL_GetProcAddress), "self");
  }
  if (proc_name == "SDL_GL_ExtensionSupported") {
    return ReturnResolved(reinterpret_cast<void*>(SDL_GL_ExtensionSupported), "self");
  }
  if (proc_name == "SDL_GL_LoadLibrary") {
    return ReturnResolved(reinterpret_cast<void*>(SDL_GL_LoadLibrary), "self");
  }
  if (proc_name == "SDL_GL_UnloadLibrary") {
    return ReturnResolved(reinterpret_cast<void*>(SDL_GL_UnloadLibrary), "self");
  }
  if (proc_name == "SDL_GL_ResetAttributes") {
    return ReturnResolved(reinterpret_cast<void*>(SDL_GL_ResetAttributes), "self");
  }
  if (proc_name == "SDL_GL_MakeCurrent") {
    return ReturnResolved(reinterpret_cast<void*>(SDL_GL_MakeCurrent), "self");
  }
  if (proc_name == "SDL_GL_GetCurrentWindow") {
    return ReturnResolved(reinterpret_cast<void*>(SDL_GL_GetCurrentWindow), "self");
  }
  if (proc_name == "SDL_GL_GetCurrentContext") {
    return ReturnResolved(reinterpret_cast<void*>(SDL_GL_GetCurrentContext), "self");
  }
  if (proc_name == "SDL_GL_GetSwapInterval") {
    return ReturnResolved(reinterpret_cast<void*>(SDL_GL_GetSwapInterval), "self");
  }
  if (proc_name == "SDL_GL_DestroyContext") {
    return ReturnResolved(reinterpret_cast<void*>(SDL_GL_DestroyContext), "self");
  }
  if (proc_name == "SDL_GL_DeleteContext") {
    return ReturnResolved(reinterpret_cast<void*>(SDL_GL_DeleteContext), "self");
  }
  if (proc_name == "glClearDepth") {
    return ReturnResolved(reinterpret_cast<void*>(GLCompatClearDepth), "compat(glClearDepthf)");
  }
  if (proc_name == "glClearStencil") {
    return ReturnResolved(reinterpret_cast<void*>(GLCompatClearStencil), "compat(glClearStencil)");
  }
  if (proc_name == "glDepthRange") {
    return ReturnResolved(reinterpret_cast<void*>(GLCompatDepthRange), "compat(glDepthRangef)");
  }
  if (proc_name == "glPolygonMode") {
    if (void* resolved = SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glPolygonMode"))) {
      return ReturnResolved(resolved, "glXGetProcAddress");
    }
    if (void* resolved = SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glPolygonModeEXT"))) {
      return ReturnResolved(resolved, "glXGetProcAddress(alias)");
    }
    if (void* resolved = SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glPolygonModeNV"))) {
      return ReturnResolved(resolved, "glXGetProcAddress(alias)");
    }
    return ReturnResolved(reinterpret_cast<void*>(GLCompatPolygonMode), "compat(polygonmode-fallback)");
  }
  if (proc_name == "glTexImage1D") {
    return ReturnResolved(reinterpret_cast<void*>(GLCompatTexImage1D), "compat(glTexImage2D,h=1)");
  }
  if (proc_name == "glTexImage2DMultisample") {
    return ReturnResolved(reinterpret_cast<void*>(GLCompatTexImage2DMultisample), "compat(multisample)");
  }
  if (proc_name == "glDrawBuffer") {
    return ReturnResolved(reinterpret_cast<void*>(GLCompatDrawBuffer), "compat(drawbuffers)");
  }
  if (proc_name == "glGetBufferSubData") {
    return ReturnResolved(reinterpret_cast<void*>(GLCompatGetBufferSubData), "compat(mapbufferrange)");
  }
  if (proc_name == "glVertexAttribI2i") {
    return ReturnResolved(reinterpret_cast<void*>(GLCompatVertexAttribI2i), "compat(vertexattribi4i)");
  }
  if (proc_name == "glGetFramebufferAttachmentParameteriv" ||
      proc_name == "glGetFramebufferAttachmentParameterivEXT") {
    if (void* resolved = SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glGetFramebufferAttachmentParameteriv"))) {
      return ReturnResolved(resolved, "glXGetProcAddress");
    }
    if (void* resolved = SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>("glGetFramebufferAttachmentParameterivEXT"))) {
      return ReturnResolved(resolved, "glXGetProcAddress(alias)");
    }
    return ReturnResolved(reinterpret_cast<void*>(GLCompatGetFramebufferAttachmentParameteriv),
                         "compat(framebuffer-attach-query-fallback)");
  }
  if (proc_name == "glGetTexImage") {
    return ReturnResolved(reinterpret_cast<void*>(GLCompatGetTexImage), "compat(glReadPixels)");
  }

  const bool is_gl_or_egl_proc = proc_name.rfind("gl", 0) == 0 || proc_name.rfind("egl", 0) == 0;
  if (is_gl_or_egl_proc) {
    if (void* resolved = SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>(proc))) {
      return ReturnResolved(resolved, "glXGetProcAddress");
    }

    for (const char* alias : GetAndroidGLProcAliases(proc_name)) {
      if (!alias) {
        break;
      }
      if (void* resolved = SDL3ResolveGLProc(reinterpret_cast<const GLubyte*>(alias))) {
        return ReturnResolved(resolved, "glXGetProcAddress(alias)");
      }
    }
  }

  // If the GL thunk couldn't bridge a GL/EGL entry point then falling back to
  // RTLD_DEFAULT only returns the guest-side export, which can still point at a
  // null host function pointer.
  if (is_gl_or_egl_proc) {
    TraceGLProcLookup(proc, nullptr, "gl-miss");
    return nullptr;
  }

  // Table miss fallback: let dynamic linker resolve GL symbols from libGL thunk.
  if (void* resolved = dlsym(RTLD_DEFAULT, proc)) {
    fprintf(stderr, "[SDL3 thunk] SDL_GL_GetProcAddress RTLD_DEFAULT('%s') => %p\n", proc, resolved);
    return ReturnResolved(resolved, "RTLD_DEFAULT");
  }

  static void* gl_handle = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (gl_handle) {
    fprintf(stderr, "[SDL3 thunk] SDL_GL_GetProcAddress gl_handle=libGL.so.1 => %p\n", gl_handle);
  }
  if (!gl_handle) {
    gl_handle = dlopen("libGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (gl_handle) {
      fprintf(stderr, "[SDL3 thunk] SDL_GL_GetProcAddress gl_handle=libGL.so => %p\n", gl_handle);
    }
  }
  if (gl_handle) {
    if (void* resolved = dlsym(gl_handle, proc)) {
      fprintf(stderr, "[SDL3 thunk] SDL_GL_GetProcAddress dlsym(handle,'%s') => %p\n", proc, resolved);
      return ReturnResolved(resolved, "libGL handle");
    }
  }

  TraceGLProcLookup(proc, nullptr, "miss");
  return nullptr;
}

extern "C" bool SDL_PollEvent(SDL_Event* event) {
  fprintf(stderr, "[SDL3 thunk] SDL_PollEvent(event=%p)\n", event);
  {
    std::scoped_lock lock {SyntheticEventQueueMutex};
    if (!SyntheticEventQueue.empty()) {
      SDL_Event queued = SyntheticEventQueue.front();
      SyntheticEventQueue.pop_front();
      if (event) {
        std::memcpy(event, &queued, sizeof(SDL_Event));
      }
      fprintf(stderr,
                   "[SDL3 thunk] SDL_PollEvent => 1 type=0x%x (synthetic)\n",
                   static_cast<unsigned>(queued.type));
      return true;
    }
  }
  const bool success = fexfn_pack_FEX_SDL_PollEvent(event) != 0;
  fprintf(stderr,
               "[SDL3 thunk] SDL_PollEvent => %d type=0x%x\n",
               success ? 1 : 0,
               (success && event) ? static_cast<unsigned>(event->type) : 0U);
  return success;
}

extern "C" bool SDL_WaitEvent(SDL_Event* event) {
  while (true) {
    if (SDL_PollEvent(event)) {
      return true;
    }
    SDL_Delay(1);
  }
}

extern "C" bool SDL_WaitEventTimeout(SDL_Event* event, Sint32 timeoutMS) {
  if (timeoutMS < 0) {
    return SDL_WaitEvent(event);
  }

  const auto start = std::chrono::steady_clock::now();
  while (true) {
    if (SDL_PollEvent(event)) {
      return true;
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    if (elapsed_ms >= static_cast<long long>(timeoutMS)) {
      return false;
    }
    SDL_Delay(1);
  }
}

extern "C" const char* SDL_GetPlatform() {
  return "Android";
}

extern "C" int SDL_GetVersion() {
  return SDL_VERSION;
}

extern "C" const char* SDL_GetRevision() {
  // VEXA_FIXES: Return a stable thunk identity token used in runtime diagnostics.
  return "vexa-sdl3-thunk";
}

extern "C" void SDL_PumpEvents() {
  fprintf(stderr, "[SDL3 thunk] SDL_PumpEvents()\n");
}

extern "C" Uint64 SDL_GetTicks() {
  return fexfn_pack_FEX_SDL_GetTicks();
}

extern "C" Uint64 SDL_GetTicks64() {
  return SDL_GetTicks();
}

extern "C" void SDL_Delay(Uint32 ms) {
  fexfn_pack_FEX_SDL_Delay(ms);
}

extern "C" bool SDL_StartTextInput(SDL_Window* window) {
  return fexfn_pack_FEX_SDL_StartTextInput(window) != 0;
}

extern "C" bool SDL_StopTextInput(SDL_Window* window) {
  return fexfn_pack_FEX_SDL_StopTextInput(window) != 0;
}

extern "C" bool SDL_TextInputActive(SDL_Window* window) {
  return fexfn_pack_FEX_SDL_TextInputActive(window) != 0;
}

extern "C" SDL_Surface* SDL_CreateSurface(int width, int height, SDL_PixelFormat format) {
  return fexfn_pack_FEX_SDL_CreateSurface(width, height, format);
}

extern "C" SDL_Surface* SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void* pixels, int pitch) {
  return fexfn_pack_FEX_SDL_CreateSurfaceFrom(width, height, format, pixels, pitch);
}

extern "C" void SDL_DestroySurface(SDL_Surface* surface) {
  fexfn_pack_FEX_SDL_DestroySurface(surface);
}

extern "C" SDL_Surface* SDL_ScaleSurface(SDL_Surface* surface, int width, int height, SDL_ScaleMode scaleMode) {
  return fexfn_pack_FEX_SDL_ScaleSurface(surface, width, height, scaleMode);
}

extern "C" SDL_Cursor* SDL_CreateColorCursor(SDL_Surface* surface, int hot_x, int hot_y) {
  return fexfn_pack_FEX_SDL_CreateColorCursor(surface, hot_x, hot_y);
}

extern "C" SDL_Cursor* SDL_CreateSystemCursor(SDL_SystemCursor id) {
  return fexfn_pack_FEX_SDL_CreateSystemCursor(id);
}

extern "C" bool SDL_SetCursor(SDL_Cursor* cursor) {
  return fexfn_pack_FEX_SDL_SetCursor(cursor);
}

extern "C" SDL_Cursor* SDL_GetCursor() {
  return fexfn_pack_FEX_SDL_GetCursor();
}

extern "C" SDL_Cursor* SDL_GetDefaultCursor() {
  return fexfn_pack_FEX_SDL_GetDefaultCursor();
}

extern "C" void SDL_DestroyCursor(SDL_Cursor* cursor) {
  fexfn_pack_FEX_SDL_DestroyCursor(cursor);
}

extern "C" bool SDL_ShowCursor() {
  return fexfn_pack_FEX_SDL_ShowCursor();
}

extern "C" bool SDL_HideCursor() {
  return fexfn_pack_FEX_SDL_HideCursor();
}

extern "C" bool SDL_CursorVisible() {
  return fexfn_pack_FEX_SDL_CursorVisible();
}

extern "C" SDL_Keymod SDL_GetModState() {
  return static_cast<SDL_Keymod>(CurrentModState.load(std::memory_order_acquire));
}

extern "C" void SDL_SetModState(SDL_Keymod modstate) {
  CurrentModState.store(modstate, std::memory_order_release);
}

extern "C" const char* SDL_GetScancodeName(SDL_Scancode scancode) {
  if (const auto* entry = FindByScancode(scancode); entry) {
    return entry->Name.data();
  }

  if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
    const char value = static_cast<char>('A' + (scancode - SDL_SCANCODE_A));
    KeyNameBuffer.assign(1, value);
    return KeyNameBuffer.c_str();
  }

  if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
    const char value = static_cast<char>('1' + (scancode - SDL_SCANCODE_1));
    KeyNameBuffer.assign(1, value);
    return KeyNameBuffer.c_str();
  }

  if (scancode == SDL_SCANCODE_0) {
    return "0";
  }

  return "";
}

extern "C" SDL_Scancode SDL_GetScancodeFromName(const char* name) {
  if (!name) {
    return SDL_SCANCODE_UNKNOWN;
  }

  const std::string_view input {name};
  if (input.empty()) {
    return SDL_SCANCODE_UNKNOWN;
  }

  if (const auto* entry = FindByName(input); entry) {
    return entry->Scancode;
  }

  if (EqualsIgnoreCase(input, "Enter")) {
    return SDL_SCANCODE_RETURN;
  }
  if (EqualsIgnoreCase(input, "Esc")) {
    return SDL_SCANCODE_ESCAPE;
  }

  if (input.size() == 1) {
    const unsigned char ch = static_cast<unsigned char>(input[0]);
    if (ch >= 'a' && ch <= 'z') {
      return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (ch - 'a'));
    }
    if (ch >= 'A' && ch <= 'Z') {
      return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (ch - 'A'));
    }
    if (ch >= '1' && ch <= '9') {
      return static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (ch - '1'));
    }
    if (ch == '0') {
      return SDL_SCANCODE_0;
    }
  }

  return SDL_SCANCODE_UNKNOWN;
}


extern "C" SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key, SDL_Keymod* modstate) {
  if (modstate) {
    *modstate = SDL_KMOD_NONE;
  }

  if (const auto* entry = FindByKeycode(key); entry) {
    return entry->Scancode;
  }

  const Uint32 key_value = static_cast<Uint32>(key);
  if ((key_value & SDLK_SCANCODE_MASK) == SDLK_SCANCODE_MASK) {
    return static_cast<SDL_Scancode>(key_value & ~SDLK_SCANCODE_MASK);
  }

  if (key_value >= 'a' && key_value <= 'z') {
    return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (key_value - 'a'));
  }
  if (key_value >= 'A' && key_value <= 'Z') {
    return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (key_value - 'A'));
  }
  if (key_value >= '1' && key_value <= '9') {
    return static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (key_value - '1'));
  }
  if (key_value == '0') {
    return SDL_SCANCODE_0;
  }

  return SDL_SCANCODE_UNKNOWN;
}

extern "C" SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode scancode, SDL_Keymod modstate, bool key_event) {
  (void)modstate;
  (void)key_event;

  if (const auto* entry = FindByScancode(scancode); entry) {
    return entry->Keycode;
  }

  if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
    return static_cast<SDL_Keycode>('a' + (scancode - SDL_SCANCODE_A));
  }
  if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
    return static_cast<SDL_Keycode>('1' + (scancode - SDL_SCANCODE_1));
  }
  if (scancode == SDL_SCANCODE_0) {
    return static_cast<SDL_Keycode>('0');
  }

  return SDLK_UNKNOWN;
}
extern "C" const char* SDL_GetKeyName(SDL_Keycode key) {
  if (const auto* entry = FindByKeycode(key); entry) {
    return entry->Name.data();
  }

  const Uint32 key_value = static_cast<Uint32>(key);
  if ((key_value & SDLK_SCANCODE_MASK) == SDLK_SCANCODE_MASK) {
    const auto scancode = static_cast<SDL_Scancode>(key_value & ~SDLK_SCANCODE_MASK);
    return SDL_GetScancodeName(scancode);
  }

  if (key_value < 128 && std::isprint(static_cast<int>(key_value)) != 0) {
    char value = static_cast<char>(key_value);
    if (value >= 'a' && value <= 'z') {
      value = static_cast<char>(value - ('a' - 'A'));
    }
    KeyNameBuffer.assign(1, value);
    return KeyNameBuffer.c_str();
  }

  return "";
}

extern "C" SDL_Keycode SDL_GetKeyFromName(const char* name) {
  if (!name) {
    return SDLK_UNKNOWN;
  }

  const std::string_view input {name};
  if (input.empty()) {
    return SDLK_UNKNOWN;
  }

  if (const auto* entry = FindByName(input); entry) {
    return entry->Keycode;
  }

  if (EqualsIgnoreCase(input, "Enter")) {
    return SDLK_RETURN;
  }
  if (EqualsIgnoreCase(input, "Esc")) {
    return SDLK_ESCAPE;
  }

  if (input.size() == 1) {
    const unsigned char ch = static_cast<unsigned char>(input[0]);
    if (ch >= 'A' && ch <= 'Z') {
      return static_cast<SDL_Keycode>('a' + (ch - 'A'));
    }
    return static_cast<SDL_Keycode>(ch);
  }

  return SDLK_UNKNOWN;
}

extern "C" bool SDL_SetError(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  auto result = SetErrorFromVA(fmt, ap);
  va_end(ap);
  return result;
}

extern "C" bool SDL_SetErrorV(const char* fmt, va_list ap) {
  return SetErrorFromVA(fmt, ap);
}

extern "C" const char* SDL_GetError() {
  return LastError.c_str();
}

extern "C" bool SDL_ClearError() {
  LastError.clear();
  return true;
}

extern "C" bool SDL_ShowMessageBox(const SDL_MessageBoxData* messageboxdata, int* buttonid) {
  const char* title = (messageboxdata && messageboxdata->title) ? messageboxdata->title : "SDL_MessageBox";
  const char* message = (messageboxdata && messageboxdata->message) ? messageboxdata->message : "";
  fprintf(stderr, "SDL_MessageBox[%s]: %s\n", title, message);

  if (buttonid) {
    if (messageboxdata && messageboxdata->numbuttons > 0 && messageboxdata->buttons) {
      *buttonid = messageboxdata->buttons[0].buttonID;
    } else {
      *buttonid = 0;
    }
  }

  return true;
}

extern "C" bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags, const char* title, const char* message, SDL_Window*) {
  fprintf(stderr, "SDL_SimpleMessageBox[%s]: %s\n", title ? title : "SDL_MessageBox", message ? message : "");
  return true;
}

extern "C" void SDL_LogMessageV(int category, SDL_LogPriority priority, const char* fmt, va_list ap) {
  fprintf(stderr, "SDL3[%d:%d]: ", category, priority);
  vfprintf(stderr, fmt, ap);
  std::fputc('\n', stderr);
}

#define SDL3_LOG_WRAPPER(name, category_value, priority_value) \
  extern "C" void name(const char* fmt, ...) { \
    va_list ap; \
    va_start(ap, fmt); \
    SDL_LogMessageV(category_value, priority_value, fmt, ap); \
    va_end(ap); \
  }

#define SDL3_LOG_WRAPPER_CATEGORY(name, priority_value) \
  extern "C" void name(int category, const char* fmt, ...) { \
    va_list ap; \
    va_start(ap, fmt); \
    SDL_LogMessageV(category, priority_value, fmt, ap); \
    va_end(ap); \
  }

SDL3_LOG_WRAPPER(SDL_Log, SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO)
SDL3_LOG_WRAPPER_CATEGORY(SDL_LogTrace, SDL_LOG_PRIORITY_TRACE)
SDL3_LOG_WRAPPER_CATEGORY(SDL_LogVerbose, SDL_LOG_PRIORITY_VERBOSE)
SDL3_LOG_WRAPPER_CATEGORY(SDL_LogDebug, SDL_LOG_PRIORITY_DEBUG)
SDL3_LOG_WRAPPER_CATEGORY(SDL_LogInfo, SDL_LOG_PRIORITY_INFO)
SDL3_LOG_WRAPPER_CATEGORY(SDL_LogWarn, SDL_LOG_PRIORITY_WARN)
SDL3_LOG_WRAPPER_CATEGORY(SDL_LogError, SDL_LOG_PRIORITY_ERROR)
SDL3_LOG_WRAPPER_CATEGORY(SDL_LogCritical, SDL_LOG_PRIORITY_CRITICAL)

extern "C" void SDL_LogMessage(int category, SDL_LogPriority priority, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  SDL_LogMessageV(category, priority, fmt, ap);
  va_end(ap);
}

extern "C" SDL_SharedObject* SDL_LoadObject(const char* sofile) {
  fprintf(stderr, "[SDL3 thunk] SDL_LoadObject(sofile=%s)\n", sofile ? sofile : "(null)");
  auto* handle = dlopen(sofile, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char* error = dlerror();
    LastError = error ? error : "dlopen failed";
    fprintf(stderr, "[SDL3 thunk] SDL_LoadObject => null error=%s\n", LastError.c_str());
  } else {
    fprintf(stderr, "[SDL3 thunk] SDL_LoadObject => %p\n", handle);
  }
  return reinterpret_cast<SDL_SharedObject*>(handle);
}

extern "C" SDL_FunctionPointer SDL_LoadFunction(SDL_SharedObject* handle, const char* name) {
  fprintf(stderr, "[SDL3 thunk] SDL_LoadFunction(handle=%p name=%s)\n",
               handle,
               name ? name : "(null)");
  auto* resolved = dlsym(handle, name);
  fprintf(stderr, "[SDL3 thunk] SDL_LoadFunction => %p\n", resolved);
  return reinterpret_cast<SDL_FunctionPointer>(resolved);
}

extern "C" void SDL_UnloadObject(SDL_SharedObject* handle) {
  fprintf(stderr, "[SDL3 thunk] SDL_UnloadObject(handle=%p)\n", handle);
  if (handle) {
    dlclose(handle);
  }
}

LOAD_LIB(libSDL3)
