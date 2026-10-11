/*
$info$
tags: thunklibs|SDL2
desc: Android SDL2 guest thunk with local GL proc lookup and guest-side window/event/surface state
$end_info$
*/

#define GL_GLEXT_PROTOTYPES 1

// VEXA SDL2 headers are the flat release-2.32.x include dir (VEXA_SDL2_PREFIX/include).
#include "SDL.h"
#include "SDL_syswm.h"

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
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
}

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/Guest.h"

#if defined(BUILD_ANDROID)
static bool VexaSDL2Verbose() {
  static const bool verbose = [] {
    const char* v = getenv("VEXA_SDL2_TRACE");
    return v && v[0] && v[0] != '0';
  }();
  return verbose;
}

static int VexaSDL2GuestVfprintf(FILE* stream, const char* fmt, va_list args) {
  if (stream == stderr && !VexaSDL2Verbose() && std::strncmp(fmt, "[SDL2 thunk]", 12) == 0) {
    return 0;
  }
  va_list copy;
  va_copy(copy, args);
  const int rc = vfprintf(stream, fmt, args);
  if (stream == stderr) {
#if VEXA_HAS_ANDROID_LOG
    __android_log_vprint(ANDROID_LOG_ERROR, "Vexa-SDL2Thunk", fmt, copy);
#endif
  }
  va_end(copy);
  return rc;
}

static int VexaSDL2GuestFprintf(FILE* stream, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const int rc = VexaSDL2GuestVfprintf(stream, fmt, args);
  va_end(args);
  return rc;
}

#define fprintf(stream, ...) VexaSDL2GuestFprintf(stream, __VA_ARGS__)
#define vfprintf(stream, fmt, args) VexaSDL2GuestVfprintf(stream, fmt, args)

// VEXA_FIXES: dump a guest backtrace on fatal signals so heap aborts can be
// attributed to a library without a debugger attached.
#include <execinfo.h>
#include <signal.h>

__attribute__((constructor)) static void VexaSDL2InstallCrashHandler() {
  struct sigaction sa {};
  sa.sa_handler = [](int sig) {
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, n, STDERR_FILENO);
    ::signal(sig, SIG_DFL);
    ::raise(sig);
  };
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND;
  sigaction(SIGABRT, &sa, nullptr);
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
}
#endif

#include "thunkgen_guest_libSDL2.inl"

namespace {
// VEXA_FIXES: Keep SDL thunk GL proc diagnostics in app-internal artifacts storage.
constexpr const char* SDL2GLProcTracePath = "/data/user/0/com.critical.vexaemulator/files/artifacts/gl_proc_trace_sdl2.log";

// ---------------------------------------------------------------------------
// Init / subsystem state (SDL2 refcounts each subsystem flag)
// ---------------------------------------------------------------------------
std::mutex InitStateMutex;
std::array<int, 32> SubsystemRefCounts {};
Uint32 InitializedSubsystemMask = 0;

// Internal keyboard modifier tracking for synthesized key events. The public
// SDL_GetModState/SDL_SetModState exports live in the FNA stubs file.
std::atomic<Uint16> CurrentModState {KMOD_NONE};

// ---------------------------------------------------------------------------
// Guest-side window state
// ---------------------------------------------------------------------------
std::atomic<int> WindowWidth {1920};
std::atomic<int> WindowHeight {1080};
std::atomic<int> WindowPosX {0};
std::atomic<int> WindowPosY {0};
std::atomic<int> WindowMinWidth {0};
std::atomic<int> WindowMinHeight {0};
std::atomic<int> WindowMaxWidth {0};
std::atomic<int> WindowMaxHeight {0};
std::atomic<uintptr_t> PrimaryWindowPointer {0};
std::atomic<uintptr_t> WindowSurfacePointer {0};
std::atomic<Uint32> WindowFlagsState {0};
std::mutex WindowStateMutex;
std::mutex WindowTitleMutex;
std::mutex SyntheticEventQueueMutex;
std::deque<SDL_Event> SyntheticEventQueue;
// VEXA_FIXES: Default synthetic window title should match Vexa branding.
std::string WindowTitle {"Vexa SDL2"};
SDL_DisplayMode FullscreenWindowMode {};
bool HasFullscreenWindowMode = false;
std::atomic<uintptr_t> WindowIconSurface {0};
std::atomic<bool> ScreenSaverEnabled {true};
std::atomic<float> WindowBrightness {1.0f};
std::atomic<float> WindowOpacity {1.0f};
bool WindowGammaRampSet = false;
std::array<Uint16, 256> WindowGammaRamp[3];
bool WindowHasMouseRect = false;
SDL_Rect WindowMouseRect {};
SDL_HitTest WindowHitTestCallback = nullptr;
void* WindowHitTestData = nullptr;
SDL_Rect TextInputRect {};

std::mutex WindowDataMutex;
std::unordered_map<SDL_Window*, std::unordered_map<std::string, void*>> WindowDataByWindow;

constexpr int PrimaryDisplayIndex = 0;
constexpr Uint32 PrimaryWindowId = 1;
constexpr size_t GLAttributeCount = static_cast<size_t>(SDL_GL_FLOATBUFFERS) + 1;

std::array<int, GLAttributeCount> GLAttributes {};
std::atomic<bool> GLAttributesInitialized {false};
std::atomic<uintptr_t> CurrentGLWindowPointer {0};
std::atomic<uintptr_t> CurrentGLContextPointer {0};
std::atomic<int> CurrentSwapInterval {0};
std::atomic<bool> GLLibraryLoaded {true};
std::atomic<uint32_t> SDLValidationLogBudget {192};

// ---------------------------------------------------------------------------
// Event filter / watch / state tracking
// ---------------------------------------------------------------------------
std::mutex EventCallbackMutex;
SDL_EventFilter UserEventFilter = nullptr;
void* UserEventFilterData = nullptr;
std::vector<std::pair<SDL_EventFilter, void*>> EventWatches;
std::mutex EventStateMutex;
std::unordered_map<Uint32, Uint8> EventTypeStates;
std::atomic<Uint32> NextRegisteredEventType {SDL_USEREVENT};

// ---------------------------------------------------------------------------
// Keyboard lookup helpers (shared subset, used for synthesized key events)
// ---------------------------------------------------------------------------
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

const KeyLookupEntry* FindByKeycode(SDL_Keycode keycode) {
  for (const auto& entry : KeyLookupTable) {
    if (entry.Keycode == keycode) {
      return &entry;
    }
  }
  return nullptr;
}

// Local scancode derivation used only for synthesized events; the public
// keyboard APIs (SDL_GetScancodeFromKey etc.) are owned by the FNA stubs file.
SDL_Scancode CompatScancodeFromKeycode(SDL_Keycode key) {
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
  if (key_value == ' ') {
    return SDL_SCANCODE_SPACE;
  }

  switch (key_value) {
  case '`': return SDL_SCANCODE_GRAVE;
  case '-': return SDL_SCANCODE_MINUS;
  case '=': return SDL_SCANCODE_EQUALS;
  case '[': return SDL_SCANCODE_LEFTBRACKET;
  case ']': return SDL_SCANCODE_RIGHTBRACKET;
  case '\\': return SDL_SCANCODE_BACKSLASH;
  case ';': return SDL_SCANCODE_SEMICOLON;
  case '\'': return SDL_SCANCODE_APOSTROPHE;
  case ',': return SDL_SCANCODE_COMMA;
  case '.': return SDL_SCANCODE_PERIOD;
  case '/': return SDL_SCANCODE_SLASH;
  default: break;
  }

  return SDL_SCANCODE_UNKNOWN;
}

// ---------------------------------------------------------------------------
// Window size helpers
// ---------------------------------------------------------------------------
void UpdateFallbackWindowSize(int width, int height) {
  if (width > 0) {
    WindowWidth.store(width, std::memory_order_release);
  }
  if (height > 0) {
    WindowHeight.store(height, std::memory_order_release);
  }
}

void GetFallbackWindowSize(int* width, int* height) {
  if (width) {
    *width = WindowWidth.load(std::memory_order_acquire);
  }
  if (height) {
    *height = WindowHeight.load(std::memory_order_acquire);
  }
}

bool RefreshFallbackWindowSizeFromHost(SDL_Window* window) {
  SDL_Window* target = window;
  if (!target) {
    target = reinterpret_cast<SDL_Window*>(PrimaryWindowPointer.load(std::memory_order_acquire));
  }
  if (!target) {
    return false;
  }

  int host_width = 0;
  int host_height = 0;
  fexfn_pack_FEX_SDL2_GetWindowSize(target, &host_width, &host_height);
  if (host_width <= 0 || host_height <= 0) {
    return false;
  }

  UpdateFallbackWindowSize(host_width, host_height);
  return true;
}

bool IsKnownDisplay(int display_index) {
  return display_index == PrimaryDisplayIndex;
}

SDL_DisplayMode BuildFallbackDisplayMode() {
  int width {};
  int height {};
  GetFallbackWindowSize(&width, &height);

  SDL_DisplayMode mode {};
  mode.format = SDL_PIXELFORMAT_ARGB8888;
  mode.w = width;
  mode.h = height;
  mode.refresh_rate = 60;
  mode.driverdata = nullptr;
  return mode;
}

Uint32 GetTicksMS() {
  return fexfn_pack_FEX_SDL2_GetTicks();
}

bool IsValidGLAttr(SDL_GLattr attr) {
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

  fprintf(stderr, "[SDL2 thunk][validation] %s: %s\n",
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

void UpdateWindowFlags(Uint32 set_bits, Uint32 clear_bits) {
  Uint32 flags = WindowFlagsState.load(std::memory_order_acquire);
  flags |= set_bits;
  flags &= ~clear_bits;
  WindowFlagsState.store(flags, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Synthetic event queue plumbing
// ---------------------------------------------------------------------------
bool IsEventTypeEnabled(Uint32 type) {
  std::lock_guard<std::mutex> guard(EventStateMutex);
  auto it = EventTypeStates.find(type);
  return it == EventTypeStates.end() || it->second != SDL_DISABLE;
}

int RunUserEventFilter(SDL_Event* event) {
  SDL_EventFilter filter {};
  void* userdata {};
  {
    std::lock_guard<std::mutex> guard(EventCallbackMutex);
    filter = UserEventFilter;
    userdata = UserEventFilterData;
  }
  if (!filter) {
    return 1;
  }
  return filter(userdata, event);
}

void RunEventWatches(SDL_Event* event) {
  std::vector<std::pair<SDL_EventFilter, void*>> watches;
  {
    std::lock_guard<std::mutex> guard(EventCallbackMutex);
    watches = EventWatches;
  }
  for (auto& watch : watches) {
    if (watch.first) {
      (void)watch.first(watch.second, event);
    }
  }
}

// Queues an internally generated event. Disabled types are dropped, watches
// fire for everything that makes it into the queue.
bool QueueSyntheticEvent(const SDL_Event& event) {
  if (!IsEventTypeEnabled(event.type)) {
    return false;
  }
  SDL_Event copy = event;
  RunEventWatches(&copy);
  std::scoped_lock lock {SyntheticEventQueueMutex};
  SyntheticEventQueue.push_back(copy);
  return true;
}

void QueueSyntheticWindowEvent(Uint8 window_event_id, Sint32 data1 = 0, Sint32 data2 = 0) {
  SDL_Event event {};
  event.type = SDL_WINDOWEVENT;
  event.window.type = SDL_WINDOWEVENT;
  event.window.timestamp = GetTicksMS();
  event.window.windowID = PrimaryWindowId;
  event.window.event = window_event_id;
  event.window.data1 = data1;
  event.window.data2 = data2;
  QueueSyntheticEvent(event);
}

void QueueSyntheticWindowShownEvents() {
  int w = 0;
  int h = 0;
  GetFallbackWindowSize(&w, &h);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_SHOWN);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_DISPLAY_CHANGED, PrimaryDisplayIndex, 0);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_RESIZED, w, h);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_SIZE_CHANGED, w, h);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_EXPOSED);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_ENTER);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_FOCUS_GAINED);
}

void SetWindowVisibleFocused(bool visible) {
  if (visible) {
    UpdateWindowFlags(SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS,
                      SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED);
  } else {
    UpdateWindowFlags(SDL_WINDOW_HIDDEN,
                      SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
  }
}

Uint32 NormalizeInitFlags(Uint32 flags) {
  if ((flags & SDL_INIT_GAMECONTROLLER) != 0) {
    flags |= SDL_INIT_JOYSTICK;
  }
  if ((flags & (SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_SENSOR |
                SDL_INIT_GAMECONTROLLER)) != 0) {
    flags |= SDL_INIT_EVENTS;
  }
  return flags;
}

// ---------------------------------------------------------------------------
// /tmp/vexa-pointer input pump.
// The Android shell (GameActivity.kt) appends 16-byte records
// (type, x, y, state) using X11 event numbers: KeyPress=2, KeyRelease=3,
// ButtonPress=4, ButtonRelease=5, MotionNotify=6. They become SDL2 mouse and
// keyboard events here.
// ---------------------------------------------------------------------------
std::atomic<Sint32> CompatMouseX {0};
std::atomic<Sint32> CompatMouseY {0};
std::atomic<Sint32> CompatMouseRelX {0};
std::atomic<Sint32> CompatMouseRelY {0};
std::atomic<Uint32> CompatMouseButtons {0};
// Polled by SDL_GetKeyboardState (FNA reads this every frame). Written under
// PointerMutex, read lock-free; per-key torn reads are acceptable.
Uint8 CompatKeyStates[SDL_NUM_SCANCODES] = {};
// Virtual gamepad: the on-screen GuestPad emits X11-style records for every
// control, and these arrays give it an SDL_Joystick/SDL_GameController face
// (implemented in the FNA stubs include).
Uint8 CompatPadButtons[SDL_CONTROLLER_BUTTON_MAX] = {};
Sint16 CompatPadAxes[SDL_CONTROLLER_AXIS_MAX] = {};

int CompatPadButtonForKey(SDL_Keycode key) {
  switch (key) {
    case SDLK_ESCAPE: return SDL_CONTROLLER_BUTTON_B;
    case SDLK_SPACE: return SDL_CONTROLLER_BUTTON_X;
    case SDLK_e: return SDL_CONTROLLER_BUTTON_Y;
    case SDLK_RETURN: return SDL_CONTROLLER_BUTTON_START;
    case SDLK_TAB: return SDL_CONTROLLER_BUTTON_BACK;
    case SDLK_LSHIFT: return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    case SDLK_c: return SDL_CONTROLLER_BUTTON_LEFTSTICK;
    case SDLK_r: return SDL_CONTROLLER_BUTTON_RIGHTSTICK;
    case SDLK_UP: return SDL_CONTROLLER_BUTTON_DPAD_UP;
    case SDLK_DOWN: return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    case SDLK_LEFT: return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    case SDLK_RIGHT: return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    default: return -1;
  }
}

void CompatPadSetAxis(SDL_GameControllerAxis axis, Sint16 value) {
  if (axis >= 0 && axis < SDL_CONTROLLER_AXIS_MAX) {
    CompatPadAxes[axis] = value;
  }
}

void CompatPadKey(SDL_Keycode key, bool down) {
  const int button = CompatPadButtonForKey(key);
  if (button >= 0) {
    CompatPadButtons[button] = down ? 1 : 0;
  }
  switch (key) {
    case SDLK_w: CompatPadSetAxis(SDL_CONTROLLER_AXIS_LEFTY, down ? -32768 : 0); break;
    case SDLK_s: CompatPadSetAxis(SDL_CONTROLLER_AXIS_LEFTY, down ? 32767 : 0); break;
    case SDLK_a: CompatPadSetAxis(SDL_CONTROLLER_AXIS_LEFTX, down ? -32768 : 0); break;
    case SDLK_d: CompatPadSetAxis(SDL_CONTROLLER_AXIS_LEFTX, down ? 32767 : 0); break;
    case SDLK_LCTRL: CompatPadSetAxis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, down ? 32767 : 0); break;
    default: break;
  }
}

// Button records encode the pad source in state bits 24+: 1 = left click
// (face A / RT on the overlay), 3 = right click (RB). Raw taps carry 0.
void CompatPadButtonRecord(int32_t type, int32_t state) {
  const int source = state >> 24;
  const bool down = type == 4;
  if (source == 1) {
    CompatPadButtons[SDL_CONTROLLER_BUTTON_A] = down ? 1 : 0;
  } else if (source == 3) {
    CompatPadButtons[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = down ? 1 : 0;
  }
}
std::mutex PointerMutex;
off_t PointerOffset = 0;
std::chrono::steady_clock::time_point PointerLastPoll {};

SDL_Keycode CompatKeysymToKeycode(uint32_t sym) {
  switch (sym) {
  case 0xff0d: return SDLK_RETURN;
  case 0xff1b: return SDLK_ESCAPE;
  case 0xff08: return SDLK_BACKSPACE;
  case 0xff09: return SDLK_TAB;
  case 0xff51: return SDLK_LEFT;
  case 0xff52: return SDLK_UP;
  case 0xff53: return SDLK_RIGHT;
  case 0xff54: return SDLK_DOWN;
  case 0xffe1: return SDLK_LSHIFT;
  case 0xffe3: return SDLK_LCTRL;
  case 0xffe9: return SDLK_LALT;
  default: break;
  }
  if (sym >= 'A' && sym <= 'Z') {
    return static_cast<SDL_Keycode>(sym - 'A' + 'a');
  }
  return sym < 0x80 ? static_cast<SDL_Keycode>(sym) : SDLK_UNKNOWN;
}

void CompatUpdateModState(SDL_Keycode key, bool down) {
  Uint16 bit = 0;
  switch (key) {
  case SDLK_LSHIFT: bit = KMOD_LSHIFT; break;
  case SDLK_RSHIFT: bit = KMOD_RSHIFT; break;
  case SDLK_LCTRL: bit = KMOD_LCTRL; break;
  case SDLK_RCTRL: bit = KMOD_RCTRL; break;
  case SDLK_LALT: bit = KMOD_LALT; break;
  case SDLK_RALT: bit = KMOD_RALT; break;
  case SDLK_LGUI: bit = KMOD_LGUI; break;
  case SDLK_RGUI: bit = KMOD_RGUI; break;
  default: break;
  }
  if (bit == 0) {
    return;
  }
  Uint16 state = CurrentModState.load(std::memory_order_acquire);
  state = down ? static_cast<Uint16>(state | bit) : static_cast<Uint16>(state & ~bit);
  CurrentModState.store(state, std::memory_order_release);
}

void VexaSDL2PollPointerEvents() {
  std::lock_guard<std::mutex> guard(PointerMutex);
  const auto now = std::chrono::steady_clock::now();
  if (now - PointerLastPoll < std::chrono::milliseconds(4)) {
    return;
  }
  PointerLastPoll = now;
  const int fd = open("/tmp/vexa-pointer", O_RDONLY);
  if (fd < 0) {
    return;
  }
  const off_t end = lseek(fd, 0, SEEK_END);
  if (end < PointerOffset) {
    PointerOffset = 0;
  }
  if (end > PointerOffset && lseek(fd, PointerOffset, SEEK_SET) >= 0) {
    unsigned char rec[16];
    int handled = 0;
    const Uint32 timestamp = GetTicksMS();
    while (handled < 64 && read(fd, rec, sizeof(rec)) == static_cast<ssize_t>(sizeof(rec))) {
      PointerOffset += sizeof(rec);
      ++handled;
      int32_t type, x, y, state;
      std::memcpy(&type, rec, 4);
      std::memcpy(&x, rec + 4, 4);
      std::memcpy(&y, rec + 8, 4);
      std::memcpy(&state, rec + 12, 4);
      if (type == 4 || type == 5) {
        CompatPadButtonRecord(type, state);
      }
      SDL_Event event {};
      if (type == 6 || type == 4 || type == 5) {
        const Sint32 fx = static_cast<Sint32>(x);
        const Sint32 fy = static_cast<Sint32>(y);
        const Sint32 old_x = CompatMouseX.exchange(fx);
        const Sint32 old_y = CompatMouseY.exchange(fy);
        if (type == 4) {
          CompatMouseButtons |= SDL_BUTTON_LMASK;
        } else if (type == 5) {
          CompatMouseButtons &= ~static_cast<Uint32>(SDL_BUTTON_LMASK);
        }
        if (type == 6 || old_x != fx || old_y != fy) {
          event.type = SDL_MOUSEMOTION;
          event.motion.timestamp = timestamp;
          event.motion.windowID = PrimaryWindowId;
          event.motion.which = 0;
          event.motion.state = CompatMouseButtons.load();
          event.motion.x = fx;
          event.motion.y = fy;
          event.motion.xrel = fx - old_x;
          event.motion.yrel = fy - old_y;
          CompatMouseRelX.fetch_add(event.motion.xrel, std::memory_order_relaxed);
          CompatMouseRelY.fetch_add(event.motion.yrel, std::memory_order_relaxed);
          QueueSyntheticEvent(event);
        }
        if (type != 6) {
          SDL_Event button {};
          button.type = type == 4 ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
          button.button.timestamp = timestamp;
          button.button.windowID = PrimaryWindowId;
          button.button.which = 0;
          button.button.button = SDL_BUTTON_LEFT;
          button.button.state = type == 4 ? SDL_PRESSED : SDL_RELEASED;
          button.button.clicks = 1;
          button.button.x = fx;
          button.button.y = fy;
          QueueSyntheticEvent(button);
        }
      } else if (type == 2 || type == 3) {
        const SDL_Keycode key = CompatKeysymToKeycode(static_cast<uint32_t>(x));
        if (key != SDLK_UNKNOWN) {
          const bool down = type == 2;
          CompatUpdateModState(key, down);
          event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
          event.key.timestamp = timestamp;
          event.key.windowID = PrimaryWindowId;
          event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
          event.key.repeat = 0;
          event.key.keysym.scancode = CompatScancodeFromKeycode(key);
          if (event.key.keysym.scancode < SDL_NUM_SCANCODES) {
            CompatKeyStates[event.key.keysym.scancode] = down ? 1 : 0;
          }
          CompatPadKey(key, down);
          event.key.keysym.sym = key;
          event.key.keysym.mod = CurrentModState.load(std::memory_order_acquire);
          QueueSyntheticEvent(event);
        }
      }
    }
  }
  close(fd);
}

// ---------------------------------------------------------------------------
// GL proc tracing + resolvers (ported from libSDL3 guest)
// ---------------------------------------------------------------------------
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

  fprintf(stderr, "[SDL2 thunk] SDL_GL_GetProcAddress('%s') => %p via %s\n",
          proc,
          resolved,
          source ? source : "unknown");

  if (FILE* f = std::fopen(SDL2GLProcTracePath, "a")) {
    fprintf(f, "[SDL2 thunk] SDL_GL_GetProcAddress('%s') => %p via %s\n",
            proc,
            resolved,
            source ? source : "unknown");
    std::fclose(f);
  }
}

enum class ProcResolverInitState : uint32_t {
  Uninitialized = 0,
  Initializing = 1,
  Ready = 2,
  Failed = 3,
};

struct SDL2GLResolverState {
  std::atomic<uint32_t> InitState {static_cast<uint32_t>(ProcResolverInitState::Uninitialized)};
  void* GLHandle {};
  const char* GLHandleName {};
  void* (*GLXGetProc)(const GLubyte*) {};
  bool ResolverMissingLogged {};
  bool DlopenFailureLogged {};
  bool GLXDlsymFailureLogged {};
  bool SymbolDlsymFailureLogged {};
};

struct SDL2EGLResolverState {
  std::atomic<uint32_t> InitState {static_cast<uint32_t>(ProcResolverInitState::Uninitialized)};
  void* EGLHandle {};
  const char* EGLHandleName {};
  void* (*EGLGetProc)(const char*) {};
  bool ResolverMissingLogged {};
  bool DlopenFailureLogged {};
  bool EGLProcDlsymFailureLogged {};
  bool SymbolDlsymFailureLogged {};
};

SDL2GLResolverState& GetSDL2GLResolverState() {
  static SDL2GLResolverState State {};
  return State;
}

SDL2EGLResolverState& GetSDL2EGLResolverState() {
  static SDL2EGLResolverState State {};
  return State;
}

bool SDL2EnsureGLResolverInitialized() {
  auto& state = GetSDL2GLResolverState();
  static thread_local bool InGLResolverInitialization = false;

  while (true) {
    const auto init_state = static_cast<ProcResolverInitState>(state.InitState.load(std::memory_order_acquire));
    if (init_state == ProcResolverInitState::Ready) {
      return state.GLXGetProc != nullptr;
    }
    if (init_state == ProcResolverInitState::Failed) {
      return false;
    }
    if (init_state == ProcResolverInitState::Initializing) {
      if (InGLResolverInitialization) {
        return false;
      }
      std::this_thread::yield();
      continue;
    }

    uint32_t expected = static_cast<uint32_t>(ProcResolverInitState::Uninitialized);
    if (!state.InitState.compare_exchange_strong(
          expected,
          static_cast<uint32_t>(ProcResolverInitState::Initializing),
          std::memory_order_acq_rel,
          std::memory_order_acquire)) {
      continue;
    }

    InGLResolverInitialization = true;
    state.GLXGetProc = reinterpret_cast<void* (*)(const GLubyte*)>(dlsym(RTLD_DEFAULT, "glXGetProcAddress"));
    if (!state.GLXGetProc) {
      state.GLXGetProc = reinterpret_cast<void* (*)(const GLubyte*)>(dlsym(RTLD_DEFAULT, "glXGetProcAddressARB"));
    }

    if (!state.GLXGetProc) {
      // Prefer explicitly loading the VEXA GL guest thunk so proc resolution
      // does not depend on global namespace ordering.
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
          state.GLHandle = handle;
          state.GLHandleName = candidate;
          fprintf(stderr, "[SDL2 thunk] gl resolver handle loaded: %s => %p\n", candidate, handle);
          break;
        }

        if (!state.DlopenFailureLogged) {
          const char* err = dlerror();
          fprintf(stderr,
                  "[SDL2 thunk] dlopen('%s') failed while probing GL resolver: %s\n",
                  candidate,
                  err ? err : "<no dlerror>");
          state.DlopenFailureLogged = true;
        }
      }

      if (state.GLHandle) {
        dlerror();
        state.GLXGetProc = reinterpret_cast<void* (*)(const GLubyte*)>(dlsym(state.GLHandle, "glXGetProcAddress"));
        if (!state.GLXGetProc) {
          if (!state.GLXDlsymFailureLogged) {
            const char* err = dlerror();
            fprintf(stderr,
                    "[SDL2 thunk] dlsym(%s,'glXGetProcAddress') failed: %s\n",
                    state.GLHandleName ? state.GLHandleName : "<unknown>",
                    err ? err : "<no dlerror>");
          }
          dlerror();
          state.GLXGetProc = reinterpret_cast<void* (*)(const GLubyte*)>(dlsym(state.GLHandle, "glXGetProcAddressARB"));
          if (!state.GLXGetProc && !state.GLXDlsymFailureLogged) {
            const char* err = dlerror();
            fprintf(stderr,
                    "[SDL2 thunk] dlsym(%s,'glXGetProcAddressARB') failed: %s\n",
                    state.GLHandleName ? state.GLHandleName : "<unknown>",
                    err ? err : "<no dlerror>");
            state.GLXDlsymFailureLogged = true;
          }
        }
      }
    }

    InGLResolverInitialization = false;
    const bool initialized = state.GLXGetProc != nullptr;
    state.InitState.store(
      static_cast<uint32_t>(initialized ? ProcResolverInitState::Ready : ProcResolverInitState::Failed),
      std::memory_order_release);
    return initialized;
  }
}

bool SDL2EnsureEGLResolverInitialized() {
  auto& state = GetSDL2EGLResolverState();
  static thread_local bool InEGLResolverInitialization = false;

  while (true) {
    const auto init_state = static_cast<ProcResolverInitState>(state.InitState.load(std::memory_order_acquire));
    if (init_state == ProcResolverInitState::Ready) {
      return state.EGLGetProc != nullptr || state.EGLHandle != nullptr;
    }
    if (init_state == ProcResolverInitState::Failed) {
      return false;
    }
    if (init_state == ProcResolverInitState::Initializing) {
      if (InEGLResolverInitialization) {
        return false;
      }
      std::this_thread::yield();
      continue;
    }

    uint32_t expected = static_cast<uint32_t>(ProcResolverInitState::Uninitialized);
    if (!state.InitState.compare_exchange_strong(
          expected,
          static_cast<uint32_t>(ProcResolverInitState::Initializing),
          std::memory_order_acq_rel,
          std::memory_order_acquire)) {
      continue;
    }

    InEGLResolverInitialization = true;
    state.EGLGetProc = reinterpret_cast<void* (*)(const char*)>(dlsym(RTLD_DEFAULT, "eglGetProcAddress"));

    if (!state.EGLHandle) {
      constexpr const char* EGLHandleCandidates[] = {
        "/apex/com.android.opengl/lib64/libEGL.so",
        "/system/lib64/libEGL.so",
        "libEGL.so",
      };

      for (const char* candidate : EGLHandleCandidates) {
        dlerror();
        void* handle = dlopen(candidate, RTLD_NOW | RTLD_GLOBAL);
        if (handle) {
          state.EGLHandle = handle;
          state.EGLHandleName = candidate;
          break;
        }

        if (!state.DlopenFailureLogged) {
          const char* err = dlerror();
          fprintf(stderr,
                  "[SDL2 thunk] dlopen('%s') failed while probing EGL resolver: %s\n",
                  candidate,
                  err ? err : "<no dlerror>");
          state.DlopenFailureLogged = true;
        }
      }
    }

    if (!state.EGLGetProc && state.EGLHandle) {
      dlerror();
      state.EGLGetProc = reinterpret_cast<void* (*)(const char*)>(dlsym(state.EGLHandle, "eglGetProcAddress"));
      if (!state.EGLGetProc && !state.EGLProcDlsymFailureLogged) {
        const char* err = dlerror();
        fprintf(stderr,
                "[SDL2 thunk] dlsym(%s,'eglGetProcAddress') failed: %s\n",
                state.EGLHandleName ? state.EGLHandleName : "<unknown>",
                err ? err : "<no dlerror>");
        state.EGLProcDlsymFailureLogged = true;
      }
    }

    InEGLResolverInitialization = false;
    const bool initialized = state.EGLGetProc != nullptr || state.EGLHandle != nullptr;
    state.InitState.store(
      static_cast<uint32_t>(initialized ? ProcResolverInitState::Ready : ProcResolverInitState::Failed),
      std::memory_order_release);
    return initialized;
  }
}

// VEXA_FIXES: Resolve glXGetProcAddress with one-time guarded initialization so
// we don't race/re-enter dlopen(libGL-guest.so) from multiple probing threads.
void* SDL2ResolveGLProc(const GLubyte* procname) {
  const char* procname_str = reinterpret_cast<const char*>(procname);
  if (!procname_str || !*procname_str) {
    return nullptr;
  }

  auto& state = GetSDL2GLResolverState();
  (void)SDL2EnsureGLResolverInitialized();

  if (!state.GLXGetProc) {
    if (!state.ResolverMissingLogged) {
      state.ResolverMissingLogged = true;
      fprintf(stderr, "[SDL2 thunk] missing glXGetProcAddress resolver while looking up '%s'\n", procname_str);
    }

    // Fallback path: resolve direct exports when glXGetProcAddress bridge
    // is not available yet in this namespace.
    if (void* direct = dlsym(RTLD_DEFAULT, procname_str)) {
      fprintf(stderr, "[SDL2 thunk] direct dlsym fallback('%s') => %p via RTLD_DEFAULT\n",
              procname_str, direct);
      return direct;
    }

    if (state.GLHandle) {
      dlerror();
      if (void* direct = dlsym(state.GLHandle, procname_str)) {
        fprintf(stderr, "[SDL2 thunk] direct dlsym fallback('%s') => %p via libGL handle\n",
                procname_str, direct);
        return direct;
      }
      if (!state.SymbolDlsymFailureLogged) {
        const char* err = dlerror();
        fprintf(stderr,
                "[SDL2 thunk] dlsym(%s,'%s') failed in direct fallback: %s\n",
                state.GLHandleName ? state.GLHandleName : "<unknown>",
                procname_str,
                err ? err : "<no dlerror>");
        state.SymbolDlsymFailureLogged = true;
      }
    }
    return nullptr;
  }

  if (void* resolved = state.GLXGetProc(procname)) {
    return resolved;
  }

  // Secondary fallback even when resolver exists but returns null.
  if (void* direct = dlsym(RTLD_DEFAULT, procname_str)) {
    fprintf(stderr, "[SDL2 thunk] secondary dlsym fallback('%s') => %p via RTLD_DEFAULT\n",
            procname_str, direct);
    return direct;
  }
  if (state.GLHandle) {
    dlerror();
    if (void* direct = dlsym(state.GLHandle, procname_str)) {
      fprintf(stderr, "[SDL2 thunk] secondary dlsym fallback('%s') => %p via libGL handle\n",
              procname_str, direct);
      return direct;
    }
    if (!state.SymbolDlsymFailureLogged) {
      const char* err = dlerror();
      fprintf(stderr,
              "[SDL2 thunk] dlsym(%s,'%s') failed in secondary fallback: %s\n",
              state.GLHandleName ? state.GLHandleName : "<unknown>",
              procname_str,
              err ? err : "<no dlerror>");
      state.SymbolDlsymFailureLogged = true;
    }
  }
  return nullptr;
}

// VEXA_FIXES: Keep EGL entry-point resolution independent from glX bridge
// setup to avoid pulling libGL-guest initialization into EGL lookups.
void* SDL2ResolveEGLProc(const char* procname) {
  if (!procname || !*procname) {
    return nullptr;
  }

  auto& state = GetSDL2EGLResolverState();
  (void)SDL2EnsureEGLResolverInitialized();

  if (state.EGLGetProc) {
    if (void* resolved = state.EGLGetProc(procname)) {
      return resolved;
    }
  } else if (!state.ResolverMissingLogged) {
    state.ResolverMissingLogged = true;
    fprintf(stderr, "[SDL2 thunk] missing eglGetProcAddress resolver while looking up '%s'\n", procname);
  }

  if (void* direct = dlsym(RTLD_DEFAULT, procname)) {
    return direct;
  }

  if (state.EGLHandle) {
    dlerror();
    if (void* direct = dlsym(state.EGLHandle, procname)) {
      return direct;
    }
    if (!state.SymbolDlsymFailureLogged) {
      const char* err = dlerror();
      fprintf(stderr,
              "[SDL2 thunk] dlsym(%s,'%s') failed in EGL fallback: %s\n",
              state.EGLHandleName ? state.EGLHandleName : "<unknown>",
              procname,
              err ? err : "<no dlerror>");
      state.SymbolDlsymFailureLogged = true;
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

// ---------------------------------------------------------------------------
// Desktop-GL compatibility shims returned through SDL_GL_GetProcAddress so
// FNA-style titles get sane behavior on GLES-only Android drivers.
// ---------------------------------------------------------------------------
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
  auto fn = reinterpret_cast<GLClearDepthfFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glClearDepthf")));
  CachedGLClearDepthf.store(fn, std::memory_order_release);
  return fn;
}

GLClearStencilFn ResolveGLClearStencil() {
  if (auto fn = CachedGLClearStencil.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLClearStencilFn>(
      SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glClearStencil")));
  CachedGLClearStencil.store(fn, std::memory_order_release);
  return fn;
}

GLDepthRangefFn ResolveGLDepthRangef() {
  if (auto fn = CachedGLDepthRangef.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLDepthRangefFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glDepthRangef")));
  CachedGLDepthRangef.store(fn, std::memory_order_release);
  return fn;
}

GLTexImage2DFn ResolveGLTexImage2D() {
  if (auto fn = CachedGLTexImage2D.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLTexImage2DFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glTexImage2D")));
  CachedGLTexImage2D.store(fn, std::memory_order_release);
  return fn;
}

GLTexImage2DMultisampleFn ResolveGLTexImage2DMultisample() {
  if (CachedGLTexImage2DMultisampleResolved.load(std::memory_order_acquire)) {
    return CachedGLTexImage2DMultisample.load(std::memory_order_acquire);
  }
  GLTexImage2DMultisampleFn fn = reinterpret_cast<GLTexImage2DMultisampleFn>(
      SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glTexImage2DMultisample")));
  if (!fn) {
    fn = reinterpret_cast<GLTexImage2DMultisampleFn>(
        SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glTexImage2DMultisampleEXT")));
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
      SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glTexStorage2DMultisample")));
  CachedGLTexStorage2DMultisample.store(fn, std::memory_order_release);
  return fn;
}

GLDrawBufferFn ResolveGLDrawBuffer() {
  if (CachedGLDrawBufferResolved.load(std::memory_order_acquire)) {
    return CachedGLDrawBuffer.load(std::memory_order_acquire);
  }
  GLDrawBufferFn fn =
      reinterpret_cast<GLDrawBufferFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glDrawBuffer")));
  if (!fn) {
    fn = reinterpret_cast<GLDrawBufferFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glDrawBufferEXT")));
  }
  CachedGLDrawBuffer.store(fn, std::memory_order_release);
  CachedGLDrawBufferResolved.store(true, std::memory_order_release);
  return fn;
}

GLDrawBuffersFn ResolveGLDrawBuffers() {
  if (auto fn = CachedGLDrawBuffers.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLDrawBuffersFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glDrawBuffers")));
  CachedGLDrawBuffers.store(fn, std::memory_order_release);
  return fn;
}

GLGetBufferSubDataFn ResolveGLGetBufferSubData() {
  if (CachedGLGetBufferSubDataResolved.load(std::memory_order_acquire)) {
    return CachedGLGetBufferSubData.load(std::memory_order_acquire);
  }
  GLGetBufferSubDataFn fn = reinterpret_cast<GLGetBufferSubDataFn>(
      SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glGetBufferSubData")));
  if (!fn) {
    fn = reinterpret_cast<GLGetBufferSubDataFn>(
        SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glGetBufferSubDataARB")));
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
      SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glMapBufferRange")));
  if (!fn) {
    fn = reinterpret_cast<GLMapBufferRangeFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glMapBufferRangeEXT")));
  }
  CachedGLMapBufferRange.store(fn, std::memory_order_release);
  return fn;
}

GLUnmapBufferFn ResolveGLUnmapBuffer() {
  if (auto fn = CachedGLUnmapBuffer.load(std::memory_order_acquire)) {
    return fn;
  }
  GLUnmapBufferFn fn =
      reinterpret_cast<GLUnmapBufferFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glUnmapBuffer")));
  if (!fn) {
    fn = reinterpret_cast<GLUnmapBufferFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glUnmapBufferOES")));
  }
  CachedGLUnmapBuffer.store(fn, std::memory_order_release);
  return fn;
}

GLVertexAttribI2iFn ResolveGLVertexAttribI2i() {
  if (CachedGLVertexAttribI2iResolved.load(std::memory_order_acquire)) {
    return CachedGLVertexAttribI2i.load(std::memory_order_acquire);
  }
  GLVertexAttribI2iFn fn = reinterpret_cast<GLVertexAttribI2iFn>(
      SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glVertexAttribI2i")));
  if (!fn) {
    fn = reinterpret_cast<GLVertexAttribI2iFn>(
        SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glVertexAttribI2iEXT")));
  }
  CachedGLVertexAttribI2i.store(fn, std::memory_order_release);
  CachedGLVertexAttribI2iResolved.store(true, std::memory_order_release);
  return fn;
}

GLVertexAttribI4iFn ResolveGLVertexAttribI4i() {
  if (auto fn = CachedGLVertexAttribI4i.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLVertexAttribI4iFn>(
      SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glVertexAttribI4i")));
  if (!fn) {
    fn = reinterpret_cast<GLVertexAttribI4iFn>(
        SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glVertexAttribI4iEXT")));
  }
  CachedGLVertexAttribI4i.store(fn, std::memory_order_release);
  return fn;
}

GLGetFramebufferAttachmentParameterivFn ResolveGLGetFramebufferAttachmentParameteriv() {
  if (CachedGLGetFramebufferAttachmentParameterivResolved.load(std::memory_order_acquire)) {
    return CachedGLGetFramebufferAttachmentParameteriv.load(std::memory_order_acquire);
  }
  GLGetFramebufferAttachmentParameterivFn fn = reinterpret_cast<GLGetFramebufferAttachmentParameterivFn>(
      SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glGetFramebufferAttachmentParameteriv")));
  if (!fn) {
    fn = reinterpret_cast<GLGetFramebufferAttachmentParameterivFn>(
        SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glGetFramebufferAttachmentParameterivEXT")));
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
      SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glGetTexLevelParameteriv")));
  CachedGLGetTexLevelParameteriv.store(fn, std::memory_order_release);
  return fn;
}

GLGetIntegervFn ResolveGLGetIntegerv() {
  if (auto fn = CachedGLGetIntegerv.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLGetIntegervFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glGetIntegerv")));
  CachedGLGetIntegerv.store(fn, std::memory_order_release);
  return fn;
}

GLGenFramebuffersFn ResolveGLGenFramebuffers() {
  if (auto fn = CachedGLGenFramebuffers.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLGenFramebuffersFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glGenFramebuffers")));
  CachedGLGenFramebuffers.store(fn, std::memory_order_release);
  return fn;
}

GLDeleteFramebuffersFn ResolveGLDeleteFramebuffers() {
  if (auto fn = CachedGLDeleteFramebuffers.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLDeleteFramebuffersFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glDeleteFramebuffers")));
  CachedGLDeleteFramebuffers.store(fn, std::memory_order_release);
  return fn;
}

GLBindFramebufferFn ResolveGLBindFramebuffer() {
  if (auto fn = CachedGLBindFramebuffer.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLBindFramebufferFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glBindFramebuffer")));
  CachedGLBindFramebuffer.store(fn, std::memory_order_release);
  return fn;
}

GLFramebufferTexture2DFn ResolveGLFramebufferTexture2D() {
  if (auto fn = CachedGLFramebufferTexture2D.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLFramebufferTexture2DFn>(
      SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glFramebufferTexture2D")));
  CachedGLFramebufferTexture2D.store(fn, std::memory_order_release);
  return fn;
}

GLCheckFramebufferStatusFn ResolveGLCheckFramebufferStatus() {
  if (auto fn = CachedGLCheckFramebufferStatus.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLCheckFramebufferStatusFn>(
      SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glCheckFramebufferStatus")));
  CachedGLCheckFramebufferStatus.store(fn, std::memory_order_release);
  return fn;
}

GLReadPixelsFn ResolveGLReadPixels() {
  if (auto fn = CachedGLReadPixels.load(std::memory_order_acquire)) {
    return fn;
  }
  auto fn = reinterpret_cast<GLReadPixelsFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glReadPixels")));
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
    fprintf(stderr, "[SDL2 thunk] glClearDepth fallback: missing glClearDepthf, keeping no-op\n");
  }
}

#ifndef GL_FRAMEBUFFER_DEFAULT
#define GL_FRAMEBUFFER_DEFAULT 0x8218
#endif
// Legacy GetIntegerv pnames trimmed from the vendored glcorearb.h.
#ifndef GL_RED_BITS
#define GL_RED_BITS 0x0D52
#endif
#ifndef GL_GREEN_BITS
#define GL_GREEN_BITS 0x0D53
#endif
#ifndef GL_BLUE_BITS
#define GL_BLUE_BITS 0x0D54
#endif
#ifndef GL_ALPHA_BITS
#define GL_ALPHA_BITS 0x0D55
#endif
#ifndef GL_DEPTH_BITS
#define GL_DEPTH_BITS 0x0D56
#endif
#ifndef GL_STENCIL_BITS
#define GL_STENCIL_BITS 0x0D57
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
    fprintf(stderr, "[SDL2 thunk] glClearStencil fallback: missing glClearStencil, keeping no-op\n");
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

  GLPolygonModeFn fn = reinterpret_cast<GLPolygonModeFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glPolygonMode")));
  if (!fn) {
    fn = reinterpret_cast<GLPolygonModeFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glPolygonModeEXT")));
  }
  if (!fn) {
    fn = reinterpret_cast<GLPolygonModeFn>(SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glPolygonModeNV")));
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

  fprintf(stderr, "[SDL2 thunk] glPolygonMode fallback ignored unsupported mode=0x%x\n", static_cast<unsigned>(mode));
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

  fprintf(stderr, "[SDL2 thunk] glDrawBuffer fallback ignored unsupported buf=0x%x\n", static_cast<unsigned>(buf));
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

  fprintf(stderr, "[SDL2 thunk] glVertexAttribI2i fallback unavailable for index=%u\n", index);
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
      *params = (!color_attachment && !depth_stencil_attachment)
                    ? GL_NONE
                    : (default_fb ? GL_FRAMEBUFFER_DEFAULT : GL_RENDERBUFFER);
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

struct GLProcEntry {
  std::string_view Name;
  void* Function;
};

#define SDL2_GL_PROC_ENTRY(name) {#name, nullptr},
const GLProcEntry GLProcTable[] = {
  FEX_ANDROID_GL_PROC_LIST(SDL2_GL_PROC_ENTRY)
};
#undef SDL2_GL_PROC_ENTRY

// ---------------------------------------------------------------------------
// Per-surface guest-side bookkeeping (clip rect, mods, keys, palettes).
// ---------------------------------------------------------------------------
struct SurfaceExtraState {
  SDL_BlendMode BlendMode {SDL_BLENDMODE_NONE};
  bool HasBlendMode = false;
  bool HasColorKey = false;
  Uint32 ColorKey = 0;
  Uint8 ColorModR = 255;
  Uint8 ColorModG = 255;
  Uint8 ColorModB = 255;
  Uint8 AlphaMod = 255;
  SDL_Palette* Palette = nullptr;
  // Guest-owned backing store for surfaces created via SDL_CreateCursor;
  // released when the surface is freed (EraseSurfaceExtra).
  std::vector<Uint8> OwnedPixels;
};

std::mutex SurfaceStateMutex;
std::unordered_map<SDL_Surface*, SurfaceExtraState> SurfaceStateByPointer;
SDL_YUV_CONVERSION_MODE YUVConversionMode = SDL_YUV_CONVERSION_BT601;

SurfaceExtraState& GetSurfaceExtraLocked(SDL_Surface* surface) {
  return SurfaceStateByPointer[surface];
}

void EraseSurfaceExtra(SDL_Surface* surface) {
  std::lock_guard<std::mutex> guard(SurfaceStateMutex);
  SurfaceStateByPointer.erase(surface);
}

// ---------------------------------------------------------------------------
// Guest-side SDL timer registry (SDL_AddTimer / SDL_RemoveTimer)
// ---------------------------------------------------------------------------
struct GuestTimerState {
  std::atomic<bool> Cancelled {false};
};
std::mutex TimerRegistryMutex;
std::unordered_map<SDL_TimerID, std::shared_ptr<GuestTimerState>> GuestTimers;
std::atomic<int> NextTimerId {1};

// ---------------------------------------------------------------------------
// Pixel format helpers for guest-side surface ops (little-endian x86 guest).
// Canonical formats only; SDL2 alias enumerators (RGB888, BGR555, RGBA32...)
// share these values and are covered by the same table entries.
// ---------------------------------------------------------------------------
struct PixelFormatTableEntry {
  Uint32 Format;
  int BitsPerPixel;
  Uint32 Rmask;
  Uint32 Gmask;
  Uint32 Bmask;
  Uint32 Amask;
  const char* Name;
};

constexpr PixelFormatTableEntry PixelFormatTable[] = {
  {SDL_PIXELFORMAT_INDEX8, 8, 0x000000, 0x000000, 0x000000, 0x00000000, "SDL_PIXELFORMAT_INDEX8"},
  {SDL_PIXELFORMAT_RGB332, 8, 0x0000E0, 0x00001C, 0x000003, 0x00000000, "SDL_PIXELFORMAT_RGB332"},
  {SDL_PIXELFORMAT_XRGB4444, 12, 0x000F00, 0x0000F0, 0x00000F, 0x00000000, "SDL_PIXELFORMAT_XRGB4444"},
  {SDL_PIXELFORMAT_XBGR4444, 12, 0x00000F, 0x0000F0, 0x000F00, 0x00000000, "SDL_PIXELFORMAT_XBGR4444"},
  {SDL_PIXELFORMAT_XRGB1555, 15, 0x007C00, 0x0003E0, 0x00001F, 0x00000000, "SDL_PIXELFORMAT_XRGB1555"},
  {SDL_PIXELFORMAT_XBGR1555, 15, 0x00001F, 0x0003E0, 0x007C00, 0x00000000, "SDL_PIXELFORMAT_XBGR1555"},
  {SDL_PIXELFORMAT_ARGB4444, 16, 0x000F00, 0x0000F0, 0x00000F, 0x0000F000, "SDL_PIXELFORMAT_ARGB4444"},
  {SDL_PIXELFORMAT_RGBA4444, 16, 0x00F000, 0x000F00, 0x0000F0, 0x0000000F, "SDL_PIXELFORMAT_RGBA4444"},
  {SDL_PIXELFORMAT_ABGR4444, 16, 0x00000F, 0x0000F0, 0x000F00, 0x0000F000, "SDL_PIXELFORMAT_ABGR4444"},
  {SDL_PIXELFORMAT_BGRA4444, 16, 0x0000F0, 0x000F00, 0x00F000, 0x0000000F, "SDL_PIXELFORMAT_BGRA4444"},
  {SDL_PIXELFORMAT_ARGB1555, 16, 0x007C00, 0x0003E0, 0x00001F, 0x00008000, "SDL_PIXELFORMAT_ARGB1555"},
  {SDL_PIXELFORMAT_RGBA5551, 16, 0x00F800, 0x0007C0, 0x00003E, 0x00000001, "SDL_PIXELFORMAT_RGBA5551"},
  {SDL_PIXELFORMAT_ABGR1555, 16, 0x00001F, 0x0003E0, 0x007C00, 0x00008000, "SDL_PIXELFORMAT_ABGR1555"},
  {SDL_PIXELFORMAT_BGRA5551, 16, 0x00003E, 0x0007C0, 0x00F800, 0x00000001, "SDL_PIXELFORMAT_BGRA5551"},
  {SDL_PIXELFORMAT_RGB565, 16, 0x00F800, 0x0007E0, 0x00001F, 0x00000000, "SDL_PIXELFORMAT_RGB565"},
  {SDL_PIXELFORMAT_BGR565, 16, 0x00001F, 0x0007E0, 0x00F800, 0x00000000, "SDL_PIXELFORMAT_BGR565"},
  {SDL_PIXELFORMAT_RGB24, 24, 0x0000FF, 0x00FF00, 0xFF0000, 0x00000000, "SDL_PIXELFORMAT_RGB24"},
  {SDL_PIXELFORMAT_BGR24, 24, 0xFF0000, 0x00FF00, 0x0000FF, 0x00000000, "SDL_PIXELFORMAT_BGR24"},
  {SDL_PIXELFORMAT_XRGB8888, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0x00000000, "SDL_PIXELFORMAT_XRGB8888"},
  {SDL_PIXELFORMAT_RGBX8888, 32, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x00000000, "SDL_PIXELFORMAT_RGBX8888"},
  {SDL_PIXELFORMAT_XBGR8888, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0x00000000, "SDL_PIXELFORMAT_XBGR8888"},
  {SDL_PIXELFORMAT_BGRX8888, 32, 0x0000FF00, 0x00FF0000, 0xFF000000, 0x00000000, "SDL_PIXELFORMAT_BGRX8888"},
  {SDL_PIXELFORMAT_ARGB8888, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000, "SDL_PIXELFORMAT_ARGB8888"},
  {SDL_PIXELFORMAT_RGBA8888, 32, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF, "SDL_PIXELFORMAT_RGBA8888"},
  {SDL_PIXELFORMAT_ABGR8888, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000, "SDL_PIXELFORMAT_ABGR8888"},
  {SDL_PIXELFORMAT_BGRA8888, 32, 0x0000FF00, 0x00FF0000, 0xFF000000, 0x000000FF, "SDL_PIXELFORMAT_BGRA8888"},
  {SDL_PIXELFORMAT_ARGB2101010, 32, 0x3FF00000, 0x000FFC00, 0x000003FF, 0xC0000000, "SDL_PIXELFORMAT_ARGB2101010"},
};

const PixelFormatTableEntry* FindPixelFormatEntry(Uint32 format) {
  for (const auto& entry : PixelFormatTable) {
    if (entry.Format == format) {
      return &entry;
    }
  }
  return nullptr;
}

bool MasksForPixelFormat(Uint32 format, int* bpp, Uint32* rmask, Uint32* gmask, Uint32* bmask, Uint32* amask) {
  const PixelFormatTableEntry* entry = FindPixelFormatEntry(format);
  if (!entry) {
    return false;
  }
  if (bpp) {
    *bpp = entry->BitsPerPixel;
  }
  if (rmask) {
    *rmask = entry->Rmask;
  }
  if (gmask) {
    *gmask = entry->Gmask;
  }
  if (bmask) {
    *bmask = entry->Bmask;
  }
  if (amask) {
    *amask = entry->Amask;
  }
  return true;
}

int MaskShift(Uint32 mask) {
  if (mask == 0) {
    return 0;
  }
  int shift = 0;
  while ((mask & 1) == 0) {
    mask >>= 1;
    ++shift;
  }
  return shift;
}

int MaskBits(Uint32 mask) {
  int bits = 0;
  while (mask) {
    bits += mask & 1;
    mask >>= 1;
  }
  return bits;
}

Uint8 ExpandChannelTo8(Uint32 value, int bits) {
  if (bits <= 0 || value == 0) {
    return 0;
  }
  const Uint32 max_value = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
  return static_cast<Uint8>((value * 255u + max_value / 2u) / max_value);
}

Uint32 ShrinkChannelFrom8(Uint8 value, int bits) {
  if (bits <= 0) {
    return 0;
  }
  const Uint32 max_value = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
  return (static_cast<Uint32>(value) * max_value + 127u) / 255u;
}

// A light-weight pixel format descriptor used for guest-side conversions.
struct PixelDesc {
  int BytesPerPixel = 0;
  bool IsIndexed = false;
  bool Is24BitArray = false;
  Uint32 Rmask = 0, Gmask = 0, Bmask = 0, Amask = 0;
  int Rshift = 0, Gshift = 0, Bshift = 0, Ashift = 0;
  int Rbits = 0, Gbits = 0, Bbits = 0, Abits = 0;
  SDL_Palette* Palette = nullptr;
};

void FillPixelDescShifts(PixelDesc& desc) {
  desc.Rshift = MaskShift(desc.Rmask);
  desc.Gshift = MaskShift(desc.Gmask);
  desc.Bshift = MaskShift(desc.Bmask);
  desc.Ashift = MaskShift(desc.Amask);
  desc.Rbits = MaskBits(desc.Rmask);
  desc.Gbits = MaskBits(desc.Gmask);
  desc.Bbits = MaskBits(desc.Bmask);
  desc.Abits = MaskBits(desc.Amask);
}

bool DescForPixelFormatEnum(Uint32 format, PixelDesc& desc) {
  int bpp = 0;
  Uint32 r = 0, g = 0, b = 0, a = 0;
  if (!MasksForPixelFormat(format, &bpp, &r, &g, &b, &a)) {
    return false;
  }
  desc = {};
  desc.BytesPerPixel = (bpp + 7) / 8;
  desc.IsIndexed = (format == SDL_PIXELFORMAT_INDEX8);
  desc.Is24BitArray = (format == SDL_PIXELFORMAT_RGB24 || format == SDL_PIXELFORMAT_BGR24);
  desc.Rmask = r;
  desc.Gmask = g;
  desc.Bmask = b;
  desc.Amask = a;
  FillPixelDescShifts(desc);
  return true;
}

bool DescForSurfaceFormat(const SDL_PixelFormat* fmt, PixelDesc& desc) {
  if (!fmt) {
    return false;
  }
  desc = {};
  desc.BytesPerPixel = fmt->BytesPerPixel;
  // Only true palette-indexed formats count as indexed here; packed 1-byte
  // formats such as RGB332 still go through the mask path.
  desc.IsIndexed = (fmt->format == SDL_PIXELFORMAT_INDEX8) ||
                   (fmt->format == SDL_PIXELFORMAT_INDEX4LSB) ||
                   (fmt->format == SDL_PIXELFORMAT_INDEX4MSB) ||
                   (fmt->format == SDL_PIXELFORMAT_INDEX1LSB) ||
                   (fmt->format == SDL_PIXELFORMAT_INDEX1MSB);
  desc.Is24BitArray = (fmt->format == SDL_PIXELFORMAT_RGB24) ||
                      (fmt->format == SDL_PIXELFORMAT_BGR24) ||
                      (fmt->BytesPerPixel == 3);
  desc.Rmask = fmt->Rmask;
  desc.Gmask = fmt->Gmask;
  desc.Bmask = fmt->Bmask;
  desc.Amask = fmt->Amask;
  // Some callers hand back formats with masks unset for 24-bit array formats;
  // recover sane little-endian masks in that case.
  if (desc.Is24BitArray && desc.Rmask == 0 && desc.Gmask == 0 && desc.Bmask == 0) {
    if (fmt->format == SDL_PIXELFORMAT_BGR24) {
      desc.Rmask = 0xFF0000;
      desc.Gmask = 0x00FF00;
      desc.Bmask = 0x0000FF;
    } else {
      desc.Rmask = 0x0000FF;
      desc.Gmask = 0x00FF00;
      desc.Bmask = 0xFF0000;
    }
  }
  desc.Palette = fmt->palette;
  FillPixelDescShifts(desc);
  // Prefer the format's own shift/loss bookkeeping when the masks are set;
  // it is authoritative for the host-allocated surface layouts.
  if (fmt->Rmask != 0) {
    desc.Rshift = fmt->Rshift;
    desc.Rbits = fmt->Rloss <= 8 ? 8 - fmt->Rloss : desc.Rbits;
  }
  if (fmt->Gmask != 0) {
    desc.Gshift = fmt->Gshift;
    desc.Gbits = fmt->Gloss <= 8 ? 8 - fmt->Gloss : desc.Gbits;
  }
  if (fmt->Bmask != 0) {
    desc.Bshift = fmt->Bshift;
    desc.Bbits = fmt->Bloss <= 8 ? 8 - fmt->Bloss : desc.Bbits;
  }
  if (fmt->Amask != 0) {
    desc.Ashift = fmt->Ashift;
    desc.Abits = fmt->Aloss <= 8 ? 8 - fmt->Aloss : desc.Abits;
  }
  return true;
}

Uint32 ReadPixel(const Uint8* row, int x, const PixelDesc& desc) {
  switch (desc.BytesPerPixel) {
    case 1: return row[x];
    case 2: {
      Uint16 v;
      std::memcpy(&v, row + static_cast<size_t>(x) * 2, 2);
      return v;
    }
    case 3: {
      const Uint8* p = row + static_cast<size_t>(x) * 3;
      return static_cast<Uint32>(p[0]) |
             (static_cast<Uint32>(p[1]) << 8) |
             (static_cast<Uint32>(p[2]) << 16);
    }
    case 4: {
      Uint32 v;
      std::memcpy(&v, row + static_cast<size_t>(x) * 4, 4);
      return v;
    }
    default: return 0;
  }
}

void WritePixel(Uint8* row, int x, Uint32 value, const PixelDesc& desc) {
  switch (desc.BytesPerPixel) {
    case 1:
      row[x] = static_cast<Uint8>(value);
      return;
    case 2: {
      const Uint16 v = static_cast<Uint16>(value);
      std::memcpy(row + static_cast<size_t>(x) * 2, &v, 2);
      return;
    }
    case 3: {
      Uint8* p = row + static_cast<size_t>(x) * 3;
      p[0] = static_cast<Uint8>(value & 0xFF);
      p[1] = static_cast<Uint8>((value >> 8) & 0xFF);
      p[2] = static_cast<Uint8>((value >> 16) & 0xFF);
      return;
    }
    case 4: {
      std::memcpy(row + static_cast<size_t>(x) * 4, &value, 4);
      return;
    }
    default: return;
  }
}

void DecodePixelRGBA(const PixelDesc& desc, Uint32 pixel, Uint8* r, Uint8* g, Uint8* b, Uint8* a) {
  if (desc.IsIndexed) {
    const unsigned index = pixel & 0xFF;
    if (desc.Palette && index < static_cast<unsigned>(desc.Palette->ncolors) && desc.Palette->colors) {
      const SDL_Color& c = desc.Palette->colors[index];
      *r = c.r;
      *g = c.g;
      *b = c.b;
      *a = c.a;
      return;
    }
    *r = *g = *b = static_cast<Uint8>(index);
    *a = 255;
    return;
  }
  *r = ExpandChannelTo8((pixel & desc.Rmask) >> desc.Rshift, desc.Rbits);
  *g = ExpandChannelTo8((pixel & desc.Gmask) >> desc.Gshift, desc.Gbits);
  *b = ExpandChannelTo8((pixel & desc.Bmask) >> desc.Bshift, desc.Bbits);
  *a = desc.Amask ? ExpandChannelTo8((pixel & desc.Amask) >> desc.Ashift, desc.Abits) : 255;
}

Uint32 EncodePixelRGBA(const PixelDesc& desc, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
  return (ShrinkChannelFrom8(r, desc.Rbits) << desc.Rshift) |
         (ShrinkChannelFrom8(g, desc.Gbits) << desc.Gshift) |
         (ShrinkChannelFrom8(b, desc.Bbits) << desc.Bshift) |
         (ShrinkChannelFrom8(a, desc.Abits) << desc.Ashift);
}

bool ConvertPixelsGeneric(int width, int height,
                          const PixelDesc& src_desc, const void* src_pixels, int src_pitch,
                          const PixelDesc& dst_desc, void* dst_pixels, int dst_pitch) {
  if (!src_pixels || !dst_pixels || width <= 0 || height <= 0) {
    return false;
  }
  if (src_desc.IsIndexed && !src_desc.Palette && dst_desc.BytesPerPixel != 1) {
    return false;
  }
  if (dst_desc.IsIndexed) {
    // Only trivially support indexed destinations for index->index copies.
    if (!(src_desc.IsIndexed && src_desc.BytesPerPixel == 1 && dst_desc.BytesPerPixel == 1)) {
      return false;
    }
  }

  const auto* src_row = static_cast<const Uint8*>(src_pixels);
  auto* dst_row = static_cast<Uint8*>(dst_pixels);
  const size_t src_row_bytes = static_cast<size_t>(width) * src_desc.BytesPerPixel;
  const size_t dst_row_bytes = static_cast<size_t>(width) * dst_desc.BytesPerPixel;

  const bool same_layout =
      src_desc.BytesPerPixel == dst_desc.BytesPerPixel &&
      src_desc.Rmask == dst_desc.Rmask && src_desc.Gmask == dst_desc.Gmask &&
      src_desc.Bmask == dst_desc.Bmask && src_desc.Amask == dst_desc.Amask &&
      src_desc.IsIndexed == dst_desc.IsIndexed &&
      src_desc.Is24BitArray == dst_desc.Is24BitArray;

  for (int y = 0; y < height; ++y) {
    if (same_layout) {
      std::memcpy(dst_row, src_row, dst_row_bytes < src_row_bytes ? dst_row_bytes : src_row_bytes);
    } else {
      for (int x = 0; x < width; ++x) {
        const Uint32 pixel = ReadPixel(src_row, x, src_desc);
        Uint8 r, g, b, a;
        DecodePixelRGBA(src_desc, pixel, &r, &g, &b, &a);
        WritePixel(dst_row, x, EncodePixelRGBA(dst_desc, r, g, b, a), dst_desc);
      }
    }
    src_row += src_pitch;
    dst_row += dst_pitch;
  }
  return true;
}

bool FillSurfaceRect(SDL_Surface* surface, const SDL_Rect& rect, Uint32 color) {
  if (!surface || !surface->pixels || !surface->format) {
    return false;
  }
  const int bpp = surface->format->BytesPerPixel;
  if (bpp < 1 || bpp > 4) {
    return false;
  }
  auto* base = static_cast<Uint8*>(surface->pixels);
  for (int y = rect.y; y < rect.y + rect.h; ++y) {
    Uint8* row = base + static_cast<size_t>(y) * surface->pitch;
    switch (bpp) {
      case 1: {
        std::memset(row + rect.x, static_cast<int>(color & 0xFF), static_cast<size_t>(rect.w));
        break;
      }
      case 2: {
        const Uint16 v = static_cast<Uint16>(color);
        for (int x = 0; x < rect.w; ++x) {
          std::memcpy(row + static_cast<size_t>(rect.x + x) * 2, &v, 2);
        }
        break;
      }
      case 3: {
        const Uint8 b0 = static_cast<Uint8>(color & 0xFF);
        const Uint8 b1 = static_cast<Uint8>((color >> 8) & 0xFF);
        const Uint8 b2 = static_cast<Uint8>((color >> 16) & 0xFF);
        for (int x = 0; x < rect.w; ++x) {
          Uint8* p = row + static_cast<size_t>(rect.x + x) * 3;
          p[0] = b0;
          p[1] = b1;
          p[2] = b2;
        }
        break;
      }
      case 4: {
        for (int x = 0; x < rect.w; ++x) {
          std::memcpy(row + static_cast<size_t>(rect.x + x) * 4, &color, 4);
        }
        break;
      }
      default:
        return false;
    }
  }
  return true;
}

bool IntersectRect(const SDL_Rect& a, const SDL_Rect& b, SDL_Rect* out) {
  const int x1 = a.x > b.x ? a.x : b.x;
  const int y1 = a.y > b.y ? a.y : b.y;
  const int x2 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
  const int y2 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
  out->x = x1;
  out->y = y1;
  out->w = x2 - x1;
  out->h = y2 - y1;
  return out->w > 0 && out->h > 0;
}

} // namespace

// ===========================================================================
// Init / shutdown
// ===========================================================================

extern "C" int SDL_InitSubSystem(Uint32 flags) {

  fprintf(stderr, "[SDL2 thunk] SDL_InitSubSystem(flags=0x%x)\n", static_cast<unsigned>(flags));
  const Uint32 normalized = NormalizeInitFlags(flags);
  Uint32 added = 0;
  {
    std::lock_guard<std::mutex> guard(InitStateMutex);
    added = normalized & ~InitializedSubsystemMask;
  }

  if (added != 0) {
    if (fexfn_pack_FEX_SDL2_Init(added) != 0) {
      fprintf(stderr, "[SDL2 thunk] SDL_InitSubSystem => -1 (host init failed flags=0x%x)\n",
              static_cast<unsigned>(added));
      return SDL_SetError("SDL_InitSubSystem: host init failed for flags 0x%x", static_cast<unsigned>(added));
    }
  }

  {
    std::lock_guard<std::mutex> guard(InitStateMutex);
    for (int bit = 0; bit < 31; ++bit) {
      if ((normalized & (1u << bit)) != 0) {
        ++SubsystemRefCounts[bit];
      }
    }
    InitializedSubsystemMask |= normalized;
  }
  fprintf(stderr, "[SDL2 thunk] SDL_InitSubSystem => 0 added=0x%x mask=0x%x\n",
          static_cast<unsigned>(added), static_cast<unsigned>(InitializedSubsystemMask));
  return 0;
}

extern "C" int SDL_Init(Uint32 flags) {

  fprintf(stderr, "[SDL2 thunk] SDL_Init(flags=0x%x)\n", static_cast<unsigned>(flags));
  return SDL_InitSubSystem(flags);
}

extern "C" void SDL_Quit() {

  {
    std::lock_guard<std::mutex> guard(InitStateMutex);
    SubsystemRefCounts.fill(0);
    InitializedSubsystemMask = 0;
  }
  {
    std::scoped_lock lock {SyntheticEventQueueMutex};
    SyntheticEventQueue.clear();
  }
  fexfn_pack_FEX_SDL2_Quit();
}

extern "C" void SDL_QuitSubSystem(Uint32 flags) {

  const Uint32 normalized = NormalizeInitFlags(flags);
  bool call_host_quit = false;
  {
    std::lock_guard<std::mutex> guard(InitStateMutex);
    const Uint32 previous = InitializedSubsystemMask;
    for (int bit = 0; bit < 31; ++bit) {
      const Uint32 mask = 1u << bit;
      if ((normalized & mask) != 0 && SubsystemRefCounts[bit] > 0) {
        if (--SubsystemRefCounts[bit] == 0) {
          InitializedSubsystemMask &= ~mask;
        }
      }
    }
    call_host_quit = previous != 0 && InitializedSubsystemMask == 0;
  }
  if (call_host_quit) {
    fexfn_pack_FEX_SDL2_Quit();
  }
}

extern "C" Uint32 SDL_WasInit(Uint32 flags) {

  std::lock_guard<std::mutex> guard(InitStateMutex);
  return flags == 0 ? InitializedSubsystemMask : (InitializedSubsystemMask & flags);
}

// ===========================================================================
// Video driver + display queries
// ===========================================================================

extern "C" int SDL_VideoInit(const char* driver_name) {

  fprintf(stderr, "[SDL2 thunk] SDL_VideoInit(driver=%s)\n", driver_name ? driver_name : "(null)");
  if (driver_name && std::strcmp(driver_name, "android") != 0) {
    return SDL_SetError("SDL_VideoInit: driver '%s' not available (only 'android')", driver_name);
  }
  return SDL_InitSubSystem(SDL_INIT_VIDEO);
}

extern "C" void SDL_VideoQuit() {

  SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

extern "C" const char* SDL_GetCurrentVideoDriver() {

  return "android";
}

extern "C" int SDL_GetNumVideoDrivers() {

  return 1;
}

extern "C" const char* SDL_GetVideoDriver(int index) {

  return index == 0 ? "android" : nullptr;
}

extern "C" int SDL_GetNumVideoDisplays() {

  return 1;
}

extern "C" const char* SDL_GetDisplayName(int displayIndex) {

  return IsKnownDisplay(displayIndex) ? "Android Display" : nullptr;
}

extern "C" int SDL_GetDisplayBounds(int displayIndex, SDL_Rect* rect) {

  if (!rect || !IsKnownDisplay(displayIndex)) {
    return SDL_SetError("SDL_GetDisplayBounds: invalid display index %d", displayIndex);
  }
  RefreshFallbackWindowSizeFromHost(nullptr);
  rect->x = 0;
  rect->y = 0;
  GetFallbackWindowSize(&rect->w, &rect->h);
  return 0;
}

extern "C" int SDL_GetDisplayUsableBounds(int displayIndex, SDL_Rect* rect) {

  return SDL_GetDisplayBounds(displayIndex, rect);
}

extern "C" int SDL_GetDisplayDPI(int displayIndex, float* ddpi, float* hdpi, float* vdpi) {

  if (!IsKnownDisplay(displayIndex)) {
    return SDL_SetError("SDL_GetDisplayDPI: invalid display index %d", displayIndex);
  }
  // Plausible mid-range Android density (xhdpi).
  if (ddpi) {
    *ddpi = 320.0f;
  }
  if (hdpi) {
    *hdpi = 320.0f;
  }
  if (vdpi) {
    *vdpi = 320.0f;
  }
  return 0;
}

extern "C" SDL_DisplayOrientation SDL_GetDisplayOrientation(int displayIndex) {

  return IsKnownDisplay(displayIndex) ? SDL_ORIENTATION_LANDSCAPE : SDL_ORIENTATION_UNKNOWN;
}

extern "C" int SDL_GetNumDisplayModes(int displayIndex) {

  if (!IsKnownDisplay(displayIndex)) {
    return SDL_SetError("SDL_GetNumDisplayModes: invalid display index %d", displayIndex);
  }
  return 1;
}

extern "C" int SDL_GetDisplayMode(int displayIndex, int modeIndex, SDL_DisplayMode* mode) {

  if (!IsKnownDisplay(displayIndex) || modeIndex != 0 || !mode) {
    return SDL_SetError("SDL_GetDisplayMode: invalid display/mode index %d/%d", displayIndex, modeIndex);
  }
  RefreshFallbackWindowSizeFromHost(nullptr);
  *mode = BuildFallbackDisplayMode();
  return 0;
}

extern "C" int SDL_GetDesktopDisplayMode(int displayIndex, SDL_DisplayMode* mode) {

  return SDL_GetDisplayMode(displayIndex, 0, mode);
}

extern "C" int SDL_GetCurrentDisplayMode(int displayIndex, SDL_DisplayMode* mode) {

  return SDL_GetDisplayMode(displayIndex, 0, mode);
}

extern "C" SDL_DisplayMode* SDL_GetClosestDisplayMode(int displayIndex, const SDL_DisplayMode* mode, SDL_DisplayMode* closest) {

  (void)mode;
  if (!closest || SDL_GetDisplayMode(displayIndex, 0, closest) != 0) {
    return nullptr;
  }
  return closest;
}

extern "C" int SDL_GetPointDisplayIndex(const SDL_Point*) {

  return PrimaryDisplayIndex;
}

extern "C" int SDL_GetRectDisplayIndex(const SDL_Rect*) {

  return PrimaryDisplayIndex;
}

extern "C" int SDL_GetWindowDisplayIndex(SDL_Window* window) {

  (void)window;
  return PrimaryDisplayIndex;
}

// ===========================================================================
// Window management
// ===========================================================================

extern "C" SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags) {

  fprintf(stderr,
          "[SDL2 thunk] SDL_CreateWindow(title=%s x=%d y=%d w=%d h=%d flags=0x%x)\n",
          title ? title : "(null)",
          x, y, w, h,
          static_cast<unsigned>(flags));
  SDL_Window* window = fexfn_pack_FEX_SDL2_CreateWindow(title, x, y, w, h, flags);
  fprintf(stderr, "[SDL2 thunk] SDL_CreateWindow => %p\n", window);
  if (!window) {
    SDL_SetError("SDL_CreateWindow: host window creation failed");
    return nullptr;
  }

  UpdateFallbackWindowSize(w, h);
  WindowPosX.store(SDL_WINDOWPOS_ISUNDEFINED(x) || SDL_WINDOWPOS_ISCENTERED(x) ? 0 : x, std::memory_order_release);
  WindowPosY.store(SDL_WINDOWPOS_ISUNDEFINED(y) || SDL_WINDOWPOS_ISCENTERED(y) ? 0 : y, std::memory_order_release);
  WindowMinWidth.store(0, std::memory_order_release);
  WindowMinHeight.store(0, std::memory_order_release);
  WindowMaxWidth.store(0, std::memory_order_release);
  WindowMaxHeight.store(0, std::memory_order_release);
  PrimaryWindowPointer.store(reinterpret_cast<uintptr_t>(window), std::memory_order_release);
  WindowSurfacePointer.store(0, std::memory_order_release);
  WindowBrightness.store(1.0f, std::memory_order_release);
  WindowOpacity.store(1.0f, std::memory_order_release);
  {
    std::lock_guard<std::mutex> guard(WindowTitleMutex);
    WindowTitle = title ? title : "";
  }

  Uint32 initial_flags = flags;
  if ((initial_flags & SDL_WINDOW_HIDDEN) == 0) {
    initial_flags |= (SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
    initial_flags &= ~(SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED);
  } else {
    initial_flags &= ~(SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
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
  return window;
}

extern "C" SDL_Window* SDL_CreateWindowFrom(const void* data) {

  (void)data;
  SDL_SetError("SDL_CreateWindowFrom: creating a window from native handles is not supported");
  return nullptr;
}

extern "C" void SDL_DestroyWindow(SDL_Window* window) {

  if (window) {
    const uintptr_t current = PrimaryWindowPointer.load(std::memory_order_acquire);
    if (current == reinterpret_cast<uintptr_t>(window)) {
      PrimaryWindowPointer.store(0, std::memory_order_release);
    }
    const uintptr_t surface = WindowSurfacePointer.load(std::memory_order_acquire);
    if (surface != 0) {
      fexfn_pack_FEX_SDL2_FreeSurface(reinterpret_cast<SDL_Surface*>(surface));
      EraseSurfaceExtra(reinterpret_cast<SDL_Surface*>(surface));
      WindowSurfacePointer.store(0, std::memory_order_release);
    }
    {
      std::lock_guard<std::mutex> guard(WindowDataMutex);
      WindowDataByWindow.erase(window);
    }
  }
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
  fexfn_pack_FEX_SDL2_DestroyWindow(window);
}

extern "C" Uint32 SDL_GetWindowID(SDL_Window* window) {

  return window ? PrimaryWindowId : 0;
}

extern "C" SDL_Window* SDL_GetWindowFromID(Uint32 id) {

  if (id != PrimaryWindowId) {
    return nullptr;
  }
  return reinterpret_cast<SDL_Window*>(PrimaryWindowPointer.load(std::memory_order_acquire));
}

extern "C" Uint32 SDL_GetWindowFlags(SDL_Window* window) {

  return window ? WindowFlagsState.load(std::memory_order_acquire) : 0;
}

extern "C" void SDL_SetWindowTitle(SDL_Window* window, const char* title) {

  if (!window) {
    return;
  }
  std::lock_guard<std::mutex> guard(WindowTitleMutex);
  WindowTitle = title ? title : "";
}

extern "C" const char* SDL_GetWindowTitle(SDL_Window* window) {

  if (!window) {
    return "";
  }
  std::lock_guard<std::mutex> guard(WindowTitleMutex);
  return WindowTitle.c_str();
}

extern "C" void SDL_SetWindowIcon(SDL_Window* window, SDL_Surface* icon) {

  if (!window) {
    return;
  }
  WindowIconSurface.store(reinterpret_cast<uintptr_t>(icon), std::memory_order_release);
}

extern "C" void* SDL_SetWindowData(SDL_Window* window, const char* name, void* userdata) {

  if (!window || !name) {
    return nullptr;
  }
  std::lock_guard<std::mutex> guard(WindowDataMutex);
  auto& table = WindowDataByWindow[window];
  auto it = table.find(name);
  void* previous = it != table.end() ? it->second : nullptr;
  if (userdata) {
    table[name] = userdata;
  } else if (it != table.end()) {
    table.erase(it);
  }
  return previous;
}

extern "C" void* SDL_GetWindowData(SDL_Window* window, const char* name) {

  if (!window || !name) {
    return nullptr;
  }
  std::lock_guard<std::mutex> guard(WindowDataMutex);
  auto table_it = WindowDataByWindow.find(window);
  if (table_it == WindowDataByWindow.end()) {
    return nullptr;
  }
  auto it = table_it->second.find(name);
  return it != table_it->second.end() ? it->second : nullptr;
}

extern "C" void SDL_SetWindowPosition(SDL_Window* window, int x, int y) {

  if (!window) {
    return;
  }
  const int px = SDL_WINDOWPOS_ISUNDEFINED(x) || SDL_WINDOWPOS_ISCENTERED(x) ? 0 : x;
  const int py = SDL_WINDOWPOS_ISUNDEFINED(y) || SDL_WINDOWPOS_ISCENTERED(y) ? 0 : y;
  WindowPosX.store(px, std::memory_order_release);
  WindowPosY.store(py, std::memory_order_release);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_MOVED, px, py);
}

extern "C" void SDL_GetWindowPosition(SDL_Window* window, int* x, int* y) {

  if (x) {
    *x = window ? WindowPosX.load(std::memory_order_acquire) : 0;
  }
  if (y) {
    *y = window ? WindowPosY.load(std::memory_order_acquire) : 0;
  }
}

extern "C" void SDL_SetWindowSize(SDL_Window* window, int w, int h) {

  if (!window || w <= 0 || h <= 0) {
    return;
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
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_RESIZED, clamped_w, clamped_h);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_SIZE_CHANGED, clamped_w, clamped_h);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_EXPOSED);
}

extern "C" void SDL_GetWindowSize(SDL_Window* window, int* w, int* h) {

  if (window) {
    int host_w = 0;
    int host_h = 0;
    fexfn_pack_FEX_SDL2_GetWindowSize(window, &host_w, &host_h);
    if (host_w > 0 && host_h > 0) {
      UpdateFallbackWindowSize(host_w, host_h);
    }
  }
  GetFallbackWindowSize(w, h);
}

extern "C" int SDL_GetWindowBordersSize(SDL_Window* window, int* top, int* left, int* bottom, int* right) {

  if (!window) {
    return SDL_SetError("SDL_GetWindowBordersSize: window is null");
  }
  if (top) {
    *top = 0;
  }
  if (left) {
    *left = 0;
  }
  if (bottom) {
    *bottom = 0;
  }
  if (right) {
    *right = 0;
  }
  return 0;
}

extern "C" void SDL_GetWindowSizeInPixels(SDL_Window* window, int* w, int* h) {

  SDL_GetWindowSize(window, w, h);
}

extern "C" void SDL_SetWindowMinimumSize(SDL_Window* window, int min_w, int min_h) {

  if (!window || min_w < 0 || min_h < 0) {
    return;
  }
  WindowMinWidth.store(min_w, std::memory_order_release);
  WindowMinHeight.store(min_h, std::memory_order_release);
}

extern "C" void SDL_GetWindowMinimumSize(SDL_Window* window, int* w, int* h) {

  if (w) {
    *w = window ? WindowMinWidth.load(std::memory_order_acquire) : 0;
  }
  if (h) {
    *h = window ? WindowMinHeight.load(std::memory_order_acquire) : 0;
  }
}

extern "C" void SDL_SetWindowMaximumSize(SDL_Window* window, int max_w, int max_h) {

  if (!window || max_w < 0 || max_h < 0) {
    return;
  }
  WindowMaxWidth.store(max_w, std::memory_order_release);
  WindowMaxHeight.store(max_h, std::memory_order_release);
}

extern "C" void SDL_GetWindowMaximumSize(SDL_Window* window, int* w, int* h) {

  if (w) {
    *w = window ? WindowMaxWidth.load(std::memory_order_acquire) : 0;
  }
  if (h) {
    *h = window ? WindowMaxHeight.load(std::memory_order_acquire) : 0;
  }
}

extern "C" void SDL_SetWindowBordered(SDL_Window* window, SDL_bool bordered) {

  if (!window) {
    return;
  }
  if (bordered) {
    UpdateWindowFlags(0, SDL_WINDOW_BORDERLESS);
  } else {
    UpdateWindowFlags(SDL_WINDOW_BORDERLESS, 0);
  }
}

extern "C" void SDL_SetWindowResizable(SDL_Window* window, SDL_bool resizable) {

  if (!window) {
    return;
  }
  if (resizable) {
    UpdateWindowFlags(SDL_WINDOW_RESIZABLE, 0);
  } else {
    UpdateWindowFlags(0, SDL_WINDOW_RESIZABLE);
  }
}

extern "C" void SDL_SetWindowAlwaysOnTop(SDL_Window* window, SDL_bool on_top) {

  if (!window) {
    return;
  }
  if (on_top) {
    UpdateWindowFlags(SDL_WINDOW_ALWAYS_ON_TOP, 0);
  } else {
    UpdateWindowFlags(0, SDL_WINDOW_ALWAYS_ON_TOP);
  }
}

extern "C" void SDL_ShowWindow(SDL_Window* window) {

  fprintf(stderr, "[SDL2 thunk] SDL_ShowWindow(window=%p)\n", window);
  if (!window) {
    return;
  }
  SetWindowVisibleFocused(true);
  QueueSyntheticWindowShownEvents();
}

extern "C" void SDL_HideWindow(SDL_Window* window) {

  fprintf(stderr, "[SDL2 thunk] SDL_HideWindow(window=%p)\n", window);
  if (!window) {
    return;
  }
  SetWindowVisibleFocused(false);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_HIDDEN);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_FOCUS_LOST);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_LEAVE);
}

extern "C" void SDL_RaiseWindow(SDL_Window* window) {

  if (!window) {
    return;
  }
  UpdateWindowFlags(SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS, 0);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_ENTER);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_FOCUS_GAINED);
}

extern "C" void SDL_MaximizeWindow(SDL_Window* window) {

  if (!window) {
    return;
  }
  UpdateWindowFlags(SDL_WINDOW_MAXIMIZED, SDL_WINDOW_MINIMIZED);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_MAXIMIZED);
}

extern "C" void SDL_MinimizeWindow(SDL_Window* window) {

  if (!window) {
    return;
  }
  UpdateWindowFlags(SDL_WINDOW_MINIMIZED,
                    SDL_WINDOW_MAXIMIZED | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_MINIMIZED);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_FOCUS_LOST);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_LEAVE);
}

extern "C" void SDL_RestoreWindow(SDL_Window* window) {

  if (!window) {
    return;
  }
  UpdateWindowFlags(SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS,
                    SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_HIDDEN);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_RESTORED);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_EXPOSED);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_ENTER);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_FOCUS_GAINED);
}

extern "C" int SDL_SetWindowFullscreen(SDL_Window* window, Uint32 flags) {

  fprintf(stderr, "[SDL2 thunk] SDL_SetWindowFullscreen(window=%p flags=0x%x)\n",
          window, static_cast<unsigned>(flags));
  if (!window) {
    return SDL_SetError("SDL_SetWindowFullscreen: window is null");
  }

  if ((flags & SDL_WINDOW_FULLSCREEN) != 0) {
    // Preserve the desktop-fullscreen bit (0x1000) inside our flag mirror so
    // SDL_GetWindowFlags reports SDL_WINDOW_FULLSCREEN_DESKTOP correctly.
    const Uint32 desktop_bit = flags & 0x00001000;
    UpdateWindowFlags(SDL_WINDOW_FULLSCREEN | desktop_bit,
                      SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_HIDDEN);
    QueueSyntheticWindowEvent(SDL_WINDOWEVENT_EXPOSED);
  } else {
    UpdateWindowFlags(SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS,
                      SDL_WINDOW_FULLSCREEN_DESKTOP);
    QueueSyntheticWindowEvent(SDL_WINDOWEVENT_RESTORED);
    QueueSyntheticWindowEvent(SDL_WINDOWEVENT_EXPOSED);
  }
  return 0;
}

extern "C" SDL_bool SDL_HasWindowSurface(SDL_Window* window) {

  (void)window;
  return WindowSurfacePointer.load(std::memory_order_acquire) != 0 ? SDL_TRUE : SDL_FALSE;
}

extern "C" SDL_Surface* SDL_GetWindowSurface(SDL_Window* window) {

  if (!window) {
    SDL_SetError("SDL_GetWindowSurface: window is null");
    return nullptr;
  }
  uintptr_t surface = WindowSurfacePointer.load(std::memory_order_acquire);
  if (surface != 0) {
    return reinterpret_cast<SDL_Surface*>(surface);
  }

  int w = 0;
  int h = 0;
  SDL_GetWindowSize(window, &w, &h);
  if (w <= 0 || h <= 0) {
    SDL_SetError("SDL_GetWindowSurface: window has no valid size");
    return nullptr;
  }

  SDL_Surface* created = fexfn_pack_FEX_SDL2_CreateRGBSurface(
      0, w, h, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
  if (!created) {
    SDL_SetError("SDL_GetWindowSurface: host surface creation failed");
    return nullptr;
  }
  WindowSurfacePointer.store(reinterpret_cast<uintptr_t>(created), std::memory_order_release);
  return created;
}

extern "C" int SDL_UpdateWindowSurface(SDL_Window* window) {

  if (!window) {
    return SDL_SetError("SDL_UpdateWindowSurface: window is null");
  }
  // The host has no software-present bridge yet; presenting is a benign no-op.
  static std::atomic<uint32_t> warn_budget {4};
  uint32_t remaining = warn_budget.load(std::memory_order_acquire);
  while (remaining > 0 &&
         !warn_budget.compare_exchange_weak(remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  if (remaining > 0) {
    fprintf(stderr, "[SDL2 thunk] SDL_UpdateWindowSurface: present is a no-op (no host bridge)\n");
  }
  return 0;
}

extern "C" int SDL_UpdateWindowSurfaceRects(SDL_Window* window, const SDL_Rect* rects, int numrects) {

  (void)rects;
  (void)numrects;
  return SDL_UpdateWindowSurface(window);
}

extern "C" int SDL_DestroyWindowSurface(SDL_Window* window) {

  (void)window;
  const uintptr_t surface = WindowSurfacePointer.exchange(0, std::memory_order_acq_rel);
  if (surface != 0) {
    fexfn_pack_FEX_SDL2_FreeSurface(reinterpret_cast<SDL_Surface*>(surface));
    EraseSurfaceExtra(reinterpret_cast<SDL_Surface*>(surface));
  }
  return 0;
}

extern "C" void SDL_SetWindowGrab(SDL_Window* window, SDL_bool grabbed) {

  if (!window) {
    return;
  }
  if (grabbed) {
    UpdateWindowFlags(SDL_WINDOW_MOUSE_GRABBED | SDL_WINDOW_KEYBOARD_GRABBED, 0);
  } else {
    UpdateWindowFlags(0, SDL_WINDOW_MOUSE_GRABBED | SDL_WINDOW_KEYBOARD_GRABBED);
  }
}

extern "C" void SDL_SetWindowKeyboardGrab(SDL_Window* window, SDL_bool grabbed) {

  if (!window) {
    return;
  }
  if (grabbed) {
    UpdateWindowFlags(SDL_WINDOW_KEYBOARD_GRABBED, 0);
  } else {
    UpdateWindowFlags(0, SDL_WINDOW_KEYBOARD_GRABBED);
  }
}

extern "C" void SDL_SetWindowMouseGrab(SDL_Window* window, SDL_bool grabbed) {

  if (!window) {
    return;
  }
  if (grabbed) {
    UpdateWindowFlags(SDL_WINDOW_MOUSE_GRABBED, 0);
  } else {
    UpdateWindowFlags(0, SDL_WINDOW_MOUSE_GRABBED);
  }
}

extern "C" SDL_bool SDL_GetWindowGrab(SDL_Window* window) {

  return (SDL_GetWindowFlags(window) & SDL_WINDOW_MOUSE_GRABBED) != 0 ? SDL_TRUE : SDL_FALSE;
}

extern "C" SDL_bool SDL_GetWindowKeyboardGrab(SDL_Window* window) {

  return (SDL_GetWindowFlags(window) & SDL_WINDOW_KEYBOARD_GRABBED) != 0 ? SDL_TRUE : SDL_FALSE;
}

extern "C" SDL_bool SDL_GetWindowMouseGrab(SDL_Window* window) {

  return SDL_GetWindowGrab(window);
}

extern "C" SDL_Window* SDL_GetGrabbedWindow() {

  const Uint32 flags = WindowFlagsState.load(std::memory_order_acquire);
  if ((flags & (SDL_WINDOW_MOUSE_GRABBED | SDL_WINDOW_KEYBOARD_GRABBED)) == 0) {
    return nullptr;
  }
  return reinterpret_cast<SDL_Window*>(PrimaryWindowPointer.load(std::memory_order_acquire));
}

extern "C" int SDL_SetWindowMouseRect(SDL_Window* window, const SDL_Rect* rect) {

  if (!window) {
    return SDL_SetError("SDL_SetWindowMouseRect: window is null");
  }
  if (rect) {
    WindowMouseRect = *rect;
    WindowHasMouseRect = true;
  } else {
    WindowHasMouseRect = false;
  }
  return 0;
}

extern "C" const SDL_Rect* SDL_GetWindowMouseRect(SDL_Window* window) {

  if (!window || !WindowHasMouseRect) {
    return nullptr;
  }
  return &WindowMouseRect;
}

extern "C" int SDL_SetWindowBrightness(SDL_Window* window, float brightness) {

  if (!window) {
    return SDL_SetError("SDL_SetWindowBrightness: window is null");
  }
  if (brightness < 0.0f) {
    brightness = 0.0f;
  }
  if (brightness > 1.0f) {
    brightness = 1.0f;
  }
  WindowBrightness.store(brightness, std::memory_order_release);
  return 0;
}

extern "C" float SDL_GetWindowBrightness(SDL_Window* window) {

  return window ? WindowBrightness.load(std::memory_order_acquire) : 1.0f;
}

extern "C" int SDL_SetWindowOpacity(SDL_Window* window, float opacity) {

  if (!window) {
    return SDL_SetError("SDL_SetWindowOpacity: window is null");
  }
  if (opacity < 0.0f) {
    opacity = 0.0f;
  }
  if (opacity > 1.0f) {
    opacity = 1.0f;
  }
  WindowOpacity.store(opacity, std::memory_order_release);
  return 0;
}

extern "C" int SDL_GetWindowOpacity(SDL_Window* window, float* out_opacity) {

  if (!window || !out_opacity) {
    return SDL_SetError("SDL_GetWindowOpacity: invalid arguments");
  }
  *out_opacity = WindowOpacity.load(std::memory_order_acquire);
  return 0;
}

extern "C" int SDL_SetWindowModalFor(SDL_Window* modal_window, SDL_Window* parent_window) {

  (void)modal_window;
  (void)parent_window;
  return 0;
}

extern "C" int SDL_SetWindowInputFocus(SDL_Window* window) {

  if (!window) {
    return SDL_SetError("SDL_SetWindowInputFocus: window is null");
  }
  UpdateWindowFlags(SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS | SDL_WINDOW_SHOWN,
                    SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_FOCUS_GAINED);
  QueueSyntheticWindowEvent(SDL_WINDOWEVENT_ENTER);
  return 0;
}

extern "C" int SDL_SetWindowGammaRamp(SDL_Window* window, const Uint16* red, const Uint16* green, const Uint16* blue) {

  if (!window || !red || !green || !blue) {
    return SDL_SetError("SDL_SetWindowGammaRamp: invalid arguments");
  }
  std::memcpy(WindowGammaRamp[0].data(), red, sizeof(Uint16) * 256);
  std::memcpy(WindowGammaRamp[1].data(), green, sizeof(Uint16) * 256);
  std::memcpy(WindowGammaRamp[2].data(), blue, sizeof(Uint16) * 256);
  WindowGammaRampSet = true;
  return 0;
}

extern "C" int SDL_GetWindowGammaRamp(SDL_Window* window, Uint16* red, Uint16* green, Uint16* blue) {

  if (!window) {
    return SDL_SetError("SDL_GetWindowGammaRamp: window is null");
  }
  if (WindowGammaRampSet) {
    if (red) {
      std::memcpy(red, WindowGammaRamp[0].data(), sizeof(Uint16) * 256);
    }
    if (green) {
      std::memcpy(green, WindowGammaRamp[1].data(), sizeof(Uint16) * 256);
    }
    if (blue) {
      std::memcpy(blue, WindowGammaRamp[2].data(), sizeof(Uint16) * 256);
    }
    return 0;
  }
  Uint16* channels[3] = {red, green, blue};
  for (int c = 0; c < 3; ++c) {
    if (!channels[c]) {
      continue;
    }
    for (int i = 0; i < 256; ++i) {
      channels[c][i] = static_cast<Uint16>(i * 257);
    }
  }
  return 0;
}

extern "C" int SDL_SetWindowHitTest(SDL_Window* window, SDL_HitTest callback, void* callback_data) {

  if (!window) {
    return SDL_SetError("SDL_SetWindowHitTest: window is null");
  }
  WindowHitTestCallback = callback;
  WindowHitTestData = callback_data;
  return 0;
}

extern "C" int SDL_FlashWindow(SDL_Window* window, SDL_FlashOperation operation) {

  (void)window;
  (void)operation;
  return 0;
}

extern "C" void* SDL_GetWindowICCProfile(SDL_Window* window, size_t* size) {

  if (!window) {
    SDL_SetError("SDL_GetWindowICCProfile: window is null");
    return nullptr;
  }
  if (size) {
    *size = 0;
  }
  return nullptr;
}

extern "C" Uint32 SDL_GetWindowPixelFormat(SDL_Window* window) {

  (void)window;
  return SDL_PIXELFORMAT_ARGB8888;
}

extern "C" int SDL_SetWindowDisplayMode(SDL_Window* window, const SDL_DisplayMode* mode) {

  if (!window) {
    return SDL_SetError("SDL_SetWindowDisplayMode: window is null");
  }
  std::lock_guard<std::mutex> guard(WindowStateMutex);
  if (mode) {
    FullscreenWindowMode = *mode;
    HasFullscreenWindowMode = true;
  } else {
    FullscreenWindowMode = {};
    HasFullscreenWindowMode = false;
  }
  return 0;
}

extern "C" int SDL_GetWindowDisplayMode(SDL_Window* window, SDL_DisplayMode* mode) {

  if (!window || !mode) {
    return SDL_SetError("SDL_GetWindowDisplayMode: invalid arguments");
  }
  {
    std::lock_guard<std::mutex> guard(WindowStateMutex);
    if (HasFullscreenWindowMode) {
      *mode = FullscreenWindowMode;
      return 0;
    }
  }
  *mode = BuildFallbackDisplayMode();
  return 0;
}

extern "C" SDL_bool SDL_GetWindowWMInfo(SDL_Window* window, SDL_SysWMinfo* info) {

  (void)window;
  (void)info;
  return SDL_FALSE;
}

// Shaped-window exports (SDL_CreateShapedWindow, SDL_IsShapedWindow,
// SDL_SetWindowShape, SDL_GetShapedWindowMode) are owned by
// libSDL2_FNAStubs.inc and intentionally not defined here.

extern "C" SDL_bool SDL_IsScreenSaverEnabled() {

  return ScreenSaverEnabled.load(std::memory_order_acquire) ? SDL_TRUE : SDL_FALSE;
}

extern "C" void SDL_EnableScreenSaver() {

  ScreenSaverEnabled.store(true, std::memory_order_release);
}

extern "C" void SDL_DisableScreenSaver() {

  ScreenSaverEnabled.store(false, std::memory_order_release);
}

// ===========================================================================
// OpenGL support
// ===========================================================================

extern "C" int SDL_GL_LoadLibrary(const char* path) {

  fprintf(stderr, "[SDL2 thunk] SDL_GL_LoadLibrary(path=%s)\n", path ? path : "(null)");
  GLLibraryLoaded.store(true, std::memory_order_release);
  (void)SDL2EnsureGLResolverInitialized();
  (void)SDL2EnsureEGLResolverInitialized();
  return 0;
}

extern "C" void SDL_GL_UnloadLibrary() {

  fprintf(stderr, "[SDL2 thunk] SDL_GL_UnloadLibrary()\n");
  GLLibraryLoaded.store(false, std::memory_order_release);
}

extern "C" SDL_bool SDL_GL_ExtensionSupported(const char* extension) {

  fprintf(stderr, "[SDL2 thunk] SDL_GL_ExtensionSupported('%s')\n", extension ? extension : "<null>");
  if (!extension || extension[0] == '\0') {
    return SDL_FALSE;
  }
  if (!GLLibraryLoaded.load(std::memory_order_acquire)) {
    return SDL_FALSE;
  }

  using GLGetStringFn = const GLubyte* (*)(GLenum);
  auto gl_get_string = reinterpret_cast<GLGetStringFn>(SDL_GL_GetProcAddress("glGetString"));
  if (!gl_get_string) {
    TraceGLProcLookup("glGetString", nullptr, "SDL_GL_ExtensionSupported(missing)");
    return SDL_FALSE;
  }

  const char* all_extensions = reinterpret_cast<const char*>(gl_get_string(GL_EXTENSIONS));
  if (!all_extensions || all_extensions[0] == '\0') {
    return SDL_FALSE;
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
      return SDL_TRUE;
    }

    search = match + ext_len;
  }
  return SDL_FALSE;
}

extern "C" void SDL_GL_ResetAttributes() {

  GLAttributesInitialized.store(false, std::memory_order_release);
  EnsureGLAttributesInitialized();
}

extern "C" int SDL_GL_SetAttribute(SDL_GLattr attr, int value) {

  EnsureGLAttributesInitialized();
  fprintf(stderr, "[SDL2 thunk] SDL_GL_SetAttribute(attr=%d value=%d)\n", static_cast<int>(attr), value);
  if (!IsValidGLAttr(attr)) {
    LogSDLValidationFailure("SDL_GL_SetAttribute", "invalid SDL_GLattr value");
    return SDL_SetError("SDL_GL_SetAttribute invalid attr=%d", static_cast<int>(attr));
  }

  GLAttributes[static_cast<size_t>(attr)] = value;
  return 0;
}

extern "C" int SDL_GL_GetAttribute(SDL_GLattr attr, int* value) {

  EnsureGLAttributesInitialized();
  if (!value) {
    LogSDLValidationFailure("SDL_GL_GetAttribute", "value output pointer is null");
    return SDL_SetError("SDL_GL_GetAttribute output pointer is null");
  }
  if (!IsValidGLAttr(attr)) {
    LogSDLValidationFailure("SDL_GL_GetAttribute", "invalid SDL_GLattr value");
    return SDL_SetError("SDL_GL_GetAttribute invalid attr=%d", static_cast<int>(attr));
  }

  // VEXA_FIXES: Desktop clients often treat a 0 double-buffer report as a
  // hard capability mismatch and stop rendering, even though Android presents
  // via eglSwapBuffers.
  if (attr == SDL_GL_DOUBLEBUFFER) {
    *value = 1;
    fprintf(stderr, "[SDL2 thunk] SDL_GL_GetAttribute(attr=%d) => %d (forced)\n",
            static_cast<int>(attr), *value);
    return 0;
  }

  *value = GLAttributes[static_cast<size_t>(attr)];
  fprintf(stderr, "[SDL2 thunk] SDL_GL_GetAttribute(attr=%d) => %d\n", static_cast<int>(attr), *value);
  return 0;
}

extern "C" SDL_GLContext SDL_GL_CreateContext(SDL_Window* window) {

  fprintf(stderr, "[SDL2 thunk] SDL_GL_CreateContext(window=%p)\n", window);
  SDL_GLContext context = fexfn_pack_FEX_SDL2_GL_CreateContext(window);
  fprintf(stderr, "[SDL2 thunk] SDL_GL_CreateContext => %p\n", context);
  if (context) {
    CurrentGLContextPointer.store(reinterpret_cast<uintptr_t>(context), std::memory_order_release);
    CurrentGLWindowPointer.store(reinterpret_cast<uintptr_t>(window), std::memory_order_release);
    GLLibraryLoaded.store(true, std::memory_order_release);
    (void)SDL2EnsureGLResolverInitialized();
    (void)SDL2EnsureEGLResolverInitialized();
  } else {
    SDL_SetError("SDL_GL_CreateContext: host context creation failed");
  }
  return context;
}

extern "C" int SDL_GL_MakeCurrent(SDL_Window* window, SDL_GLContext context) {

  fprintf(stderr, "[SDL2 thunk] SDL_GL_MakeCurrent(window=%p context=%p)\n", window, context);
  if (!window && !context) {
    const int rc = fexfn_pack_FEX_SDL2_GL_MakeCurrent(nullptr, nullptr);
    fprintf(stderr, "[SDL2 thunk] SDL_GL_MakeCurrent(NULL,NULL) => %d\n", rc);
    if (rc == 0) {
      CurrentGLWindowPointer.store(0, std::memory_order_release);
      CurrentGLContextPointer.store(0, std::memory_order_release);
    }
    return rc;
  }

  if (!window || !context) {
    LogSDLValidationFailure("SDL_GL_MakeCurrent", "window/context must be both null or both non-null");
    return SDL_SetError("SDL_GL_MakeCurrent requires both window and context");
  }

  const int rc = fexfn_pack_FEX_SDL2_GL_MakeCurrent(window, context);
  fprintf(stderr, "[SDL2 thunk] SDL_GL_MakeCurrent => %d\n", rc);
  if (rc == 0) {
    CurrentGLWindowPointer.store(reinterpret_cast<uintptr_t>(window), std::memory_order_release);
    CurrentGLContextPointer.store(reinterpret_cast<uintptr_t>(context), std::memory_order_release);
  }
  return rc;
}

extern "C" SDL_Window* SDL_GL_GetCurrentWindow() {

  return reinterpret_cast<SDL_Window*>(CurrentGLWindowPointer.load(std::memory_order_acquire));
}

extern "C" SDL_GLContext SDL_GL_GetCurrentContext() {

  return reinterpret_cast<SDL_GLContext>(CurrentGLContextPointer.load(std::memory_order_acquire));
}

extern "C" void SDL_GL_GetDrawableSize(SDL_Window* window, int* w, int* h) {

  SDL_GetWindowSizeInPixels(window, w, h);
}

extern "C" int SDL_GL_SetSwapInterval(int interval) {

  const int rc = fexfn_pack_FEX_SDL2_GL_SetSwapInterval(interval);
  fprintf(stderr, "[SDL2 thunk] SDL_GL_SetSwapInterval(%d) => %d\n", interval, rc);
  if (rc == 0) {
    CurrentSwapInterval.store(interval, std::memory_order_release);
  }
  return rc;
}

extern "C" int SDL_GL_GetSwapInterval() {

  const int interval = CurrentSwapInterval.load(std::memory_order_acquire);
  fprintf(stderr, "[SDL2 thunk] SDL_GL_GetSwapInterval() => %d\n", interval);
  return interval;
}

extern "C" void SDL_GL_SwapWindow(SDL_Window* window) {

  if (!window) {
    LogSDLValidationFailure("SDL_GL_SwapWindow", "window is null");
    return;
  }
  fexfn_pack_FEX_SDL2_GL_SwapWindow(window);
}

extern "C" void SDL_GL_DeleteContext(SDL_GLContext context) {

  fprintf(stderr, "[SDL2 thunk] SDL_GL_DeleteContext(context=%p)\n", context);
  fexfn_pack_FEX_SDL2_GL_DeleteContext(context);
  ClearCurrentGLStateIfMatching(context);
}

extern "C" void* SDL_GL_GetProcAddress(const char* proc) {

  if (!proc) {
    LogSDLValidationFailure("SDL_GL_GetProcAddress", "proc name pointer is null");
    return nullptr;
  }
  if (!*proc) {
    LogSDLValidationFailure("SDL_GL_GetProcAddress", "proc name is empty");
    return nullptr;
  }

  const std::string_view proc_name {proc};

  auto ReturnResolved = [&](void* resolved, const char* source) -> void* {
    TraceGLProcLookup(proc, resolved, source);
    return resolved;
  };

  // VEXA_FIXES: Lookup pipeline order:
  // 1) shader bridge + thunk self exports, 2) static GL proc table + Android aliases,
  // 3) dynamic fallback (non-GL symbols only).
  if (proc_name == "glShaderSource" || proc_name == "glShaderSourceARB") {
    if (void* resolved = SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>(proc))) {
      return ReturnResolved(resolved, "glXGetProcAddress(force-shadersource)");
    }
  }

  for (const auto& entry : GLProcTable) {
    if (entry.Name == proc_name) {
      if (entry.Function) {
        return ReturnResolved(entry.Function, "table");
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
  if (proc_name == "SDL_GL_SetSwapInterval") {
    return ReturnResolved(reinterpret_cast<void*>(SDL_GL_SetSwapInterval), "self");
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
    if (void* resolved = SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glPolygonMode"))) {
      return ReturnResolved(resolved, "glXGetProcAddress");
    }
    if (void* resolved = SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glPolygonModeEXT"))) {
      return ReturnResolved(resolved, "glXGetProcAddress(alias)");
    }
    if (void* resolved = SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glPolygonModeNV"))) {
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
    if (void* resolved = SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glGetFramebufferAttachmentParameteriv"))) {
      return ReturnResolved(resolved, "glXGetProcAddress");
    }
    if (void* resolved = SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>("glGetFramebufferAttachmentParameterivEXT"))) {
      return ReturnResolved(resolved, "glXGetProcAddress(alias)");
    }
    return ReturnResolved(reinterpret_cast<void*>(GLCompatGetFramebufferAttachmentParameteriv),
                          "compat(framebuffer-attach-query-fallback)");
  }
  if (proc_name == "glGetTexImage") {
    return ReturnResolved(reinterpret_cast<void*>(GLCompatGetTexImage), "compat(glReadPixels)");
  }

  const bool is_gl_proc = proc_name.rfind("gl", 0) == 0;
  const bool is_egl_proc = proc_name.rfind("egl", 0) == 0;
  if (is_egl_proc) {
    if (void* resolved = SDL2ResolveEGLProc(proc)) {
      return ReturnResolved(resolved, "eglGetProcAddress");
    }
    TraceGLProcLookup(proc, nullptr, "egl-miss");
    return nullptr;
  }

  if (is_gl_proc) {
    if (void* resolved = SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>(proc))) {
      return ReturnResolved(resolved, "glXGetProcAddress");
    }

    for (const char* alias : GetAndroidGLProcAliases(proc_name)) {
      if (!alias) {
        break;
      }
      if (void* resolved = SDL2ResolveGLProc(reinterpret_cast<const GLubyte*>(alias))) {
        return ReturnResolved(resolved, "glXGetProcAddress(alias)");
      }
    }
  }

  // If the GL thunk couldn't bridge a GL/EGL entry point then falling back to
  // RTLD_DEFAULT only returns the guest-side export, which can still point at a
  // null host function pointer.
  if (is_gl_proc) {
    TraceGLProcLookup(proc, nullptr, "gl-miss");
    return nullptr;
  }

  // Table miss fallback: let dynamic linker resolve non-GL symbols.
  if (void* resolved = dlsym(RTLD_DEFAULT, proc)) {
    fprintf(stderr, "[SDL2 thunk] SDL_GL_GetProcAddress RTLD_DEFAULT('%s') => %p\n", proc, resolved);
    return ReturnResolved(resolved, "RTLD_DEFAULT");
  }

  static void* gl_handle = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (!gl_handle) {
    gl_handle = dlopen("libGL.so", RTLD_NOW | RTLD_GLOBAL);
  }
  if (gl_handle) {
    if (void* resolved = dlsym(gl_handle, proc)) {
      fprintf(stderr, "[SDL2 thunk] SDL_GL_GetProcAddress dlsym(handle,'%s') => %p\n", proc, resolved);
      return ReturnResolved(resolved, "libGL handle");
    }
  }

  TraceGLProcLookup(proc, nullptr, "miss");
  return nullptr;
}

// ===========================================================================
// Events
// ===========================================================================

extern "C" void SDL_PumpEvents() {

  VexaSDL2PollPointerEvents();
}

extern "C" int SDL_PollEvent(SDL_Event* event) {

  VexaSDL2PollPointerEvents();
  while (true) {
    SDL_Event popped {};
    bool have = false;
    {
      std::scoped_lock lock {SyntheticEventQueueMutex};
      if (event) {
        if (!SyntheticEventQueue.empty()) {
          popped = SyntheticEventQueue.front();
          SyntheticEventQueue.pop_front();
          have = true;
        }
      } else if (!SyntheticEventQueue.empty()) {
        return 1;
      }
    }
    if (have) {
      if (!IsEventTypeEnabled(popped.type)) {
        continue;
      }
      if (RunUserEventFilter(&popped) == 0) {
        continue;
      }
      std::memcpy(event, &popped, sizeof(SDL_Event));
      return 1;
    }
    return fexfn_pack_FEX_SDL2_PollEvent(event);
  }
}

extern "C" int SDL_WaitEvent(SDL_Event* event) {

  while (true) {
    if (SDL_PollEvent(event)) {
      return 1;
    }
    SDL_Delay(1);
  }
}

extern "C" int SDL_WaitEventTimeout(SDL_Event* event, int timeout) {

  if (timeout < 0) {
    return SDL_WaitEvent(event);
  }

  const auto start = std::chrono::steady_clock::now();
  while (true) {
    if (SDL_PollEvent(event)) {
      return 1;
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    if (elapsed_ms >= static_cast<long long>(timeout)) {
      return 0;
    }
    SDL_Delay(1);
  }
}

extern "C" int SDL_PeepEvents(SDL_Event* events, int numevents, SDL_eventaction action,
                              Uint32 minType, Uint32 maxType) {

  if (numevents < 0 || action < SDL_ADDEVENT || action > SDL_GETEVENT) {
    return -1;
  }

  if (action == SDL_ADDEVENT) {
    if (!events && numevents > 0) {
      return -1;
    }
    int added = 0;
    for (int i = 0; i < numevents; ++i) {
      SDL_Event copy = events[i];
      if (!IsEventTypeEnabled(copy.type)) {
        continue;
      }
      if (RunUserEventFilter(&copy) == 0) {
        continue;
      }
      RunEventWatches(&copy);
      {
        std::scoped_lock lock {SyntheticEventQueueMutex};
        SyntheticEventQueue.push_back(copy);
      }
      ++added;
    }
    return added;
  }

  // SDL_PEEKEVENT / SDL_GETEVENT over the guest-side queue.
  VexaSDL2PollPointerEvents();
  const Uint32 min_type = minType == 0 && maxType == 0 ? 0 : minType;
  const Uint32 max_type = minType == 0 && maxType == 0 ? 0xFFFFFFFFu : maxType;
  int count = 0;
  std::scoped_lock lock {SyntheticEventQueueMutex};
  for (auto it = SyntheticEventQueue.begin(); it != SyntheticEventQueue.end() && count < numevents;) {
    const Uint32 type = it->type;
    const bool in_range = type >= min_type && type <= max_type;
    const bool enabled = in_range && IsEventTypeEnabled(type);
    if (enabled) {
      if (events) {
        events[count] = *it;
      }
      ++count;
      if (action == SDL_GETEVENT) {
        it = SyntheticEventQueue.erase(it);
        continue;
      }
    } else if (action == SDL_GETEVENT && in_range) {
      // Disabled events are silently dropped from the queue during a get.
      it = SyntheticEventQueue.erase(it);
      continue;
    }
    ++it;
  }
  return count;
}

extern "C" SDL_bool SDL_HasEvent(Uint32 type) {

  VexaSDL2PollPointerEvents();
  std::scoped_lock lock {SyntheticEventQueueMutex};
  for (const auto& event : SyntheticEventQueue) {
    if (event.type == type) {
      return SDL_TRUE;
    }
  }
  return SDL_FALSE;
}

extern "C" SDL_bool SDL_HasEvents(Uint32 minType, Uint32 maxType) {

  VexaSDL2PollPointerEvents();
  std::scoped_lock lock {SyntheticEventQueueMutex};
  for (const auto& event : SyntheticEventQueue) {
    if (event.type >= minType && event.type <= maxType) {
      return SDL_TRUE;
    }
  }
  return SDL_FALSE;
}

extern "C" void SDL_FlushEvent(Uint32 type) {

  std::scoped_lock lock {SyntheticEventQueueMutex};
  for (auto it = SyntheticEventQueue.begin(); it != SyntheticEventQueue.end();) {
    if (it->type == type) {
      it = SyntheticEventQueue.erase(it);
    } else {
      ++it;
    }
  }
}

extern "C" void SDL_FlushEvents(Uint32 minType, Uint32 maxType) {

  std::scoped_lock lock {SyntheticEventQueueMutex};
  for (auto it = SyntheticEventQueue.begin(); it != SyntheticEventQueue.end();) {
    if (it->type >= minType && it->type <= maxType) {
      it = SyntheticEventQueue.erase(it);
    } else {
      ++it;
    }
  }
}

extern "C" int SDL_PushEvent(SDL_Event* event) {

  if (!event) {
    return -1;
  }
  SDL_Event copy = *event;
  if (!IsEventTypeEnabled(copy.type)) {
    return 0;
  }
  if (RunUserEventFilter(&copy) == 0) {
    return 0;
  }
  RunEventWatches(&copy);
  {
    std::scoped_lock lock {SyntheticEventQueueMutex};
    SyntheticEventQueue.push_back(copy);
  }
  return 1;
}

extern "C" void SDL_SetEventFilter(SDL_EventFilter filter, void* userdata) {

  std::lock_guard<std::mutex> guard(EventCallbackMutex);
  UserEventFilter = filter;
  UserEventFilterData = userdata;
}

extern "C" SDL_bool SDL_GetEventFilter(SDL_EventFilter* filter, void** userdata) {

  std::lock_guard<std::mutex> guard(EventCallbackMutex);
  if (!UserEventFilter) {
    if (filter) {
      *filter = nullptr;
    }
    if (userdata) {
      *userdata = nullptr;
    }
    return SDL_FALSE;
  }
  if (filter) {
    *filter = UserEventFilter;
  }
  if (userdata) {
    *userdata = UserEventFilterData;
  }
  return SDL_TRUE;
}

extern "C" void SDL_AddEventWatch(SDL_EventFilter filter, void* userdata) {

  if (!filter) {
    return;
  }
  std::lock_guard<std::mutex> guard(EventCallbackMutex);
  for (const auto& watch : EventWatches) {
    if (watch.first == filter && watch.second == userdata) {
      return;
    }
  }
  EventWatches.emplace_back(filter, userdata);
}

extern "C" void SDL_DelEventWatch(SDL_EventFilter filter, void* userdata) {

  std::lock_guard<std::mutex> guard(EventCallbackMutex);
  for (auto it = EventWatches.begin(); it != EventWatches.end(); ++it) {
    if (it->first == filter && it->second == userdata) {
      EventWatches.erase(it);
      return;
    }
  }
}

extern "C" void SDL_FilterEvents(SDL_EventFilter filter, void* userdata) {

  if (!filter) {
    return;
  }
  std::scoped_lock lock {SyntheticEventQueueMutex};
  for (auto it = SyntheticEventQueue.begin(); it != SyntheticEventQueue.end();) {
    SDL_Event copy = *it;
    if (filter(userdata, &copy) == 0) {
      it = SyntheticEventQueue.erase(it);
    } else {
      ++it;
    }
  }
}

extern "C" Uint8 SDL_EventState(Uint32 type, int state) {

  Uint8 previous = SDL_ENABLE;
  {
    std::lock_guard<std::mutex> guard(EventStateMutex);
    auto it = EventTypeStates.find(type);
    if (it != EventTypeStates.end()) {
      previous = it->second;
    }
    if (state == SDL_ENABLE || state == SDL_DISABLE) {
      EventTypeStates[type] = static_cast<Uint8>(state);
    }
  }
  if (state == SDL_DISABLE && previous != SDL_DISABLE) {
    SDL_FlushEvent(type);
  }
  return previous;
}

extern "C" Uint32 SDL_RegisterEvents(int numevents) {

  if (numevents <= 0) {
    return static_cast<Uint32>(-1);
  }
  const Uint32 base = NextRegisteredEventType.load(std::memory_order_acquire);
  if (base == 0 || base > SDL_LASTEVENT ||
      static_cast<Uint64>(base) + static_cast<Uint64>(numevents) - 1 > SDL_LASTEVENT) {
    return static_cast<Uint32>(-1);
  }
  const Uint32 next = base + static_cast<Uint32>(numevents);
  NextRegisteredEventType.store(next <= SDL_LASTEVENT ? next : 0, std::memory_order_release);
  return base;
}

// ===========================================================================
// Platform / version
// ===========================================================================

extern "C" const char* SDL_GetPlatform() {

  // Report Linux, not Android: Celeste/FNA pick fmod_SDL for the FMOD output
  // only on the desktop-Linux path; "Android" diverts it and FMOD silently
  // falls back to NOSOUND.
  //
  // VEXA_FIXES: ...but libFNA3D branches on SDL_GetPlatform() to pick its GL
  // context attributes (ES3 vs desktop profile) and swap-interval handling.
  // Answering "Linux" there selects the desktop-GL path, which breaks the
  // thunk's EGL context ownership (SDL_GL_SwapWindow: EGL state invalid).
  // So answer per caller: native FNA3D sees "Android", everyone else "Linux".
  Dl_info info {};
  if (dladdr(__builtin_return_address(0), &info) && info.dli_fname &&
      std::strstr(info.dli_fname, "libFNA3D")) {
    return "Android";
  }
  return "Linux";
}

extern "C" void SDL_GetVersion(SDL_version* ver) {

  if (ver) {
    ver->major = SDL_MAJOR_VERSION;
    ver->minor = SDL_MINOR_VERSION;
    ver->patch = SDL_PATCHLEVEL;
  }
}

extern "C" const char* SDL_GetRevision() {

  // VEXA_FIXES: Return a stable thunk identity token used in runtime diagnostics.
  return "vexa-sdl2-thunk";
}

extern "C" int SDL_GetRevisionNumber() {

  return 0;
}

// ===========================================================================
// Dynamic loading (SDL_loadso) — guest-local dlopen wrappers
// ===========================================================================

extern "C" void* SDL_LoadObject(const char* sofile) {

  void* handle = dlopen(sofile, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char* error = dlerror();
    SDL_SetError("SDL_LoadObject('%s'): %s",
                 sofile ? sofile : "(null)",
                 error ? error : "dlopen failed");
  }
  return handle;
}

extern "C" void* SDL_LoadFunction(void* handle, const char* name) {

  void* resolved = dlsym(handle, name);
  if (!resolved) {
    const char* error = dlerror();
    SDL_SetError("SDL_LoadFunction('%s'): %s",
                 name ? name : "(null)",
                 error ? error : "dlsym failed");
  }
  return resolved;
}

extern "C" void SDL_UnloadObject(void* handle) {

  if (handle) {
    dlclose(handle);
  }
}

// ===========================================================================
// Timers
// ===========================================================================

extern "C" Uint32 SDL_GetTicks() {

  return fexfn_pack_FEX_SDL2_GetTicks();
}

extern "C" Uint64 SDL_GetTicks64() {

  return fexfn_pack_FEX_SDL2_GetTicks64();
}

extern "C" void SDL_Delay(Uint32 ms) {

  fexfn_pack_FEX_SDL2_Delay(ms);
}

extern "C" Uint64 SDL_GetPerformanceCounter() {

  // Nanosecond-resolution counter; paired with a frequency of 1e9.
  return static_cast<Uint64>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

extern "C" Uint64 SDL_GetPerformanceFrequency() {

  return 1000000000ULL;
}

extern "C" SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback, void* param) {

  if (!callback || interval == 0) {
    SDL_SetError("SDL_AddTimer: invalid callback or zero interval");
    return 0;
  }

  auto state = std::make_shared<GuestTimerState>();
  const SDL_TimerID id = NextTimerId.fetch_add(1, std::memory_order_acq_rel);
  {
    std::lock_guard<std::mutex> guard(TimerRegistryMutex);
    GuestTimers.emplace(id, state);
  }

  std::thread([id, state, interval, callback, param]() mutable {
    Uint32 wait = interval;
    while (!state->Cancelled.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(wait));
      if (state->Cancelled.load(std::memory_order_acquire)) {
        break;
      }
      const Uint32 next = callback(wait, param);
      if (next == 0) {
        break;
      }
      wait = next;
    }
    std::lock_guard<std::mutex> guard(TimerRegistryMutex);
    GuestTimers.erase(id);
  }).detach();

  return id;
}

extern "C" SDL_bool SDL_RemoveTimer(SDL_TimerID id) {

  std::lock_guard<std::mutex> guard(TimerRegistryMutex);
  auto it = GuestTimers.find(id);
  if (it == GuestTimers.end()) {
    return SDL_FALSE;
  }
  it->second->Cancelled.store(true, std::memory_order_release);
  GuestTimers.erase(it);
  return SDL_TRUE;
}

// ===========================================================================
// Text input
// ===========================================================================

extern "C" void SDL_StartTextInput() {

  (void)fexfn_pack_FEX_SDL2_StartTextInput();
}

extern "C" void SDL_StopTextInput() {

  fexfn_pack_FEX_SDL2_StopTextInput();
}

extern "C" SDL_bool SDL_IsTextInputActive() {

  return fexfn_pack_FEX_SDL2_IsTextInputActive();
}

extern "C" void SDL_SetTextInputRect(const SDL_Rect* rect) {

  if (rect) {
    TextInputRect = *rect;
  } else {
    TextInputRect = {};
  }
}

// ===========================================================================
// Pixel formats (guest-side; tightly coupled to the surface ops below)
// ===========================================================================

extern "C" const char* SDL_GetPixelFormatName(Uint32 format) {

  const PixelFormatTableEntry* entry = FindPixelFormatEntry(format);
  if (entry) {
    return entry->Name;
  }
  switch (format) {
    case SDL_PIXELFORMAT_INDEX1LSB: return "SDL_PIXELFORMAT_INDEX1LSB";
    case SDL_PIXELFORMAT_INDEX1MSB: return "SDL_PIXELFORMAT_INDEX1MSB";
    case SDL_PIXELFORMAT_INDEX4LSB: return "SDL_PIXELFORMAT_INDEX4LSB";
    case SDL_PIXELFORMAT_INDEX4MSB: return "SDL_PIXELFORMAT_INDEX4MSB";
    case SDL_PIXELFORMAT_RGB444: return "SDL_PIXELFORMAT_RGB444";
    case SDL_PIXELFORMAT_BGR444: return "SDL_PIXELFORMAT_BGR444";
    case SDL_PIXELFORMAT_RGB555: return "SDL_PIXELFORMAT_RGB555";
    case SDL_PIXELFORMAT_BGR555: return "SDL_PIXELFORMAT_BGR555";
    case SDL_PIXELFORMAT_RGB888: return "SDL_PIXELFORMAT_RGB888";
    case SDL_PIXELFORMAT_BGR888: return "SDL_PIXELFORMAT_BGR888";
    case SDL_PIXELFORMAT_YV12: return "SDL_PIXELFORMAT_YV12";
    case SDL_PIXELFORMAT_IYUV: return "SDL_PIXELFORMAT_IYUV";
    case SDL_PIXELFORMAT_YUY2: return "SDL_PIXELFORMAT_YUY2";
    case SDL_PIXELFORMAT_UYVY: return "SDL_PIXELFORMAT_UYVY";
    case SDL_PIXELFORMAT_YVYU: return "SDL_PIXELFORMAT_YVYU";
    case SDL_PIXELFORMAT_NV12: return "SDL_PIXELFORMAT_NV12";
    case SDL_PIXELFORMAT_NV21: return "SDL_PIXELFORMAT_NV21";
    default: return "SDL_PIXELFORMAT_UNKNOWN";
  }
}

extern "C" SDL_bool SDL_PixelFormatEnumToMasks(Uint32 format, int* bpp,
                                             Uint32* Rmask, Uint32* Gmask, Uint32* Bmask, Uint32* Amask) {

  return MasksForPixelFormat(format, bpp, Rmask, Gmask, Bmask, Amask) ? SDL_TRUE : SDL_FALSE;
}

extern "C" Uint32 SDL_MasksToPixelFormatEnum(int bpp, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {

  for (const auto& entry : PixelFormatTable) {
    if (entry.BitsPerPixel == bpp && entry.Rmask == Rmask && entry.Gmask == Gmask &&
        entry.Bmask == Bmask && entry.Amask == Amask) {
      return entry.Format;
    }
  }
  return SDL_PIXELFORMAT_UNKNOWN;
}

extern "C" SDL_PixelFormat* SDL_AllocFormat(Uint32 pixel_format) {

  int bpp = 0;
  Uint32 r = 0, g = 0, b = 0, a = 0;
  if (!MasksForPixelFormat(pixel_format, &bpp, &r, &g, &b, &a)) {
    SDL_SetError("SDL_AllocFormat: unknown pixel format 0x%x", static_cast<unsigned>(pixel_format));
    return nullptr;
  }

  auto* fmt = static_cast<SDL_PixelFormat*>(std::malloc(sizeof(SDL_PixelFormat)));
  if (!fmt) {
    SDL_SetError("SDL_AllocFormat: out of memory");
    return nullptr;
  }
  std::memset(fmt, 0, sizeof(*fmt));
  fmt->format = pixel_format;
  fmt->palette = nullptr;
  fmt->BitsPerPixel = static_cast<Uint8>(bpp);
  fmt->BytesPerPixel = static_cast<Uint8>((bpp + 7) / 8);
  fmt->Rmask = r;
  fmt->Gmask = g;
  fmt->Bmask = b;
  fmt->Amask = a;
  fmt->Rshift = static_cast<Uint8>(MaskShift(r));
  fmt->Gshift = static_cast<Uint8>(MaskShift(g));
  fmt->Bshift = static_cast<Uint8>(MaskShift(b));
  fmt->Ashift = static_cast<Uint8>(MaskShift(a));
  fmt->Rloss = static_cast<Uint8>(8 - MaskBits(r));
  fmt->Gloss = static_cast<Uint8>(8 - MaskBits(g));
  fmt->Bloss = static_cast<Uint8>(8 - MaskBits(b));
  fmt->Aloss = static_cast<Uint8>(8 - MaskBits(a));
  fmt->refcount = 1;
  return fmt;
}

extern "C" void SDL_FreeFormat(SDL_PixelFormat* format) {

  if (!format) {
    return;
  }
  if (--format->refcount <= 0) {
    std::free(format);
  }
}

extern "C" SDL_Palette* SDL_AllocPalette(int ncolors) {

  if (ncolors <= 0) {
    SDL_SetError("SDL_AllocPalette: invalid color count %d", ncolors);
    return nullptr;
  }
  auto* palette = static_cast<SDL_Palette*>(std::malloc(sizeof(SDL_Palette)));
  if (!palette) {
    SDL_SetError("SDL_AllocPalette: out of memory");
    return nullptr;
  }
  palette->colors = static_cast<SDL_Color*>(std::malloc(sizeof(SDL_Color) * static_cast<size_t>(ncolors)));
  if (!palette->colors) {
    std::free(palette);
    SDL_SetError("SDL_AllocPalette: out of memory");
    return nullptr;
  }
  std::memset(palette->colors, 0xFF, sizeof(SDL_Color) * static_cast<size_t>(ncolors));
  palette->ncolors = ncolors;
  palette->version = 1;
  palette->refcount = 1;
  return palette;
}

extern "C" int SDL_SetPaletteColors(SDL_Palette* palette, const SDL_Color* colors, int firstcolor, int ncolors) {

  if (!palette || !colors || firstcolor < 0 || ncolors < 0 ||
      firstcolor + ncolors > palette->ncolors) {
    return SDL_SetError("SDL_SetPaletteColors: invalid arguments");
  }
  if (ncolors > 0) {
    std::memcpy(palette->colors + firstcolor, colors, sizeof(SDL_Color) * static_cast<size_t>(ncolors));
    ++palette->version;
  }
  return 0;
}

extern "C" int SDL_SetPixelFormatPalette(SDL_PixelFormat* format, SDL_Palette* palette) {

  if (!format) {
    return SDL_SetError("SDL_SetPixelFormatPalette: format is null");
  }
  format->palette = palette;
  return 0;
}

extern "C" void SDL_FreePalette(SDL_Palette* palette) {

  if (!palette) {
    return;
  }
  if (--palette->refcount <= 0) {
    std::free(palette->colors);
    std::free(palette);
  }
}

namespace {
Uint32 MapColorToFormat(const SDL_PixelFormat* format, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
  PixelDesc desc {};
  if (!DescForSurfaceFormat(format, desc)) {
    return 0;
  }
  if (desc.IsIndexed) {
    if (desc.Palette && desc.Palette->colors && desc.Palette->ncolors > 0) {
      int best = 0;
      long best_dist = 0x7FFFFFFF;
      for (int i = 0; i < desc.Palette->ncolors; ++i) {
        const SDL_Color& c = desc.Palette->colors[i];
        const long dr = static_cast<long>(c.r) - r;
        const long dg = static_cast<long>(c.g) - g;
        const long db = static_cast<long>(c.b) - b;
        const long dist = dr * dr + dg * dg + db * db;
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
    return static_cast<Uint32>(r);
  }
  return EncodePixelRGBA(desc, r, g, b, a);
}
} // namespace

extern "C" Uint32 SDL_MapRGB(const SDL_PixelFormat* format, Uint8 r, Uint8 g, Uint8 b) {

  if (!format) {
    return 0;
  }
  return MapColorToFormat(format, r, g, b, 255);
}

extern "C" Uint32 SDL_MapRGBA(const SDL_PixelFormat* format, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {

  if (!format) {
    return 0;
  }
  return MapColorToFormat(format, r, g, b, a);
}

extern "C" void SDL_GetRGB(Uint32 pixel, const SDL_PixelFormat* format, Uint8* r, Uint8* g, Uint8* b) {

  Uint8 rr = 0, gg = 0, bb = 0, aa = 0;
  PixelDesc desc {};
  if (format && DescForSurfaceFormat(format, desc)) {
    DecodePixelRGBA(desc, pixel, &rr, &gg, &bb, &aa);
  }
  if (r) {
    *r = rr;
  }
  if (g) {
    *g = gg;
  }
  if (b) {
    *b = bb;
  }
}

extern "C" void SDL_GetRGBA(Uint32 pixel, const SDL_PixelFormat* format,
                            Uint8* r, Uint8* g, Uint8* b, Uint8* a) {

  Uint8 rr = 0, gg = 0, bb = 0, aa = 0;
  PixelDesc desc {};
  if (format && DescForSurfaceFormat(format, desc)) {
    DecodePixelRGBA(desc, pixel, &rr, &gg, &bb, &aa);
  }
  if (r) {
    *r = rr;
  }
  if (g) {
    *g = gg;
  }
  if (b) {
    *b = bb;
  }
  if (a) {
    *a = aa;
  }
}

extern "C" void SDL_CalculateGammaRamp(float gamma, Uint16* ramp) {

  if (!ramp) {
    return;
  }
  if (gamma <= 0.0f) {
    for (int i = 0; i < 256; ++i) {
      ramp[i] = static_cast<Uint16>(i * 257);
    }
    return;
  }
  for (int i = 0; i < 256; ++i) {
    const float value = powf(static_cast<float>(i) / 255.0f, 1.0f / gamma) * 65535.0f + 0.5f;
    ramp[i] = static_cast<Uint16>(value > 65535.0f ? 65535.0f : value);
  }
}

// ===========================================================================
// Surfaces
// ===========================================================================

extern "C" SDL_Surface* SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
                                             Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {

  return fexfn_pack_FEX_SDL2_CreateRGBSurface(flags, width, height, depth, Rmask, Gmask, Bmask, Amask);
}

extern "C" SDL_Surface* SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int width, int height, int depth,
                                                       Uint32 format) {

  int bpp = 0;
  Uint32 r = 0, g = 0, b = 0, a = 0;
  if (!MasksForPixelFormat(format, &bpp, &r, &g, &b, &a)) {
    SDL_SetError("SDL_CreateRGBSurfaceWithFormat: unsupported pixel format 0x%x", static_cast<unsigned>(format));
    return nullptr;
  }
  if (depth <= 0) {
    depth = bpp;
  }
  return fexfn_pack_FEX_SDL2_CreateRGBSurface(flags, width, height, depth, r, g, b, a);
}

extern "C" SDL_Surface* SDL_CreateRGBSurfaceFrom(void* pixels, int width, int height, int depth, int pitch,
                                                 Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {

  return fexfn_pack_FEX_SDL2_CreateRGBSurfaceFrom(pixels, width, height, depth, pitch, Rmask, Gmask, Bmask, Amask);
}

extern "C" SDL_Surface* SDL_CreateRGBSurfaceWithFormatFrom(void* pixels, int width, int height, int depth,
                                                           int pitch, Uint32 format) {

  int bpp = 0;
  Uint32 r = 0, g = 0, b = 0, a = 0;
  if (!MasksForPixelFormat(format, &bpp, &r, &g, &b, &a)) {
    SDL_SetError("SDL_CreateRGBSurfaceWithFormatFrom: unsupported pixel format 0x%x", static_cast<unsigned>(format));
    return nullptr;
  }
  if (depth <= 0) {
    depth = bpp;
  }
  return fexfn_pack_FEX_SDL2_CreateRGBSurfaceFrom(pixels, width, height, depth, pitch, r, g, b, a);
}

extern "C" void SDL_FreeSurface(SDL_Surface* surface) {

  if (!surface) {
    return;
  }
  EraseSurfaceExtra(surface);
  const uintptr_t window_surface = WindowSurfacePointer.load(std::memory_order_acquire);
  if (window_surface == reinterpret_cast<uintptr_t>(surface)) {
    WindowSurfacePointer.store(0, std::memory_order_release);
  }
  fexfn_pack_FEX_SDL2_FreeSurface(surface);
}

extern "C" int SDL_SetSurfacePalette(SDL_Surface* surface, SDL_Palette* palette) {

  if (!surface) {
    return SDL_SetError("SDL_SetSurfacePalette: surface is null");
  }
  std::lock_guard<std::mutex> guard(SurfaceStateMutex);
  GetSurfaceExtraLocked(surface).Palette = palette;
  return 0;
}

extern "C" int SDL_LockSurface(SDL_Surface* surface) {

  if (!surface) {
    return SDL_SetError("SDL_LockSurface: surface is null");
  }
  ++surface->locked;
  return 0;
}

extern "C" void SDL_UnlockSurface(SDL_Surface* surface) {

  if (surface && surface->locked > 0) {
    --surface->locked;
  }
}

extern "C" int SDL_SetSurfaceRLE(SDL_Surface* surface, int flag) {

  if (!surface) {
    return SDL_SetError("SDL_SetSurfaceRLE: surface is null");
  }
  (void)flag;
  return 0;
}

extern "C" SDL_bool SDL_HasSurfaceRLE(SDL_Surface* surface) {

  (void)surface;
  return SDL_FALSE;
}

extern "C" int SDL_SetColorKey(SDL_Surface* surface, int flag, Uint32 key) {

  if (!surface) {
    return SDL_SetError("SDL_SetColorKey: surface is null");
  }
  std::lock_guard<std::mutex> guard(SurfaceStateMutex);
  auto& extra = GetSurfaceExtraLocked(surface);
  extra.HasColorKey = flag != 0;
  extra.ColorKey = key;
  return 0;
}

extern "C" SDL_bool SDL_HasColorKey(SDL_Surface* surface) {

  if (!surface) {
    return SDL_FALSE;
  }
  std::lock_guard<std::mutex> guard(SurfaceStateMutex);
  auto it = SurfaceStateByPointer.find(surface);
  return (it != SurfaceStateByPointer.end() && it->second.HasColorKey) ? SDL_TRUE : SDL_FALSE;
}

extern "C" int SDL_GetColorKey(SDL_Surface* surface, Uint32* key) {

  if (!surface || !key) {
    return SDL_SetError("SDL_GetColorKey: invalid arguments");
  }
  std::lock_guard<std::mutex> guard(SurfaceStateMutex);
  auto it = SurfaceStateByPointer.find(surface);
  if (it == SurfaceStateByPointer.end() || !it->second.HasColorKey) {
    return SDL_SetError("SDL_GetColorKey: surface has no color key");
  }
  *key = it->second.ColorKey;
  return 0;
}

extern "C" int SDL_SetSurfaceColorMod(SDL_Surface* surface, Uint8 r, Uint8 g, Uint8 b) {

  if (!surface) {
    return SDL_SetError("SDL_SetSurfaceColorMod: surface is null");
  }
  std::lock_guard<std::mutex> guard(SurfaceStateMutex);
  auto& extra = GetSurfaceExtraLocked(surface);
  extra.ColorModR = r;
  extra.ColorModG = g;
  extra.ColorModB = b;
  return 0;
}

extern "C" int SDL_GetSurfaceColorMod(SDL_Surface* surface, Uint8* r, Uint8* g, Uint8* b) {

  if (!surface) {
    return SDL_SetError("SDL_GetSurfaceColorMod: surface is null");
  }
  std::lock_guard<std::mutex> guard(SurfaceStateMutex);
  auto it = SurfaceStateByPointer.find(surface);
  const SurfaceExtraState empty {};
  const SurfaceExtraState& extra = it != SurfaceStateByPointer.end() ? it->second : empty;
  if (r) {
    *r = extra.ColorModR;
  }
  if (g) {
    *g = extra.ColorModG;
  }
  if (b) {
    *b = extra.ColorModB;
  }
  return 0;
}

extern "C" int SDL_SetSurfaceAlphaMod(SDL_Surface* surface, Uint8 alpha) {

  if (!surface) {
    return SDL_SetError("SDL_SetSurfaceAlphaMod: surface is null");
  }
  std::lock_guard<std::mutex> guard(SurfaceStateMutex);
  GetSurfaceExtraLocked(surface).AlphaMod = alpha;
  return 0;
}

extern "C" int SDL_GetSurfaceAlphaMod(SDL_Surface* surface, Uint8* alpha) {

  if (!surface || !alpha) {
    return SDL_SetError("SDL_GetSurfaceAlphaMod: invalid arguments");
  }
  std::lock_guard<std::mutex> guard(SurfaceStateMutex);
  auto it = SurfaceStateByPointer.find(surface);
  *alpha = it != SurfaceStateByPointer.end() ? it->second.AlphaMod : 255;
  return 0;
}

extern "C" int SDL_SetSurfaceBlendMode(SDL_Surface* surface, SDL_BlendMode blendMode) {

  if (!surface) {
    return SDL_SetError("SDL_SetSurfaceBlendMode: surface is null");
  }
  const int rc = fexfn_pack_FEX_SDL2_SetSurfaceBlendMode(surface, blendMode);
  if (rc == 0) {
    std::lock_guard<std::mutex> guard(SurfaceStateMutex);
    auto& extra = GetSurfaceExtraLocked(surface);
    extra.BlendMode = blendMode;
    extra.HasBlendMode = true;
  }
  return rc;
}

extern "C" int SDL_GetSurfaceBlendMode(SDL_Surface* surface, SDL_BlendMode* blendMode) {

  if (!surface || !blendMode) {
    return SDL_SetError("SDL_GetSurfaceBlendMode: invalid arguments");
  }
  std::lock_guard<std::mutex> guard(SurfaceStateMutex);
  auto it = SurfaceStateByPointer.find(surface);
  *blendMode = (it != SurfaceStateByPointer.end() && it->second.HasBlendMode)
                   ? it->second.BlendMode
                   : SDL_BLENDMODE_NONE;
  return 0;
}

extern "C" SDL_bool SDL_SetClipRect(SDL_Surface* surface, const SDL_Rect* rect) {

  if (!surface) {
    return SDL_FALSE;
  }
  const SDL_Rect full {0, 0, surface->w, surface->h};
  if (!rect) {
    surface->clip_rect = full;
    return SDL_TRUE;
  }
  return IntersectRect(*rect, full, &surface->clip_rect) ? SDL_TRUE : SDL_FALSE;
}

extern "C" void SDL_GetClipRect(SDL_Surface* surface, SDL_Rect* rect) {

  if (surface && rect) {
    *rect = surface->clip_rect;
  }
}

extern "C" SDL_Surface* SDL_DuplicateSurface(SDL_Surface* surface) {

  if (!surface || !surface->format) {
    SDL_SetError("SDL_DuplicateSurface: invalid surface");
    return nullptr;
  }
  SDL_Surface* copy = fexfn_pack_FEX_SDL2_CreateRGBSurface(
      surface->flags, surface->w, surface->h, surface->format->BitsPerPixel,
      surface->format->Rmask, surface->format->Gmask, surface->format->Bmask, surface->format->Amask);
  if (!copy || !copy->format) {
    SDL_SetError("SDL_DuplicateSurface: host surface creation failed");
    return nullptr;
  }
  PixelDesc src_desc {}, dst_desc {};
  if (!DescForSurfaceFormat(surface->format, src_desc) || !DescForSurfaceFormat(copy->format, dst_desc) ||
      !ConvertPixelsGeneric(surface->w, surface->h, src_desc, surface->pixels, surface->pitch,
                            dst_desc, copy->pixels, copy->pitch)) {
    fexfn_pack_FEX_SDL2_FreeSurface(copy);
    SDL_SetError("SDL_DuplicateSurface: pixel copy failed");
    return nullptr;
  }
  return copy;
}

extern "C" int SDL_ConvertPixels(int width, int height, Uint32 src_format, const void* src, int src_pitch,
                                 Uint32 dst_format, void* dst, int dst_pitch) {

  PixelDesc src_desc {}, dst_desc {};
  if (!DescForPixelFormatEnum(src_format, src_desc) || !DescForPixelFormatEnum(dst_format, dst_desc) ||
      src == nullptr || dst == nullptr || src_pitch <= 0 || dst_pitch <= 0) {
    return SDL_SetError("SDL_ConvertPixels: unsupported or invalid arguments");
  }
  if (!ConvertPixelsGeneric(width, height, src_desc, src, src_pitch, dst_desc, dst, dst_pitch)) {
    return SDL_SetError("SDL_ConvertPixels: unsupported conversion 0x%x -> 0x%x",
                        static_cast<unsigned>(src_format), static_cast<unsigned>(dst_format));
  }
  return 0;
}

extern "C" int SDL_PremultiplyAlpha(int width, int height, Uint32 src_format, const void* src, int src_pitch,
                                    Uint32 dst_format, void* dst, int dst_pitch) {

  PixelDesc src_desc {}, dst_desc {};
  if (!DescForPixelFormatEnum(src_format, src_desc) || !DescForPixelFormatEnum(dst_format, dst_desc) ||
      src == nullptr || dst == nullptr || src_pitch <= 0 || dst_pitch <= 0 ||
      src_desc.Amask == 0 || src_desc.IsIndexed || dst_desc.IsIndexed) {
    return SDL_SetError("SDL_PremultiplyAlpha: unsupported or invalid arguments");
  }

  const auto* src_row = static_cast<const Uint8*>(src);
  auto* dst_row = static_cast<Uint8*>(dst);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const Uint32 pixel = ReadPixel(src_row, x, src_desc);
      Uint8 r, g, b, a;
      DecodePixelRGBA(src_desc, pixel, &r, &g, &b, &a);
      r = static_cast<Uint8>((static_cast<unsigned>(r) * a + 127u) / 255u);
      g = static_cast<Uint8>((static_cast<unsigned>(g) * a + 127u) / 255u);
      b = static_cast<Uint8>((static_cast<unsigned>(b) * a + 127u) / 255u);
      WritePixel(dst_row, x, EncodePixelRGBA(dst_desc, r, g, b, a), dst_desc);
    }
    src_row += src_pitch;
    dst_row += dst_pitch;
  }
  return 0;
}

extern "C" SDL_Surface* SDL_ConvertSurface(SDL_Surface* src, const SDL_PixelFormat* fmt, Uint32 flags) {

  if (!src || !src->format || !fmt) {
    SDL_SetError("SDL_ConvertSurface: invalid arguments");
    return nullptr;
  }

  PixelDesc src_desc {}, dst_desc {};
  if (!DescForSurfaceFormat(src->format, src_desc)) {
    SDL_SetError("SDL_ConvertSurface: unreadable source format");
    return nullptr;
  }
  if (!DescForSurfaceFormat(fmt, dst_desc)) {
    SDL_SetError("SDL_ConvertSurface: unreadable destination format");
    return nullptr;
  }

  Uint32 rmask = fmt->Rmask;
  Uint32 gmask = fmt->Gmask;
  Uint32 bmask = fmt->Bmask;
  Uint32 amask = fmt->Amask;
  int depth = fmt->BitsPerPixel;
  if (rmask == 0 && gmask == 0 && bmask == 0 && amask == 0 && fmt->format != SDL_PIXELFORMAT_INDEX8) {
    int bpp = 0;
    if (MasksForPixelFormat(fmt->format, &bpp, &rmask, &gmask, &bmask, &amask)) {
      depth = bpp;
    }
  }

  SDL_Surface* dst = fexfn_pack_FEX_SDL2_CreateRGBSurface(
      flags, src->w, src->h, depth, rmask, gmask, bmask, amask);
  if (!dst || !dst->format) {
    SDL_SetError("SDL_ConvertSurface: host surface creation failed");
    return nullptr;
  }

  PixelDesc real_dst_desc {};
  if (!DescForSurfaceFormat(dst->format, real_dst_desc) ||
      !ConvertPixelsGeneric(src->w, src->h, src_desc, src->pixels, src->pitch,
                            real_dst_desc, dst->pixels, dst->pitch)) {
    fexfn_pack_FEX_SDL2_FreeSurface(dst);
    SDL_SetError("SDL_ConvertSurface: pixel conversion failed");
    return nullptr;
  }
  return dst;
}

extern "C" SDL_Surface* SDL_ConvertSurfaceFormat(SDL_Surface* src, Uint32 pixel_format, Uint32 flags) {

  if (!src || !src->format) {
    SDL_SetError("SDL_ConvertSurfaceFormat: invalid source surface");
    return nullptr;
  }

  int bpp = 0;
  Uint32 r = 0, g = 0, b = 0, a = 0;
  if (!MasksForPixelFormat(pixel_format, &bpp, &r, &g, &b, &a)) {
    SDL_SetError("SDL_ConvertSurfaceFormat: unsupported pixel format 0x%x", static_cast<unsigned>(pixel_format));
    return nullptr;
  }

  SDL_PixelFormat fmt {};
  fmt.format = pixel_format;
  fmt.palette = nullptr;
  fmt.BitsPerPixel = static_cast<Uint8>(bpp);
  fmt.BytesPerPixel = static_cast<Uint8>((bpp + 7) / 8);
  fmt.Rmask = r;
  fmt.Gmask = g;
  fmt.Bmask = b;
  fmt.Amask = a;
  fmt.Rshift = static_cast<Uint8>(MaskShift(r));
  fmt.Gshift = static_cast<Uint8>(MaskShift(g));
  fmt.Bshift = static_cast<Uint8>(MaskShift(b));
  fmt.Ashift = static_cast<Uint8>(MaskShift(a));
  fmt.Rloss = static_cast<Uint8>(8 - MaskBits(r));
  fmt.Gloss = static_cast<Uint8>(8 - MaskBits(g));
  fmt.Bloss = static_cast<Uint8>(8 - MaskBits(b));
  fmt.Aloss = static_cast<Uint8>(8 - MaskBits(a));
  return SDL_ConvertSurface(src, &fmt, flags);
}

extern "C" int SDL_FillRect(SDL_Surface* dst, const SDL_Rect* rect, Uint32 color) {

  if (!dst) {
    return SDL_SetError("SDL_FillRect: surface is null");
  }
  SDL_Rect target = dst->clip_rect;
  if (rect) {
    if (!IntersectRect(*rect, dst->clip_rect, &target)) {
      return 0;
    }
  }
  if (!FillSurfaceRect(dst, target, color)) {
    return SDL_SetError("SDL_FillRect: unsupported surface layout");
  }
  return 0;
}

extern "C" int SDL_FillRects(SDL_Surface* dst, const SDL_Rect* rects, int count, Uint32 color) {

  if (!dst || !rects || count < 0) {
    return SDL_SetError("SDL_FillRects: invalid arguments");
  }
  for (int i = 0; i < count; ++i) {
    if (SDL_FillRect(dst, &rects[i], color) != 0) {
      return -1;
    }
  }
  return 0;
}

extern "C" int SDL_UpperBlit(SDL_Surface* src, const SDL_Rect* srcrect, SDL_Surface* dst, SDL_Rect* dstrect) {

  if (!src || !dst) {
    return SDL_SetError("SDL_UpperBlit: invalid surface");
  }

  SDL_Rect src_rect {0, 0, src->w, src->h};
  if (srcrect) {
    src_rect = *srcrect;
  }

  SDL_Rect dst_rect {};
  if (dstrect) {
    dst_rect = *dstrect;
  }
  // A non-scaling blit ignores dstrect.w/h; present it to the host scaled
  // blit with the source dimensions so it degenerates into a straight copy.
  dst_rect.w = src_rect.w;
  dst_rect.h = src_rect.h;

  const int rc = fexfn_pack_FEX_SDL2_UpperBlitScaled(src, &src_rect, dst, &dst_rect);
  if (dstrect) {
    dstrect->w = dst_rect.w;
    dstrect->h = dst_rect.h;
  }
  return rc;
}

extern "C" int SDL_LowerBlit(SDL_Surface* src, SDL_Rect* srcrect, SDL_Surface* dst, SDL_Rect* dstrect) {

  return SDL_UpperBlit(src, srcrect, dst, dstrect);
}

extern "C" int SDL_UpperBlitScaled(SDL_Surface* src, const SDL_Rect* srcrect, SDL_Surface* dst, SDL_Rect* dstrect) {

  if (!src || !dst) {
    return SDL_SetError("SDL_UpperBlitScaled: invalid surface");
  }
  SDL_Rect src_rect {0, 0, src->w, src->h};
  if (srcrect) {
    src_rect = *srcrect;
  }
  return fexfn_pack_FEX_SDL2_UpperBlitScaled(src, &src_rect, dst, dstrect);
}

extern "C" int SDL_LowerBlitScaled(SDL_Surface* src, SDL_Rect* srcrect, SDL_Surface* dst, SDL_Rect* dstrect) {

  if (!src || !dst) {
    return SDL_SetError("SDL_LowerBlitScaled: invalid surface");
  }
  SDL_Rect src_rect {0, 0, src->w, src->h};
  if (srcrect) {
    src_rect = *srcrect;
  }
  return fexfn_pack_FEX_SDL2_UpperBlitScaled(src, &src_rect, dst, dstrect);
}

extern "C" int SDL_SoftStretch(SDL_Surface* src, const SDL_Rect* srcrect, SDL_Surface* dst, const SDL_Rect* dstrect) {

  if (!src || !dst) {
    return SDL_SetError("SDL_SoftStretch: invalid surface");
  }
  SDL_Rect src_rect {0, 0, src->w, src->h};
  if (srcrect) {
    src_rect = *srcrect;
  }
  SDL_Rect dst_rect {0, 0, dst->w, dst->h};
  if (dstrect) {
    dst_rect = *dstrect;
  }
  return fexfn_pack_FEX_SDL2_UpperBlitScaled(src, &src_rect, dst, &dst_rect);
}

extern "C" int SDL_SoftStretchLinear(SDL_Surface* src, const SDL_Rect* srcrect, SDL_Surface* dst, const SDL_Rect* dstrect) {

  return SDL_SoftStretch(src, srcrect, dst, dstrect);
}

extern "C" void SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_MODE mode) {

  YUVConversionMode = mode;
}

extern "C" SDL_YUV_CONVERSION_MODE SDL_GetYUVConversionMode() {

  return YUVConversionMode;
}

extern "C" SDL_YUV_CONVERSION_MODE SDL_GetYUVConversionModeForResolution(int width, int height) {

  (void)width;
  (void)height;
  return YUVConversionMode;
}

// ===========================================================================
// Cursors
// ===========================================================================

extern "C" SDL_Cursor* SDL_CreateCursor(const Uint8* data, const Uint8* mask, int w, int h,
                                        int hot_x, int hot_y) {

  if (!data || !mask || w <= 0 || h <= 0) {
    SDL_SetError("SDL_CreateCursor: invalid arguments");
    return nullptr;
  }

  // Expand the (data, mask) bitmask pair into an ARGB8888 image the host
  // CreateColorCursor bridge can consume. SDL semantics per pixel:
  //   data=0 mask=0 -> transparent, data=1 mask=0 -> black,
  //   data=0 mask=1 -> white,       data=1 mask=1 -> inverted (approx. gray).
  std::vector<Uint8> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
  const int row_bytes = (w + 7) / 8;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const int byte_index = y * row_bytes + (x >> 3);
      const int bit = 7 - (x & 7);
      const int d = (data[byte_index] >> bit) & 1;
      const int m = (mask[byte_index] >> bit) & 1;
      Uint32 px;
      if (d == 0 && m == 0) {
        px = 0x00000000;
      } else if (d != 0 && m == 0) {
        px = 0xFF000000;
      } else if (d == 0 && m != 0) {
        px = 0xFFFFFFFF;
      } else {
        px = 0xFF7F7F7F;
      }
      std::memcpy(&pixels[(static_cast<size_t>(y) * w + x) * 4], &px, sizeof(px));
    }
  }

  SDL_Surface* surface =
      fexfn_pack_FEX_SDL2_CreateRGBSurfaceFrom(pixels.data(), w, h, 32, w * 4,
                                             0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
  if (!surface) {
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> guard(SurfaceStateMutex);
    GetSurfaceExtraLocked(surface).OwnedPixels = std::move(pixels);
  }

  SDL_Cursor* cursor = fexfn_pack_FEX_SDL2_CreateColorCursor(surface, hot_x, hot_y);
  SDL_FreeSurface(surface);
  if (!cursor) {
    cursor = fexfn_pack_FEX_SDL2_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
  }
  return cursor;
}

extern "C" SDL_Cursor* SDL_CreateColorCursor(SDL_Surface* surface, int hot_x, int hot_y) {

  return fexfn_pack_FEX_SDL2_CreateColorCursor(surface, hot_x, hot_y);
}

extern "C" SDL_Cursor* SDL_CreateSystemCursor(SDL_SystemCursor id) {

  return fexfn_pack_FEX_SDL2_CreateSystemCursor(id);
}

extern "C" void SDL_SetCursor(SDL_Cursor* cursor) {

  fexfn_pack_FEX_SDL2_SetCursor(cursor);
}

extern "C" SDL_Cursor* SDL_GetCursor() {

  return fexfn_pack_FEX_SDL2_GetCursor();
}

extern "C" SDL_Cursor* SDL_GetDefaultCursor() {

  return fexfn_pack_FEX_SDL2_GetDefaultCursor();
}

extern "C" void SDL_FreeCursor(SDL_Cursor* cursor) {

  fexfn_pack_FEX_SDL2_FreeCursor(cursor);
}

extern "C" int SDL_ShowCursor(int toggle) {

  return fexfn_pack_FEX_SDL2_ShowCursor(toggle);
}

extern "C" SDL_Window* SDL_GetMouseFocus() {

  return reinterpret_cast<SDL_Window*>(PrimaryWindowPointer.load(std::memory_order_acquire));
}

// ---------------------------------------------------------------------------
// Boundary: the definitions above cover init/window/video/events/GL/surface/
// cursor only. Everything below comes from the companion include files:
//   libSDL2_Compat.inc   - stdinc/memory/thread/semaphore/mutex/RWops/LoadFile/
//                          SIMD/error/log/audio/hints/env guest-local helpers
//   libSDL2_FNAStubs.inc - joystick/gamecontroller/haptic/sensor/touch/
//                          clipboard/messagebox/vulkan/power/keyboard/mouse/cpu
//                          stubs required by FNA-era games
// Do NOT redefine any symbol owned by those files here.
// ---------------------------------------------------------------------------

#include "libSDL2_Compat.inc"

#include "libSDL2_FNAStubs.inc"

LOAD_LIB(libSDL2)
