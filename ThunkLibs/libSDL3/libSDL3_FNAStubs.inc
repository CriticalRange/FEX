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
// Virtual gamepad + joystick backed by CompatPadButtons/CompatPadAxes in libSDL3_Compat.inc.

namespace {
SDL_EventFilter CompatEventFilter = nullptr;
void* CompatEventFilterData = nullptr;
bool CompatRelativeMouse = false;

struct VexaJoyImpl {
  Uint32 Magic;
};
struct VexaGamepadImpl {
  VexaJoyImpl* Joy;
};
VexaJoyImpl VexaJoyInst {0x56455841u};
VexaGamepadImpl VexaGamepadInst {&VexaJoyInst};
constexpr const char* VexaPadName = "VEXA Virtual Gamepad";
constexpr const char* VexaPadPath = "vexa://virtual-pad";
constexpr Uint16 VexaPadVendor = 0x045e;  // Microsoft
constexpr Uint16 VexaPadProduct = 0x028e; // Xbox 360 Controller
constexpr const char* VexaPadMappingString =
    "03564558415649525455415041443031,VEXA Virtual Gamepad,"
    "a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,"
    "leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,"
    "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
    "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,"
    "platform:Android,";

bool VexaIsJoy(SDL_Joystick* joy) {
  return joy == reinterpret_cast<SDL_Joystick*>(&VexaJoyInst);
}
bool VexaIsGamepad(SDL_Gamepad* gp) {
  return gp == reinterpret_cast<SDL_Gamepad*>(&VexaGamepadInst);
}
bool VexaIsPadId(SDL_JoystickID id) {
  return id == kVexaPadInstanceId || id == 0;
}
SDL_GUID VexaPadGuid() {
  SDL_GUID guid {};
  guid.data[0] = 0x03;
  std::memcpy(guid.data + 1, "VEXAVIRTUAPAD01", 15);
  return guid;
}
} // namespace

extern "C" bool SDL_BindAudioStream(SDL_AudioDeviceID devid, SDL_AudioStream *stream) { return false; }
extern "C" void SDL_CloseGamepad(SDL_Gamepad *gamepad) {  }
extern "C" void SDL_CloseJoystick(SDL_Joystick *joystick) {  }
extern "C" SDL_AudioStream * SDL_CreateAudioStream(const SDL_AudioSpec *src_spec, const SDL_AudioSpec *dst_spec) { return nullptr; }
extern "C" bool SDL_DisableScreenSaver() { return true; }
extern "C" bool SDL_EnableScreenSaver() { return true; }
extern "C" bool SDL_GamepadConnected(SDL_Gamepad *gamepad) { return VexaIsGamepad(gamepad); }
extern "C" bool SDL_GamepadEventsEnabled() { return true; }
extern "C" void SDL_SetGamepadEventsEnabled(bool enabled) { (void)enabled; }
extern "C" bool SDL_JoystickEventsEnabled() { return true; }
extern "C" void SDL_SetJoystickEventsEnabled(bool enabled) { (void)enabled; }
extern "C" bool SDL_GamepadHasAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis) {
  return VexaIsGamepad(gamepad) && axis >= 0 && axis < SDL_GAMEPAD_AXIS_COUNT;
}
extern "C" bool SDL_GamepadHasButton(SDL_Gamepad *gamepad, SDL_GamepadButton button) {
  return VexaIsGamepad(gamepad) && button >= 0 && button <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
}
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
extern "C" bool SDL_HasGamepad() { return true; }
extern "C" bool SDL_HasJoystick() { return true; }
extern "C" SDL_JoystickID * SDL_GetGamepads(int *count) {
  VexaAnnouncePadSDL3();
  auto* list = static_cast<SDL_JoystickID*>(std::malloc(2 * sizeof(SDL_JoystickID)));
  if (!list) {
    if (count) *count = 0;
    return nullptr;
  }
  list[0] = kVexaPadInstanceId;
  list[1] = 0;
  if (count) *count = 1;
  return list;
}
extern "C" SDL_JoystickID * SDL_GetJoysticks(int *count) {
  return SDL_GetGamepads(count);
}
extern "C" bool SDL_IsGamepad(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id);
}
extern "C" const char * SDL_GetGamepadNameForID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? VexaPadName : nullptr;
}
extern "C" const char * SDL_GetGamepadPathForID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? VexaPadPath : nullptr;
}
extern "C" int SDL_GetGamepadPlayerIndexForID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? 0 : -1;
}
extern "C" SDL_GUID SDL_GetGamepadGUIDForID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? VexaPadGuid() : SDL_GUID {};
}
extern "C" Uint16 SDL_GetGamepadVendorForID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? VexaPadVendor : 0;
}
extern "C" Uint16 SDL_GetGamepadProductForID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? VexaPadProduct : 0;
}
extern "C" Uint16 SDL_GetGamepadProductVersionForID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? 1 : 0;
}
extern "C" SDL_GamepadType SDL_GetGamepadTypeForID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? SDL_GAMEPAD_TYPE_XBOX360 : SDL_GAMEPAD_TYPE_UNKNOWN;
}
extern "C" SDL_GamepadType SDL_GetRealGamepadTypeForID(SDL_JoystickID instance_id) {
  return SDL_GetGamepadTypeForID(instance_id);
}
extern "C" char * SDL_GetGamepadMappingForID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? strdup(VexaPadMappingString) : nullptr;
}
extern "C" Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis) {
  VexaPollPointerEvents();
  if (!VexaIsGamepad(gamepad) || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT) {
    return 0;
  }
  return CompatPadAxes[axis];
}
extern "C" bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button) {
  VexaPollPointerEvents();
  if (!VexaIsGamepad(gamepad) || button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT) {
    return false;
  }
  return CompatPadButtons[button] != 0;
}
extern "C" SDL_Joystick * SDL_GetGamepadJoystick(SDL_Gamepad *gamepad) {
  return VexaIsGamepad(gamepad) ? reinterpret_cast<SDL_Joystick*>(&VexaJoyInst) : nullptr;
}
extern "C" char * SDL_GetGamepadMapping(SDL_Gamepad *gamepad) {
  return VexaIsGamepad(gamepad) ? strdup(VexaPadMappingString) : nullptr;
}
extern "C" const char * SDL_GetGamepadName(SDL_Gamepad *gamepad) {
  return VexaIsGamepad(gamepad) ? VexaPadName : nullptr;
}
extern "C" const char * SDL_GetGamepadPath(SDL_Gamepad *gamepad) {
  return VexaIsGamepad(gamepad) ? VexaPadPath : nullptr;
}
extern "C" SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad *gamepad) { return 0; }
extern "C" bool SDL_GetGamepadSensorData(SDL_Gamepad *gamepad, SDL_SensorType type, float *data, int num_values) { if (data) { *data = 0.0f; } return false; }
extern "C" SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad) {
  return VexaIsGamepad(gamepad) ? SDL_GAMEPAD_TYPE_XBOX360 : SDL_GAMEPAD_TYPE_UNKNOWN;
}
extern "C" SDL_GamepadType SDL_GetRealGamepadType(SDL_Gamepad *gamepad) {
  return SDL_GetGamepadType(gamepad);
}
extern "C" int SDL_GetGamepadPlayerIndex(SDL_Gamepad *gamepad) {
  return VexaIsGamepad(gamepad) ? 0 : -1;
}
extern "C" bool SDL_SetGamepadPlayerIndex(SDL_Gamepad *gamepad, int player_index) {
  (void)player_index;
  return VexaIsGamepad(gamepad);
}
extern "C" Uint16 SDL_GetGamepadVendor(SDL_Gamepad *gamepad) {
  return VexaIsGamepad(gamepad) ? VexaPadVendor : 0;
}
extern "C" Uint16 SDL_GetGamepadProduct(SDL_Gamepad *gamepad) {
  return VexaIsGamepad(gamepad) ? VexaPadProduct : 0;
}
extern "C" Uint16 SDL_GetGamepadProductVersion(SDL_Gamepad *gamepad) {
  return VexaIsGamepad(gamepad) ? 1 : 0;
}
extern "C" SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *gamepad) {
  return VexaIsGamepad(gamepad) ? kVexaPadInstanceId : 0;
}
extern "C" SDL_Gamepad * SDL_GetGamepadFromID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? reinterpret_cast<SDL_Gamepad*>(&VexaGamepadInst) : nullptr;
}
extern "C" SDL_Gamepad * SDL_GetGamepadFromPlayerIndex(int player_index) {
  return player_index == 0 ? reinterpret_cast<SDL_Gamepad*>(&VexaGamepadInst) : nullptr;
}
extern "C" void SDL_UpdateGamepads() { VexaPollPointerEvents(); }
extern "C" void SDL_UpdateJoysticks() { VexaPollPointerEvents(); }
extern "C" SDL_MouseButtonFlags SDL_GetGlobalMouseState(float *x, float *y) { if (x) { *x = CompatMouseX.load(); } if (y) { *y = CompatMouseY.load(); } return CompatMouseButtons.load(); }
extern "C" SDL_Joystick * SDL_OpenJoystick(SDL_JoystickID instance_id) {
  if (!VexaIsPadId(instance_id)) {
    return nullptr;
  }
  VexaAnnouncePadSDL3();
  return reinterpret_cast<SDL_Joystick*>(&VexaJoyInst);
}
extern "C" SDL_Joystick * SDL_GetJoystickFromID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? reinterpret_cast<SDL_Joystick*>(&VexaJoyInst) : nullptr;
}
extern "C" SDL_Joystick * SDL_GetJoystickFromPlayerIndex(int player_index) {
  return player_index == 0 ? reinterpret_cast<SDL_Joystick*>(&VexaJoyInst) : nullptr;
}
extern "C" const char * SDL_GetJoystickName(SDL_Joystick *joystick) {
  return VexaIsJoy(joystick) ? VexaPadName : nullptr;
}
extern "C" const char * SDL_GetJoystickNameForID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? VexaPadName : nullptr;
}
extern "C" const char * SDL_GetJoystickPath(SDL_Joystick *joystick) {
  return VexaIsJoy(joystick) ? VexaPadPath : nullptr;
}
extern "C" const char * SDL_GetJoystickPathForID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? VexaPadPath : nullptr;
}
extern "C" SDL_GUID SDL_GetJoystickGUID(SDL_Joystick *joystick) {
  return VexaIsJoy(joystick) ? VexaPadGuid() : SDL_GUID {};
}
extern "C" SDL_GUID SDL_GetJoystickGUIDForID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? VexaPadGuid() : SDL_GUID {};
}
extern "C" SDL_JoystickID SDL_GetJoystickID(SDL_Joystick *joystick) {
  return VexaIsJoy(joystick) ? kVexaPadInstanceId : 0;
}
extern "C" Uint16 SDL_GetJoystickProduct(SDL_Joystick *joystick) {
  return VexaIsJoy(joystick) ? VexaPadProduct : 0;
}
extern "C" Uint16 SDL_GetJoystickProductForID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? VexaPadProduct : 0;
}
extern "C" SDL_JoystickType SDL_GetJoystickType(SDL_Joystick *joystick) {
  return VexaIsJoy(joystick) ? SDL_JOYSTICK_TYPE_GAMEPAD : SDL_JOYSTICK_TYPE_UNKNOWN;
}
extern "C" SDL_JoystickType SDL_GetJoystickTypeForID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? SDL_JOYSTICK_TYPE_GAMEPAD : SDL_JOYSTICK_TYPE_UNKNOWN;
}
extern "C" Uint16 SDL_GetJoystickVendor(SDL_Joystick *joystick) {
  return VexaIsJoy(joystick) ? VexaPadVendor : 0;
}
extern "C" Uint16 SDL_GetJoystickVendorForID(SDL_JoystickID instance_id) {
  return VexaIsPadId(instance_id) ? VexaPadVendor : 0;
}
extern "C" bool SDL_JoystickConnected(SDL_Joystick *joystick) {
  return VexaIsJoy(joystick);
}
extern "C" int SDL_GetNumJoystickAxes(SDL_Joystick *joystick) {
  return VexaIsJoy(joystick) ? SDL_GAMEPAD_AXIS_COUNT : 0;
}
extern "C" int SDL_GetNumJoystickButtons(SDL_Joystick *joystick) {
  return VexaIsJoy(joystick) ? SDL_GAMEPAD_BUTTON_COUNT : 0;
}
extern "C" int SDL_GetNumJoystickHats(SDL_Joystick *joystick) {
  return VexaIsJoy(joystick) ? 1 : 0;
}
extern "C" Sint16 SDL_GetJoystickAxis(SDL_Joystick *joystick, int axis) {
  VexaPollPointerEvents();
  if (!VexaIsJoy(joystick) || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT) {
    return 0;
  }
  return CompatPadAxes[axis];
}
extern "C" bool SDL_GetJoystickButton(SDL_Joystick *joystick, int button) {
  VexaPollPointerEvents();
  if (!VexaIsJoy(joystick) || button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT) {
    return false;
  }
  return CompatPadButtons[button] != 0;
}
extern "C" Uint8 SDL_GetJoystickHat(SDL_Joystick *joystick, int hat) {
  VexaPollPointerEvents();
  if (!VexaIsJoy(joystick) || hat != 0) {
    return SDL_HAT_CENTERED;
  }
  return VexaComputePadHat();
}
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
extern "C" SDL_Gamepad * SDL_OpenGamepad(SDL_JoystickID instance_id) {
  if (!VexaIsPadId(instance_id)) {
    return nullptr;
  }
  VexaAnnouncePadSDL3();
  return reinterpret_cast<SDL_Gamepad*>(&VexaGamepadInst);
}
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
