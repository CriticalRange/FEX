/*
$info$
tags: thunklibs|SDL3
desc: Android SDL3 host thunk for window/event/GL context bridging
$end_info$
*/

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <EGL/egl.h>
#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>

#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "common/Host.h"

#include "thunkgen_host_libSDL3.inl"

namespace {
constexpr const char* LOG_TAG = "HyMobile-SDL3Thunk";
constexpr const char* SDLContextReadyFlagPath = "/sdcard/HyMobile.Android/sdl_context_ready.flag";

#define SDL3_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define SDL3_LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define SDL3_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct HyMobileSDL3Window {
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
  int Width {};
  int Height {};
};

EGLState CurrentEGL;
bool ManagedEGLActive = false;

Uint64 GetTimestampNS() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
           std::chrono::steady_clock::now().time_since_epoch())
    .count();
}

void* ResolveRuntimeSymbol(const char* name) {
  if (void* symbol = dlsym(RTLD_DEFAULT, name); symbol) {
    return symbol;
  }

  static void* runtime_handle = nullptr;
  static bool attempted_runtime_open = false;
  static bool missing_runtime_logged = false;
  if (!attempted_runtime_open) {
    attempted_runtime_open = true;
    runtime_handle = dlopen("libhymobile_fexcore.so", RTLD_NOW | RTLD_NOLOAD);
    if (!runtime_handle) {
      runtime_handle = dlopen("libhymobile_fexcore.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!runtime_handle && !missing_runtime_logged) {
      missing_runtime_logged = true;
      SDL3_LOGW("Could not open libhymobile_fexcore.so while resolving runtime symbols");
    }
  }

  if (!runtime_handle) {
    return nullptr;
  }

  return dlsym(runtime_handle, name);
}

void SetSDLContextReadyState(bool ready, const char* source) {
  static bool missing_slot_logged = false;
  auto* context_created_slot =
    reinterpret_cast<int*>(ResolveRuntimeSymbol("g_HyMobile_SDLContextCreated"));
  if (context_created_slot) {
    *context_created_slot = ready ? 1 : 0;
  } else if (!missing_slot_logged) {
    missing_slot_logged = true;
    SDL3_LOGW("%s: g_HyMobile_SDLContextCreated is unavailable", source);
  }

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

  std::remove(SDLContextReadyFlagPath);
}

ANativeWindow* ResolveNativeWindowFromRuntime(int* width, int* height) {
  auto* window_slot = reinterpret_cast<ANativeWindow**>(ResolveRuntimeSymbol("g_HyMobile_NativeWindow"));
  auto* ready = reinterpret_cast<int*>(ResolveRuntimeSymbol("g_HyMobile_NativeWindowReady"));
  auto* native_width = reinterpret_cast<int*>(ResolveRuntimeSymbol("g_HyMobile_NativeWindowWidth"));
  auto* native_height = reinterpret_cast<int*>(ResolveRuntimeSymbol("g_HyMobile_NativeWindowHeight"));

  if (!window_slot || !ready) {
    static bool missing_symbols_logged = false;
    if (!missing_symbols_logged) {
      missing_symbols_logged = true;
      SDL3_LOGW("Runtime native-window symbols are unavailable (window_slot=%p ready=%p)",
                window_slot, ready);
    }
    return nullptr;
  }

  if (!*window_slot || *ready == 0) {
    return nullptr;
  }

  if (width) {
    *width = native_width ? *native_width : 0;
  }
  if (height) {
    *height = native_height ? *native_height : 0;
  }
  return *window_slot;
}

void DestroyManagedEGLContext() {
  if (!ManagedEGLActive) {
    return;
  }

  if (CurrentEGL.Display != EGL_NO_DISPLAY) {
    eglMakeCurrent(CurrentEGL.Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (CurrentEGL.Context != EGL_NO_CONTEXT) {
      eglDestroyContext(CurrentEGL.Display, CurrentEGL.Context);
    }
    if (CurrentEGL.Surface != EGL_NO_SURFACE) {
      eglDestroySurface(CurrentEGL.Display, CurrentEGL.Surface);
    }
  }

  ManagedEGLActive = false;
  CurrentEGL = EGLState {};
}

bool TryCreateManagedEGLContext() {
  int native_width = 0;
  int native_height = 0;
  constexpr int NativeWindowPollMs = 20;
  constexpr int NativeWindowTimeoutMs = 2000;
  bool waited_for_window = false;
  ANativeWindow* window = nullptr;
  for (int waited_ms = 0; waited_ms <= NativeWindowTimeoutMs; waited_ms += NativeWindowPollMs) {
    window = ResolveNativeWindowFromRuntime(&native_width, &native_height);
    if (window) {
      break;
    }

    if (!waited_for_window) {
      waited_for_window = true;
      SDL3_LOGW("Managed EGL fallback waiting for native window readiness");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(NativeWindowPollMs));
  }
  if (!window) {
    SDL3_LOGW("Managed EGL creation skipped: native window is not ready after %dms",
              NativeWindowTimeoutMs);
    return false;
  }
  if (native_width <= 0 || native_height <= 0) {
    native_width = ANativeWindow_getWidth(window);
    native_height = ANativeWindow_getHeight(window);
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

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif
  EGLConfig config = nullptr;
  EGLint config_count = 0;
  const EGLint config_attributes_es3[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 16,
    EGL_NONE,
  };
  if (eglChooseConfig(display, config_attributes_es3, &config, 1, &config_count) != EGL_TRUE || config_count == 0) {
    const EGLint config_attributes_es2[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_DEPTH_SIZE, 16,
      EGL_NONE,
    };
    if (eglChooseConfig(display, config_attributes_es2, &config, 1, &config_count) != EGL_TRUE || config_count == 0) {
      SDL3_LOGE("eglChooseConfig failed: 0x%x", eglGetError());
      return false;
    }
  }

  ANativeWindow_acquire(window);
  EGLSurface surface = eglCreateWindowSurface(display, config, window, nullptr);
  ANativeWindow_release(window);
  if (surface == EGL_NO_SURFACE) {
    SDL3_LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
    return false;
  }

  EGLContext context = EGL_NO_CONTEXT;
  const EGLint context_attributes_es3[] = {
    EGL_CONTEXT_CLIENT_VERSION, 3,
    EGL_NONE,
  };
  context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes_es3);

  if (context == EGL_NO_CONTEXT) {
    const EGLint context_attributes_es2[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE,
    };
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes_es2);
  }

  if (context == EGL_NO_CONTEXT) {
    SDL3_LOGE("eglCreateContext failed: 0x%x", eglGetError());
    eglDestroySurface(display, surface);
    return false;
  }

  if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
    SDL3_LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    return false;
  }

  CurrentEGL.Display = display;
  CurrentEGL.Surface = surface;
  CurrentEGL.Context = context;
  CurrentEGL.Width = native_width;
  CurrentEGL.Height = native_height;
  ManagedEGLActive = true;

  SDL3_LOGI("Managed EGL context created (%dx%d, EGL %d.%d)",
            CurrentEGL.Width, CurrentEGL.Height, major, minor);
  return true;
}

bool CaptureCurrentEGLContext() {
  CurrentEGL.Display = eglGetCurrentDisplay();
  CurrentEGL.Surface = eglGetCurrentSurface(EGL_DRAW);
  CurrentEGL.Context = eglGetCurrentContext();

  return CurrentEGL.Display != EGL_NO_DISPLAY &&
         CurrentEGL.Surface != EGL_NO_SURFACE &&
         CurrentEGL.Context != EGL_NO_CONTEXT;
}

bool EnsureEGLContextReady() {
  if (CaptureCurrentEGLContext()) {
    return true;
  }

  SDL3_LOGW("No current EGL context; falling back to managed EGL context path");

  if (ManagedEGLActive) {
    if (CurrentEGL.Display != EGL_NO_DISPLAY &&
        CurrentEGL.Surface != EGL_NO_SURFACE &&
        CurrentEGL.Context != EGL_NO_CONTEXT &&
        eglMakeCurrent(CurrentEGL.Display, CurrentEGL.Surface, CurrentEGL.Surface, CurrentEGL.Context) == EGL_TRUE &&
        CaptureCurrentEGLContext()) {
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

extern "C" __attribute__((visibility("default"))) void HyMobile_Thunks_UseExternalEGLContext(int width, int height) {
  if (CaptureCurrentEGLContext()) {
    CurrentEGL.Width = width;
    CurrentEGL.Height = height;
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

static int fexfn_impl_libSDL3_FEX_SDL_Init(SDL_InitFlags) {
  g_GLCreateOwnerTid = 0;
  SetSDLContextReadyState(false, "SDL_Init");
  SDLInitialized = true;
  return 1;
}

static void fexfn_impl_libSDL3_FEX_SDL_Quit() {
  g_GLCreateOwnerTid = 0;
  SetSDLContextReadyState(false, "SDL_Quit");
  DestroyManagedEGLContext();
  SDLInitialized = false;
  TextInputActiveFlag = false;

  std::scoped_lock lock {EventQueueMutex};
  EventQueue.clear();
}

static SDL_Window* fexfn_impl_libSDL3_FEX_SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags) {
  auto* window = new HyMobileSDL3Window {
    .Width = w,
    .Height = h,
    .Flags = flags,
    .WindowID = NextWindowID.fetch_add(1),
    .Title = title ? title : "",
  };
  return reinterpret_cast<SDL_Window*>(window);
}

static void fexfn_impl_libSDL3_FEX_SDL_DestroyWindow(SDL_Window* window) {
  delete reinterpret_cast<HyMobileSDL3Window*>(window);
}

static int fexfn_impl_libSDL3_FEX_SDL_GetWindowSize(SDL_Window* window, int* w, int* h) {
  auto* host_window = reinterpret_cast<HyMobileSDL3Window*>(window);
  if (!host_window) {
    return 0;
  }

  if (w) {
    *w = host_window->Width;
  }
  if (h) {
    *h = host_window->Height;
  }
  return 1;
}

static SDL_GLContext fexfn_impl_libSDL3_FEX_SDL_GL_CreateContext(SDL_Window*) {
  if (!EnsureEGLContextReady()) {
    SetSDLContextReadyState(false, "SDL_GL_CreateContext");
    SDL3_LOGW("SDL_GL_CreateContext failed: no EGL context is available");
    return nullptr;
  }

  SetSDLContextReadyState(true, "SDL_GL_CreateContext");
  g_GLCreateOwnerTid = CurrentThreadToken();
  SDL3_LOGI("SDL_GL_CreateContext succeeded with SDL3 thunk-managed EGL (owner=%lu)",
            g_GLCreateOwnerTid);
  return reinterpret_cast<SDL_GLContext>(CurrentEGL.Context);
}

static void fexfn_impl_libSDL3_FEX_SDL_GL_DeleteContext(SDL_GLContext) {
  if (!ValidateGLOwnerThread("SDL_GL_DeleteContext")) {
    return;
  }

  SetSDLContextReadyState(false, "SDL_GL_DeleteContext");
  DestroyManagedEGLContext();
  g_GLCreateOwnerTid = 0;
}

static int fexfn_impl_libSDL3_FEX_SDL_GL_SetSwapInterval(int interval) {
  if (!ValidateGLOwnerThread("SDL_GL_SetSwapInterval")) {
    return 0;
  }

  if (!EnsureEGLContextReady()) {
    return 0;
  }

  return eglSwapInterval(CurrentEGL.Display, interval) == EGL_TRUE ? 1 : 0;
}

static int fexfn_impl_libSDL3_FEX_SDL_GL_SwapWindow(SDL_Window*) {
  if (!ValidateGLOwnerThread("SDL_GL_SwapWindow")) {
    return 0;
  }

  if (!EnsureEGLContextReady()) {
    return 0;
  }

  return eglSwapBuffers(CurrentEGL.Display, CurrentEGL.Surface) == EGL_TRUE ? 1 : 0;
}

static int fexfn_impl_libSDL3_FEX_SDL_PollEvent(void* event) {
  std::scoped_lock lock {EventQueueMutex};
  if (EventQueue.empty()) {
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

EXPORTS(libSDL3)
