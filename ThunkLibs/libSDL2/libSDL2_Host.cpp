/*
$info$
tags: thunklibs|SDL2
desc: Android SDL2 host thunk for window/event/GL context bridging
$end_info$
*/

#include <SDL.h>

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
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "common/Host.h"

#if defined(BUILD_ANDROID)
static int VexaSDL2HostFprintf(FILE* stream, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  const int rc = std::vfprintf(stream, fmt, args);
  va_end(args);
  if (stream == stderr) {
#if VEXA_HAS_ANDROID_LOG
    __android_log_vprint(ANDROID_LOG_ERROR, "Vexa-SDL2Thunk", fmt, copy);
#endif
  }
  va_end(copy);
  return rc;
}
#define fprintf(stream, ...) VexaSDL2HostFprintf(stream, __VA_ARGS__)
#endif

#include "thunkgen_host_libSDL2.inl"

namespace {
// VEXA_FIXES: Keep SDL2 thunk diagnostics tagged distinctly in logcat.
constexpr const char* LOG_TAG = "Vexa-SDL2Thunk";
// VEXA_FIXES: Persist runtime marker in app-internal artifacts storage.
constexpr const char* SDLContextReadyFlagPath = "/data/user/0/com.critical.vexaemulator/files/artifacts/sdl_context_ready.flag";
constexpr uint64_t kThunkSDLGLCreateContext = 0x53444C320001ULL;
constexpr uint64_t kThunkSDLGLMakeCurrent = 0x53444C320002ULL;
constexpr uint64_t kThunkSDLGLDeleteContext = 0x53444C320003ULL;
constexpr uint64_t kThunkSDLGLSetSwapInterval = 0x53444C320004ULL;
constexpr uint64_t kThunkSDLGLSwapWindow = 0x53444C320005ULL;
using RuntimeNativeWindowGetter = ANativeWindow* (*)();
using RuntimeSurfaceSerialGetter = uint64_t (*)();

static bool SDL2VerboseLogging() {
  static const bool verbose = [] {
    const char* v = getenv("VEXA_SDL2_TRACE");
    return v && v[0] && v[0] != '0';
  }();
  return verbose;
}
#define SDL2_LOGI(...) do { if (SDL2VerboseLogging()) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__); } while (0)
#define SDL2_LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define SDL2_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif

struct VexaSDL2Window {
  int X {};
  int Y {};
  int Width {};
  int Height {};
  Uint32 Flags {};
  Uint32 WindowID {};
  std::string Title;
};

struct QueuedEvent {
  SDL_Event Event {};
};

std::mutex EventQueueMutex;
std::deque<QueuedEvent> EventQueue;
std::atomic<bool> TextInputActiveFlag {false};
std::atomic<bool> SDLInitialized {false};
std::atomic<Uint32> NextWindowID {1};
std::atomic<float> LastMouseX {0.0f};
std::atomic<float> LastMouseY {0.0f};

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
// VEXA_FIXES: The context handle created by TryCreateManagedEGLContext. Capture
// paths may overwrite CurrentEGL.* with a foreign thread-current context (e.g.
// the libGL thunk's GLES context), so the swap path needs to tell them apart.
EGLContext ManagedContextHandle = EGL_NO_CONTEXT;
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
  SDL2_LOGI("%s: managed=%d display=%p surface=%p context=%p size=%dx%d owner=%lu",
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
    SDL2_LOGI("MAGENTA-PROBE resolve clearColor=%p clear=%p flush=%p bindFB=%p viewport=%p disable=%p colorMask=%p getIntegerv=%p",
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
    SDL2_LOGI("MAGENTA-PROBE mode=%s", force_magenta ? "forced" : "diagnostic-only");
  }

  if (force_magenta) {
    if (!p_glClearColor || !p_glClear) {
      SDL2_LOGE("MAGENTA-PROBE missing GL clear entrypoints");
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
      SDL2_LOGI("PB-PROBE rgba=(%u,%u,%u,%u) size=%dx%d",
                static_cast<unsigned>(pixel[0]),
                static_cast<unsigned>(pixel[1]),
                static_cast<unsigned>(pixel[2]),
                static_cast<unsigned>(pixel[3]),
                CurrentEGL.Width,
                CurrentEGL.Height);
    } else {
      SDL2_LOGW("PB-PROBE glReadPixels unresolved");
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

      SDL2_LOGI("EGL-VERIFY pre-swap-state draw_fbo=%d read_fbo=%d current_program=%d readpix_ok=%d rgba=(%u,%u,%u,%u)",
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

// VEXA_FIXES: SDL2 event timestamps are milliseconds (Uint32), not nanoseconds.
Uint32 GetEventTimestampMS() {
  return static_cast<Uint32>(GetTimestampNS() / 1000000ULL);
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
    SDL2_LOGW("stall-warn call=%s duration_ms=%llu",
              call ? call : "<null>",
              static_cast<unsigned long long>(elapsed_ns / 1000000ULL));
  } else if (elapsed_ns >= kStallInfoThresholdNs) {
    SDL2_LOGI("stall-info call=%s duration_ms=%llu",
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
    SDL2_LOGW("watchdog time_since_progress_ms=%llu last_sdl_call=%s last_progress_marker=%s gl_last_call=%s gl_last_requested_proc=%s gl_since_ms=%llu gl_serial=%llu",
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

  SDL2_LOGI("runtime-symbol phase=%s symbol=%s lib=%s addr=%p",
            phase ? phase : "unknown",
            symbol_name ? symbol_name : "<null>",
            library_name ? library_name : "<none>",
            address);
}

void* ResolveRuntimeSymbol(const char* name) {
  if (!name || !*name) {
    SDL2_LOGW("ResolveRuntimeSymbol called with empty symbol name");
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
      SDL2_LOGW("Could not open Vexa runtime libraries while resolving symbols");
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
    SDL2_LOGW("%s: g_Vexa_SDLContextCreated is unavailable", source);
  }

  LastContextReadyState.store(ready, std::memory_order_release);

  if (ready) {
    FILE* marker = std::fopen(SDLContextReadyFlagPath, "w");
    if (!marker) {
      SDL2_LOGW("%s: failed to write context marker %s", source, SDLContextReadyFlagPath);
      return;
    }

    std::fputs("ready\n", marker);
    std::fclose(marker);
    return;
  }

  if (std::remove(SDLContextReadyFlagPath) != 0 && errno != ENOENT) {
    SDL2_LOGW("%s: failed to remove context marker %s errno=%d", source, SDLContextReadyFlagPath, errno);
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
      SDL2_LOGW("Vexa_GetRuntimeNativeWindowRetained returned null window");
      return nullptr;
    }

    const int window_width = ANativeWindow_getWidth(window);
    const int window_height = ANativeWindow_getHeight(window);
    if (window_width <= 1 || window_height <= 1) {
      SDL2_LOGW("Retained native window not ready yet size=%dx%d; keep pbuffer",
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
      SDL2_LOGW("Vexa_GetRuntimeNativeWindow returned null window");
      return nullptr;
    }

    ANativeWindow_acquire(window);

    const int window_width = ANativeWindow_getWidth(window);
    const int window_height = ANativeWindow_getHeight(window);
    if (window_width <= 1 || window_height <= 1) {
      SDL2_LOGW("Vexa_GetRuntimeNativeWindow not ready yet size=%dx%d; keep pbuffer",
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
      SDL2_LOGW("Runtime native-window symbols are unavailable (getter/window_slot/ready)");
    }
    return nullptr;
  }

  if (!*window_slot || *ready == 0) {
    SDL2_LOGW("Fallback native-window globals report not-ready (window=%p ready=%d)",
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
    SDL2_LOGW("Fallback native-window globals not ready size=%dx%d; keep pbuffer",
              native_width ? *native_width : 0,
              native_height ? *native_height : 0);
    ANativeWindow_release(*window_slot);
    return nullptr;
  }
  SDL2_LOGI("Using fallback native-window globals window=%p size=%dx%d",
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
      SDL2_LOGW("eglMakeCurrent(detach) failed in DestroyManagedEGLContext: 0x%x", eglGetError());
    }
    if (CurrentEGL.Context != EGL_NO_CONTEXT) {
      if (eglDestroyContext(display, CurrentEGL.Context) != EGL_TRUE) {
        SDL2_LOGW("eglDestroyContext failed: 0x%x", eglGetError());
      }
    }
    if (CurrentEGL.Surface != EGL_NO_SURFACE) {
      if (eglDestroySurface(display, CurrentEGL.Surface) != EGL_TRUE) {
        SDL2_LOGW("eglDestroySurface failed: 0x%x", eglGetError());
      }
      CurrentEGL.Surface = EGL_NO_SURFACE;
    }

    if (eglTerminate(display) != EGL_TRUE) {
      SDL2_LOGW("eglTerminate failed: 0x%x", eglGetError());
    }
  }

  if (CurrentEGL.NativeWindow) {
    ANativeWindow_release(CurrentEGL.NativeWindow);
    CurrentEGL.NativeWindow = nullptr;
  }

  ManagedEGLActive = false;
  ManagedContextHandle = EGL_NO_CONTEXT;
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
      SDL2_LOGI("Runtime window became ready during wait: %p size=%dx%d", window, w, h);
      return window;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  return nullptr;
}

void PushEvent(const SDL_Event& event) {
  std::scoped_lock lock {EventQueueMutex};
  EventQueue.push_back(QueuedEvent {
    .Event = event,
  });
}

void PushWindowEvent(Uint8 window_event, Sint32 data1, Sint32 data2, Uint32 window_id) {
  SDL_Event event {};
  event.type = SDL_WINDOWEVENT;
  event.window.type = SDL_WINDOWEVENT;
  event.window.timestamp = GetEventTimestampMS();
  event.window.windowID = window_id;
  event.window.event = window_event;
  event.window.data1 = data1;
  event.window.data2 = data2;
  PushEvent(event);
}

bool RebuildManagedSurfaceFromRuntimeWindow() {
  if (!CurrentEGL.Display || CurrentEGL.Context == EGL_NO_CONTEXT || !CurrentEGL.Config) {
    SDL2_LOGE("RebuildManagedSurfaceFromRuntimeWindow skipped: missing display/context/config");
    return false;
  }

  int native_width = 0;
  int native_height = 0;
  ANativeWindow* runtime_window = ResolveNativeWindowFromRuntime(&native_width, &native_height);
  if (!runtime_window) {
    runtime_window = WaitForRuntimeWindowReady(&native_width, &native_height, 3000);
  }
  if (!runtime_window) {
    SDL2_LOGE("Strict window-surface mode: runtime window unavailable");
    return false;
  }
  SDL2_LOGI("RebuildManagedSurfaceFromRuntimeWindow: runtime_window=%p size=%dx%d",
            runtime_window,
            native_width,
            native_height);

  if (eglMakeCurrent(CurrentEGL.Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
    SDL2_LOGW("eglMakeCurrent(detach) failed before surface rebuild: 0x%x", eglGetError());
  }

  if (CurrentEGL.Surface != EGL_NO_SURFACE) {
    if (eglDestroySurface(CurrentEGL.Display, CurrentEGL.Surface) != EGL_TRUE) {
      SDL2_LOGW("eglDestroySurface (rebuild) failed: 0x%x", eglGetError());
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
      SDL2_LOGW("ANativeWindow_setBuffersGeometry failed: format=%d",
                native_visual_id);
    }
  }

  EGLSurface new_surface = eglCreateWindowSurface(CurrentEGL.Display, CurrentEGL.Config, runtime_window, nullptr);
  if (new_surface == EGL_NO_SURFACE) {
    const EGLint error = eglGetError();
    SDL2_LOGE("Strict window-surface mode: eglCreateWindowSurface failed: 0x%x", error);
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
    SDL2_LOGE("eglMakeCurrent after surface rebuild failed: 0x%x", error);

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
    SDL2_LOGI("EGL-VERIFY window-surface bound: native_window=%p render_buffer=0x%x egl_size=%dx%d logical=%dx%d",
              CurrentEGL.NativeWindow,
              render_buffer,
              static_cast<int>(query_width),
              static_cast<int>(query_height),
              CurrentEGL.Width,
              CurrentEGL.Height);
  }

  // VEXA_FIXES: Notify the guest that the drawable was (re)bound; the size may
  // have changed alongside the runtime surface handoff. Primary window ID is 1.
  PushWindowEvent(SDL_WINDOWEVENT_SIZE_CHANGED, final_width, final_height, 1);

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
    SDL2_LOGW("Still on pbuffer-like surface; retrying runtime window handoff (serial=%llu)",
              static_cast<unsigned long long>(serial));
  } else {
    SDL2_LOGI("Runtime surface serial changed %llu -> %llu; rebuilding EGL surface",
              static_cast<unsigned long long>(last),
              static_cast<unsigned long long>(serial));
  }

  if (!RebuildManagedSurfaceFromRuntimeWindow()) {
    SDL2_LOGE("Runtime surface handoff rebuild failed (strict mode)");
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
    SDL2_LOGI("EGL-VERIFY config selected: renderable=0x%x surface=0x%x window_bit=%d pbuffer_bit=%d visual=%d depth=%d stencil=%d",
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
    SDL2_LOGE("eglGetDisplay failed: 0x%x", eglGetError());
    return false;
  }

  EGLint major = 0;
  EGLint minor = 0;
  if (eglInitialize(display, &major, &minor) != EGL_TRUE) {
    SDL2_LOGE("eglInitialize failed: 0x%x", eglGetError());
    return false;
  }

  if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
    SDL2_LOGE("eglBindAPI(EGL_OPENGL_ES_API) failed: 0x%x", eglGetError());
    return false;
  }

  EGLConfig config = nullptr;
  int client_version = 3;
  if (!SelectDeterministicEGLConfig(display, &config, client_version)) {
    client_version = 2;
    if (!SelectDeterministicEGLConfig(display, &config, client_version)) {
      SDL2_LOGE("eglChooseConfig failed: 0x%x", eglGetError());
      return false;
    }
  }

  const EGLint context_attributes[] = {
    EGL_CONTEXT_CLIENT_VERSION, client_version,
    EGL_NONE,
  };
  EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
  if (context == EGL_NO_CONTEXT) {
    SDL2_LOGE("eglCreateContext failed: 0x%x", eglGetError());
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
  ManagedContextHandle = context;
  RuntimeSurfaceRebindPending.store(true, std::memory_order_release);
  RuntimePbufferRetryTick.store(0, std::memory_order_release);
  LoggedWindowSurfaceVerification.store(false, std::memory_order_release);
  LoggedFirstSwapVerification.store(false, std::memory_order_release);

  if (!RebuildManagedSurfaceFromRuntimeWindow()) {
    SDL2_LOGE("Managed EGL creation failed while building initial surface");
    DestroyManagedEGLContext();
    return false;
  }

  SDL2_LOGI("Managed EGL context created (%dx%d, EGL %d.%d)",
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
    static int captured_log_count = 0;
    if (captured_log_count++ < 8) {
      SDL2_LOGW("EnsureEGLContextReady: captured foreign context (managed=%d config=%p display=%p surface=%p context=%p)",
                ManagedEGLActive ? 1 : 0,
                reinterpret_cast<void*>(CurrentEGL.Config),
                reinterpret_cast<void*>(CurrentEGL.Display),
                reinterpret_cast<void*>(CurrentEGL.Surface),
                reinterpret_cast<void*>(CurrentEGL.Context));
    }
    LogEGLState("EnsureEGLContextReady(captured)");
    return true;
  }

  SDL2_LOGW("No current EGL context; falling back to managed EGL context path");

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

    SDL2_LOGW("Managed EGL context became invalid; recreating");
    DestroyManagedEGLContext();
  }

  return TryCreateManagedEGLContext();
}

// ---------------------------------------------------------------------------
// VEXA_FIXES: Host-side software surface implementation.
//
// There is no real libSDL2.so on Android: SDL_Surface is a public struct in
// SDL_surface.h, so the thunk allocates surfaces, pixel formats, palettes and
// pixel buffers on the host heap and implements the blit path in-file.
// Semantics mirror third_party/SDL2/src/video/SDL_surface.c / SDL_pixels.c.
// ---------------------------------------------------------------------------

// Per-surface blit state stored through the opaque SDL_Surface::map pointer.
// Real SDL2 stores blend flags in map->info.flags; only blend mode is modeled.
struct VexaBlitMap {
  SDL_BlendMode BlendMode = SDL_BLENDMODE_NONE;
};

VexaBlitMap* GetBlitMap(SDL_Surface* surface) {
  return reinterpret_cast<VexaBlitMap*>(surface->map);
}

const VexaBlitMap* GetBlitMap(const SDL_Surface* surface) {
  return reinterpret_cast<const VexaBlitMap*>(surface->map);
}

// Port of SDL_MasksToPixelFormatEnum() from src/video/SDL_pixels.c.
Uint32 VexaMasksToPixelFormatEnum(int bpp, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {
  switch (bpp) {
  case 1:
    /* SDL defaults to MSB ordering */
    return SDL_PIXELFORMAT_INDEX1MSB;
  case 2:
    return SDL_PIXELFORMAT_INDEX2MSB;
  case 4:
    return SDL_PIXELFORMAT_INDEX4MSB;
  case 8:
    if (Rmask == 0) {
      return SDL_PIXELFORMAT_INDEX8;
    }
    if (Rmask == 0xE0 && Gmask == 0x1C && Bmask == 0x03 && Amask == 0x00) {
      return SDL_PIXELFORMAT_RGB332;
    }
    break;
  case 12:
    if (Rmask == 0) {
      return SDL_PIXELFORMAT_RGB444;
    }
    if (Rmask == 0x0F00 && Gmask == 0x00F0 && Bmask == 0x000F && Amask == 0x0000) {
      return SDL_PIXELFORMAT_RGB444;
    }
    if (Rmask == 0x000F && Gmask == 0x00F0 && Bmask == 0x0F00 && Amask == 0x0000) {
      return SDL_PIXELFORMAT_BGR444;
    }
    break;
  case 15:
    if (Rmask == 0) {
      return SDL_PIXELFORMAT_RGB555;
    }
    /* fallthrough */
  case 16:
    if (Rmask == 0) {
      return SDL_PIXELFORMAT_RGB565;
    }
    if (Rmask == 0x7C00 && Gmask == 0x03E0 && Bmask == 0x001F && Amask == 0x0000) {
      return SDL_PIXELFORMAT_RGB555;
    }
    if (Rmask == 0x001F && Gmask == 0x03E0 && Bmask == 0x7C00 && Amask == 0x0000) {
      return SDL_PIXELFORMAT_BGR555;
    }
    if (Rmask == 0x0F00 && Gmask == 0x00F0 && Bmask == 0x000F && Amask == 0xF000) {
      return SDL_PIXELFORMAT_ARGB4444;
    }
    if (Rmask == 0xF000 && Gmask == 0x0F00 && Bmask == 0x00F0 && Amask == 0x000F) {
      return SDL_PIXELFORMAT_RGBA4444;
    }
    if (Rmask == 0x000F && Gmask == 0x00F0 && Bmask == 0x0F00 && Amask == 0xF000) {
      return SDL_PIXELFORMAT_ABGR4444;
    }
    if (Rmask == 0x00F0 && Gmask == 0x0F00 && Bmask == 0xF000 && Amask == 0x000F) {
      return SDL_PIXELFORMAT_BGRA4444;
    }
    if (Rmask == 0x7C00 && Gmask == 0x03E0 && Bmask == 0x001F && Amask == 0x8000) {
      return SDL_PIXELFORMAT_ARGB1555;
    }
    if (Rmask == 0xF800 && Gmask == 0x07C0 && Bmask == 0x003E && Amask == 0x0001) {
      return SDL_PIXELFORMAT_RGBA5551;
    }
    if (Rmask == 0x001F && Gmask == 0x03E0 && Bmask == 0x7C00 && Amask == 0x8000) {
      return SDL_PIXELFORMAT_ABGR1555;
    }
    if (Rmask == 0x003E && Gmask == 0x07C0 && Bmask == 0xF800 && Amask == 0x0001) {
      return SDL_PIXELFORMAT_BGRA5551;
    }
    if (Rmask == 0xF800 && Gmask == 0x07E0 && Bmask == 0x001F && Amask == 0x0000) {
      return SDL_PIXELFORMAT_RGB565;
    }
    if (Rmask == 0x001F && Gmask == 0x07E0 && Bmask == 0xF800 && Amask == 0x0000) {
      return SDL_PIXELFORMAT_BGR565;
    }
    if (Rmask == 0x003F && Gmask == 0x07C0 && Bmask == 0xF800 && Amask == 0x0000) {
      /* Technically this would be BGR556, but SDL maps this to RGB565 (bug 3158) */
      return SDL_PIXELFORMAT_RGB565;
    }
    break;
  case 24:
    switch (Rmask) {
    case 0:
    case 0x00FF0000:
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
      return SDL_PIXELFORMAT_RGB24;
#else
      return SDL_PIXELFORMAT_BGR24;
#endif
    case 0x000000FF:
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
      return SDL_PIXELFORMAT_BGR24;
#else
      return SDL_PIXELFORMAT_RGB24;
#endif
    default:
      break;
    }
    break;
  case 32:
    if (Rmask == 0) {
      return SDL_PIXELFORMAT_RGB888;
    }
    if (Rmask == 0x00FF0000 && Gmask == 0x0000FF00 && Bmask == 0x000000FF && Amask == 0x00000000) {
      return SDL_PIXELFORMAT_RGB888;
    }
    if (Rmask == 0xFF000000 && Gmask == 0x00FF0000 && Bmask == 0x0000FF00 && Amask == 0x00000000) {
      return SDL_PIXELFORMAT_RGBX8888;
    }
    if (Rmask == 0x000000FF && Gmask == 0x0000FF00 && Bmask == 0x00FF0000 && Amask == 0x00000000) {
      return SDL_PIXELFORMAT_BGR888;
    }
    if (Rmask == 0x0000FF00 && Gmask == 0x00FF0000 && Bmask == 0xFF000000 && Amask == 0x00000000) {
      return SDL_PIXELFORMAT_BGRX8888;
    }
    if (Rmask == 0x00FF0000 && Gmask == 0x0000FF00 && Bmask == 0x000000FF && Amask == 0xFF000000) {
      return SDL_PIXELFORMAT_ARGB8888;
    }
    if (Rmask == 0xFF000000 && Gmask == 0x00FF0000 && Bmask == 0x0000FF00 && Amask == 0x000000FF) {
      return SDL_PIXELFORMAT_RGBA8888;
    }
    if (Rmask == 0x000000FF && Gmask == 0x0000FF00 && Bmask == 0x00FF0000 && Amask == 0xFF000000) {
      return SDL_PIXELFORMAT_ABGR8888;
    }
    if (Rmask == 0x0000FF00 && Gmask == 0x00FF0000 && Bmask == 0xFF000000 && Amask == 0x000000FF) {
      return SDL_PIXELFORMAT_BGRA8888;
    }
    if (Rmask == 0x3FF00000 && Gmask == 0x000FFC00 && Bmask == 0x000003FF && Amask == 0xC0000000) {
      return SDL_PIXELFORMAT_ARGB2101010;
    }
    break;
  default:
    break;
  }
  return SDL_PIXELFORMAT_UNKNOWN;
}

// Inverse of VexaMasksToPixelFormatEnum for the formats it can produce; used to
// recover canonical masks when callers pass all-zero masks (SDL2 defaults).
bool VexaPixelFormatEnumToMasks(Uint32 format, int* bpp,
                                Uint32* Rmask, Uint32* Gmask, Uint32* Bmask, Uint32* Amask) {
  struct Entry {
    Uint32 format;
    int bpp;
    Uint32 R, G, B, A;
  };
  static constexpr Entry entries[] = {
    {SDL_PIXELFORMAT_INDEX1MSB, 1, 0, 0, 0, 0},
    {SDL_PIXELFORMAT_INDEX2MSB, 2, 0, 0, 0, 0},
    {SDL_PIXELFORMAT_INDEX4MSB, 4, 0, 0, 0, 0},
    {SDL_PIXELFORMAT_INDEX8, 8, 0, 0, 0, 0},
    {SDL_PIXELFORMAT_RGB332, 8, 0xE0, 0x1C, 0x03, 0x00},
    {SDL_PIXELFORMAT_RGB444, 12, 0x0F00, 0x00F0, 0x000F, 0x0000},
    {SDL_PIXELFORMAT_BGR444, 12, 0x000F, 0x00F0, 0x0F00, 0x0000},
    {SDL_PIXELFORMAT_RGB555, 15, 0x7C00, 0x03E0, 0x001F, 0x0000},
    {SDL_PIXELFORMAT_BGR555, 15, 0x001F, 0x03E0, 0x7C00, 0x0000},
    {SDL_PIXELFORMAT_ARGB4444, 16, 0x0F00, 0x00F0, 0x000F, 0xF000},
    {SDL_PIXELFORMAT_RGBA4444, 16, 0xF000, 0x0F00, 0x00F0, 0x000F},
    {SDL_PIXELFORMAT_ABGR4444, 16, 0x000F, 0x00F0, 0x0F00, 0xF000},
    {SDL_PIXELFORMAT_BGRA4444, 16, 0x00F0, 0x0F00, 0xF000, 0x000F},
    {SDL_PIXELFORMAT_ARGB1555, 16, 0x7C00, 0x03E0, 0x001F, 0x8000},
    {SDL_PIXELFORMAT_RGBA5551, 16, 0xF800, 0x07C0, 0x003E, 0x0001},
    {SDL_PIXELFORMAT_ABGR1555, 16, 0x001F, 0x03E0, 0x7C00, 0x8000},
    {SDL_PIXELFORMAT_BGRA5551, 16, 0x003E, 0x07C0, 0xF800, 0x0001},
    {SDL_PIXELFORMAT_RGB565, 16, 0xF800, 0x07E0, 0x001F, 0x0000},
    {SDL_PIXELFORMAT_BGR565, 16, 0x001F, 0x07E0, 0xF800, 0x0000},
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    {SDL_PIXELFORMAT_RGB24, 24, 0x00FF0000, 0x0000FF00, 0x000000FF, 0x00000000},
    {SDL_PIXELFORMAT_BGR24, 24, 0x000000FF, 0x0000FF00, 0x00FF0000, 0x00000000},
#else
    {SDL_PIXELFORMAT_BGR24, 24, 0x00FF0000, 0x0000FF00, 0x000000FF, 0x00000000},
    {SDL_PIXELFORMAT_RGB24, 24, 0x000000FF, 0x0000FF00, 0x00FF0000, 0x00000000},
#endif
    {SDL_PIXELFORMAT_RGB888, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0x00000000},
    {SDL_PIXELFORMAT_RGBX8888, 32, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x00000000},
    {SDL_PIXELFORMAT_BGR888, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0x00000000},
    {SDL_PIXELFORMAT_BGRX8888, 32, 0x0000FF00, 0x00FF0000, 0xFF000000, 0x00000000},
    {SDL_PIXELFORMAT_ARGB8888, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000},
    {SDL_PIXELFORMAT_RGBA8888, 32, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF},
    {SDL_PIXELFORMAT_ABGR8888, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000},
    {SDL_PIXELFORMAT_BGRA8888, 32, 0x0000FF00, 0x00FF0000, 0xFF000000, 0x000000FF},
    {SDL_PIXELFORMAT_ARGB2101010, 32, 0x3FF00000, 0x000FFC00, 0x000003FF, 0xC0000000},
  };
  for (const Entry& entry : entries) {
    if (entry.format == format) {
      if (bpp) {
        *bpp = entry.bpp;
      }
      if (Rmask) {
        *Rmask = entry.R;
      }
      if (Gmask) {
        *Gmask = entry.G;
      }
      if (Bmask) {
        *Bmask = entry.B;
      }
      if (Amask) {
        *Amask = entry.A;
      }
      return true;
    }
  }
  return false;
}

void VexaMaskShiftLoss(Uint32 mask, Uint8* shift, Uint8* loss) {
  Uint8 s = 0;
  int bits = 0;
  if (mask) {
    while ((mask & 1U) == 0U) {
      mask >>= 1U;
      ++s;
    }
    while (mask & 1U) {
      mask >>= 1U;
      ++bits;
    }
  }
  *shift = s;
  *loss = static_cast<Uint8>(bits < 8 ? 8 - bits : 0);
}

SDL_Palette* VexaAllocPalette(int ncolors) {
  auto* palette = static_cast<SDL_Palette*>(std::calloc(1, sizeof(SDL_Palette)));
  if (!palette) {
    return nullptr;
  }
  palette->ncolors = ncolors;
  palette->colors = static_cast<SDL_Color*>(std::calloc(static_cast<size_t>(ncolors), sizeof(SDL_Color)));
  if (!palette->colors) {
    std::free(palette);
    return nullptr;
  }
  palette->refcount = 1;
  return palette;
}

void VexaFreePalette(SDL_Palette* palette) {
  if (!palette) {
    return;
  }
  if (--palette->refcount > 0) {
    return;
  }
  std::free(palette->colors);
  std::free(palette);
}

// Port of SDL_AllocFormat(): allocate and fill a public SDL_PixelFormat for a
// pre-resolved SDL_PIXELFORMAT_* enum value.
SDL_PixelFormat* VexaAllocFormat(Uint32 format_enum, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {
  if (format_enum == SDL_PIXELFORMAT_UNKNOWN || SDL_ISPIXELFORMAT_FOURCC(format_enum)) {
    SDL2_LOGW("VexaAllocFormat: unknown pixel format 0x%x", format_enum);
    return nullptr;
  }

  if ((Rmask | Gmask | Bmask | Amask) == 0) {
    /* Recover the canonical masks for the default format of this depth. */
    VexaPixelFormatEnumToMasks(format_enum, nullptr, &Rmask, &Gmask, &Bmask, &Amask);
  }

  auto* format = static_cast<SDL_PixelFormat*>(std::calloc(1, sizeof(SDL_PixelFormat)));
  if (!format) {
    return nullptr;
  }
  format->format = format_enum;
  format->BitsPerPixel = SDL_BITSPERPIXEL(format_enum);
  format->BytesPerPixel = SDL_BYTESPERPIXEL(format_enum);
  format->Rmask = Rmask;
  format->Gmask = Gmask;
  format->Bmask = Bmask;
  format->Amask = Amask;
  VexaMaskShiftLoss(Rmask, &format->Rshift, &format->Rloss);
  VexaMaskShiftLoss(Gmask, &format->Gshift, &format->Gloss);
  VexaMaskShiftLoss(Bmask, &format->Bshift, &format->Bloss);
  VexaMaskShiftLoss(Amask, &format->Ashift, &format->Aloss);
  format->refcount = 1;

  if (SDL_ISPIXELFORMAT_INDEXED(format_enum)) {
    SDL_Palette* palette = VexaAllocPalette(1 << format->BitsPerPixel);
    if (!palette) {
      std::free(format);
      return nullptr;
    }
    if (palette->ncolors == 2) {
      /* Create a black and white bitmap palette, matching SDL2. */
      palette->colors[0].r = 0xFF;
      palette->colors[0].g = 0xFF;
      palette->colors[0].b = 0xFF;
      palette->colors[1].r = 0x00;
      palette->colors[1].g = 0x00;
      palette->colors[1].b = 0x00;
    }
    format->palette = palette;
  }
  return format;
}

void VexaFreeFormat(SDL_PixelFormat* format) {
  if (!format) {
    return;
  }
  if (--format->refcount > 0) {
    return;
  }
  VexaFreePalette(format->palette);
  std::free(format);
}

// Port of SDL_CalculatePitch(): bytes-per-row with 4-byte alignment.
int VexaCalculatePitch(Uint32 format_enum, int width) {
  int pitch;
  if (SDL_BITSPERPIXEL(format_enum) < 8) {
    pitch = (width * SDL_BITSPERPIXEL(format_enum) + 7) / 8;
  } else {
    pitch = width * SDL_BYTESPERPIXEL(format_enum);
  }
  if (pitch < 0) {
    return 0;
  }
  return (pitch + 3) & ~3;
}

int VexaSetSurfaceBlendMode(SDL_Surface* surface, SDL_BlendMode blendMode) {
  if (!surface) {
    return -1;
  }
  VexaBlitMap* map = GetBlitMap(surface);
  if (!map) {
    return -1;
  }
  switch (blendMode) {
  case SDL_BLENDMODE_NONE:
  case SDL_BLENDMODE_BLEND:
  case SDL_BLENDMODE_ADD:
  case SDL_BLENDMODE_MOD:
  case SDL_BLENDMODE_MUL:
    map->BlendMode = blendMode;
    return 0;
  default:
    return -1;
  }
}

// Port of SDL_CreateRGBSurfaceWithFormat() core allocation (called after the
// mask->enum translation that SDL_CreateRGBSurface performs). The flags
// parameter is unused in SDL2 (callers should pass 0); kept for parity.
SDL_Surface* VexaCreateSurface(Uint32 flags, int width, int height, Uint32 format_enum,
                               Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {
  (void)flags;

  if (width < 0 || height < 0) {
    return nullptr;
  }

  auto* surface = static_cast<SDL_Surface*>(std::calloc(1, sizeof(SDL_Surface)));
  if (!surface) {
    return nullptr;
  }

  surface->format = VexaAllocFormat(format_enum, Rmask, Gmask, Bmask, Amask);
  if (!surface->format) {
    std::free(surface);
    return nullptr;
  }
  surface->w = width;
  surface->h = height;
  surface->pitch = VexaCalculatePitch(surface->format->format, width);
  surface->clip_rect.x = 0;
  surface->clip_rect.y = 0;
  surface->clip_rect.w = width;
  surface->clip_rect.h = height;

  if (surface->w && surface->h) {
    const size_t size = static_cast<size_t>(surface->h) * static_cast<size_t>(surface->pitch);
    surface->pixels = std::calloc(1, size);
    if (!surface->pixels) {
      VexaFreeFormat(surface->format);
      std::free(surface);
      return nullptr;
    }
  }

  surface->map = reinterpret_cast<SDL_BlitMap*>(new VexaBlitMap {});
  surface->refcount = 1;

  /* By default surfaces with an alpha mask are set up for blending (SDL2). */
  if (surface->format->Amask) {
    VexaSetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND);
  }
  return surface;
}

SDL_Surface* VexaCreateRGBSurface(Uint32 flags, int width, int height, int depth,
                                Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {
  const Uint32 format_enum = VexaMasksToPixelFormatEnum(depth, Rmask, Gmask, Bmask, Amask);
  if (format_enum == SDL_PIXELFORMAT_UNKNOWN) {
    SDL2_LOGW("SDL_CreateRGBSurface: unknown pixel format");
    return nullptr;
  }

  if ((Rmask | Gmask | Bmask | Amask) == 0) {
    VexaPixelFormatEnumToMasks(format_enum, nullptr, &Rmask, &Gmask, &Bmask, &Amask);
  }

  return VexaCreateSurface(flags, width, height, format_enum, Rmask, Gmask, Bmask, Amask);
}

SDL_Surface* VexaCreateRGBSurfaceFrom(void* pixels, int width, int height, int depth, int pitch,
                                    Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {
  if (width < 0 || height < 0) {
    return nullptr;
  }

  const Uint32 format_enum = VexaMasksToPixelFormatEnum(depth, Rmask, Gmask, Bmask, Amask);
  if (format_enum == SDL_PIXELFORMAT_UNKNOWN) {
    SDL2_LOGW("SDL_CreateRGBSurfaceFrom: unknown pixel format");
    return nullptr;
  }

  if ((Rmask | Gmask | Bmask | Amask) == 0) {
    VexaPixelFormatEnumToMasks(format_enum, nullptr, &Rmask, &Gmask, &Bmask, &Amask);
  }

  /* SDL2 validates the caller pitch against the minimal (unaligned) pitch. */
  int minimal_pitch;
  if (SDL_BITSPERPIXEL(format_enum) < 8) {
    minimal_pitch = (width * SDL_BITSPERPIXEL(format_enum) + 7) / 8;
  } else {
    minimal_pitch = width * SDL_BYTESPERPIXEL(format_enum);
  }
  if (pitch < 0 || (pitch > 0 && pitch < minimal_pitch)) {
    SDL2_LOGW("SDL_CreateRGBSurfaceFrom: invalid pitch %d (minimal %d)", pitch, minimal_pitch);
    return nullptr;
  }

  SDL_Surface* surface = VexaCreateSurface(0, 0, 0, format_enum, Rmask, Gmask, Bmask, Amask);
  if (surface) {
    surface->flags |= SDL_PREALLOC;
    surface->pixels = pixels;
    surface->w = width;
    surface->h = height;
    surface->pitch = pitch;
    surface->clip_rect.x = 0;
    surface->clip_rect.y = 0;
    surface->clip_rect.w = width;
    surface->clip_rect.h = height;
  }
  return surface;
}

void VexaFreeSurface(SDL_Surface* surface) {
  if (!surface) {
    return;
  }
  if (surface->flags & SDL_DONTFREE) {
    return;
  }
  if (--surface->refcount > 0) {
    return;
  }
  if (surface->format) {
    VexaFreeFormat(surface->format);
    surface->format = nullptr;
  }
  if (surface->flags & SDL_PREALLOC) {
    /* Don't free caller-provided pixels. */
  } else {
    std::free(surface->pixels);
  }
  delete reinterpret_cast<VexaBlitMap*>(surface->map);
  surface->map = nullptr;
  std::free(surface);
}

SDL_bool VexaIntersectRect(const SDL_Rect* A, const SDL_Rect* B, SDL_Rect* result) {
  int Amin, Amax, Bmin, Bmax;

  if (!A || !B || !result) {
    return SDL_FALSE;
  }

  /* Special cases for empty rects */
  if (A->w <= 0 || A->h <= 0 || B->w <= 0 || B->h <= 0) {
    result->w = 0;
    result->h = 0;
    return SDL_FALSE;
  }

  /* Horizontal intersection */
  Amin = A->x;
  Amax = Amin + A->w;
  Bmin = B->x;
  Bmax = Bmin + B->w;
  if (Bmin > Amin) {
    Amin = Bmin;
  }
  result->x = Amin;
  if (Bmax < Amax) {
    Amax = Bmax;
  }
  result->w = Amax - Amin;

  /* Vertical intersection */
  Amin = A->y;
  Amax = Amin + A->h;
  Bmin = B->y;
  Bmax = Bmin + B->h;
  if (Bmin > Amin) {
    Amin = Bmin;
  }
  result->y = Amin;
  if (Bmax < Amax) {
    Amax = Bmax;
  }
  result->h = Amax - Amin;

  return result->w > 0 && result->h > 0 ? SDL_TRUE : SDL_FALSE;
}

// --- Pixel access helpers ---------------------------------------------------

// Read one raw pixel (or palette index) honoring the surface pitch and
// sub-byte indexed formats (MSB ordering, as per SDL2 defaults).
Uint32 VexaReadRawPixel(const SDL_Surface* surface, int x, int y) {
  const Uint8* row = static_cast<const Uint8*>(surface->pixels) +
                     static_cast<size_t>(y) * static_cast<size_t>(surface->pitch);
  const SDL_PixelFormat* fmt = surface->format;
  switch (fmt->BytesPerPixel) {
  case 1:
    if (fmt->BitsPerPixel < 8) {
      const int bpp = fmt->BitsPerPixel;
      const int bitpos = x * bpp;
      const Uint8 byte = row[bitpos >> 3];
      const int shift = 8 - bpp - (bitpos & 7);
      return (byte >> shift) & ((1U << bpp) - 1U);
    }
    return row[x];
  case 2:
    return reinterpret_cast<const Uint16*>(row)[x];
  case 3:
    return static_cast<Uint32>(row[x * 3]) |
           (static_cast<Uint32>(row[x * 3 + 1]) << 8) |
           (static_cast<Uint32>(row[x * 3 + 2]) << 16);
  case 4:
  default:
    return reinterpret_cast<const Uint32*>(row)[x];
  }
}

void VexaWriteRawPixel(SDL_Surface* surface, int x, int y, Uint32 pixel) {
  Uint8* row = static_cast<Uint8*>(surface->pixels) +
               static_cast<size_t>(y) * static_cast<size_t>(surface->pitch);
  const SDL_PixelFormat* fmt = surface->format;
  switch (fmt->BytesPerPixel) {
  case 1:
    if (fmt->BitsPerPixel < 8) {
      const int bpp = fmt->BitsPerPixel;
      const int bitpos = x * bpp;
      const int shift = 8 - bpp - (bitpos & 7);
      const Uint8 mask = static_cast<Uint8>(((1U << bpp) - 1U) << shift);
      row[bitpos >> 3] = static_cast<Uint8>((row[bitpos >> 3] & ~mask) |
        ((static_cast<Uint8>(pixel) << shift) & mask));
      return;
    }
    row[x] = static_cast<Uint8>(pixel);
    return;
  case 2:
    reinterpret_cast<Uint16*>(row)[x] = static_cast<Uint16>(pixel);
    return;
  case 3:
    row[x * 3] = static_cast<Uint8>(pixel);
    row[x * 3 + 1] = static_cast<Uint8>(pixel >> 8);
    row[x * 3 + 2] = static_cast<Uint8>(pixel >> 16);
    return;
  case 4:
  default:
    reinterpret_cast<Uint32*>(row)[x] = pixel;
    return;
  }
}

Uint8 VexaExpandChannel(Uint32 value, Uint32 mask, Uint8 shift) {
  if (!mask) {
    return 0xFF;
  }
  const Uint32 maxv = mask >> shift;
  const Uint32 v = (value & mask) >> shift;
  return static_cast<Uint8>((v * 255U + maxv / 2U) / (maxv ? maxv : 1U));
}

Uint32 VexaPackChannel(Uint8 value, Uint32 mask, Uint8 shift) {
  if (!mask) {
    return 0;
  }
  const Uint32 maxv = mask >> shift;
  const Uint32 v = (static_cast<Uint32>(value) * maxv + 127U) / 255U;
  return (v << shift) & mask;
}

// Map a raw pixel to normalized RGBA using the format masks (or palette).
void VexaGetRGBA(const SDL_PixelFormat* fmt, Uint32 pixel,
                 Uint8* r, Uint8* g, Uint8* b, Uint8* a) {
  if (fmt->palette) {
    const SDL_Color* colors = fmt->palette->colors;
    const int index = static_cast<int>(pixel) < fmt->palette->ncolors ? static_cast<int>(pixel) : 0;
    *r = colors[index].r;
    *g = colors[index].g;
    *b = colors[index].b;
    *a = 0xFF;
    return;
  }
  *r = fmt->Rmask ? VexaExpandChannel(pixel, fmt->Rmask, fmt->Rshift) : 0;
  *g = fmt->Gmask ? VexaExpandChannel(pixel, fmt->Gmask, fmt->Gshift) : 0;
  *b = fmt->Bmask ? VexaExpandChannel(pixel, fmt->Bmask, fmt->Bshift) : 0;
  *a = fmt->Amask ? VexaExpandChannel(pixel, fmt->Amask, fmt->Ashift) : 0xFF;
}

Uint32 VexaMapRGBA(const SDL_PixelFormat* fmt, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
  if (fmt->palette) {
    /* Nearest-color search for indexed destinations. */
    int best = 0;
    int best_dist = INT32_MAX;
    for (int i = 0; i < fmt->palette->ncolors; ++i) {
      const SDL_Color& c = fmt->palette->colors[i];
      const int dr = c.r - r;
      const int dg = c.g - g;
      const int db = c.b - b;
      const int dist = dr * dr + dg * dg + db * db;
      if (dist < best_dist) {
        best_dist = dist;
        best = i;
        if (dist == 0) {
          break;
        }
      }
    }
    return static_cast<Uint32>(best);
  }
  return VexaPackChannel(r, fmt->Rmask, fmt->Rshift) |
         VexaPackChannel(g, fmt->Gmask, fmt->Gshift) |
         VexaPackChannel(b, fmt->Bmask, fmt->Bshift) |
         VexaPackChannel(a, fmt->Amask, fmt->Ashift);
}

// Apply the source surface blend mode; mirrors SDL2 blit flags semantics for
// the common modes (NONE/BLEND/ADD/MOD/MUL) at 8-bit channel resolution.
void VexaBlendPixel(SDL_BlendMode mode,
                    Uint8 sr, Uint8 sg, Uint8 sb, Uint8 sa,
                    Uint8 dr, Uint8 dg, Uint8 db, Uint8 da,
                    Uint8* out_r, Uint8* out_g, Uint8* out_b, Uint8* out_a) {
  const unsigned inv = 255U - sa;
  unsigned or_, og, ob, oa;
  switch (mode) {
  case SDL_BLENDMODE_BLEND:
    or_ = (static_cast<unsigned>(sr) * sa + static_cast<unsigned>(dr) * inv) / 255U;
    og = (static_cast<unsigned>(sg) * sa + static_cast<unsigned>(dg) * inv) / 255U;
    ob = (static_cast<unsigned>(sb) * sa + static_cast<unsigned>(db) * inv) / 255U;
    oa = sa + (static_cast<unsigned>(da) * inv) / 255U;
    break;
  case SDL_BLENDMODE_ADD: {
    const unsigned rr = static_cast<unsigned>(sr) * sa / 255U + dr;
    const unsigned gg = static_cast<unsigned>(sg) * sa / 255U + dg;
    const unsigned bb = static_cast<unsigned>(sb) * sa / 255U + db;
    or_ = rr > 255U ? 255U : rr;
    og = gg > 255U ? 255U : gg;
    ob = bb > 255U ? 255U : bb;
    oa = da;
    break;
  }
  case SDL_BLENDMODE_MOD:
    or_ = static_cast<unsigned>(sr) * dr / 255U;
    og = static_cast<unsigned>(sg) * dg / 255U;
    ob = static_cast<unsigned>(sb) * db / 255U;
    oa = da;
    break;
  case SDL_BLENDMODE_MUL:
    or_ = (static_cast<unsigned>(sr) * dr + static_cast<unsigned>(dr) * inv) / 255U;
    og = (static_cast<unsigned>(sg) * dg + static_cast<unsigned>(dg) * inv) / 255U;
    ob = (static_cast<unsigned>(sb) * db + static_cast<unsigned>(db) * inv) / 255U;
    oa = sa + (static_cast<unsigned>(da) * inv) / 255U;
    break;
  case SDL_BLENDMODE_NONE:
  default:
    or_ = sr;
    og = sg;
    ob = sb;
    oa = sa;
    break;
  }
  *out_r = static_cast<Uint8>(or_);
  *out_g = static_cast<Uint8>(og);
  *out_b = static_cast<Uint8>(ob);
  *out_a = static_cast<Uint8>(oa);
}

// Nearest-neighbor scaled blit across pre-clipped rects (LowerBlit equivalent).
// Handles format conversion and the surface blend mode; same-format blits take
// a raw-copy fast path like SDL_SoftStretch.
void VexaLowerBlitScaled(SDL_Surface* src, const SDL_Rect* srcrect,
                         SDL_Surface* dst, const SDL_Rect* dstrect) {
  if (!src || !dst || !src->pixels || !dst->pixels ||
      srcrect->w <= 0 || srcrect->h <= 0 || dstrect->w <= 0 || dstrect->h <= 0) {
    return;
  }

  const VexaBlitMap* map = GetBlitMap(src);
  const SDL_BlendMode blend_mode = map ? map->BlendMode : SDL_BLENDMODE_NONE;
  const bool same_format = src->format->format == dst->format->format &&
                           src->format->Rmask == dst->format->Rmask &&
                           src->format->Gmask == dst->format->Gmask &&
                           src->format->Bmask == dst->format->Bmask &&
                           src->format->Amask == dst->format->Amask &&
                           !SDL_ISPIXELFORMAT_INDEXED(src->format->format);
  const bool fast_copy = same_format && blend_mode == SDL_BLENDMODE_NONE;
  const size_t bpp = src->format->BytesPerPixel;

  for (int dy = 0; dy < dstrect->h; ++dy) {
    const int sy = srcrect->y +
      static_cast<int>((static_cast<int64_t>(dy) * srcrect->h) / dstrect->h);
    Uint8* dst_row = static_cast<Uint8*>(dst->pixels) +
                     static_cast<size_t>(dstrect->y + dy) * static_cast<size_t>(dst->pitch);
    const Uint8* src_row = static_cast<const Uint8*>(src->pixels) +
                           static_cast<size_t>(sy) * static_cast<size_t>(src->pitch);

    if (fast_copy && dstrect->w == srcrect->w) {
      /* Unscaled same-format row copy. */
      std::memmove(dst_row + static_cast<size_t>(dstrect->x) * bpp,
                   src_row + static_cast<size_t>(srcrect->x) * bpp,
                   static_cast<size_t>(dstrect->w) * bpp);
      continue;
    }

    for (int dx = 0; dx < dstrect->w; ++dx) {
      const int sx = srcrect->x +
        static_cast<int>((static_cast<int64_t>(dx) * srcrect->w) / dstrect->w);
      const int dst_x = dstrect->x + dx;
      const int dst_y = dstrect->y + dy;

      const Uint32 raw = VexaReadRawPixel(src, sx, sy);
      if (fast_copy) {
        VexaWriteRawPixel(dst, dst_x, dst_y, raw);
        continue;
      }

      Uint8 sr, sg, sb, sa;
      VexaGetRGBA(src->format, raw, &sr, &sg, &sb, &sa);
      if (blend_mode != SDL_BLENDMODE_NONE) {
        Uint8 dr, dg, db, da;
        VexaGetRGBA(dst->format, VexaReadRawPixel(dst, dst_x, dst_y), &dr, &dg, &db, &da);
        VexaBlendPixel(blend_mode, sr, sg, sb, sa, dr, dg, db, da, &sr, &sg, &sb, &sa);
      }
      VexaWriteRawPixel(dst, dst_x, dst_y, VexaMapRGBA(dst->format, sr, sg, sb, sa));
    }
  }
}

// Port of SDL_UpperBlit() rectangle normalization; used by UpperBlitScaled's
// no-scaling fast path.
int VexaUpperBlit(SDL_Surface* src, const SDL_Rect* srcrect,
                  SDL_Surface* dst, SDL_Rect* dstrect) {
  SDL_Rect r_src, r_dst;

  if (!src || !dst) {
    return -1;
  }
  if (src->locked || dst->locked) {
    return -1;
  }

  /* Full src surface */
  r_src.x = 0;
  r_src.y = 0;
  r_src.w = src->w;
  r_src.h = src->h;

  if (dstrect) {
    r_dst.x = dstrect->x;
    r_dst.y = dstrect->y;
  } else {
    r_dst.x = 0;
    r_dst.y = 0;
  }

  /* clip the source rectangle to the source surface */
  if (srcrect) {
    SDL_Rect tmp;
    if (VexaIntersectRect(srcrect, &r_src, &tmp) == SDL_FALSE) {
      goto end;
    }

    /* Shift dstrect, if srcrect origin has changed */
    r_dst.x += tmp.x - srcrect->x;
    r_dst.y += tmp.y - srcrect->y;

    /* Update srcrect */
    r_src = tmp;
  }

  /* There're no dstrect.w/h parameters. It's the same as srcrect */
  r_dst.w = r_src.w;
  r_dst.h = r_src.h;

  /* clip the destination rectangle against the clip rectangle */
  {
    SDL_Rect tmp;
    if (VexaIntersectRect(&r_dst, &dst->clip_rect, &tmp) == SDL_FALSE) {
      goto end;
    }

    /* Shift srcrect, if dstrect has changed */
    r_src.x += tmp.x - r_dst.x;
    r_src.y += tmp.y - r_dst.y;
    r_src.w = tmp.w;
    r_src.h = tmp.h;

    /* Update dstrect */
    r_dst = tmp;
  }

  if (r_dst.w > 0 && r_dst.h > 0) {
    if (dstrect) { /* update output parameter */
      *dstrect = r_dst;
    }
    VexaLowerBlitScaled(src, &r_src, dst, &r_dst);
    return 0;
  }

end:
  if (dstrect) { /* update output parameter */
    dstrect->w = dstrect->h = 0;
  }
  return 0;
}

// Port of SDL_PrivateUpperBlitScaled() with SDL_ScaleModeNearest, which is
// exactly what SDL2's SDL_UpperBlitScaled() dispatches to.
int VexaUpperBlitScaled(SDL_Surface* src, const SDL_Rect* srcrect,
                        SDL_Surface* dst, SDL_Rect* dstrect) {
  double src_x0, src_y0, src_x1, src_y1;
  double dst_x0, dst_y0, dst_x1, dst_y1;
  SDL_Rect final_src, final_dst;
  double scaling_w, scaling_h;
  int src_w, src_h;
  int dst_w, dst_h;

  if (!src || !dst) {
    return -1;
  }
  if (src->locked || dst->locked) {
    return -1;
  }

  if (!srcrect) {
    src_w = src->w;
    src_h = src->h;
  } else {
    src_w = srcrect->w;
    src_h = srcrect->h;
  }

  if (!dstrect) {
    dst_w = dst->w;
    dst_h = dst->h;
  } else {
    dst_w = dstrect->w;
    dst_h = dstrect->h;
  }

  if (dst_w == src_w && dst_h == src_h) {
    /* No scaling, defer to regular blit */
    return VexaUpperBlit(src, srcrect, dst, dstrect);
  }
  if (src_w <= 0 || src_h <= 0) {
    if (dstrect) {
      dstrect->w = dstrect->h = 0;
    }
    return 0;
  }

  scaling_w = static_cast<double>(dst_w) / src_w;
  scaling_h = static_cast<double>(dst_h) / src_h;

  if (!dstrect) {
    dst_x0 = 0;
    dst_y0 = 0;
    dst_x1 = dst_w;
    dst_y1 = dst_h;
  } else {
    dst_x0 = dstrect->x;
    dst_y0 = dstrect->y;
    dst_x1 = dst_x0 + dst_w;
    dst_y1 = dst_y0 + dst_h;
  }

  if (!srcrect) {
    src_x0 = 0;
    src_y0 = 0;
    src_x1 = src_w;
    src_y1 = src_h;
  } else {
    src_x0 = srcrect->x;
    src_y0 = srcrect->y;
    src_x1 = src_x0 + src_w;
    src_y1 = src_y0 + src_h;

    /* Clip source rectangle to the source surface */

    if (src_x0 < 0) {
      dst_x0 -= src_x0 * scaling_w;
      src_x0 = 0;
    }

    if (src_x1 > src->w) {
      dst_x1 -= (src_x1 - src->w) * scaling_w;
      src_x1 = src->w;
    }

    if (src_y0 < 0) {
      dst_y0 -= src_y0 * scaling_h;
      src_y0 = 0;
    }

    if (src_y1 > src->h) {
      dst_y1 -= (src_y1 - src->h) * scaling_h;
      src_y1 = src->h;
    }
  }

  /* Clip destination rectangle to the clip rectangle */

  /* Translate to clip space for easier calculations */
  dst_x0 -= dst->clip_rect.x;
  dst_x1 -= dst->clip_rect.x;
  dst_y0 -= dst->clip_rect.y;
  dst_y1 -= dst->clip_rect.y;

  if (dst_x0 < 0) {
    src_x0 -= dst_x0 / scaling_w;
    dst_x0 = 0;
  }

  if (dst_x1 > dst->clip_rect.w) {
    src_x1 -= (dst_x1 - dst->clip_rect.w) / scaling_w;
    dst_x1 = dst->clip_rect.w;
  }

  if (dst_y0 < 0) {
    src_y0 -= dst_y0 / scaling_h;
    dst_y0 = 0;
  }

  if (dst_y1 > dst->clip_rect.h) {
    src_y1 -= (dst_y1 - dst->clip_rect.h) / scaling_h;
    dst_y1 = dst->clip_rect.h;
  }

  /* Translate back to surface coordinates */
  dst_x0 += dst->clip_rect.x;
  dst_x1 += dst->clip_rect.x;
  dst_y0 += dst->clip_rect.y;
  dst_y1 += dst->clip_rect.y;

  final_src.x = static_cast<int>(std::lround(src_x0));
  final_src.y = static_cast<int>(std::lround(src_y0));
  final_src.w = static_cast<int>(std::lround(src_x1 - src_x0));
  final_src.h = static_cast<int>(std::lround(src_y1 - src_y0));

  final_dst.x = static_cast<int>(std::lround(dst_x0));
  final_dst.y = static_cast<int>(std::lround(dst_y0));
  final_dst.w = static_cast<int>(std::lround(dst_x1 - dst_x0));
  final_dst.h = static_cast<int>(std::lround(dst_y1 - dst_y0));

  /* Clip again */
  {
    SDL_Rect tmp;
    tmp.x = 0;
    tmp.y = 0;
    tmp.w = src->w;
    tmp.h = src->h;
    VexaIntersectRect(&tmp, &final_src, &final_src);
  }

  /* Clip again */
  VexaIntersectRect(&dst->clip_rect, &final_dst, &final_dst);

  if (dstrect) {
    *dstrect = final_dst;
  }

  if (final_dst.w == 0 || final_dst.h == 0 ||
      final_src.w <= 0 || final_src.h <= 0) {
    /* No-op. */
    return 0;
  }

  VexaLowerBlitScaled(src, &final_src, dst, &final_dst);
  return 0;
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

  SDL2_LOGW("%s called on non-owner thread (owner=%lu current=%lu)",
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
    SDL2_LOGI("Captured external EGL context %dx%d", width, height);
  } else {
    SDL2_LOGW("No current EGL context available to capture");
  }
}

extern "C" __attribute__((visibility("default"))) void SDL2_PumpGLQueue() {
}

extern "C" __attribute__((visibility("default"))) int SDL2Thunk_IsTextInputActive() {
  return TextInputActiveFlag.load() ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) void SDL2Thunk_InjectTouch(int action, int pointer_id, float x, float y, float pressure) {
  SDL_Event event {};
  event.type = action == 0 ? SDL_FINGERDOWN :
               action == 1 ? SDL_FINGERUP :
                             SDL_FINGERMOTION;
  event.tfinger.type = event.type;
  event.tfinger.timestamp = GetEventTimestampMS();
  event.tfinger.touchId = 1;
  event.tfinger.fingerId = pointer_id;
  event.tfinger.x = x;
  event.tfinger.y = y;
  event.tfinger.dx = 0.0f;
  event.tfinger.dy = 0.0f;
  event.tfinger.pressure = pressure;
  event.tfinger.windowID = 1;
  PushEvent(event);
}

extern "C" __attribute__((visibility("default"))) void SDL2Thunk_InjectMouseMotion(float x, float y) {
  const float last_x = LastMouseX.exchange(x);
  const float last_y = LastMouseY.exchange(y);

  SDL_Event event {};
  event.type = SDL_MOUSEMOTION;
  event.motion.type = SDL_MOUSEMOTION;
  event.motion.timestamp = GetEventTimestampMS();
  event.motion.windowID = 1;
  event.motion.which = 0;
  event.motion.state = 0;
  event.motion.x = static_cast<Sint32>(x);
  event.motion.y = static_cast<Sint32>(y);
  event.motion.xrel = static_cast<Sint32>(x - last_x);
  event.motion.yrel = static_cast<Sint32>(y - last_y);
  PushEvent(event);
}

extern "C" __attribute__((visibility("default"))) void SDL2Thunk_InjectMouseButton(int button, int down, float x, float y) {
  SDL_Event event {};
  event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
  event.button.type = event.type;
  event.button.timestamp = GetEventTimestampMS();
  event.button.windowID = 1;
  event.button.which = 0;
  event.button.button = static_cast<Uint8>(button);
  event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
  event.button.clicks = 1;
  event.button.x = static_cast<Sint32>(x);
  event.button.y = static_cast<Sint32>(y);
  PushEvent(event);
}

extern "C" __attribute__((visibility("default"))) void SDL2Thunk_InjectMouseWheel(float x, float y) {
  SDL_Event event {};
  event.type = SDL_MOUSEWHEEL;
  event.wheel.type = SDL_MOUSEWHEEL;
  event.wheel.timestamp = GetEventTimestampMS();
  event.wheel.windowID = 1;
  event.wheel.which = 0;
  event.wheel.x = static_cast<Sint32>(x);
  event.wheel.y = static_cast<Sint32>(y);
  event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
  event.wheel.preciseX = x;
  event.wheel.preciseY = y;
  event.wheel.mouseX = static_cast<Sint32>(LastMouseX.load(std::memory_order_acquire));
  event.wheel.mouseY = static_cast<Sint32>(LastMouseY.load(std::memory_order_acquire));
  PushEvent(event);
}

extern "C" __attribute__((visibility("default"))) void SDL2Thunk_InjectText(const char* utf8) {
  SDL_Event event {};
  event.type = SDL_TEXTINPUT;
  event.text.type = SDL_TEXTINPUT;
  event.text.timestamp = GetEventTimestampMS();
  event.text.windowID = 1;
  // VEXA_FIXES: SDL2 embeds the UTF-8 text directly in the event payload.
  if (utf8) {
    std::strncpy(event.text.text, utf8, SDL_TEXTINPUTEVENT_TEXT_SIZE - 1);
    event.text.text[SDL_TEXTINPUTEVENT_TEXT_SIZE - 1] = '\0';
  } else {
    event.text.text[0] = '\0';
  }
  PushEvent(event);
}

extern "C" __attribute__((visibility("default"))) void SDL2Thunk_InjectKey(uint32_t scancode, uint32_t key, int down, int repeat) {
  SDL_Event event {};
  event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
  event.key.type = event.type;
  event.key.timestamp = GetEventTimestampMS();
  event.key.windowID = 1;
  event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
  event.key.repeat = static_cast<Uint8>(repeat != 0);
  event.key.keysym.scancode = static_cast<SDL_Scancode>(scancode);
  event.key.keysym.sym = static_cast<SDL_Keycode>(key);
  event.key.keysym.mod = KMOD_NONE;
  PushEvent(event);
}

static int fexfn_impl_libSDL2_FEX_SDL2_Init(Uint32 flags) {
  MarkProgress("SDL_Init", "enter");
  SDL2_LOGI("SDL_Init(flags=0x%x)", static_cast<unsigned>(flags));
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
  {
    std::scoped_lock lock {EventQueueMutex};
    EventQueue.clear();
  }
  // VEXA_FIXES: Only tear down a stale managed EGL context on a *fresh* init.
  // SDL_InitSubSystem() forwards each new flag set here too (e.g. the game adds
  // SDL_INIT_AUDIO mid-run via the FMOD SDL path), and real SDL2 never destroys
  // the video state on subsystem add — doing so here killed the live context
  // and permanently blacked the screen.
  if (!SDLInitialized && ManagedEGLActive) {
    DestroyManagedEGLContext();
  }
  g_GLCreateOwnerTid = 0;
  SetSDLContextReadyState(false, "SDL_Init");
  SDLInitialized = true;
  StartWatchdogIfNeeded();
  MarkProgress("SDL_Init", "exit");
  // VEXA_FIXES: SDL2 SDL_Init returns 0 on success (not 1 like SDL3).
  return 0;
}

static void fexfn_impl_libSDL2_FEX_SDL2_Quit() {
  MarkProgress("SDL_Quit", "enter");
  SDL2_LOGI("SDL_Quit()");
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

static SDL_Window* fexfn_impl_libSDL2_FEX_SDL2_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags) {
  SDL2_LOGI("SDL_CreateWindow(title=%s x=%d y=%d w=%d h=%d flags=0x%x)",
            title ? title : "(null)",
            x,
            y,
            w,
            h,
            static_cast<unsigned>(flags));
  auto* window = new VexaSDL2Window {
    .X = x,
    .Y = y,
    .Width = w,
    .Height = h,
    .Flags = flags,
    .WindowID = NextWindowID.fetch_add(1),
    .Title = title ? title : "",
  };
  SDL2_LOGI("SDL_CreateWindow => %p id=%u", window, window->WindowID);

  // VEXA_FIXES: Synthesize SDL2 window visibility events; the host drawable is
  // shown as soon as the guest creates it unless SDL_WINDOW_HIDDEN was passed.
  if ((flags & SDL_WINDOW_HIDDEN) == 0) {
    PushWindowEvent(SDL_WINDOWEVENT_SHOWN, 0, 0, window->WindowID);
  }
  return reinterpret_cast<SDL_Window*>(window);
}

static void fexfn_impl_libSDL2_FEX_SDL2_DestroyWindow(SDL_Window* window) {
  SDL2_LOGI("SDL_DestroyWindow(window=%p)", window);
  delete reinterpret_cast<VexaSDL2Window*>(window);
}

static void fexfn_impl_libSDL2_FEX_SDL2_GetWindowSize(SDL_Window* window, int* w, int* h) {
  SDL2_LOGI("SDL_GetWindowSize(window=%p)", window);
  auto* host_window = reinterpret_cast<VexaSDL2Window*>(window);
  if (!host_window) {
    SDL2_LOGW("SDL_GetWindowSize failed: null window");
    return;
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
  SDL2_LOGI("SDL_GetWindowSize => w=%d h=%d", host_window->Width, host_window->Height);
}

static SDL_GLContext fexfn_impl_libSDL2_FEX_SDL2_GL_CreateContext(SDL_Window*) {
  LogEGLState("SDL_GL_CreateContext(enter)");
  VexaTraceThunkEvent("SDL_GL_CreateContext", VEXA_THUNK_EVENT_ENTER, kThunkSDLGLCreateContext,
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Display)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Surface)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Context)));
  if (!EnsureEGLContextReady()) {
    SetSDLContextReadyState(false, "SDL_GL_CreateContext");
    SDL2_LOGW("SDL_GL_CreateContext failed: no EGL context is available");
    VexaTraceThunkEvent("SDL_GL_CreateContext", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLCreateContext, 0, 0, 0, 0, 0, 0);
    return nullptr;
  }
  if (!IsManagedEGLContextReady()) {
    SDL2_LOGE("SDL_GL_CreateContext abort: managed context not ready after Ensure");
    SetSDLContextReadyState(false, "SDL_GL_CreateContext");
    return nullptr;
  }

  SetSDLContextReadyState(true, "SDL_GL_CreateContext");
  g_GLCreateOwnerTid = 0;
  SDL2_LOGI("SDL_GL_CreateContext created SDL2 thunk-managed EGL context %p",
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

static int fexfn_impl_libSDL2_FEX_SDL2_GL_MakeCurrent(SDL_Window*, SDL_GLContext context) {
  LogEGLState("SDL_GL_MakeCurrent(enter)");
  VexaTraceThunkEvent("SDL_GL_MakeCurrent", VEXA_THUNK_EVENT_ENTER, kThunkSDLGLMakeCurrent,
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(context)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Display)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Surface)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Context)),
                          static_cast<uint64_t>(g_GLCreateOwnerTid));

  if (context == nullptr) {
    // VEXA_FIXES: SDL2 SDL_GL_MakeCurrent returns 0 on success, -1 on error.
    int result = 0;
    EGLint error = EGL_SUCCESS;
    if (ManagedEGLActive &&
        CurrentEGL.Display != EGL_NO_DISPLAY &&
        CurrentEGL.Context != EGL_NO_CONTEXT) {
      if (eglMakeCurrent(CurrentEGL.Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
        error = eglGetError();
        SDL2_LOGE("SDL_GL_MakeCurrent(NULL) failed: 0x%x", error);
        result = -1;
      }
    }

    if (result == 0) {
      SetSDLContextReadyState(false, "SDL_GL_MakeCurrent(null)");
      g_GLCreateOwnerTid = 0;
    }
    LogEGLState("SDL_GL_MakeCurrent(exit-null)");

    VexaTraceThunkEvent("SDL_GL_MakeCurrent", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLMakeCurrent,
                            static_cast<uint64_t>(static_cast<uint32_t>(result)),
                            static_cast<uint64_t>(error),
                            static_cast<uint64_t>(g_GLCreateOwnerTid), 0, 0, 0);
    return result;
  }

  if (reinterpret_cast<EGLContext>(context) != CurrentEGL.Context) {
    SDL2_LOGW("SDL_GL_MakeCurrent rejected unknown context %p (current=%p)",
              context,
              CurrentEGL.Context);
    VexaTraceThunkEvent("SDL_GL_MakeCurrent", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLMakeCurrent,
                            0, 0, 0, 0, 0, 0);
    return -1;
  }

  if (!EnsureEGLContextReady()) {
    SetSDLContextReadyState(false, "SDL_GL_MakeCurrent");
    VexaTraceThunkEvent("SDL_GL_MakeCurrent", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLMakeCurrent,
                            0, 0, 0, 0, 0, 0);
    return -1;
  }
  if (!IsManagedEGLContextReady()) {
    SDL2_LOGW("SDL_GL_MakeCurrent abort: EGL context invalid after Ensure");
    SetSDLContextReadyState(false, "SDL_GL_MakeCurrent");
    VexaTraceThunkEvent("SDL_GL_MakeCurrent", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLMakeCurrent,
                        0, 0, 0, 0, 0, 0);
    return -1;
  }

  if (eglMakeCurrent(CurrentEGL.Display, CurrentEGL.Surface, CurrentEGL.Surface, CurrentEGL.Context) != EGL_TRUE) {
    const EGLint error = eglGetError();
    SDL2_LOGE("SDL_GL_MakeCurrent failed: 0x%x", error);
    SetSDLContextReadyState(false, "SDL_GL_MakeCurrent");
    VexaTraceThunkEvent("SDL_GL_MakeCurrent", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLMakeCurrent,
                            0,
                            static_cast<uint64_t>(error),
                            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Display)),
                            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Surface)),
                            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Context)),
                            0);
    return -1;
  }

  SetSDLContextReadyState(true, "SDL_GL_MakeCurrent");
  g_GLCreateOwnerTid = CurrentThreadToken();
  SDL2_LOGI("SDL_GL_MakeCurrent bound EGL context on owner=%lu", g_GLCreateOwnerTid);
  LogEGLState("SDL_GL_MakeCurrent(exit)");
  VexaTraceThunkEvent("SDL_GL_MakeCurrent", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLMakeCurrent,
                          1,
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Display)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Surface)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Context)),
                          static_cast<uint64_t>(g_GLCreateOwnerTid),
                          0);
  return 0;
}

static void fexfn_impl_libSDL2_FEX_SDL2_GL_DeleteContext(SDL_GLContext) {
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

static int fexfn_impl_libSDL2_FEX_SDL2_GL_SetSwapInterval(int interval) {
  LogEGLState("SDL_GL_SetSwapInterval(enter)");
  VexaTraceThunkEvent("SDL_GL_SetSwapInterval", VEXA_THUNK_EVENT_ENTER, kThunkSDLGLSetSwapInterval,
                          static_cast<uint64_t>(interval),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Display)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Surface)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Context)));
  if (!ValidateGLOwnerThread("SDL_GL_SetSwapInterval")) {
    VexaTraceThunkEvent("SDL_GL_SetSwapInterval", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLSetSwapInterval, 0, 0, 0, 0, 0, 0);
    return -1;
  }

  if (!EnsureEGLContextReady()) {
    VexaTraceThunkEvent("SDL_GL_SetSwapInterval", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLSetSwapInterval, 0, 0, 0, 0, 0, 0);
    return -1;
  }
  if (!IsManagedEGLContextReady()) {
    SDL2_LOGW("SDL_GL_SetSwapInterval abort: EGL context invalid");
    return -1;
  }

  // VEXA_FIXES: SDL2 SDL_GL_SetSwapInterval returns 0 on success, -1 on error.
  const int result = eglSwapInterval(CurrentEGL.Display, interval) == EGL_TRUE ? 0 : -1;
  SDL2_LOGI("SDL_GL_SetSwapInterval interval=%d result=%d egl=0x%x", interval, result, eglGetError());
  VexaTraceThunkEvent("SDL_GL_SetSwapInterval", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLSetSwapInterval,
                          static_cast<uint64_t>(static_cast<uint32_t>(result)),
                          static_cast<uint64_t>(eglGetError()),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Display)));
  return result;
}

static void fexfn_impl_libSDL2_FEX_SDL2_GL_SwapWindow(SDL_Window*) {
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
    return;
  }

  if (!EnsureEGLContextReady()) {
    MarkProgress("SDL_GL_SwapWindow", "egl-not-ready");
    VexaTraceThunkEvent("SDL_GL_SwapWindow", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLSwapWindow, 0, 0, 0, 0, 0, 0);
    return;
  }
  if (!IsManagedEGLContextReady()) {
    MarkProgress("SDL_GL_SwapWindow", "invalid-egl-state");
    SDL2_LOGW("SDL_GL_SwapWindow abort: EGL state invalid (managed=%d display=%p surface=%p context=%p config=%p native=%p %dx%d)",
              ManagedEGLActive ? 1 : 0,
              reinterpret_cast<void*>(CurrentEGL.Display),
              reinterpret_cast<void*>(CurrentEGL.Surface),
              reinterpret_cast<void*>(CurrentEGL.Context),
              reinterpret_cast<void*>(CurrentEGL.Config),
              reinterpret_cast<void*>(CurrentEGL.NativeWindow),
              CurrentEGL.Width,
              CurrentEGL.Height);
    VexaTraceThunkEvent("SDL_GL_SwapWindow", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLSwapWindow, 0, 0, 0, 0, 0, 0);
    return;
  }

  if (!MaybeHandoffRuntimeSurfaceBeforeSwap()) {
    MarkProgress("SDL_GL_SwapWindow", "surface-handoff-failed");
    VexaTraceThunkEvent("SDL_GL_SwapWindow", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLSwapWindow, 0, 0, 0, 0, 0, 0);
    return;
  }

  int result = 0;
  EGLint error = EGL_SUCCESS;
  EGLint swap_width = 0;
  EGLint swap_height = 0;
  if (CurrentEGL.Display != EGL_NO_DISPLAY && CurrentEGL.Surface != EGL_NO_SURFACE) {
    eglQuerySurface(CurrentEGL.Display, CurrentEGL.Surface, EGL_WIDTH, &swap_width);
    eglQuerySurface(CurrentEGL.Display, CurrentEGL.Surface, EGL_HEIGHT, &swap_height);
  }
  SDL2_LOGI("MAGENTA-PROBE pre-swap native_window=%p egl_surface_size=%dx%d logical=%dx%d",
            CurrentEGL.NativeWindow,
            static_cast<int>(swap_width),
            static_cast<int>(swap_height),
            CurrentEGL.Width,
            CurrentEGL.Height);
  SDL2_LOGI("Swap target native_window=%p size=%dx%d",
            CurrentEGL.NativeWindow,
            CurrentEGL.Width,
            CurrentEGL.Height);
  if (CurrentEGL.NativeWindow == nullptr || swap_width <= 1 || swap_height <= 1) {
    SDL2_LOGW("MAGENTA-PROBE likely offscreen/native-window-missing native_window=%p egl_surface_size=%dx%d",
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
      SDL2_LOGW("SDL_GL_SwapWindow saw EGL_BAD_SURFACE; rebuilding surface");
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
      SDL2_LOGI("EGL-VERIFY first-swap-ok: native_window=%p egl_size=%dx%d",
                CurrentEGL.NativeWindow,
                static_cast<int>(post_w),
                static_cast<int>(post_h));
    }
  }

  SDL2_LOGI("SDL_GL_SwapWindow result=%d egl=0x%x", result, error);
  if (result == 0) {
    SetSDLContextReadyState(false, "SDL_GL_SwapWindow");
  }
  MarkProgress("SDL_GL_SwapWindow", "exit");
  VexaTraceThunkEvent("SDL_GL_SwapWindow", VEXA_THUNK_EVENT_EXIT, kThunkSDLGLSwapWindow,
                          static_cast<uint64_t>(result),
                          static_cast<uint64_t>(error),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Display)),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CurrentEGL.Surface)));
}

static int fexfn_impl_libSDL2_FEX_SDL2_PollEvent(void* event) {
  const uint64_t t0 = GetTimestampNS();
  MarkProgress("SDL_PollEvent", "enter");
  std::scoped_lock lock {EventQueueMutex};
  SDL2_LOGI("SDL_PollEvent(event=%p queue=%zu)", event, EventQueue.size());
  if (EventQueue.empty()) {
    MarkProgress("SDL_PollEvent", "empty");
    LogSlowCall("SDL_PollEvent", GetTimestampNS() - t0);
    return 0;
  }

  auto queued = std::move(EventQueue.front());
  EventQueue.pop_front();

  if (event) {
    // VEXA_FIXES: SDL_Event is a 56-byte union in SDL2 on 64-bit targets.
    static_assert(sizeof(SDL_Event) == 56, "SDL2 host requires the 56-byte SDL_Event layout");
    std::memcpy(event, &queued.Event, sizeof(SDL_Event));
  }
  SDL2_LOGI("SDL_PollEvent => type=0x%x remaining=%zu",
            static_cast<unsigned>(queued.Event.type),
            EventQueue.size());
  MarkProgress("SDL_PollEvent", "event");
  LogSlowCall("SDL_PollEvent", GetTimestampNS() - t0);
  return 1;
}

static Uint32 fexfn_impl_libSDL2_FEX_SDL2_GetTicks() {
  return static_cast<Uint32>(GetTimestampNS() / 1000000ULL);
}

static Uint64 fexfn_impl_libSDL2_FEX_SDL2_GetTicks64() {
  return GetTimestampNS() / 1000000ULL;
}

static void fexfn_impl_libSDL2_FEX_SDL2_Delay(Uint32 ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static int fexfn_impl_libSDL2_FEX_SDL2_StartTextInput() {
  TextInputActiveFlag = true;
  return 1;
}

static void fexfn_impl_libSDL2_FEX_SDL2_StopTextInput() {
  TextInputActiveFlag = false;
}

static SDL_bool fexfn_impl_libSDL2_FEX_SDL2_IsTextInputActive() {
  return TextInputActiveFlag.load() ? SDL_TRUE : SDL_FALSE;
}

static SDL_Surface* fexfn_impl_libSDL2_FEX_SDL2_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
                                                               Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {
  SDL2_LOGI("SDL_CreateRGBSurface(flags=0x%x %dx%d depth=%d masks=%08x/%08x/%08x/%08x)",
            static_cast<unsigned>(flags), width, height, depth, Rmask, Gmask, Bmask, Amask);
  return VexaCreateRGBSurface(flags, width, height, depth, Rmask, Gmask, Bmask, Amask);
}

static SDL_Surface* fexfn_impl_libSDL2_FEX_SDL2_CreateRGBSurfaceFrom(void* pixels, int width, int height, int depth, int pitch,
                                                                   Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {
  SDL2_LOGI("SDL_CreateRGBSurfaceFrom(pixels=%p %dx%d depth=%d pitch=%d masks=%08x/%08x/%08x/%08x)",
            pixels, width, height, depth, pitch, Rmask, Gmask, Bmask, Amask);
  return VexaCreateRGBSurfaceFrom(pixels, width, height, depth, pitch, Rmask, Gmask, Bmask, Amask);
}

static void fexfn_impl_libSDL2_FEX_SDL2_FreeSurface(SDL_Surface* surface) {
  SDL2_LOGI("SDL_FreeSurface(surface=%p)", surface);
  VexaFreeSurface(surface);
}

static int fexfn_impl_libSDL2_FEX_SDL2_UpperBlitScaled(SDL_Surface* src, const SDL_Rect* srcrect,
                                                     SDL_Surface* dst, SDL_Rect* dstrect) {
  SDL2_LOGI("SDL_UpperBlitScaled(src=%p dst=%p)", src, dst);
  return VexaUpperBlitScaled(src, srcrect, dst, dstrect);
}

static int fexfn_impl_libSDL2_FEX_SDL2_SetSurfaceBlendMode(SDL_Surface* surface, SDL_BlendMode blendMode) {
  SDL2_LOGI("SDL_SetSurfaceBlendMode(surface=%p mode=%d)", surface, static_cast<int>(blendMode));
  return VexaSetSurfaceBlendMode(surface, blendMode);
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
    SDL2_LOGW("Cursor bridge using synthetic mode (host SDL cursor calls bypassed)");
  }
}
} // namespace

static SDL_Cursor* fexfn_impl_libSDL2_FEX_SDL2_CreateColorCursor(SDL_Surface* surface, int hot_x, int hot_y) {
  (void)surface;
  (void)hot_x;
  (void)hot_y;
  LogSyntheticCursorBridgeOnce();
  return GetSyntheticCursorHandle();
}

static SDL_Cursor* fexfn_impl_libSDL2_FEX_SDL2_CreateSystemCursor(SDL_SystemCursor id) {
  (void)id;
  LogSyntheticCursorBridgeOnce();
  return GetSyntheticCursorHandle();
}

static void fexfn_impl_libSDL2_FEX_SDL2_SetCursor(SDL_Cursor* cursor) {
  LogSyntheticCursorBridgeOnce();
  uintptr_t token = reinterpret_cast<uintptr_t>(cursor);
  if (token == 0) {
    token = kSyntheticCursorToken;
  }
  CurrentSyntheticCursor.store(token, std::memory_order_release);
}

static SDL_Cursor* fexfn_impl_libSDL2_FEX_SDL2_GetCursor() {
  LogSyntheticCursorBridgeOnce();
  uintptr_t token = CurrentSyntheticCursor.load(std::memory_order_acquire);
  if (token == 0) {
    token = kSyntheticCursorToken;
  }
  return reinterpret_cast<SDL_Cursor*>(token);
}

static SDL_Cursor* fexfn_impl_libSDL2_FEX_SDL2_GetDefaultCursor() {
  LogSyntheticCursorBridgeOnce();
  return GetSyntheticCursorHandle();
}

static void fexfn_impl_libSDL2_FEX_SDL2_FreeCursor(SDL_Cursor* cursor) {
  LogSyntheticCursorBridgeOnce();
  const uintptr_t token = reinterpret_cast<uintptr_t>(cursor);
  if (token != 0 && token == CurrentSyntheticCursor.load(std::memory_order_acquire)) {
    CurrentSyntheticCursor.store(kSyntheticCursorToken, std::memory_order_release);
  }
}

static int fexfn_impl_libSDL2_FEX_SDL2_ShowCursor(int toggle) {
  LogSyntheticCursorBridgeOnce();
  // VEXA_FIXES: SDL2 SDL_ShowCursor applies the toggle (SDL_QUERY=-1 skips it)
  // and returns the *previous* visibility state.
  const bool previously_shown = SyntheticCursorVisible.load(std::memory_order_acquire);
  if (toggle >= 0) {
    SyntheticCursorVisible.store(toggle != 0, std::memory_order_release);
  }
  return previously_shown ? 1 : 0;
}

// VEXA_FIXES: Export explicit FEX_SDL2_* bridge symbols so fexldr dlsym() succeeds
#define FEX_SDL2_EXPORT_RET(ret, name, args, callargs) \
  extern "C" __attribute__((visibility("default"), used)) ret FEX_SDL2_##name args { \
    return fexfn_impl_libSDL2_FEX_SDL2_##name callargs; \
  }

#define FEX_SDL2_EXPORT_VOID(name, args, callargs) \
  extern "C" __attribute__((visibility("default"), used)) void FEX_SDL2_##name args { \
    fexfn_impl_libSDL2_FEX_SDL2_##name callargs; \
  }

FEX_SDL2_EXPORT_RET(int, Init, (Uint32 flags), (flags));
FEX_SDL2_EXPORT_VOID(Quit, (), ());
FEX_SDL2_EXPORT_RET(SDL_Window*, CreateWindow, (const char* title, int x, int y, int w, int h, Uint32 flags), (title, x, y, w, h, flags));
FEX_SDL2_EXPORT_VOID(DestroyWindow, (SDL_Window* window), (window));
FEX_SDL2_EXPORT_VOID(GetWindowSize, (SDL_Window* window, int* w, int* h), (window, w, h));

FEX_SDL2_EXPORT_RET(SDL_GLContext, GL_CreateContext, (SDL_Window* window), (window));
FEX_SDL2_EXPORT_RET(int, GL_MakeCurrent, (SDL_Window* window, SDL_GLContext context), (window, context));
FEX_SDL2_EXPORT_VOID(GL_DeleteContext, (SDL_GLContext context), (context));
FEX_SDL2_EXPORT_RET(int, GL_SetSwapInterval, (int interval), (interval));
FEX_SDL2_EXPORT_VOID(GL_SwapWindow, (SDL_Window* window), (window));

FEX_SDL2_EXPORT_RET(int, PollEvent, (void* event), (event));
FEX_SDL2_EXPORT_RET(Uint32, GetTicks, (), ());
FEX_SDL2_EXPORT_RET(Uint64, GetTicks64, (), ());
FEX_SDL2_EXPORT_VOID(Delay, (Uint32 ms), (ms));
FEX_SDL2_EXPORT_RET(int, StartTextInput, (), ());
FEX_SDL2_EXPORT_VOID(StopTextInput, (), ());
FEX_SDL2_EXPORT_RET(SDL_bool, IsTextInputActive, (), ());

FEX_SDL2_EXPORT_RET(SDL_Surface*, CreateRGBSurface, (Uint32 flags, int width, int height, int depth, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask), (flags, width, height, depth, Rmask, Gmask, Bmask, Amask));
FEX_SDL2_EXPORT_RET(SDL_Surface*, CreateRGBSurfaceFrom, (void* pixels, int width, int height, int depth, int pitch, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask), (pixels, width, height, depth, pitch, Rmask, Gmask, Bmask, Amask));
FEX_SDL2_EXPORT_VOID(FreeSurface, (SDL_Surface* surface), (surface));
FEX_SDL2_EXPORT_RET(int, UpperBlitScaled, (SDL_Surface* src, const SDL_Rect* srcrect, SDL_Surface* dst, SDL_Rect* dstrect), (src, srcrect, dst, dstrect));
FEX_SDL2_EXPORT_RET(int, SetSurfaceBlendMode, (SDL_Surface* surface, SDL_BlendMode blendMode), (surface, blendMode));

FEX_SDL2_EXPORT_RET(SDL_Cursor*, CreateColorCursor, (SDL_Surface* surface, int hot_x, int hot_y), (surface, hot_x, hot_y));
FEX_SDL2_EXPORT_RET(SDL_Cursor*, CreateSystemCursor, (SDL_SystemCursor id), (id));
FEX_SDL2_EXPORT_VOID(SetCursor, (SDL_Cursor* cursor), (cursor));
FEX_SDL2_EXPORT_RET(SDL_Cursor*, GetCursor, (), ());
FEX_SDL2_EXPORT_RET(SDL_Cursor*, GetDefaultCursor, (), ());
FEX_SDL2_EXPORT_VOID(FreeCursor, (SDL_Cursor* cursor), (cursor));
FEX_SDL2_EXPORT_RET(int, ShowCursor, (int toggle), (toggle));

#undef FEX_SDL2_EXPORT_VOID
#undef FEX_SDL2_EXPORT_RET
EXPORTS(libSDL2)
