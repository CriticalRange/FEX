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
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_version.h>

#include <GL/gl.h>
#include <GL/glext.h>

#undef GL_ARB_viewport_array
#include "../libGL/glcorearb.h"
#include "../libGL/android_gl_proc_list.h"

extern "C" {
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
}

#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
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
  return fexfn_pack_FEX_SDL_GL_CreateContext(window);
}

extern "C" void SDL_GL_DeleteContext(SDL_GLContext context) {
  fexfn_pack_FEX_SDL_GL_DeleteContext(context);
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
  return fexfn_pack_FEX_SDL_GL_SetSwapInterval(interval) != 0;
}

extern "C" bool SDL_GL_SwapWindow(SDL_Window* window) {
  return fexfn_pack_FEX_SDL_GL_SwapWindow(window) != 0;
}

extern "C" SDL_FunctionPointer SDL_GL_GetProcAddress(const char* proc) {
  if (!proc) {
    return nullptr;
  }

  for (const auto& entry : GLProcTable) {
    if (entry.Name == proc) {
      return entry.Function;
    }
  }

  if (std::string_view {proc} == "SDL_GL_GetProcAddress") {
    return reinterpret_cast<SDL_FunctionPointer>(SDL_GL_GetProcAddress);
  }

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
