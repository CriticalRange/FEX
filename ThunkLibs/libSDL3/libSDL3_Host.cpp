/*
$info$
tags: thunklibs|SDL3
desc: Android SDL3 host thunk for window/event/GL context bridging
$end_info$
*/

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <EGL/egl.h>
#if __has_include(<android/log.h>)
#include <android/log.h>
#define VEXA_HAS_ANDROID_LOG 1
#else
#define VEXA_HAS_ANDROID_LOG 0
#endif
#include <android/native_window.h>
#include <dlfcn.h>

#include <atomic>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "common/Host.h"

#if defined(BUILD_ANDROID)
static int VexaSDL3HostFprintf(FILE* stream, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  const int rc = std::vfprintf(stream, fmt, args);
  va_end(args);
  if (stream == stderr) {
#if VEXA_HAS_ANDROID_LOG
    __android_log_vprint(ANDROID_LOG_ERROR, "Vexa-SDL3Thunk", fmt, copy);
#endif
  }
  va_end(copy);
  return rc;
}
#define fprintf(stream, ...) VexaSDL3HostFprintf(stream, __VA_ARGS__)
#endif

#include "thunkgen_host_libSDL3.inl"

namespace {
// VEXA_FIXES: Keep SDL3 thunk diagnostics tagged distinctly in logcat.
constexpr const char* LOG_TAG = "Vexa-SDL3Thunk";
// VEXA_FIXES: Persist runtime marker in app-internal artifacts storage.
constexpr const char* SDLContextReadyFlagPath = "/data/user/0/com.critical.vexaemulator/files/artifacts/sdl_context_ready.flag";
constexpr uint64_t kThunkSDLGLCreateContext = 0x53444C330001ULL;
constexpr uint64_t kThunkSDLGLMakeCurrent = 0x53444C330002ULL;
constexpr uint64_t kThunkSDLGLDeleteContext = 0x53444C330003ULL;
constexpr uint64_t kThunkSDLGLSetSwapInterval = 0x53444C330004ULL;
constexpr uint64_t kThunkSDLGLSwapWindow = 0x53444C330005ULL;
using RuntimeNativeWindowGetter = ANativeWindow* (*)();
using RuntimeSurfaceSerialGetter = uint64_t (*)();

#define SDL3_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define SDL3_LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define SDL3_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif

struct VexaSDL3Window {
  int Width {};
  int Height {};
  SDL_WindowFlags Flags {};
  SDL_WindowID WindowID {};
  std::string Title;
};

struct QueuedEvent {
  SDL_Event Event {};
  std::string Text;
};

std::mutex EventQueueMutex;
std::deque<QueuedEvent> EventQueue;
std::atomic<bool> TextInputActiveFlag {false};
std::atomic<bool> SDLInitialized {false};
std::atomic<SDL_WindowID> NextWindowID {1};
std::atomic<float> LastMouseX {0.0f};
std::atomic<float> LastMouseY {0.0f};
std::string LastPolledText;

struct EGLState {
  EGLDisplay Display {EGL_NO_DISPLAY};
  EGLSurface Surface {EGL_NO_SURFACE};
  EGLContext Context {EGL_NO_CONTEXT};
  EGLConfig Config {nullptr};
  ANativeWindow* NativeWindow {nullptr};
  int Width {};
  int Height {};
};

extern "C" __attribute__((visibility("default"))) unsigned long g_GLCreateOwnerTid;

EGLState CurrentEGL;
bool ManagedEGLActive = false;
std::atomic<uint32_t> RuntimeSymbolLogBudget {128};
std::atomic<uint64_t> LastRuntimeSurfaceSerial {0};
std::atomic<bool> RuntimeSurfaceRebindPending {true};
std::atomic<uint32_t> RuntimePbufferRetryTick {0};
std::atomic<bool> LoggedEGLConfigVerification {false};
std::atomic<bool> LoggedWindowSurfaceVerification {false};
std::atomic<bool> LoggedFirstSwapVerification {false};
std::atomic<bool> LoggedSwapStateVerification {false};
std::atomic<uint64_t> LastProgressNs {0};
std::atomic<uint64_t> LastProgressSerial {0};
std::atomic<const char*> LastSDLCall {"<none>"};
std::atomic<const char*> LastProgressMarker {"<none>"};
std::atomic<bool> WatchdogRunning {false};
std::thread WatchdogThread;
std::atomic<bool> LastContextReadyState {false};

bool HasActiveManagedEGLState() {
  return CurrentEGL.Display != EGL_NO_DISPLAY ||
         CurrentEGL.Surface != EGL_NO_SURFACE ||
         CurrentEGL.Context != EGL_NO_CONTEXT ||
         CurrentEGL.Config != nullptr ||
         CurrentEGL.NativeWindow != nullptr;
}
constexpr uint64_t kStallInfoThresholdNs = 16ULL * 1000ULL * 1000ULL;
constexpr uint64_t kStallWarnThresholdNs = 100ULL * 1000ULL * 1000ULL;

using GLHostLastProgressNsGetter = uint64_t (*)();
using GLHostLastProgressSerialGetter = uint64_t (*)();
using GLHostLastCallGetter = const char* (*)();
using GLHostLastRequestedProcGetter = const char* (*)();

void LogEGLState(const char* source) {
  SDL3_LOGI("%s: managed=%d display=%p surface=%p context=%p size=%dx%d owner=%lu",
            source ? source : "EGL",
            ManagedEGLActive ? 1 : 0,
            reinterpret_cast<void*>(CurrentEGL.Display),
            reinterpret_cast<void*>(CurrentEGL.Surface),
            reinterpret_cast<void*>(CurrentEGL.Context),
            CurrentEGL.Width,
            CurrentEGL.Height,
            g_GLCreateOwnerTid);
}

bool IsManagedEGLContextReady() {
  return CurrentEGL.Display != EGL_NO_DISPLAY &&
         CurrentEGL.Surface != EGL_NO_SURFACE &&
         CurrentEGL.Context != EGL_NO_CONTEXT &&
         CurrentEGL.Config != nullptr;
}

#ifndef GL_COLOR_BUFFER_BIT
#define GL_COLOR_BUFFER_BIT 0x00004000
#endif
#ifndef GL_RGBA
#define GL_RGBA 0x1908
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1401
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_SCISSOR_TEST
#define GL_SCISSOR_TEST 0x0C11
#endif
#ifndef GL_TRUE
#define GL_TRUE 1
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif

using GLClearColorFn = void (*)(float, float, float, float);
using GLClearFn = void (*)(unsigned int);
using GLFlushFn = void (*)();
using GLBindFramebufferFn = void (*)(unsigned int, unsigned int);
using GLViewportFn = void (*)(int, int, int, int);
using GLDisableFn = void (*)(unsigned int);
using GLColorMaskFn = void (*)(unsigned char, unsigned char, unsigned char, unsigned char);
using GLReadPixelsFn = void (*)(int, int, int, int, unsigned int, unsigned int, void*);
using GLGetIntegervFn = void (*)(unsigned int, int*);

template <typename T>
T ResolveGLProc(const char* name) {
  static void* glesv3 = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
  static void* glesv2 = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);

  void* symbol = nullptr;
  if (glesv3) {
    symbol = dlsym(glesv3, name);
  }
  if (!symbol && glesv2) {
    symbol = dlsym(glesv2, name);
  }
  if (!symbol) {
    symbol = reinterpret_cast<void*>(eglGetProcAddress(name));
  }
  return reinterpret_cast<T>(symbol);
}

void RunMagentaProbe() {
  static GLClearColorFn p_glClearColor = nullptr;
  static GLClearFn p_glClear = nullptr;
  static GLFlushFn p_glFlush = nullptr;
  static GLBindFramebufferFn p_glBindFramebuffer = nullptr;
  static GLViewportFn p_glViewport = nullptr;
  static GLDisableFn p_glDisable = nullptr;
  static GLColorMaskFn p_glColorMask = nullptr;
  static GLReadPixelsFn p_glReadPixels = nullptr;
  static GLGetIntegervFn p_glGetIntegerv = nullptr;
  static bool resolved = false;
  static bool logged_mode = false;
  static bool force_magenta = []() {
    const char* v = std::getenv("VEXA_SDL_MAGENTA_PROBE");
    return v && std::strcmp(v, "1") == 0;
  }();

  if (!resolved) {
    resolved = true;
    p_glClearColor = ResolveGLProc<GLClearColorFn>("glClearColor");
    p_glClear = ResolveGLProc<GLClearFn>("glClear");
    p_glFlush = ResolveGLProc<GLFlushFn>("glFlush");
    p_glBindFramebuffer = ResolveGLProc<GLBindFramebufferFn>("glBindFramebuffer");
    p_glViewport = ResolveGLProc<GLViewportFn>("glViewport");
    p_glDisable = ResolveGLProc<GLDisableFn>("glDisable");
    p_glColorMask = ResolveGLProc<GLColorMaskFn>("glColorMask");
    p_glReadPixels = ResolveGLProc<GLReadPixelsFn>("glReadPixels");
    p_glGetIntegerv = ResolveGLProc<GLGetIntegervFn>("glGetIntegerv");
    SDL3_LOGI("MAGENTA-PROBE resolve clearColor=%p clear=%p flush=%p bindFB=%p viewport=%p disable=%p colorMask=%p getIntegerv=%p",
              reinterpret_cast<void*>(p_glClearColor),
              reinterpret_cast<void*>(p_glClear),
              reinterpret_cast<void*>(p_glFlush),
              reinterpret_cast<void*>(p_glBindFramebuffer),
              reinterpret_cast<void*>(p_glViewport),
              reinterpret_cast<void*>(p_glDisable),
              reinterpret_cast<void*>(p_glColorMask),
              reinterpret_cast<void*>(p_glGetIntegerv));
  }

  if (!logged_mode) {
    logged_mode = true;
    SDL3_LOGI("MAGENTA-PROBE mode=%s", force_magenta ? "forced" : "diagnostic-only");
  }

  if (force_magenta) {
    if (!p_glClearColor || !p_glClear) {
      SDL3_LOGE("MAGENTA-PROBE missing GL clear entrypoints");
      return;
    }

    const int width = CurrentEGL.Width > 0 ? CurrentEGL.Width : 1;
    const int height = CurrentEGL.Height > 0 ? CurrentEGL.Height : 1;
    if (p_glBindFramebuffer) {
      p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    if (p_glDisable) {
      p_glDisable(GL_SCISSOR_TEST);
    }
    if (p_glColorMask) {
      p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    if (p_glViewport) {
      p_glViewport(0, 0, width, height);
    }

    p_glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
    if (p_glFlush) {
      p_glFlush();
    }
  }

  const bool is_pbuffer_like =
      CurrentEGL.NativeWindow == nullptr || CurrentEGL.Width <= 1 || CurrentEGL.Height <= 1;
  if (is_pbuffer_like) {
    if (p_glReadPixels) {
      unsigned char pixel[4] = {0, 0, 0, 0};
      p_glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
      SDL3_LOGI("PB-PROBE rgba=(%u,%u,%u,%u) size=%dx%d",
                static_cast<unsigned>(pixel[0]),
                static_cast<unsigned>(pixel[1]),
                static_cast<unsigned>(pixel[2]),
                static_cast<unsigned>(pixel[3]),
                CurrentEGL.Width,
                CurrentEGL.Height);
    } else {
      SDL3_LOGW("PB-PROBE glReadPixels unresolved");
    }
  }

  if (!is_pbuffer_like) {
    bool expected = false;
    if (LoggedSwapStateVerification.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      int draw_fbo = -1;
      int read_fbo = -1;
      int current_program = -1;
      if (p_glGetIntegerv) {
        p_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
        p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
        p_glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
      }

      unsigned char pixel[4] = {0, 0, 0, 0};
      int readpix_ok = 0;
      if (p_glReadPixels) {
        p_glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        readpix_ok = 1;
      }

      SDL3_LOGI("EGL-VERIFY pre-swap-state draw_fbo=%d read_fbo=%d current_program=%d readpix_ok=%d rgba=(%u,%u,%u,%u)",
                draw_fbo,
                read_fbo,
                current_program,
                readpix_ok,
                static_cast<unsigned>(pixel[0]),
                static_cast<unsigned>(pixel[1]),
                static_cast<unsigned>(pixel[2]),
                static_cast<unsigned>(pixel[3]));
    }
  }
}

Uint64 GetTimestampNS() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
           std::chrono::steady_clock::now().time_since_epoch())
    .count();
}

void MarkProgress(const char* sdl_call, const char* marker) {
  if (sdl_call) {
    LastSDLCall.store(sdl_call, std::memory_order_release);
  }
  if (marker) {
    LastProgressMarker.store(marker, std::memory_order_release);
  }
  LastProgressNs.store(GetTimestampNS(), std::memory_order_release);
  LastProgressSerial.fetch_add(1, std::memory_order_acq_rel);
}

void LogSlowCall(const char* call, uint64_t elapsed_ns) {
  if (elapsed_ns >= kStallWarnThresholdNs) {
    SDL3_LOGW("stall-warn call=%s duration_ms=%llu",
              call ? call : "<null>",
              static_cast<unsigned long long>(elapsed_ns / 1000000ULL));
  } else if (elapsed_ns >= kStallInfoThresholdNs) {
    SDL3_LOGI("stall-info call=%s duration_ms=%llu",
              call ? call : "<null>",
              static_cast<unsigned long long>(elapsed_ns / 1000000ULL));
  }
}

void* ResolveGLHostSymbol(const char* name) {
  if (!name || !*name) {
    return nullptr;
  }
  if (void* ptr = dlsym(RTLD_DEFAULT, name); ptr) {
    return ptr;
  }
  static void* gl_host = dlopen("libGL-host.so", RTLD_NOW | RTLD_NOLOAD);
  if (!gl_host) {
    gl_host = dlopen("libGL-host.so", RTLD_NOW | RTLD_LOCAL);
  }
  if (gl_host) {
    if (void* ptr = dlsym(gl_host, name); ptr) {
      return ptr;
    }
  }
  return nullptr;
}

void WatchdogMain() {
  GLHostLastProgressNsGetter gl_last_ns = reinterpret_cast<GLHostLastProgressNsGetter>(
      ResolveGLHostSymbol("Vexa_GLHost_LastProgressNs"));
  GLHostLastProgressSerialGetter gl_last_serial = reinterpret_cast<GLHostLastProgressSerialGetter>(
      ResolveGLHostSymbol("Vexa_GLHost_LastProgressSerial"));
  GLHostLastCallGetter gl_last_call = reinterpret_cast<GLHostLastCallGetter>(
      ResolveGLHostSymbol("Vexa_GLHost_LastCall"));
  GLHostLastRequestedProcGetter gl_last_proc = reinterpret_cast<GLHostLastRequestedProcGetter>(
      ResolveGLHostSymbol("Vexa_GLHost_LastRequestedProc"));

  uint64_t last_logged_serial = 0;
  while (WatchdogRunning.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::seconds(2));

    const uint64_t now_ns = GetTimestampNS();
    const uint64_t serial = LastProgressSerial.load(std::memory_order_acquire);
    const uint64_t last_ns = LastProgressNs.load(std::memory_order_acquire);
    const uint64_t since_ms = (now_ns > last_ns) ? ((now_ns - last_ns) / 1000000ULL) : 0;
    if (since_ms < 2000 && serial != last_logged_serial) {
      continue;
    }

    last_logged_serial = serial;
    const char* sdl_call = LastSDLCall.load(std::memory_order_acquire);
    const char* marker = LastProgressMarker.load(std::memory_order_acquire);
    const uint64_t gl_ns = gl_last_ns ? gl_last_ns() : 0;
    const uint64_t gl_since_ms = (gl_ns != 0 && now_ns > gl_ns) ? ((now_ns - gl_ns) / 1000000ULL) : 0;
    const uint64_t gl_serial = gl_last_serial ? gl_last_serial() : 0;
    const char* gl_call = gl_last_call ? gl_last_call() : "<unavailable>";
    const char* gl_proc = gl_last_proc ? gl_last_proc() : "<unavailable>";
    SDL3_LOGW("watchdog time_since_progress_ms=%llu last_sdl_call=%s last_progress_marker=%s gl_last_call=%s gl_last_requested_proc=%s gl_since_ms=%llu gl_serial=%llu",
              static_cast<unsigned long long>(since_ms),
              sdl_call ? sdl_call : "<none>",
              marker ? marker : "<none>",
              gl_call ? gl_call : "<none>",
              gl_proc ? gl_proc : "<none>",
              static_cast<unsigned long long>(gl_since_ms),
              static_cast<unsigned long long>(gl_serial));
  }
}

void StartWatchdogIfNeeded() {
  bool expected = false;
  if (!WatchdogRunning.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
    return;
  }
  WatchdogThread = std::thread(WatchdogMain);
}

void StopWatchdogIfNeeded() {
  bool expected = true;
  if (!WatchdogRunning.compare_exchange_strong(expected, false, std::memory_order_acq_rel, std::memory_order_acquire)) {
    return;
  }
  if (WatchdogThread.joinable()) {
    WatchdogThread.join();
  }
}

// VEXA_FIXES: Rate-limit runtime symbol diagnostics to keep logcat readable
// while still capturing first-failure context for bring-up debugging.
void TraceRuntimeSymbolLookup(const char* phase, const char* symbol_name, const char* library_name, const void* address = nullptr) {
  uint32_t remaining = RuntimeSymbolLogBudget.load(std::memory_order_acquire);
  while (remaining > 0 &&
         !RuntimeSymbolLogBudget.compare_exchange_weak(
             remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  if (remaining == 0) {
    return;
  }

  SDL3_LOGI("runtime-symbol phase=%s symbol=%s lib=%s addr=%p",
            phase ? phase : "unknown",
            symbol_name ? symbol_name : "<null>",
            library_name ? library_name : "<none>",
            address);
}

void* ResolveRuntimeSymbol(const char* name) {
  if (!name || !*name) {
    SDL3_LOGW("ResolveRuntimeSymbol called with empty symbol name");
    return nullptr;
  }

  if (void* symbol = dlsym(RTLD_DEFAULT, name); symbol) {
    TraceRuntimeSymbolLookup("rtld-default-hit", name, "RTLD_DEFAULT", symbol);
    return symbol;
  }

  // VEXA_FIXES: Android linker namespaces can hide app-private exports from
  // RTLD_DEFAULT. Probe likely runtime DSOs and retry dlsym against them.
  // VEXA_FIXES: Probe both runtime containers because some builds export helper
  // symbols from libvexa.so while others keep them in libFEXCore.so.
  static constexpr std::array<const char*, 2> RuntimeLibraries {
    "libvexa.so",
    "libFEXCore.so",
  };
  static std::array<void*, RuntimeLibraries.size()> runtime_handles {};
  static bool attempted_runtime_open = false;
  static bool missing_runtime_logged = false;
  if (!attempted_runtime_open) {
    attempted_runtime_open = true;
    for (size_t i = 0; i < RuntimeLibraries.size(); ++i) {
      runtime_handles[i] = dlopen(RuntimeLibraries[i], RTLD_NOW | RTLD_NOLOAD);
      if (!runtime_handles[i]) {
        runtime_handles[i] = dlopen(RuntimeLibraries[i], RTLD_NOW | RTLD_LOCAL);
      }
      if (runtime_handles[i]) {
        TraceRuntimeSymbolLookup("library-open", RuntimeLibraries[i], RuntimeLibraries[i], runtime_handles[i]);
      }
    }
    if (runtime_handles[0] == nullptr && runtime_handles[1] == nullptr && !missing_runtime_logged) {
      missing_runtime_logged = true;
      SDL3_LOGW("Could not open Vexa runtime libraries while resolving symbols");
    }
  }

  for (size_t i = 0; i < runtime_handles.size(); ++i) {
    void* runtime_handle = runtime_handles[i];
    if (!runtime_handle) {
      continue;
    }

    if (void* symbol = dlsym(runtime_handle, name); symbol) {
      TraceRuntimeSymbolLookup("runtime-library-hit", name, RuntimeLibraries[i], symbol);
      return symbol;
    }
  }

  TraceRuntimeSymbolLookup("miss", name, nullptr, nullptr);
  return nullptr;
}

void SetSDLContextReadyState(bool ready, const char* source) {
  // VEXA_FIXES: Keep optional runtime flag update for compatibility with
  // callers that still monitor this symbol.
  static bool missing_slot_logged = false;
  static std::atomic<int*> context_created_slot {nullptr};
  int* slot = context_created_slot.load(std::memory_order_acquire);
  if (!slot) {
    slot = reinterpret_cast<int*>(ResolveRuntimeSymbol("g_Vexa_SDLContextCreated"));
    if (slot && (reinterpret_cast<uintptr_t>(slot) % alignof(int) == 0)) {
      int* expected = nullptr;
      if (!context_created_slot.compare_exchange_strong(expected, slot, std::memory_order_acq_rel, std::memory_order_acquire)) {
        slot = context_created_slot.load(std::memory_order_acquire);
      }
    }
  }

  if (slot) {
    *slot = ready ? 1 : 0;
  } else if (!missing_slot_logged) {
    missing_slot_logged = true;
    SDL3_LOGW("%s: g_Vexa_SDLContextCreated is unavailable", source);
  }

  LastContextReadyState.store(ready, std::memory_order_release);

  if (ready) {
    FILE* marker = std::fopen(SDLContextReadyFlagPath, "w");
    if (!marker) {
      SDL3_LOGW("%s: failed to write context marker %s", source, SDLContextReadyFlagPath);
      return;
    }

    std::fputs("ready\n", marker);
    std::fclose(marker);
    return;
  }

  if (std::remove(SDLContextReadyFlagPath) != 0 && errno != ENOENT) {
    SDL3_LOGW("%s: failed to remove context marker %s errno=%d", source, SDLContextReadyFlagPath, errno);
  }
}

uint64_t QueryRuntimeSurfaceSerial() {
  static RuntimeSurfaceSerialGetter serial_getter = nullptr;
  if (!serial_getter) {
    serial_getter = reinterpret_cast<RuntimeSurfaceSerialGetter>(
      ResolveRuntimeSymbol("Vexa_GetRuntimeSurfaceSerial"));
  }
  if (!serial_getter) {
    return 0;
  }
  return serial_getter();
}

void SnapshotRuntimeSurfaceSerial() {
  const uint64_t serial = QueryRuntimeSurfaceSerial();
  if (serial != 0) {
    LastRuntimeSurfaceSerial.store(serial, std::memory_order_release);
  }
}

bool IsPbufferLikeSurface() {
  return CurrentEGL.NativeWindow == nullptr ||
         CurrentEGL.Width <= 1 ||
         CurrentEGL.Height <= 1;
}

ANativeWindow* ResolveNativeWindowFromRuntime(int* width, int* height) {
  if (width) {
    *width = 0;
  }
  if (height) {
    *height = 0;
  }

  // Preferred: runtime returns a retained ANativeWindow*.
  if (auto* retained_getter = reinterpret_cast<RuntimeNativeWindowGetter>(
        ResolveRuntimeSymbol("Vexa_GetRuntimeNativeWindowRetained"))) {
    ANativeWindow* window = retained_getter();
    if (!window) {
      SDL3_LOGW("Vexa_GetRuntimeNativeWindowRetained returned null window");
      return nullptr;
    }

    const int window_width = ANativeWindow_getWidth(window);
    const int window_height = ANativeWindow_getHeight(window);
    if (window_width <= 1 || window_height <= 1) {
      SDL3_LOGW("Retained native window not ready yet size=%dx%d; keep pbuffer",
                window_width,
                window_height);
      ANativeWindow_release(window);
      return nullptr;
    }

    if (width) {
      *width = window_width;
    }
    if (height) {
      *height = window_height;
    }
    return window;
  }

  // Compatibility path: acquire immediately so callers own the ref.
  if (auto* getter = reinterpret_cast<RuntimeNativeWindowGetter>(
        ResolveRuntimeSymbol("Vexa_GetRuntimeNativeWindow"))) {
    ANativeWindow* window = getter();
    if (!window) {
      SDL3_LOGW("Vexa_GetRuntimeNativeWindow returned null window");
      return nullptr;
    }

    ANativeWindow_acquire(window);

    const int window_width = ANativeWindow_getWidth(window);
    const int window_height = ANativeWindow_getHeight(window);
    if (window_width <= 1 || window_height <= 1) {
      SDL3_LOGW("Vexa_GetRuntimeNativeWindow not ready yet size=%dx%d; keep pbuffer",
                window_width,
                window_height);
      ANativeWindow_release(window);
      return nullptr;
    }

    if (width) {
      *width = window_width;
    }
    if (height) {
      *height = window_height;
    }
    return window;
  }

  // Fallback path for older runtime integrations exposing globals.
  auto* window_slot = reinterpret_cast<ANativeWindow**>(ResolveRuntimeSymbol("g_Vexa_NativeWindow"));
  auto* ready = reinterpret_cast<int*>(ResolveRuntimeSymbol("g_Vexa_NativeWindowReady"));
  auto* native_width = reinterpret_cast<int*>(ResolveRuntimeSymbol("g_Vexa_NativeWindowWidth"));
  auto* native_height = reinterpret_cast<int*>(ResolveRuntimeSymbol("g_Vexa_NativeWindowHeight"));
  if (!window_slot || !ready) {
    static bool missing_symbols_logged = false;
    if (!missing_symbols_logged) {
      missing_symbols_logged = true;
      SDL3_LOGW("Runtime native-window symbols are unavailable (getter/window_slot/ready)");
    }
    return nullptr;
  }

  if (!*window_slot || *ready == 0) {
    SDL3_LOGW("Fallback native-window globals report not-ready (window=%p ready=%d)",
              *window_slot,
              *ready);
    return nullptr;
  }

  ANativeWindow_acquire(*window_slot);

  if (width) {
    *width = native_width ? *native_width : 0;
  }
  if (height) {
    *height = native_height ? *native_height : 0;
  }
  if ((native_width ? *native_width : 0) <= 1 ||
      (native_height ? *native_height : 0) <= 1) {
    SDL3_LOGW("Fallback native-window globals not ready size=%dx%d; keep pbuffer",
              native_width ? *native_width : 0,
              native_height ? *native_height : 0);
    ANativeWindow_release(*window_slot);
    return nullptr;
  }
  SDL3_LOGI("Using fallback native-window globals window=%p size=%dx%d",
            *window_slot,
            native_width ? *native_width : 0,
            native_height ? *native_height : 0);
  return *window_slot;
}

bool QueryRuntimeDrawableSize(int* width, int* height) {
  int resolved_width = 0;
  int resolved_height = 0;

  if (CurrentEGL.NativeWindow) {
    resolved_width = ANativeWindow_getWidth(CurrentEGL.NativeWindow);
    resolved_height = ANativeWindow_getHeight(CurrentEGL.NativeWindow);
  }

  if ((resolved_width <= 1 || resolved_height <= 1) &&
      CurrentEGL.Display != EGL_NO_DISPLAY &&
      CurrentEGL.Surface != EGL_NO_SURFACE) {
    EGLint queried_width = 0;
    EGLint queried_height = 0;
    if (eglQuerySurface(CurrentEGL.Display, CurrentEGL.Surface, EGL_WIDTH, &queried_width) == EGL_TRUE &&
        eglQuerySurface(CurrentEGL.Display, CurrentEGL.Surface, EGL_HEIGHT, &queried_height) == EGL_TRUE) {
      resolved_width = static_cast<int>(queried_width);
      resolved_height = static_cast<int>(queried_height);
    }
  }

  if ((resolved_width <= 1 || resolved_height <= 1) &&
      CurrentEGL.Width > 1 &&
      CurrentEGL.Height > 1) {
    resolved_width = CurrentEGL.Width;
    resolved_height = CurrentEGL.Height;
  }

  if (resolved_width <= 1 || resolved_height <= 1) {
    int runtime_width = 0;
    int runtime_height = 0;
    ANativeWindow* runtime_window = ResolveNativeWindowFromRuntime(&runtime_width, &runtime_height);
    if (runtime_window) {
      resolved_width = runtime_width;
      resolved_height = runtime_height;
      ANativeWindow_release(runtime_window);
    }
  }

  if (resolved_width <= 1 || resolved_height <= 1) {
    return false;
  }

  CurrentEGL.Width = resolved_width;
  CurrentEGL.Height = resolved_height;
  if (width) {
    *width = resolved_width;
  }
  if (height) {
    *height = resolved_height;
  }
  return true;
}

void DestroyManagedEGLContext() {
  if (!ManagedEGLActive && !HasActiveManagedEGLState()) {
    LogEGLState("DestroyManagedEGLContext(skip)");
    return;
  }

  LogEGLState("DestroyManagedEGLContext(begin)");
  if (CurrentEGL.Display != EGL_NO_DISPLAY) {
    const EGLDisplay display = CurrentEGL.Display;

    if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
      SDL3_LOGW("eglMakeCurrent(detach) failed in DestroyManagedEGLContext: 0x%x", eglGetError());
    }
    if (CurrentEGL.Context != EGL_NO_CONTEXT) {
      if (eglDestroyContext(display, CurrentEGL.Context) != EGL_TRUE) {
        SDL3_LOGW("eglDestroyContext failed: 0x%x", eglGetError());
      }
    }
    if (CurrentEGL.Surface != EGL_NO_SURFACE) {
      if (eglDestroySurface(display, CurrentEGL.Surface) != EGL_TRUE) {
        SDL3_LOGW("eglDestroySurface failed: 0x%x", eglGetError());
      }
      CurrentEGL.Surface = EGL_NO_SURFACE;
    }

    if (eglTerminate(display) != EGL_TRUE) {
      SDL3_LOGW("eglTerminate failed: 0x%x", eglGetError());
    }
  }

  if (CurrentEGL.NativeWindow) {
    ANativeWindow_release(CurrentEGL.NativeWindow);
    CurrentEGL.NativeWindow = nullptr;
  }

  ManagedEGLActive = false;
  CurrentEGL = EGLState {};
  LastRuntimeSurfaceSerial.store(0, std::memory_order_release);
  RuntimeSurfaceRebindPending.store(true, std::memory_order_release);
  RuntimePbufferRetryTick.store(0, std::memory_order_release);
  LogEGLState("DestroyManagedEGLContext(end)");
}

ANativeWindow* WaitForRuntimeWindowReady(int* width, int* height, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    int w = 0;
    int h = 0;
    ANativeWindow* window = ResolveNativeWindowFromRuntime(&w, &h);
    if (window) {
      if (width) {
        *width = w;
      }
      if (height) {
        *height = h;
      }
      SDL3_LOGI("Runtime window became ready during wait: %p size=%dx%d", window, w, h);
      return window;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  return nullptr;
}

bool RebuildManagedSurfaceFromRuntimeWindow() {
  if (!CurrentEGL.Display || CurrentEGL.Context == EGL_NO_CONTEXT || !CurrentEGL.Config) {
    SDL3_LOGE("RebuildManagedSurfaceFromRuntimeWindow skipped: missing display/context/config");
    return false;
  }

  int native_width = 0;
  int native_height = 0;
  ANativeWindow* runtime_window = ResolveNativeWindowFromRuntime(&native_width, &native_height);
  if (!runtime_window) {
    runtime_window = WaitForRuntimeWindowReady(&native_width, &native_height, 3000);
  }
  if (!runtime_window) {
    SDL3_LOGE("Strict window-surface mode: runtime window unavailable");
    return false;
  }
  SDL3_LOGI("RebuildManagedSurfaceFromRuntimeWindow: runtime_window=%p size=%dx%d",
            runtime_window,
            native_width,
            native_height);

  if (eglMakeCurrent(CurrentEGL.Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
    SDL3_LOGW("eglMakeCurrent(detach) failed before surface rebuild: 0x%x", eglGetError());
  }

  if (CurrentEGL.Surface != EGL_NO_SURFACE) {
    if (eglDestroySurface(CurrentEGL.Display, CurrentEGL.Surface) != EGL_TRUE) {
      SDL3_LOGW("eglDestroySurface (rebuild) failed: 0x%x", eglGetError());
    }
    CurrentEGL.Surface = EGL_NO_SURFACE;
  }

  if (CurrentEGL.NativeWindow) {
    ANativeWindow_release(CurrentEGL.NativeWindow);
    CurrentEGL.NativeWindow = nullptr;
  }

  EGLint native_visual_id = 0;
  if (eglGetConfigAttrib(CurrentEGL.Display, CurrentEGL.Config, EGL_NATIVE_VISUAL_ID, &native_visual_id) == EGL_TRUE &&
      native_visual_id != 0) {
    if (ANativeWindow_setBuffersGeometry(runtime_window, 0, 0, native_visual_id) != 0) {
      SDL3_LOGW("ANativeWindow_setBuffersGeometry failed: format=%d",
                native_visual_id);
    }
  }

  EGLSurface new_surface = eglCreateWindowSurface(CurrentEGL.Display, CurrentEGL.Config, runtime_window, nullptr);
  if (new_surface == EGL_NO_SURFACE) {
    const EGLint error = eglGetError();
    SDL3_LOGE("Strict window-surface mode: eglCreateWindowSurface failed: 0x%x", error);
    ANativeWindow_release(runtime_window);
    return false;
  }

  int final_width = 1;
  int final_height = 1;
  EGLint queried_width = 0;
  EGLint queried_height = 0;
  if (eglQuerySurface(CurrentEGL.Display, new_surface, EGL_WIDTH, &queried_width) == EGL_TRUE &&
      eglQuerySurface(CurrentEGL.Display, new_surface, EGL_HEIGHT, &queried_height) == EGL_TRUE) {
    final_width = static_cast<int>(queried_width);
    final_height = static_cast<int>(queried_height);
  } else {
    final_width = native_width > 0 ? native_width : ANativeWindow_getWidth(runtime_window);
    final_height = native_height > 0 ? native_height : ANativeWindow_getHeight(runtime_window);
  }
  if (final_width <= 0) {
    final_width = 1;
  }
  if (final_height <= 0) {
    final_height = 1;
  }

  if (eglMakeCurrent(CurrentEGL.Display, new_surface, new_surface, CurrentEGL.Context) != EGL_TRUE) {
    const EGLint error = eglGetError();
    SDL3_LOGE("eglMakeCurrent after surface rebuild failed: 0x%x", error);

    eglDestroySurface(CurrentEGL.Display, new_surface);
    ANativeWindow_release(runtime_window);
    return false;
  }

  CurrentEGL.Surface = new_surface;
  CurrentEGL.NativeWindow = runtime_window;
  CurrentEGL.Width = final_width;
  CurrentEGL.Height = final_height;
  SnapshotRuntimeSurfaceSerial();
  RuntimeSurfaceRebindPending.store(false, std::memory_order_release);
  RuntimePbufferRetryTick.store(0, std::memory_order_release);

  bool expected = false;
  if (LoggedWindowSurfaceVerification.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    EGLint render_buffer = 0;
    EGLint query_width = 0;
    EGLint query_height = 0;
    eglQuerySurface(CurrentEGL.Display, CurrentEGL.Surface, EGL_RENDER_BUFFER, &render_buffer);
    eglQuerySurface(CurrentEGL.Display, CurrentEGL.Surface, EGL_WIDTH, &query_width);
    eglQuerySurface(CurrentEGL.Display, CurrentEGL.Surface, EGL_HEIGHT, &query_height);
    SDL3_LOGI("EGL-VERIFY window-surface bound: native_window=%p render_buffer=0x%x egl_size=%dx%d logical=%dx%d",
              CurrentEGL.NativeWindow,
              render_buffer,
              static_cast<int>(query_width),
              static_cast<int>(query_height),
              CurrentEGL.Width,
              CurrentEGL.Height);
  }

  LogEGLState("RebuildManagedSurfaceFromRuntimeWindow(success)");
  return true;
}

bool MaybeHandoffRuntimeSurfaceBeforeSwap() {
  if (!ManagedEGLActive) {
    return true;
  }
  if (!IsManagedEGLContextReady()) {
    RuntimeSurfaceRebindPending.store(true, std::memory_order_release);
    return false;
  }

  const uint64_t serial = QueryRuntimeSurfaceSerial();
  const uint64_t last = LastRuntimeSurfaceSerial.load(std::memory_order_acquire);
  const bool serial_changed = serial != 0 && serial != last;
  const bool needs_rebind = RuntimeSurfaceRebindPending.load(std::memory_order_acquire) ||
                            IsPbufferLikeSurface();

  if (!serial_changed && !needs_rebind) {
    return true;
  }

  if (!serial_changed && needs_rebind) {
    const uint32_t tick = RuntimePbufferRetryTick.fetch_add(1, std::memory_order_acq_rel);
    if ((tick % 60) != 0) {
      return true;
    }
    SDL3_LOGW("Still on pbuffer-like surface; retrying runtime window handoff (serial=%llu)",
              static_cast<unsigned long long>(serial));
  } else {
    SDL3_LOGI("Runtime surface serial changed %llu -> %llu; rebuilding EGL surface",
              static_cast<unsigned long long>(last),
              static_cast<unsigned long long>(serial));
  }

  if (!RebuildManagedSurfaceFromRuntimeWindow()) {
    SDL3_LOGE("Runtime surface handoff rebuild failed (strict mode)");
    RuntimeSurfaceRebindPending.store(true, std::memory_order_release);
    return false;
  }

  if (serial != 0) {
    LastRuntimeSurfaceSerial.store(serial, std::memory_order_release);
  }
  RuntimeSurfaceRebindPending.store(false, std::memory_order_release);
  return true;
}

bool SelectDeterministicEGLConfig(EGLDisplay display, EGLConfig* outConfig, int requestedClientVersion) {
  if (!outConfig) {
    return false;
  }

  const EGLint renderableType =
    requestedClientVersion >= 3 ? EGL_OPENGL_ES3_BIT_KHR : EGL_OPENGL_ES2_BIT;
  const EGLint config_attributes[] = {
    EGL_RENDERABLE_TYPE, renderableType,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 16,
    EGL_NONE,
  };

  std::array<EGLConfig, 16> configs {};
  EGLint config_count = 0;
  if (eglChooseConfig(display, config_attributes, configs.data(),
                      static_cast<EGLint>(configs.size()), &config_count) != EGL_TRUE ||
      config_count <= 0) {
    return false;
  }

  auto score_config = [display](EGLConfig config) -> uint64_t {
    auto attr = [display, config](EGLint key, EGLint fallback) -> EGLint {
      EGLint value = fallback;
      if (eglGetConfigAttrib(display, config, key, &value) != EGL_TRUE) {
        return fallback;
      }
      return value;
    };

    const uint64_t red = static_cast<uint64_t>(attr(EGL_RED_SIZE, 0));
    const uint64_t green = static_cast<uint64_t>(attr(EGL_GREEN_SIZE, 0));
    const uint64_t blue = static_cast<uint64_t>(attr(EGL_BLUE_SIZE, 0));
    const uint64_t alpha = static_cast<uint64_t>(attr(EGL_ALPHA_SIZE, 0));
    const uint64_t depth = static_cast<uint64_t>(attr(EGL_DEPTH_SIZE, 0));
    const uint64_t stencil = static_cast<uint64_t>(attr(EGL_STENCIL_SIZE, 0));
    const uint64_t samples = static_cast<uint64_t>(attr(EGL_SAMPLES, 0));
    const uint64_t visual = static_cast<uint64_t>(attr(EGL_NATIVE_VISUAL_ID, 0));

    uint64_t score = 0;
    score |= (red == 8 ? 0ULL : (1ULL << 60));
    score |= (green == 8 ? 0ULL : (1ULL << 59));
    score |= (blue == 8 ? 0ULL : (1ULL << 58));
    score |= (alpha == 8 ? 0ULL : (1ULL << 57));
    score |= (depth == 16 ? 0ULL : (1ULL << 56));
    score |= (stencil << 24);
    score |= (samples << 12);
    score |= visual;
    return score;
  };

  EGLConfig best = configs[0];
  uint64_t best_score = score_config(best);
  for (EGLint i = 1; i < config_count; ++i) {
    const uint64_t score = score_config(configs[static_cast<size_t>(i)]);
    if (score < best_score) {
      best = configs[static_cast<size_t>(i)];
      best_score = score;
    }
  }

  *outConfig = best;

  bool expected = false;
  if (LoggedEGLConfigVerification.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    EGLint selected_surface_type = 0;
    EGLint selected_renderable_type = 0;
    EGLint selected_native_visual = 0;
    EGLint selected_depth = 0;
    EGLint selected_stencil = 0;
    eglGetConfigAttrib(display, best, EGL_SURFACE_TYPE, &selected_surface_type);
    eglGetConfigAttrib(display, best, EGL_RENDERABLE_TYPE, &selected_renderable_type);
    eglGetConfigAttrib(display, best, EGL_NATIVE_VISUAL_ID, &selected_native_visual);
    eglGetConfigAttrib(display, best, EGL_DEPTH_SIZE, &selected_depth);
    eglGetConfigAttrib(display, best, EGL_STENCIL_SIZE, &selected_stencil);
    SDL3_LOGI("EGL-VERIFY config selected: renderable=0x%x surface=0x%x window_bit=%d pbuffer_bit=%d visual=%d depth=%d stencil=%d",
              selected_renderable_type,
              selected_surface_type,
              (selected_surface_type & EGL_WINDOW_BIT) ? 1 : 0,
              (selected_surface_type & EGL_PBUFFER_BIT) ? 1 : 0,
              selected_native_visual,
              selected_depth,
              selected_stencil);
  }
  return true;
}

bool TryCreateManagedEGLContext() {
  if (!ManagedEGLActive && HasActiveManagedEGLState()) {
    DestroyManagedEGLContext();
  }

  EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display == EGL_NO_DISPLAY) {
    SDL3_LOGE("eglGetDisplay failed: 0x%x", eglGetError());
    return false;
  }

  EGLint major = 0;
  EGLint minor = 0;
  if (eglInitialize(display, &major, &minor) != EGL_TRUE) {
    SDL3_LOGE("eglInitialize failed: 0x%x", eglGetError());
    return false;
  }

  if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
    SDL3_LOGE("eglBindAPI(EGL_OPENGL_ES_API) failed: 0x%x", eglGetError());
    return false;
  }

  EGLConfig config = nullptr;
  int client_version = 3;
  if (!SelectDeterministicEGLConfig(display, &config, client_version)) {
    client_version = 2;
    if (!SelectDeterministicEGLConfig(display, &config, client_version)) {
      SDL3_LOGE("eglChooseConfig failed: 0x%x", eglGetError());
      return false;
    }
  }

  const EGLint context_attributes[] = {
    EGL_CONTEXT_CLIENT_VERSION, client_version,
    EGL_NONE,
  };
  EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
  if (context == EGL_NO_CONTEXT) {
    SDL3_LOGE("eglCreateContext failed: 0x%x", eglGetError());
    eglTerminate(display);
    return false;
  }

  CurrentEGL.Display = display;
  CurrentEGL.Surface = EGL_NO_SURFACE;
  CurrentEGL.Context = context;
  CurrentEGL.Config = config;
  CurrentEGL.NativeWindow = nullptr;
  CurrentEGL.Width = 0;
  CurrentEGL.Height = 0;
  ManagedEGLActive = true;
  RuntimeSurfaceRebindPending.store(true, std::memory_order_release);
  RuntimePbufferRetryTick.store(0, std::memory_order_release);
  LoggedWindowSurfaceVerification.store(false, std::memory_order_release);
  LoggedFirstSwapVerification.store(false, std::memory_order_release);

  if (!RebuildManagedSurfaceFromRuntimeWindow()) {
    SDL3_LOGE("Managed EGL creation failed while building initial surface");
    DestroyManagedEGLContext();
    return false;
  }

  SDL3_LOGI("Managed EGL context created (%dx%d, EGL %d.%d)",
            CurrentEGL.Width,
            CurrentEGL.Height,
            major,
            minor);
  LogEGLState("TryCreateManagedEGLContext(success)");
  return true;
}

bool CaptureCurrentEGLContext() {
  CurrentEGL.Display = eglGetCurrentDisplay();
  CurrentEGL.Surface = eglGetCurrentSurface(EGL_DRAW);
  CurrentEGL.Context = eglGetCurrentContext();

  if (CurrentEGL.Display == EGL_NO_DISPLAY ||
      CurrentEGL.Surface == EGL_NO_SURFACE ||
      CurrentEGL.Context == EGL_NO_CONTEXT) {
    LastContextReadyState.store(false, std::memory_order_release);
    CurrentEGL.NativeWindow = nullptr;
    return false;
  }

  EGLint width = 0;
  EGLint height = 0;
  if (eglQuerySurface(CurrentEGL.Display, CurrentEGL.Surface, EGL_WIDTH, &width) != EGL_TRUE) {
    width = 0;
  }
  if (eglQuerySurface(CurrentEGL.Display, CurrentEGL.Surface, EGL_HEIGHT, &height) != EGL_TRUE) {
    height = 0;
  }
  CurrentEGL.Width = static_cast<int>(width);
  CurrentEGL.Height = static_cast<int>(height);
  LogEGLState("CaptureCurrentEGLContext");

  LastContextReadyState.store(true, std::memory_order_release);
  return true;
}

bool EnsureEGLContextReady() {
  if (CaptureCurrentEGLContext()) {
    LogEGLState("EnsureEGLContextReady(captured)");
    return true;
  }

  SDL3_LOGW("No current EGL context; falling back to managed EGL context path");

  if (ManagedEGLActive) {
    if (CurrentEGL.Display != EGL_NO_DISPLAY &&
        CurrentEGL.Surface != EGL_NO_SURFACE &&
        CurrentEGL.Context != EGL_NO_CONTEXT &&
        CurrentEGL.Config != nullptr &&
        CurrentEGL.NativeWindow != nullptr &&
        eglMakeCurrent(CurrentEGL.Display, CurrentEGL.Surface, CurrentEGL.Surface, CurrentEGL.Context) == EGL_TRUE &&
        CaptureCurrentEGLContext()) {
      LogEGLState("EnsureEGLContextReady(rebound)");
      return true;
    }

    SDL3_LOGW("Managed EGL context became invalid; recreating");
    DestroyManagedEGLContext();
  }

  return TryCreateManagedEGLContext();
}

void PushEvent(const SDL_Event& event, std::string text = {}) {
  std::scoped_lock lock {EventQueueMutex};
  EventQueue.push_back(QueuedEvent {
    .Event = event,
    .Text = std::move(text),
  });
}
} // namespace

extern "C" __attribute__((visibility("default"))) unsigned long g_GLCreateOwnerTid = 0;

static unsigned long CurrentThreadToken() {
  return std::hash<std::thread::id> {}(std::this_thread::get_id());
}

static bool ValidateGLOwnerThread(const char* source) {
  if (g_GLCreateOwnerTid == 0) {
    return true;
  }

  const unsigned long current = CurrentThreadToken();
  if (current == g_GLCreateOwnerTid) {
    return true;
  }

  SDL3_LOGW("%s called on non-owner thread (owner=%lu current=%lu)",
            source,
            g_GLCreateOwnerTid,
            current);
  return false;
}

extern "C" __attribute__((visibility("default"))) void Vexa_Thunks_UseExternalEGLContext(int width, int height) {
  // VEXA_FIXES: Allow runtime-side EGL setup to hand off current context into
  // thunk-managed SDL_GL* flow when needed.
  if (CaptureCurrentEGLContext()) {
    CurrentEGL.Width = width > 0 ? width : 1;
    CurrentEGL.Height = height > 0 ? height : 1;
    RuntimeSurfaceRebindPending.store(true, std::memory_order_release);
    RuntimePbufferRetryTick.store(0, std::memory_order_release);
    SDL3_LOGI("Captured external EGL context %dx%d", width, height);
  } else {
    SDL3_LOGW("No current EGL context available to capture");
  }
}

extern "C" __attribute__((visibility("default"))) void SDL3_PumpGLQueue() {
}

extern "C" __attribute__((visibility("default"))) int SDL3Thunk_IsTextInputActive() {
  return TextInputActiveFlag.load() ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) void SDL3Thunk_InjectTouch(int action, int pointer_id, float x, float y, float pressure) {
  SDL_TouchFingerEvent touch {};
  touch.type = action == 0 ? SDL_EVENT_FINGER_DOWN :
               action == 1 ? SDL_EVENT_FINGER_UP :
                             SDL_EVENT_FINGER_MOTION;
  touch.timestamp = GetTimestampNS();
  touch.touchID = 1;
  touch.fingerID = pointer_id;
  touch.x = x;
  touch.y = y;
  touch.dx = 0.0f;
  touch.dy = 0.0f;
  touch.pressure = pressure;
  touch.windowID = 1;

  SDL_Event event {};
  event.tfinger = touch;
  PushEvent(event);
}

extern "C" __attribute__((visibility("default"))) void SDL3Thunk_InjectMouseMotion(float x, float y) {
  const float last_x = LastMouseX.exchange(x);
  const float last_y = LastMouseY.exchange(y);

  SDL_MouseMotionEvent motion {};
  motion.type = SDL_EVENT_MOUSE_MOTION;
  motion.timestamp = GetTimestampNS();
  motion.windowID = 1;
  motion.which = 0;
  motion.state = 0;
  motion.x = x;
  motion.y = y;
  motion.xrel = x - last_x;
  motion.yrel = y - last_y;

  SDL_Event event {};
  event.motion = motion;
  PushEvent(event);
}

extern "C" __attribute__((visibility("default"))) void SDL3Thunk_InjectMouseButton(int button, int down, float x, float y) {
  SDL_MouseButtonEvent mouse_button {};
  mouse_button.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
  mouse_button.timestamp = GetTimestampNS();
  mouse_button.windowID = 1;
  mouse_button.which = 0;
  mouse_button.button = button;
  mouse_button.down = down != 0;
  mouse_button.clicks = 1;
  mouse_button.x = x;
  mouse_button.y = y;

  SDL_Event event {};
  event.button = mouse_button;
  PushEvent(event);
}

extern "C" __attribute__((visibility("default"))) void SDL3Thunk_InjectText(const char* utf8) {
  SDL_TextInputEvent text {};
  text.type = SDL_EVENT_TEXT_INPUT;
  text.timestamp = GetTimestampNS();
  text.windowID = 1;
  text.text = nullptr;

  SDL_Event event {};
  event.text = text;
  PushEvent(event, utf8 ? utf8 : "");
}

extern "C" __attribute__((visibility("default"))) void SDL3Thunk_InjectKey(uint32_t scancode, uint32_t key, int down, int repeat) {
  SDL_KeyboardEvent keyboard {};
  keyboard.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  keyboard.timestamp = GetTimestampNS();
  keyboard.windowID = 1;
  keyboard.which = 0;
  keyboard.scancode = static_cast<SDL_Scancode>(scancode);
  keyboard.key = static_cast<SDL_Keycode>(key);
  keyboard.mod = SDL_KMOD_NONE;
  keyboard.raw = static_cast<Uint16>(scancode);
  keyboard.down = down != 0;
  keyboard.repeat = repeat != 0;

  SDL_Event event {};
  event.key = keyboard;
  PushEvent(event);
}

static int fexfn_impl_libSDL3_FEX_SDL_Init(SDL_InitFlags flags) {
  MarkProgress("SDL_Init", "enter");
  SDL3_LOGI("SDL_Init(flags=0x%x)", static_cast<unsigned>(flags));
  RuntimeSymbolLogBudget.store(128, std::memory_order_release);
  LastRuntimeSurfaceSerial.store(0, std::memory_order_release);
  RuntimeSurfaceRebindPending.store(true, std::memory_order_release);
  RuntimePbufferRetryTick.store(0, std::memory_order_release);
  LoggedEGLConfigVerification.store(false, std::memory_order_release);
  LoggedWindowSurfaceVerification.store(false, std::memory_order_release);
  LoggedFirstSwapVerification.store(false, std::memory_order_release);
  LoggedSwapStateVerification.store(false, std::memory_order_release);
  TextInputActiveFlag.store(false, std::memory_order_release);
  LastMouseX.store(0.0f, std::memory_order_release);
  LastMouseY.store(0.0f, std::memory_order_release);
  LastPolledText.clear();
  {
    std::scoped_lock lock {EventQueueMutex};
    EventQueue.clear();
  }
  if (ManagedEGLActive) {
    DestroyManagedEGLContext();
  }
  g_GLCreateOwnerTid = 0;
  SetSDLContextReadyState(false, "SDL_Init");
  SDLInitialized = true;
  StartWatchdogIfNeeded();
  MarkProgress("SDL_Init", "exit");
  return 1;
}

static void fexfn_impl_libSDL3_FEX_SDL_Quit() {
  MarkProgress("SDL_Quit", "enter");
  SDL3_LOGI("SDL_Quit()");
  g_GLCreateOwnerTid = 0;
  SetSDLContextReadyState(false, "SDL_Quit");
  DestroyManagedEGLContext();
  SDLInitialized = false;
  TextInputActiveFlag = false;

  std::scoped_lock lock {EventQueueMutex};
  EventQueue.clear();
  StopWatchdogIfNeeded();
  MarkProgress("SDL_Quit", "exit");
}

static SDL_Window* fexfn_impl_libSDL3_FEX_SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags) {
  SDL3_LOGI("SDL_CreateWindow(title=%s w=%d h=%d flags=0x%llx)",
            title ? title : "(null)",
            w,
            h,
            static_cast<unsigned long long>(flags));
  auto* window = new VexaSDL3Window {
    .Width = w,
    .Height = h,
    .Flags = flags,
    .WindowID = NextWindowID.fetch_add(1),
    .Title = title ? title : "",
  };
  SDL3_LOGI("SDL_CreateWindow => %p id=%u", window, window->WindowID);
  return reinterpret_cast<SDL_Window*>(window);
}

static void fexfn_impl_libSDL3_FEX_SDL_DestroyWindow(SDL_Window* window) {
  SDL3_LOGI("SDL_DestroyWindow(window=%p)", window);
  delete reinterpret_cast<VexaSDL3Window*>(window);
}

static int fexfn_impl_libSDL3_FEX_SDL_GetWindowSize(SDL_Window* window, int* w, int* h) {
  SDL3_LOGI("SDL_GetWindowSize(window=%p)", window);
  auto* host_window = reinterpret_cast<VexaSDL3Window*>(window);
  if (!host_window) {
    SDL3_LOGW("SDL_GetWindowSize failed: null window");
    return 0;
  }

  int runtime_width = 0;
  int runtime_height = 0;
  if (QueryRuntimeDrawableSize(&runtime_width, &runtime_height)) {
    host_window->Width = runtime_width;
    host_window->Height = runtime_height;
  }

  if (w) {
    *w = host_window->Width;
  }
  if (h) {
    *h = host_window->Height;
  }
  SDL3_LOGI("SDL_GetWindowSize => w=%d h=%d", host_window->Width, host_window->Height);
  return 1;
}

static SDL_GLContext fexfn_impl_libSDL3_FEX_SDL_GL_CreateContext(SDL_Window*) {
  LogEGLState("SDL_GL_CreateContext(enter)");
  VexaTraceThunkEvent("SDL_GL_CreateContext", VEXA_THUNK_EVENT_ENTER, kThunkSDLGLCreateContext,
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Display)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Surface)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Context)));
  if (!EnsureEGLContextReady()) {
    SetSDLContextReadyState(false, "SDL_GL_CreateContext");
    SDL3_LOGW("SDL_GL_CreateContext failed: no EGL context is available");
    VexaTraceThunkEvent("SDL_GL_CreateContext", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLCreateContext, 0, 0, 0, 0, 0, 0);
    return nullptr;
  }
  if (!IsManagedEGLContextReady()) {
    SDL3_LOGE("SDL_GL_CreateContext abort: managed context not ready after Ensure");
    SetSDLContextReadyState(false, "SDL_GL_CreateContext");
    return nullptr;
  }

  SetSDLContextReadyState(true, "SDL_GL_CreateContext");
  g_GLCreateOwnerTid = 0;
  SDL3_LOGI("SDL_GL_CreateContext created SDL3 thunk-managed EGL context %p",
            CurrentEGL.Context);
  LogEGLState("SDL_GL_CreateContext(exit)");
  VexaTraceThunkEvent("SDL_GL_CreateContext", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLCreateContext,
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Context)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Display)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Surface)),
                          static_cast<uint64_t>(g_GLCreateOwnerTid),
                          static_cast<uint64_t>(CurrentEGL.Width),
                          static_cast<uint64_t>(CurrentEGL.Height));
  return reinterpret_cast<SDL_GLContext>(CurrentEGL.Context);
}

static int fexfn_impl_libSDL3_FEX_SDL_GL_MakeCurrent(SDL_Window*, SDL_GLContext context) {
  LogEGLState("SDL_GL_MakeCurrent(enter)");
  VexaTraceThunkEvent("SDL_GL_MakeCurrent", VEXA_THUNK_EVENT_ENTER, kThunkSDLGLMakeCurrent,
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(context)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Display)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Surface)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Context)),
                          static_cast<uint64_t>(g_GLCreateOwnerTid));

  if (context == nullptr) {
    int result = 1;
    EGLint error = EGL_SUCCESS;
    if (ManagedEGLActive &&
        CurrentEGL.Display != EGL_NO_DISPLAY &&
        CurrentEGL.Context != EGL_NO_CONTEXT) {
      if (eglMakeCurrent(CurrentEGL.Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
        error = eglGetError();
        SDL3_LOGE("SDL_GL_MakeCurrent(NULL) failed: 0x%x", error);
        result = 0;
      }
    }

    if (result != 0) {
      SetSDLContextReadyState(false, "SDL_GL_MakeCurrent(null)");
      g_GLCreateOwnerTid = 0;
    }
    LogEGLState("SDL_GL_MakeCurrent(exit-null)");

    VexaTraceThunkEvent("SDL_GL_MakeCurrent", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLMakeCurrent,
                            static_cast<uint64_t>(result),
                            static_cast<uint64_t>(error),
                            static_cast<uint64_t>(g_GLCreateOwnerTid), 0, 0, 0);
    return result;
  }

  if (reinterpret_cast<EGLContext>(context) != CurrentEGL.Context) {
    SDL3_LOGW("SDL_GL_MakeCurrent rejected unknown context %p (current=%p)",
              context,
              CurrentEGL.Context);
    VexaTraceThunkEvent("SDL_GL_MakeCurrent", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLMakeCurrent,
                            0, 0, 0, 0, 0, 0);
    return 0;
  }

  if (!EnsureEGLContextReady()) {
    SetSDLContextReadyState(false, "SDL_GL_MakeCurrent");
    VexaTraceThunkEvent("SDL_GL_MakeCurrent", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLMakeCurrent,
                            0, 0, 0, 0, 0, 0);
    return 0;
  }
  if (!IsManagedEGLContextReady()) {
    SDL3_LOGW("SDL_GL_MakeCurrent abort: EGL context invalid after Ensure");
    SetSDLContextReadyState(false, "SDL_GL_MakeCurrent");
    VexaTraceThunkEvent("SDL_GL_MakeCurrent", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLMakeCurrent,
                        0, 0, 0, 0, 0, 0);
    return 0;
  }

  if (eglMakeCurrent(CurrentEGL.Display, CurrentEGL.Surface, CurrentEGL.Surface, CurrentEGL.Context) != EGL_TRUE) {
    const EGLint error = eglGetError();
    SDL3_LOGE("SDL_GL_MakeCurrent failed: 0x%x", error);
    SetSDLContextReadyState(false, "SDL_GL_MakeCurrent");
    VexaTraceThunkEvent("SDL_GL_MakeCurrent", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLMakeCurrent,
                            0,
                            static_cast<uint64_t>(error),
                            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Display)),
                            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Surface)),
                            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Context)),
                            0);
    return 0;
  }

  SetSDLContextReadyState(true, "SDL_GL_MakeCurrent");
  g_GLCreateOwnerTid = CurrentThreadToken();
  SDL3_LOGI("SDL_GL_MakeCurrent bound EGL context on owner=%lu", g_GLCreateOwnerTid);
  LogEGLState("SDL_GL_MakeCurrent(exit)");
  VexaTraceThunkEvent("SDL_GL_MakeCurrent", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLMakeCurrent,
                          1,
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Display)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Surface)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Context)),
                          static_cast<uint64_t>(g_GLCreateOwnerTid),
                          0);
  return 1;
}

static void fexfn_impl_libSDL3_FEX_SDL_GL_DeleteContext(SDL_GLContext) {
  LogEGLState("SDL_GL_DeleteContext(enter)");
  VexaTraceThunkEvent("SDL_GL_DeleteContext", VEXA_THUNK_EVENT_ENTER, kThunkSDLGLDeleteContext,
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Context)),
                          static_cast<uint64_t>(g_GLCreateOwnerTid));
  if (!ValidateGLOwnerThread("SDL_GL_DeleteContext")) {
    VexaTraceThunkEvent("SDL_GL_DeleteContext", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLDeleteContext, 0, 0, 0, 0, 0, 0);
    return;
  }

  SetSDLContextReadyState(false, "SDL_GL_DeleteContext");
  DestroyManagedEGLContext();
  g_GLCreateOwnerTid = 0;
  LogEGLState("SDL_GL_DeleteContext(exit)");
  VexaTraceThunkEvent("SDL_GL_DeleteContext", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLDeleteContext, 1, 0, 0, 0, 0, 0);
}

static int fexfn_impl_libSDL3_FEX_SDL_GL_SetSwapInterval(int interval) {
  LogEGLState("SDL_GL_SetSwapInterval(enter)");
  VexaTraceThunkEvent("SDL_GL_SetSwapInterval", VEXA_THUNK_EVENT_ENTER, kThunkSDLGLSetSwapInterval,
                          static_cast<uint64_t>(interval),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Display)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Surface)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Context)));
  if (!ValidateGLOwnerThread("SDL_GL_SetSwapInterval")) {
    VexaTraceThunkEvent("SDL_GL_SetSwapInterval", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLSetSwapInterval, 0, 0, 0, 0, 0, 0);
    return 0;
  }

  if (!EnsureEGLContextReady()) {
    VexaTraceThunkEvent("SDL_GL_SetSwapInterval", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLSetSwapInterval, 0, 0, 0, 0, 0, 0);
    return 0;
  }
  if (!IsManagedEGLContextReady()) {
    SDL3_LOGW("SDL_GL_SetSwapInterval abort: EGL context invalid");
    return 0;
  }

  const int result = eglSwapInterval(CurrentEGL.Display, interval) == EGL_TRUE ? 1 : 0;
  SDL3_LOGI("SDL_GL_SetSwapInterval interval=%d result=%d egl=0x%x", interval, result, eglGetError());
  VexaTraceThunkEvent("SDL_GL_SetSwapInterval", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLSetSwapInterval,
                          static_cast<uint64_t>(result),
                          static_cast<uint64_t>(eglGetError()),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Display)));
  return result;
}

static int fexfn_impl_libSDL3_FEX_SDL_GL_SwapWindow(SDL_Window*) {
  MarkProgress("SDL_GL_SwapWindow", "enter");
  LogEGLState("SDL_GL_SwapWindow(enter)");
  VexaTraceThunkEvent("SDL_GL_SwapWindow", VEXA_THUNK_EVENT_ENTER, kThunkSDLGLSwapWindow,
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Display)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Surface)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Context)),
                          static_cast<uint64_t>(g_GLCreateOwnerTid));
  if (!ValidateGLOwnerThread("SDL_GL_SwapWindow")) {
    MarkProgress("SDL_GL_SwapWindow", "owner-mismatch");
    VexaTraceThunkEvent("SDL_GL_SwapWindow", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLSwapWindow, 0, 0, 0, 0, 0, 0);
    return 0;
  }

  if (!EnsureEGLContextReady()) {
    MarkProgress("SDL_GL_SwapWindow", "egl-not-ready");
    VexaTraceThunkEvent("SDL_GL_SwapWindow", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLSwapWindow, 0, 0, 0, 0, 0, 0);
    return 0;
  }
  if (!IsManagedEGLContextReady()) {
    MarkProgress("SDL_GL_SwapWindow", "invalid-egl-state");
    SDL3_LOGW("SDL_GL_SwapWindow abort: EGL state invalid");
    VexaTraceThunkEvent("SDL_GL_SwapWindow", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLSwapWindow, 0, 0, 0, 0, 0, 0);
    return 0;
  }

  if (!MaybeHandoffRuntimeSurfaceBeforeSwap()) {
    MarkProgress("SDL_GL_SwapWindow", "surface-handoff-failed");
    VexaTraceThunkEvent("SDL_GL_SwapWindow", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLSwapWindow, 0, 0, 0, 0, 0, 0);
    return 0;
  }

  int result = 0;
  EGLint error = EGL_SUCCESS;
  EGLint swap_width = 0;
  EGLint swap_height = 0;
  if (CurrentEGL.Display != EGL_NO_DISPLAY && CurrentEGL.Surface != EGL_NO_SURFACE) {
    eglQuerySurface(CurrentEGL.Display, CurrentEGL.Surface, EGL_WIDTH, &swap_width);
    eglQuerySurface(CurrentEGL.Display, CurrentEGL.Surface, EGL_HEIGHT, &swap_height);
  }
  SDL3_LOGI("MAGENTA-PROBE pre-swap native_window=%p egl_surface_size=%dx%d logical=%dx%d",
            CurrentEGL.NativeWindow,
            static_cast<int>(swap_width),
            static_cast<int>(swap_height),
            CurrentEGL.Width,
            CurrentEGL.Height);
  SDL3_LOGI("Swap target native_window=%p size=%dx%d",
            CurrentEGL.NativeWindow,
            CurrentEGL.Width,
            CurrentEGL.Height);
  if (CurrentEGL.NativeWindow == nullptr || swap_width <= 1 || swap_height <= 1) {
    SDL3_LOGW("MAGENTA-PROBE likely offscreen/native-window-missing native_window=%p egl_surface_size=%dx%d",
              CurrentEGL.NativeWindow,
              static_cast<int>(swap_width),
              static_cast<int>(swap_height));
  }
  RunMagentaProbe();

  const uint64_t swap_begin_ns = GetTimestampNS();
  if (eglSwapBuffers(CurrentEGL.Display, CurrentEGL.Surface) == EGL_TRUE) {
    result = 1;
  } else {
    error = eglGetError();
    if (error == EGL_BAD_SURFACE) {
      SDL3_LOGW("SDL_GL_SwapWindow saw EGL_BAD_SURFACE; rebuilding surface");
      if (RebuildManagedSurfaceFromRuntimeWindow() &&
          eglSwapBuffers(CurrentEGL.Display, CurrentEGL.Surface) == EGL_TRUE) {
        result = 1;
        error = EGL_SUCCESS;
      } else if (result == 0) {
        error = eglGetError();
      }
    }
  }
  const uint64_t swap_end_ns = GetTimestampNS();
  LogSlowCall("eglSwapBuffers", swap_end_ns - swap_begin_ns);
  MarkProgress("SDL_GL_SwapWindow", "post-eglSwapBuffers");

  if (result == 1) {
    bool expected = false;
    if (LoggedFirstSwapVerification.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      EGLint post_w = 0;
      EGLint post_h = 0;
      if (CurrentEGL.Display != EGL_NO_DISPLAY && CurrentEGL.Surface != EGL_NO_SURFACE) {
        eglQuerySurface(CurrentEGL.Display, CurrentEGL.Surface, EGL_WIDTH, &post_w);
        eglQuerySurface(CurrentEGL.Display, CurrentEGL.Surface, EGL_HEIGHT, &post_h);
      }
      SDL3_LOGI("EGL-VERIFY first-swap-ok: native_window=%p egl_size=%dx%d",
                CurrentEGL.NativeWindow,
                static_cast<int>(post_w),
                static_cast<int>(post_h));
    }
  }

  SDL3_LOGI("SDL_GL_SwapWindow result=%d egl=0x%x", result, error);
  if (result == 0) {
    SetSDLContextReadyState(false, "SDL_GL_SwapWindow");
  }
  MarkProgress("SDL_GL_SwapWindow", "exit");
  VexaTraceThunkEvent("SDL_GL_SwapWindow", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLSwapWindow,
                          static_cast<uint64_t>(result),
                          static_cast<uint64_t>(error),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Display)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Surface)));
  return result;
}

static int fexfn_impl_libSDL3_FEX_SDL_PollEvent(void* event) {
  const uint64_t t0 = GetTimestampNS();
  MarkProgress("SDL_PollEvent", "enter");
  std::scoped_lock lock {EventQueueMutex};
  SDL3_LOGI("SDL_PollEvent(event=%p queue=%zu)", event, EventQueue.size());
  if (EventQueue.empty()) {
    MarkProgress("SDL_PollEvent", "empty");
    LogSlowCall("SDL_PollEvent", GetTimestampNS() - t0);
    return 0;
  }

  auto queued = std::move(EventQueue.front());
  EventQueue.pop_front();

  if (queued.Event.type == SDL_EVENT_TEXT_INPUT) {
    LastPolledText = std::move(queued.Text);
    queued.Event.text.text = LastPolledText.c_str();
  }

  if (event) {
    std::memcpy(event, &queued.Event, sizeof(SDL_Event));
  }
  SDL3_LOGI("SDL_PollEvent => type=0x%x remaining=%zu",
            static_cast<unsigned>(queued.Event.type),
            EventQueue.size());
  MarkProgress("SDL_PollEvent", "event");
  LogSlowCall("SDL_PollEvent", GetTimestampNS() - t0);
  return 1;
}

static Uint64 fexfn_impl_libSDL3_FEX_SDL_GetTicks() {
  return GetTimestampNS() / 1000000ULL;
}

static void fexfn_impl_libSDL3_FEX_SDL_Delay(Uint32 ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static int fexfn_impl_libSDL3_FEX_SDL_StartTextInput(SDL_Window*) {
  TextInputActiveFlag = true;
  return 1;
}

static int fexfn_impl_libSDL3_FEX_SDL_StopTextInput(SDL_Window*) {
  TextInputActiveFlag = false;
  return 1;
}

static int fexfn_impl_libSDL3_FEX_SDL_TextInputActive(SDL_Window*) {
  return TextInputActiveFlag.load() ? 1 : 0;
}

static SDL_Surface* fexfn_impl_libSDL3_FEX_SDL_CreateSurface(int width, int height, SDL_PixelFormat format) {
  using CreateSurfaceFn = SDL_Surface* (*)(int, int, SDL_PixelFormat);
  static CreateSurfaceFn fn = nullptr;
  static bool initialized = false;
  if (!initialized) {
    initialized = true;
    fn = reinterpret_cast<CreateSurfaceFn>(ResolveRuntimeSymbol("SDL_CreateSurface"));
  }
  if (!fn) {
    SDL3_LOGE("SDL_CreateSurface bridge missing runtime symbol");
    return nullptr;
  }
  return fn(width, height, format);
}

static SDL_Surface* fexfn_impl_libSDL3_FEX_SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void* pixels, int pitch) {
  using CreateSurfaceFromFn = SDL_Surface* (*)(int, int, SDL_PixelFormat, void*, int);
  static CreateSurfaceFromFn fn = nullptr;
  static bool initialized = false;
  if (!initialized) {
    initialized = true;
    fn = reinterpret_cast<CreateSurfaceFromFn>(ResolveRuntimeSymbol("SDL_CreateSurfaceFrom"));
  }
  if (!fn) {
    SDL3_LOGE("SDL_CreateSurfaceFrom bridge missing runtime symbol");
    return nullptr;
  }
  return fn(width, height, format, pixels, pitch);
}

static void fexfn_impl_libSDL3_FEX_SDL_DestroySurface(SDL_Surface* surface) {
  using DestroySurfaceFn = void (*)(SDL_Surface*);
  static DestroySurfaceFn fn = nullptr;
  static bool initialized = false;
  if (!initialized) {
    initialized = true;
    fn = reinterpret_cast<DestroySurfaceFn>(ResolveRuntimeSymbol("SDL_DestroySurface"));
  }
  if (!fn) {
    SDL3_LOGE("SDL_DestroySurface bridge missing runtime symbol");
    return;
  }
  fn(surface);
}

static SDL_Surface* fexfn_impl_libSDL3_FEX_SDL_ScaleSurface(SDL_Surface* surface, int width, int height, SDL_ScaleMode scaleMode) {
  using ScaleSurfaceFn = SDL_Surface* (*)(SDL_Surface*, int, int, SDL_ScaleMode);
  static ScaleSurfaceFn fn = nullptr;
  static bool initialized = false;
  if (!initialized) {
    initialized = true;
    fn = reinterpret_cast<ScaleSurfaceFn>(ResolveRuntimeSymbol("SDL_ScaleSurface"));
  }
  if (!fn) {
    SDL3_LOGE("SDL_ScaleSurface bridge missing runtime symbol");
    return nullptr;
  }
  return fn(surface, width, height, scaleMode);
}


// VEXA_FIXES: Avoid host SDL cursor APIs on Android runtime worker threads.
// These paths can touch JNI/thread state from non-Java threads and trigger ART
// stack invariants. Provide a synthetic cursor model instead.
namespace {
constexpr uintptr_t kSyntheticCursorToken = 0x1;
std::atomic<uintptr_t> CurrentSyntheticCursor {kSyntheticCursorToken};
std::atomic<bool> SyntheticCursorVisible {true};

SDL_Cursor* GetSyntheticCursorHandle() {
  return reinterpret_cast<SDL_Cursor*>(kSyntheticCursorToken);
}

void LogSyntheticCursorBridgeOnce() {
  static std::atomic<bool> logged {false};
  if (!logged.exchange(true, std::memory_order_acq_rel)) {
    SDL3_LOGW("Cursor bridge using synthetic mode (host SDL cursor calls bypassed)");
  }
}
} // namespace

static SDL_Cursor* fexfn_impl_libSDL3_FEX_SDL_CreateColorCursor(SDL_Surface* surface, int hot_x, int hot_y) {
  (void)surface;
  (void)hot_x;
  (void)hot_y;
  LogSyntheticCursorBridgeOnce();
  return GetSyntheticCursorHandle();
}

static SDL_Cursor* fexfn_impl_libSDL3_FEX_SDL_CreateSystemCursor(SDL_SystemCursor id) {
  (void)id;
  LogSyntheticCursorBridgeOnce();
  return GetSyntheticCursorHandle();
}

static bool fexfn_impl_libSDL3_FEX_SDL_SetCursor(SDL_Cursor* cursor) {
  LogSyntheticCursorBridgeOnce();
  uintptr_t token = reinterpret_cast<uintptr_t>(cursor);
  if (token == 0) {
    token = kSyntheticCursorToken;
  }
  CurrentSyntheticCursor.store(token, std::memory_order_release);
  return true;
}

static SDL_Cursor* fexfn_impl_libSDL3_FEX_SDL_GetCursor() {
  LogSyntheticCursorBridgeOnce();
  uintptr_t token = CurrentSyntheticCursor.load(std::memory_order_acquire);
  if (token == 0) {
    token = kSyntheticCursorToken;
  }
  return reinterpret_cast<SDL_Cursor*>(token);
}

static SDL_Cursor* fexfn_impl_libSDL3_FEX_SDL_GetDefaultCursor() {
  LogSyntheticCursorBridgeOnce();
  return GetSyntheticCursorHandle();
}

static void fexfn_impl_libSDL3_FEX_SDL_DestroyCursor(SDL_Cursor* cursor) {
  LogSyntheticCursorBridgeOnce();
  const uintptr_t token = reinterpret_cast<uintptr_t>(cursor);
  if (token != 0 && token == CurrentSyntheticCursor.load(std::memory_order_acquire)) {
    CurrentSyntheticCursor.store(kSyntheticCursorToken, std::memory_order_release);
  }
}

static bool fexfn_impl_libSDL3_FEX_SDL_ShowCursor() {
  LogSyntheticCursorBridgeOnce();
  SyntheticCursorVisible.store(true, std::memory_order_release);
  return true;
}

static bool fexfn_impl_libSDL3_FEX_SDL_HideCursor() {
  LogSyntheticCursorBridgeOnce();
  SyntheticCursorVisible.store(false, std::memory_order_release);
  return true;
}

static bool fexfn_impl_libSDL3_FEX_SDL_CursorVisible() {
  LogSyntheticCursorBridgeOnce();
  return SyntheticCursorVisible.load(std::memory_order_acquire);
}
// VEXA_FIXES: Export explicit FEX_SDL_* bridge symbols so fexldr dlsym() succeeds
#define FEX_SDL_EXPORT_RET(ret, name, args, callargs) \
  extern "C" __attribute__((visibility("default"), used)) ret FEX_SDL_##name args { \
    return fexfn_impl_libSDL3_FEX_SDL_##name callargs; \
  }

#define FEX_SDL_EXPORT_VOID(name, args, callargs) \
  extern "C" __attribute__((visibility("default"), used)) void FEX_SDL_##name args { \
    fexfn_impl_libSDL3_FEX_SDL_##name callargs; \
  }

FEX_SDL_EXPORT_RET(int, Init, (SDL_InitFlags flags), (flags));
FEX_SDL_EXPORT_VOID(Quit, (), ());
FEX_SDL_EXPORT_RET(SDL_Window*, CreateWindow, (const char* title, int w, int h, SDL_WindowFlags flags), (title, w, h, flags));
FEX_SDL_EXPORT_VOID(DestroyWindow, (SDL_Window* window), (window));
FEX_SDL_EXPORT_RET(int, GetWindowSize, (SDL_Window* window, int* w, int* h), (window, w, h));

FEX_SDL_EXPORT_RET(SDL_GLContext, GL_CreateContext, (SDL_Window* window), (window));
FEX_SDL_EXPORT_RET(int, GL_MakeCurrent, (SDL_Window* window, SDL_GLContext context), (window, context));
FEX_SDL_EXPORT_VOID(GL_DeleteContext, (SDL_GLContext context), (context));
FEX_SDL_EXPORT_RET(int, GL_SetSwapInterval, (int interval), (interval));
FEX_SDL_EXPORT_RET(int, GL_SwapWindow, (SDL_Window* window), (window));

FEX_SDL_EXPORT_RET(int, PollEvent, (void* event), (event));
FEX_SDL_EXPORT_RET(Uint64, GetTicks, (), ());
FEX_SDL_EXPORT_VOID(Delay, (Uint32 ms), (ms));
FEX_SDL_EXPORT_RET(int, StartTextInput, (SDL_Window* window), (window));
FEX_SDL_EXPORT_RET(int, StopTextInput, (SDL_Window* window), (window));
FEX_SDL_EXPORT_RET(int, TextInputActive, (SDL_Window* window), (window));

FEX_SDL_EXPORT_RET(SDL_Surface*, CreateSurface, (int width, int height, SDL_PixelFormat format), (width, height, format));
FEX_SDL_EXPORT_RET(SDL_Surface*, CreateSurfaceFrom, (int width, int height, SDL_PixelFormat format, void* pixels, int pitch), (width, height, format, pixels, pitch));
FEX_SDL_EXPORT_VOID(DestroySurface, (SDL_Surface* surface), (surface));
FEX_SDL_EXPORT_RET(SDL_Surface*, ScaleSurface, (SDL_Surface* surface, int width, int height, SDL_ScaleMode scaleMode), (surface, width, height, scaleMode));

FEX_SDL_EXPORT_RET(SDL_Cursor*, CreateColorCursor, (SDL_Surface* surface, int hot_x, int hot_y), (surface, hot_x, hot_y));
FEX_SDL_EXPORT_RET(SDL_Cursor*, CreateSystemCursor, (SDL_SystemCursor id), (id));
FEX_SDL_EXPORT_RET(bool, SetCursor, (SDL_Cursor* cursor), (cursor));
FEX_SDL_EXPORT_RET(SDL_Cursor*, GetCursor, (), ());
FEX_SDL_EXPORT_RET(SDL_Cursor*, GetDefaultCursor, (), ());
FEX_SDL_EXPORT_VOID(DestroyCursor, (SDL_Cursor* cursor), (cursor));
FEX_SDL_EXPORT_RET(bool, ShowCursor, (), ());
FEX_SDL_EXPORT_RET(bool, HideCursor, (), ());
FEX_SDL_EXPORT_RET(bool, CursorVisible, (), ());

#undef FEX_SDL_EXPORT_VOID
#undef FEX_SDL_EXPORT_RET
EXPORTS(libSDL3)
