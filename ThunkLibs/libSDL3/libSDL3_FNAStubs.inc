#undef SDL_BindAudioStream
#undef SDL_CloseGamepad
#undef SDL_CreateAudioStream
#undef SDL_DisableScreenSaver
#undef SDL_EnableScreenSaver
#undef SDL_GamepadHasAxis
#undef SDL_GamepadHasButton
#undef SDL_GamepadHasSensor
#undef SDL_GamepadSensorEnabled
#undef SDL_GetAudioRecordingDevices
#undef SDL_GetAudioStreamData
#undef SDL_GetAudioStreamQueued
#undef SDL_GetBasePath
#undef SDL_GetEventFilter
#undef SDL_GetFullscreenDisplayModes
#undef SDL_GetGamepadAxis
#undef SDL_GetGamepadButton
#undef SDL_GetGamepadJoystick
#undef SDL_GetGamepadMapping
#undef SDL_GetGamepadName
#undef SDL_GetGamepadProperties
#undef SDL_GetGamepadSensorData
#undef SDL_GetGamepadType
#undef SDL_GetGlobalMouseState
#undef SDL_GetJoystickID
#undef SDL_GetJoystickProduct
#undef SDL_GetJoystickType
#undef SDL_GetJoystickVendor
#undef SDL_GetMouseState
#undef SDL_GetNumGamepadTouchpads
#undef SDL_GetPrefPath
#undef SDL_GetRelativeMouseState
#undef SDL_GetTouchDevices
#undef SDL_GetTouchFingers
#undef SDL_GetWindowRelativeMouseMode
#undef SDL_LoadBMP
#undef SDL_LoadFile
#undef SDL_OpenAudioDevice
#undef SDL_OpenGamepad
#undef SDL_PauseAudioDevice
#undef SDL_PeepEvents
#undef SDL_RumbleGamepad
#undef SDL_RumbleGamepadTriggers
#undef SDL_ScreenKeyboardShown
#undef SDL_SetEventFilter
#undef SDL_SetGamepadLED
#undef SDL_SetGamepadSensorEnabled
#undef SDL_SetTextInputArea
#undef SDL_SetWindowIcon
#undef SDL_SetWindowRelativeMouseMode
#undef SDL_WarpMouseInWindow
#undef SDL_memcpy

// Generated from the SDL3 headers: SDL entry points that FNA's SDL3 platform layer reaches.
// Audio capture, gamepads, touch and similar are inert stubs; see libSDL3_Compat.inc for real services.

namespace {
SDL_EventFilter CompatEventFilter = nullptr;
void* CompatEventFilterData = nullptr;
bool CompatRelativeMouse = false;
} // namespace

extern "C" bool SDL_BindAudioStream(SDL_AudioDeviceID devid, SDL_AudioStream *stream) { return false; }
extern "C" void SDL_CloseGamepad(SDL_Gamepad *gamepad) {  }
extern "C" SDL_AudioStream * SDL_CreateAudioStream(const SDL_AudioSpec *src_spec, const SDL_AudioSpec *dst_spec) { return nullptr; }
extern "C" bool SDL_DisableScreenSaver() { return true; }
extern "C" bool SDL_EnableScreenSaver() { return true; }
extern "C" bool SDL_GamepadHasAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis) { return false; }
extern "C" bool SDL_GamepadHasButton(SDL_Gamepad *gamepad, SDL_GamepadButton button) { return false; }
extern "C" bool SDL_GamepadHasSensor(SDL_Gamepad *gamepad, SDL_SensorType type) { return false; }
extern "C" bool SDL_GamepadSensorEnabled(SDL_Gamepad *gamepad, SDL_SensorType type) { return false; }
extern "C" SDL_AudioDeviceID * SDL_GetAudioRecordingDevices(int *count) {
  if (count) {
    *count = 0;
  }
  return static_cast<SDL_AudioDeviceID*>(std::calloc(1, sizeof(SDL_AudioDeviceID)));
}
extern "C" int SDL_GetAudioStreamData(SDL_AudioStream *stream, void *buf, int len) { return 0; }
extern "C" int SDL_GetAudioStreamQueued(SDL_AudioStream *stream) { return 0; }
extern "C" const char * SDL_GetBasePath() {
  static std::string base;
  if (base.empty()) {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = '\0';
      base = buf;
      base.resize(base.find_last_of('/') + 1);
    }
    if (base.empty() && getcwd(buf, sizeof(buf))) {
      base = std::string(buf) + "/";
    }
  }
  return base.c_str();
}
extern "C" bool SDL_GetEventFilter(SDL_EventFilter *filter, void **userdata) { if (filter) { *filter = CompatEventFilter; } if (userdata) { *userdata = CompatEventFilterData; } return CompatEventFilter != nullptr; }
extern "C" SDL_DisplayMode ** SDL_GetFullscreenDisplayModes(SDL_DisplayID displayID, int *count) {
  const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(displayID);
  if (count) {
    *count = desktop ? 1 : 0;
  }
  if (!desktop) {
    return nullptr;
  }
  auto* block = static_cast<char*>(std::malloc(2 * sizeof(SDL_DisplayMode*) + sizeof(SDL_DisplayMode)));
  if (!block) {
    return nullptr;
  }
  auto** list = reinterpret_cast<SDL_DisplayMode**>(block);
  auto* mode = reinterpret_cast<SDL_DisplayMode*>(block + 2 * sizeof(SDL_DisplayMode*));
  *mode = *desktop;
  list[0] = mode;
  list[1] = nullptr;
  return list;
}
extern "C" Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis) { return 0; }
extern "C" bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button) { return false; }
extern "C" SDL_Joystick * SDL_GetGamepadJoystick(SDL_Gamepad *gamepad) { return nullptr; }
extern "C" char * SDL_GetGamepadMapping(SDL_Gamepad *gamepad) { return nullptr; }
extern "C" const char * SDL_GetGamepadName(SDL_Gamepad *gamepad) { return nullptr; }
extern "C" SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad *gamepad) { return 0; }
extern "C" bool SDL_GetGamepadSensorData(SDL_Gamepad *gamepad, SDL_SensorType type, float *data, int num_values) { if (data) { *data = 0.0f; } return false; }
extern "C" SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad) { return SDL_GAMEPAD_TYPE_UNKNOWN; }
extern "C" SDL_MouseButtonFlags SDL_GetGlobalMouseState(float *x, float *y) { if (x) { *x = CompatMouseX.load(); } if (y) { *y = CompatMouseY.load(); } return CompatMouseButtons.load(); }
extern "C" SDL_JoystickID SDL_GetJoystickID(SDL_Joystick *joystick) { return 0; }
extern "C" Uint16 SDL_GetJoystickProduct(SDL_Joystick *joystick) { return 0; }
extern "C" SDL_JoystickType SDL_GetJoystickType(SDL_Joystick *joystick) { return SDL_JOYSTICK_TYPE_UNKNOWN; }
extern "C" Uint16 SDL_GetJoystickVendor(SDL_Joystick *joystick) { return 0; }
extern "C" SDL_MouseButtonFlags SDL_GetMouseState(float *x, float *y) { if (x) { *x = CompatMouseX.load(); } if (y) { *y = CompatMouseY.load(); } return CompatMouseButtons.load(); }
extern "C" int SDL_GetNumGamepadTouchpads(SDL_Gamepad *gamepad) { return 0; }
extern "C" char * SDL_GetPrefPath(const char *org, const char *app) {
  if (!app) {
    SDL_SetError("Parameter 'app' is invalid");
    return nullptr;
  }
  const char* xdg = std::getenv("XDG_DATA_HOME");
  const char* home = std::getenv("HOME");
  std::string path = xdg && *xdg ? xdg : (home && *home ? std::string(home) + "/.local/share" : std::string("/tmp"));
  path += "/";
  if (org && *org) {
    path += std::string(org) + "/";
  }
  path += std::string(app) + "/";
  for (size_t i = 1; i < path.size(); ++i) {
    if (path[i] == '/') {
      mkdir(path.substr(0, i).c_str(), 0700);
    }
  }
  return strdup(path.c_str());
}
extern "C" SDL_MouseButtonFlags SDL_GetRelativeMouseState(float *x, float *y) { if (x) { *x = 0.0f; } if (y) { *y = 0.0f; } return 0; }
extern "C" SDL_TouchID * SDL_GetTouchDevices(int *count) {
  if (count) {
    *count = 0;
  }
  return static_cast<SDL_TouchID*>(std::calloc(1, sizeof(SDL_TouchID)));
}
extern "C" SDL_Finger ** SDL_GetTouchFingers(SDL_TouchID touchID, int *count) { if (count) { *count = 0; } return nullptr; }
extern "C" bool SDL_GetWindowRelativeMouseMode(SDL_Window *window) { return CompatRelativeMouse; }
extern "C" SDL_Surface * SDL_LoadBMP(const char *file) { SDL_SetError("SDL_LoadBMP is not supported by the Vexa SDL3 bridge"); return nullptr; }
extern "C" void * SDL_LoadFile(const char *file, size_t *datasize) {
  FILE* f = file ? std::fopen(file, "rb") : nullptr;
  if (datasize) {
    *datasize = 0;
  }
  if (!f) {
    SDL_SetError("Couldn't open %s", file ? file : "(null)");
    return nullptr;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  auto* data = static_cast<char*>(std::malloc(size > 0 ? static_cast<size_t>(size) + 1 : 1));
  const size_t got = data ? std::fread(data, 1, size > 0 ? static_cast<size_t>(size) : 0, f) : 0;
  std::fclose(f);
  if (!data) {
    return nullptr;
  }
  data[got] = '\0';
  if (datasize) {
    *datasize = got;
  }
  return data;
}
extern "C" SDL_AudioDeviceID SDL_OpenAudioDevice(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec) { return 0; }
extern "C" SDL_Gamepad * SDL_OpenGamepad(SDL_JoystickID instance_id) { return nullptr; }
extern "C" bool SDL_PauseAudioDevice(SDL_AudioDeviceID devid) { return true; }
extern "C" int SDL_PeepEvents(SDL_Event *events, int numevents, SDL_EventAction action, Uint32 minType, Uint32 maxType) { return 0; }
extern "C" bool SDL_RumbleGamepad(SDL_Gamepad *gamepad, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble, Uint32 duration_ms) { return false; }
extern "C" bool SDL_RumbleGamepadTriggers(SDL_Gamepad *gamepad, Uint16 left_rumble, Uint16 right_rumble, Uint32 duration_ms) { return false; }
extern "C" bool SDL_ScreenKeyboardShown(SDL_Window *window) { return false; }
extern "C" void SDL_SetEventFilter(SDL_EventFilter filter, void *userdata) { CompatEventFilter = filter; CompatEventFilterData = userdata; }
extern "C" bool SDL_SetGamepadLED(SDL_Gamepad *gamepad, Uint8 red, Uint8 green, Uint8 blue) { return false; }
extern "C" bool SDL_SetGamepadSensorEnabled(SDL_Gamepad *gamepad, SDL_SensorType type, bool enabled) { return false; }
extern "C" bool SDL_SetTextInputArea(SDL_Window *window, const SDL_Rect *rect, int cursor) { return true; }
extern "C" bool SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon) { return true; }
extern "C" bool SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled) { CompatRelativeMouse = enabled; return true; }
extern "C" void SDL_WarpMouseInWindow(SDL_Window *window, float x, float y) {  }
extern "C" void * SDL_memcpy(SDL_OUT_BYTECAP(len) void *dst, SDL_IN_BYTECAP(len) const void *src, size_t len) { return std::memcpy(dst, src, len); }
