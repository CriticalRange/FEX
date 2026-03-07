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
#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_version.h>

#undef GL_ARB_viewport_array
#include "../libGL/glcorearb.h"
#include "../libGL/android_gl_proc_list.h"

extern "C" {
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>

void* glXGetProcAddress(const GLubyte* procname);
}

#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/Guest.h"

#include "thunkgen_guest_libSDL3.inl"

namespace {
thread_local std::string LastError;
thread_local std::string KeyNameBuffer;
std::atomic<SDL_InitFlags> InitializedSubsystems {};
std::atomic<Uint16> CurrentModState {SDL_KMOD_NONE};
std::atomic<int> WindowWidth {1920};
std::atomic<int> WindowHeight {1080};
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
std::string WindowTitle {"HyMobile SDL3"};
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

  if (w <= 0) {
    w = 1920;
  }
  if (h <= 0) {
    h = 1080;
  }

  if (width) {
    *width = w;
  }
  if (height) {
    *height = h;
  }
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

  std::fprintf(stderr, "[SDL3 thunk] SDL_GL_GetProcAddress('%s') => %p via %s\n",
               proc,
               resolved,
               source ? source : "unknown");

  if (FILE* f = std::fopen("/sdcard/HyMobile.Android/gl_proc_trace.log", "a")) {
    std::fprintf(f, "[SDL3 thunk] SDL_GL_GetProcAddress('%s') => %p via %s\n",
                 proc,
                 resolved,
                 source ? source : "unknown");
    std::fclose(f);
  }
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

  auto fn = reinterpret_cast<GLClearDepthfFn>(glXGetProcAddress(reinterpret_cast<const GLubyte*>("glClearDepthf")));
  CachedGLClearDepthf.store(fn, std::memory_order_release);
  return fn;
}

GLDepthRangefFn ResolveGLDepthRangef() {
  if (auto fn = CachedGLDepthRangef.load(std::memory_order_acquire)) {
    return fn;
  }

  auto fn = reinterpret_cast<GLDepthRangefFn>(glXGetProcAddress(reinterpret_cast<const GLubyte*>("glDepthRangef")));
  CachedGLDepthRangef.store(fn, std::memory_order_release);
  return fn;
}

GLTexImage2DFn ResolveGLTexImage2D() {
  if (auto fn = CachedGLTexImage2D.load(std::memory_order_acquire)) {
    return fn;
  }

  auto fn = reinterpret_cast<GLTexImage2DFn>(glXGetProcAddress(reinterpret_cast<const GLubyte*>("glTexImage2D")));
  CachedGLTexImage2D.store(fn, std::memory_order_release);
  return fn;
}

GLTexImage2DMultisampleFn ResolveGLTexImage2DMultisample() {
  if (CachedGLTexImage2DMultisampleResolved.load(std::memory_order_acquire)) {
    return CachedGLTexImage2DMultisample.load(std::memory_order_acquire);
  }

  GLTexImage2DMultisampleFn fn = reinterpret_cast<GLTexImage2DMultisampleFn>(
      glXGetProcAddress(reinterpret_cast<const GLubyte*>("glTexImage2DMultisample")));
  if (!fn) {
    fn = reinterpret_cast<GLTexImage2DMultisampleFn>(
        glXGetProcAddress(reinterpret_cast<const GLubyte*>("glTexImage2DMultisampleEXT")));
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
      glXGetProcAddress(reinterpret_cast<const GLubyte*>("glTexStorage2DMultisample")));
  CachedGLTexStorage2DMultisample.store(fn, std::memory_order_release);
  return fn;
}

GLDrawBufferFn ResolveGLDrawBuffer() {
  if (CachedGLDrawBufferResolved.load(std::memory_order_acquire)) {
    return CachedGLDrawBuffer.load(std::memory_order_acquire);
  }

  GLDrawBufferFn fn =
      reinterpret_cast<GLDrawBufferFn>(glXGetProcAddress(reinterpret_cast<const GLubyte*>("glDrawBuffer")));
  if (!fn) {
    fn = reinterpret_cast<GLDrawBufferFn>(glXGetProcAddress(reinterpret_cast<const GLubyte*>("glDrawBufferEXT")));
  }

  CachedGLDrawBuffer.store(fn, std::memory_order_release);
  CachedGLDrawBufferResolved.store(true, std::memory_order_release);
  return fn;
}

GLDrawBuffersFn ResolveGLDrawBuffers() {
  if (auto fn = CachedGLDrawBuffers.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLDrawBuffersFn>(glXGetProcAddress(reinterpret_cast<const GLubyte*>("glDrawBuffers")));
  CachedGLDrawBuffers.store(fn, std::memory_order_release);
  return fn;
}

GLGetBufferSubDataFn ResolveGLGetBufferSubData() {
  if (CachedGLGetBufferSubDataResolved.load(std::memory_order_acquire)) {
    return CachedGLGetBufferSubData.load(std::memory_order_acquire);
  }

  GLGetBufferSubDataFn fn = reinterpret_cast<GLGetBufferSubDataFn>(
      glXGetProcAddress(reinterpret_cast<const GLubyte*>("glGetBufferSubData")));
  if (!fn) {
    fn = reinterpret_cast<GLGetBufferSubDataFn>(
        glXGetProcAddress(reinterpret_cast<const GLubyte*>("glGetBufferSubDataARB")));
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
      glXGetProcAddress(reinterpret_cast<const GLubyte*>("glMapBufferRange")));
  if (!fn) {
    fn = reinterpret_cast<GLMapBufferRangeFn>(
        glXGetProcAddress(reinterpret_cast<const GLubyte*>("glMapBufferRangeEXT")));
  }

  CachedGLMapBufferRange.store(fn, std::memory_order_release);
  return fn;
}

GLUnmapBufferFn ResolveGLUnmapBuffer() {
  if (auto fn = CachedGLUnmapBuffer.load(std::memory_order_acquire)) {
    return fn;
  }

  GLUnmapBufferFn fn =
      reinterpret_cast<GLUnmapBufferFn>(glXGetProcAddress(reinterpret_cast<const GLubyte*>("glUnmapBuffer")));
  if (!fn) {
    fn = reinterpret_cast<GLUnmapBufferFn>(glXGetProcAddress(reinterpret_cast<const GLubyte*>("glUnmapBufferOES")));
  }

  CachedGLUnmapBuffer.store(fn, std::memory_order_release);
  return fn;
}

GLVertexAttribI2iFn ResolveGLVertexAttribI2i() {
  if (CachedGLVertexAttribI2iResolved.load(std::memory_order_acquire)) {
    return CachedGLVertexAttribI2i.load(std::memory_order_acquire);
  }

  GLVertexAttribI2iFn fn = reinterpret_cast<GLVertexAttribI2iFn>(
      glXGetProcAddress(reinterpret_cast<const GLubyte*>("glVertexAttribI2i")));
  if (!fn) {
    fn = reinterpret_cast<GLVertexAttribI2iFn>(
        glXGetProcAddress(reinterpret_cast<const GLubyte*>("glVertexAttribI2iEXT")));
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
      glXGetProcAddress(reinterpret_cast<const GLubyte*>("glVertexAttribI4i")));
  if (!fn) {
    fn = reinterpret_cast<GLVertexAttribI4iFn>(
        glXGetProcAddress(reinterpret_cast<const GLubyte*>("glVertexAttribI4iEXT")));
  }

  CachedGLVertexAttribI4i.store(fn, std::memory_order_release);
  return fn;
}

GLGetFramebufferAttachmentParameterivFn ResolveGLGetFramebufferAttachmentParameteriv() {
  if (CachedGLGetFramebufferAttachmentParameterivResolved.load(std::memory_order_acquire)) {
    return CachedGLGetFramebufferAttachmentParameteriv.load(std::memory_order_acquire);
  }

  GLGetFramebufferAttachmentParameterivFn fn = reinterpret_cast<GLGetFramebufferAttachmentParameterivFn>(
      glXGetProcAddress(reinterpret_cast<const GLubyte*>("glGetFramebufferAttachmentParameteriv")));
  if (!fn) {
    fn = reinterpret_cast<GLGetFramebufferAttachmentParameterivFn>(
        glXGetProcAddress(reinterpret_cast<const GLubyte*>("glGetFramebufferAttachmentParameterivEXT")));
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
      glXGetProcAddress(reinterpret_cast<const GLubyte*>("glGetTexLevelParameteriv")));
  CachedGLGetTexLevelParameteriv.store(fn, std::memory_order_release);
  return fn;
}

GLGetIntegervFn ResolveGLGetIntegerv() {
  if (auto fn = CachedGLGetIntegerv.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLGetIntegervFn>(glXGetProcAddress(reinterpret_cast<const GLubyte*>("glGetIntegerv")));
  CachedGLGetIntegerv.store(fn, std::memory_order_release);
  return fn;
}

GLGenFramebuffersFn ResolveGLGenFramebuffers() {
  if (auto fn = CachedGLGenFramebuffers.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLGenFramebuffersFn>(glXGetProcAddress(reinterpret_cast<const GLubyte*>("glGenFramebuffers")));
  CachedGLGenFramebuffers.store(fn, std::memory_order_release);
  return fn;
}

GLDeleteFramebuffersFn ResolveGLDeleteFramebuffers() {
  if (auto fn = CachedGLDeleteFramebuffers.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLDeleteFramebuffersFn>(glXGetProcAddress(reinterpret_cast<const GLubyte*>("glDeleteFramebuffers")));
  CachedGLDeleteFramebuffers.store(fn, std::memory_order_release);
  return fn;
}

GLBindFramebufferFn ResolveGLBindFramebuffer() {
  if (auto fn = CachedGLBindFramebuffer.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLBindFramebufferFn>(glXGetProcAddress(reinterpret_cast<const GLubyte*>("glBindFramebuffer")));
  CachedGLBindFramebuffer.store(fn, std::memory_order_release);
  return fn;
}

GLFramebufferTexture2DFn ResolveGLFramebufferTexture2D() {
  if (auto fn = CachedGLFramebufferTexture2D.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLFramebufferTexture2DFn>(
      glXGetProcAddress(reinterpret_cast<const GLubyte*>("glFramebufferTexture2D")));
  CachedGLFramebufferTexture2D.store(fn, std::memory_order_release);
  return fn;
}

GLCheckFramebufferStatusFn ResolveGLCheckFramebufferStatus() {
  if (auto fn = CachedGLCheckFramebufferStatus.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLCheckFramebufferStatusFn>(
      glXGetProcAddress(reinterpret_cast<const GLubyte*>("glCheckFramebufferStatus")));
  CachedGLCheckFramebufferStatus.store(fn, std::memory_order_release);
  return fn;
}

GLReadPixelsFn ResolveGLReadPixels() {
  if (auto fn = CachedGLReadPixels.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLReadPixelsFn>(glXGetProcAddress(reinterpret_cast<const GLubyte*>("glReadPixels")));
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
  (void)depth;

  // Temporary compat fallback: skip glClearDepthf on Android/FEX because some
  // drivers expose a callable entry that still resolves to a null target at
  // runtime. This avoids a hard crash but can cause visual differences.
  static std::atomic<uint32_t> warn_budget {1};
  uint32_t remaining = warn_budget.load(std::memory_order_acquire);
  while (remaining > 0 &&
         !warn_budget.compare_exchange_weak(remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  if (remaining > 0) {
    std::fprintf(stderr, "[SDL3 thunk] glClearDepth fallback: skipped (may cause visual differences)\n");
  }
}

void GLCompatClearStencil(GLint s) {
  (void)s;

  // Temporary compat fallback: skip glClearStencil on Android/FEX because the
  // resolved host trampoline can still branch through a null runtime target.
  // This avoids a hard crash but can cause visual differences.
  static std::atomic<uint32_t> warn_budget {1};
  uint32_t remaining = warn_budget.load(std::memory_order_acquire);
  while (remaining > 0 &&
         !warn_budget.compare_exchange_weak(remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  if (remaining > 0) {
    std::fprintf(stderr, "[SDL3 thunk] glClearStencil fallback: skipped (may cause visual differences)\n");
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

  GLPolygonModeFn fn = reinterpret_cast<GLPolygonModeFn>(glXGetProcAddress(reinterpret_cast<const GLubyte*>("glPolygonMode")));
  if (!fn) {
    fn = reinterpret_cast<GLPolygonModeFn>(glXGetProcAddress(reinterpret_cast<const GLubyte*>("glPolygonModeEXT")));
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

  std::fprintf(stderr, "[SDL3 thunk] glPolygonMode fallback ignored unsupported mode=0x%x\n", static_cast<unsigned>(mode));
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

  std::fprintf(stderr, "[SDL3 thunk] glDrawBuffer fallback ignored unsupported buf=0x%x\n", static_cast<unsigned>(buf));
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

  std::fprintf(stderr, "[SDL3 thunk] glVertexAttribI2i fallback unavailable for index=%u\n", index);
}

void GLCompatGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint* params) {
  if (!params) {
    return;
  }

  if (auto fn = ResolveGLGetFramebufferAttachmentParameteriv()) {
    fn(target, attachment, pname, params);
    return;
  }

  // Fallback for drivers where the desktop query entry point isn't exposed.
  // Values are conservative defaults and may cause visual differences.
  switch (pname) {
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:
      *params = GL_NONE;
      return;
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER:
      *params = 0;
      return;
    case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:
      *params = 8;
      return;
    case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE:
      *params = 24;
      return;
    case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE:
      *params = 8;
      return;
    case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING:
      *params = GL_LINEAR;
      return;
    case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:
      *params = GL_UNSIGNED_NORMALIZED;
      return;
    default:
      break;
  }

  *params = 0;
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
  std::fprintf(stderr, "SDL3: %s\n", LastError.c_str());
  return false;
}

struct GLProcEntry {
  std::string_view Name;
  SDL_FunctionPointer Function;
};

#define SDL3_GL_PROC_ENTRY(name) {#name, reinterpret_cast<SDL_FunctionPointer>(::name)},
const GLProcEntry GLProcTable[] = {
  FEX_ANDROID_GL_PROC_LIST(SDL3_GL_PROC_ENTRY)
};
#undef SDL3_GL_PROC_ENTRY
} // namespace

extern "C" bool SDL_Init(SDL_InitFlags flags) {
  const SDL_InitFlags normalized = NormalizeInitFlags(flags);
  const SDL_InitFlags previous = InitializedSubsystems.fetch_or(normalized, std::memory_order_acq_rel);
  if (previous == 0) {
    return fexfn_pack_FEX_SDL_Init(normalized) != 0;
  }
  return true;
}

extern "C" bool SDL_InitSubSystem(SDL_InitFlags flags) {
  return SDL_Init(flags);
}

extern "C" void SDL_Quit() {
  InitializedSubsystems.store(0, std::memory_order_release);
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
  SDL_Window* window = fexfn_pack_FEX_SDL_CreateWindow(title, w, h, flags);
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
    WindowFlagsState.store(flags, std::memory_order_release);
    {
      std::lock_guard<std::mutex> guard(WindowStateMutex);
      HasFullscreenWindowMode = false;
      FullscreenWindowMode = {};
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
  const bool success = fexfn_pack_FEX_SDL_GetWindowSize(window, w, h) != 0;
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
  if (count) {
    *count = 1;
  }

  auto* displays = static_cast<SDL_DisplayID*>(std::malloc(sizeof(SDL_DisplayID) * 2));
  if (!displays) {
    return nullptr;
  }

  displays[0] = PrimaryDisplayId;
  displays[1] = 0;
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

  rect->x = 0;
  rect->y = 0;
  GetFallbackWindowSize(&rect->w, &rect->h);
  return true;
}

extern "C" bool SDL_GetDisplayUsableBounds(SDL_DisplayID display_id, SDL_Rect* rect) {
  return SDL_GetDisplayBounds(display_id, rect);
}

extern "C" float SDL_GetDisplayContentScale(SDL_DisplayID display_id) {
  return IsKnownDisplay(display_id) ? 1.0f : 0.0f;
}

extern "C" const SDL_DisplayMode* SDL_GetDesktopDisplayMode(SDL_DisplayID display_id) {
  static SDL_DisplayMode mode {};
  if (!IsKnownDisplay(display_id)) {
    return nullptr;
  }

  mode = BuildFallbackDisplayMode();
  return &mode;
}

extern "C" const SDL_DisplayMode* SDL_GetCurrentDisplayMode(SDL_DisplayID display_id) {
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

extern "C" float SDL_GetWindowPixelDensity(SDL_Window*) {
  return 1.0f;
}

extern "C" float SDL_GetWindowDisplayScale(SDL_Window*) {
  return 1.0f;
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
  return true;
}

extern "C" bool SDL_GetWindowSizeInPixels(SDL_Window* window, int* w, int* h) {
  if (!window) {
    return false;
  }

  GetFallbackWindowSize(w, h);
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
  if (!window) {
    return false;
  }
  UpdateWindowFlags(0, SDL_WINDOW_HIDDEN);
  return true;
}

extern "C" bool SDL_HideWindow(SDL_Window* window) {
  if (!window) {
    return false;
  }
  UpdateWindowFlags(SDL_WINDOW_HIDDEN, 0);
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
  UpdateWindowFlags(SDL_WINDOW_MINIMIZED, SDL_WINDOW_MAXIMIZED);
  return true;
}

extern "C" bool SDL_RestoreWindow(SDL_Window* window) {
  if (!window) {
    return false;
  }
  UpdateWindowFlags(0, SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED);
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
  SDL_GLContext context = fexfn_pack_FEX_SDL_GL_CreateContext(window);
  if (context) {
    CurrentGLContextPointer.store(reinterpret_cast<uintptr_t>(context), std::memory_order_release);
    CurrentGLWindowPointer.store(reinterpret_cast<uintptr_t>(window), std::memory_order_release);
    GLLibraryLoaded.store(true, std::memory_order_release);
  }
  return context;
}

extern "C" bool SDL_GL_DestroyContext(SDL_GLContext context) {
  fexfn_pack_FEX_SDL_GL_DeleteContext(context);
  ClearCurrentGLStateIfMatching(context);
  return true;
}

extern "C" void SDL_GL_DeleteContext(SDL_GLContext context) {
  (void)SDL_GL_DestroyContext(context);
}

extern "C" bool SDL_GL_LoadLibrary(const char*) {
  GLLibraryLoaded.store(true, std::memory_order_release);
  return true;
}

extern "C" void SDL_GL_UnloadLibrary() {
  GLLibraryLoaded.store(false, std::memory_order_release);
}

extern "C" bool SDL_GL_ExtensionSupported(const char* extension) {
  if (!extension || extension[0] == '\0') {
    return false;
  }
  if (!GLLibraryLoaded.load(std::memory_order_acquire)) {
    return false;
  }

  const char* all_extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
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
  if (!window && !context) {
    CurrentGLWindowPointer.store(0, std::memory_order_release);
    CurrentGLContextPointer.store(0, std::memory_order_release);
    return true;
  }

  if (!window || !context) {
    return SDL_SetError("SDL_GL_MakeCurrent requires both window and context");
  }

  CurrentGLWindowPointer.store(reinterpret_cast<uintptr_t>(window), std::memory_order_release);
  CurrentGLContextPointer.store(reinterpret_cast<uintptr_t>(context), std::memory_order_release);
  return true;
}

extern "C" SDL_Window* SDL_GL_GetCurrentWindow() {
  return reinterpret_cast<SDL_Window*>(CurrentGLWindowPointer.load(std::memory_order_acquire));
}

extern "C" SDL_GLContext SDL_GL_GetCurrentContext() {
  return reinterpret_cast<SDL_GLContext>(CurrentGLContextPointer.load(std::memory_order_acquire));
}

extern "C" bool SDL_GL_SetAttribute(SDL_GLAttr attr, int value) {
  EnsureGLAttributesInitialized();
  if (!IsValidGLAttr(attr)) {
    return false;
  }

  GLAttributes[static_cast<size_t>(attr)] = value;
  return true;
}

extern "C" bool SDL_GL_GetAttribute(SDL_GLAttr attr, int* value) {
  EnsureGLAttributesInitialized();
  if (!value || !IsValidGLAttr(attr)) {
    return false;
  }

  *value = GLAttributes[static_cast<size_t>(attr)];
  return true;
}

extern "C" bool SDL_GL_SetSwapInterval(int interval) {
  const bool success = fexfn_pack_FEX_SDL_GL_SetSwapInterval(interval) != 0;
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
  return true;
}

extern "C" bool SDL_GL_SwapWindow(SDL_Window* window) {
  return fexfn_pack_FEX_SDL_GL_SwapWindow(window) != 0;
}

extern "C" SDL_FunctionPointer SDL_GL_GetProcAddress(const char* proc) {
  if (!proc) {
    return nullptr;
  }

  const std::string_view proc_name {proc};

  auto ReturnResolved = [&](void* resolved, const char* source) -> SDL_FunctionPointer {
    TraceGLProcLookup(proc, resolved, source);
    return reinterpret_cast<SDL_FunctionPointer>(resolved);
  };

  // Force runtime proc lookup for shader source so Android can route through
  // libGL-host's custom shader translation path when available.
  if (proc_name == "glShaderSource" || proc_name == "glShaderSourceARB") {
    if (void* resolved = reinterpret_cast<void*>(glXGetProcAddress(reinterpret_cast<const GLubyte*>(proc)))) {
      return ReturnResolved(resolved, "glXGetProcAddress(force-shadersource)");
    }
  }

  for (const auto& entry : GLProcTable) {
    if (entry.Name == proc) {
      return ReturnResolved(reinterpret_cast<void*>(entry.Function), "table");
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
    return ReturnResolved(reinterpret_cast<void*>(GLCompatClearStencil), "compat(no-op)");
  }
  if (proc_name == "glDepthRange") {
    return ReturnResolved(reinterpret_cast<void*>(GLCompatDepthRange), "compat(glDepthRangef)");
  }
  if (proc_name == "glPolygonMode") {
    return ReturnResolved(reinterpret_cast<void*>(GLCompatPolygonMode), "compat(no-op)");
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
  if (proc_name == "glGetFramebufferAttachmentParameteriv") {
    return ReturnResolved(reinterpret_cast<void*>(GLCompatGetFramebufferAttachmentParameteriv), "compat(framebuffer-attach-query)");
  }
  if (proc_name == "glGetTexImage") {
    return ReturnResolved(reinterpret_cast<void*>(GLCompatGetTexImage), "compat(glReadPixels)");
  }

  const bool is_gl_or_egl_proc = proc_name.rfind("gl", 0) == 0 || proc_name.rfind("egl", 0) == 0;
  if (is_gl_or_egl_proc) {
    if (void* resolved = reinterpret_cast<void*>(glXGetProcAddress(reinterpret_cast<const GLubyte*>(proc)))) {
      return ReturnResolved(resolved, "glXGetProcAddress");
    }

    for (const char* alias : GetAndroidGLProcAliases(proc_name)) {
      if (!alias) {
        break;
      }
      if (void* resolved = reinterpret_cast<void*>(glXGetProcAddress(reinterpret_cast<const GLubyte*>(alias)))) {
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
    return ReturnResolved(resolved, "RTLD_DEFAULT");
  }

  static void* gl_handle = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (!gl_handle) {
    gl_handle = dlopen("libGL.so", RTLD_NOW | RTLD_GLOBAL);
  }
  if (gl_handle) {
    if (void* resolved = dlsym(gl_handle, proc)) {
      return ReturnResolved(resolved, "libGL handle");
    }
  }

  TraceGLProcLookup(proc, nullptr, "miss");
  return nullptr;
}

extern "C" bool SDL_PollEvent(SDL_Event* event) {
  return fexfn_pack_FEX_SDL_PollEvent(event) != 0;
}

extern "C" const char* SDL_GetPlatform() {
  return "Android";
}

extern "C" int SDL_GetVersion() {
  return SDL_VERSION;
}

extern "C" const char* SDL_GetRevision() {
  return "hymobile-sdl3-thunk";
}

extern "C" void SDL_PumpEvents() {
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
  std::fprintf(stderr, "SDL_MessageBox[%s]: %s\n", title, message);

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
  std::fprintf(stderr, "SDL_SimpleMessageBox[%s]: %s\n", title ? title : "SDL_MessageBox", message ? message : "");
  return true;
}

extern "C" void SDL_LogMessageV(int category, SDL_LogPriority priority, const char* fmt, va_list ap) {
  std::fprintf(stderr, "SDL3[%d:%d]: ", category, priority);
  std::vfprintf(stderr, fmt, ap);
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
  auto* handle = dlopen(sofile, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char* error = dlerror();
    LastError = error ? error : "dlopen failed";
  }
  return reinterpret_cast<SDL_SharedObject*>(handle);
}

extern "C" SDL_FunctionPointer SDL_LoadFunction(SDL_SharedObject* handle, const char* name) {
  return reinterpret_cast<SDL_FunctionPointer>(dlsym(handle, name));
}

extern "C" void SDL_UnloadObject(SDL_SharedObject* handle) {
  if (handle) {
    dlclose(handle);
  }
}

LOAD_LIB(libSDL3)
