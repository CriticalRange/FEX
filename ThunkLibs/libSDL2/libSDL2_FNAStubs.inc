// VEXA_FIXES: Inert-but-safe SDL2 entry points for the long tail of the SDL2
// API surface that FNA/SDL2-CS may dlsym but Celeste never uses functionally.
// Every routine degrades gracefully: it returns the SDL2-documented
// "unsupported/empty" value (0, -1, SDL_FALSE, NULL, a zeroed struct) and never
// aborts. Real services live in libSDL2_Compat.inc; window/event/cursor bridges
// live in libSDL2_Guest.cpp. This file owns only the leftovers listed below.
// Included from libSDL2_Guest.cpp after libSDL2_Compat.inc.

#include <SDL_vulkan.h> // not pulled in by <SDL.h>; needed for VkInstance/VkSurfaceKHR

#include <cstdlib>
#include <cstring>

namespace {

// Local strdup used by the clipboard stubs. Deliberately not exported as
// SDL_strdup (which is defined in the SDL_stdinc section below): callers must
// be able to SDL_free() the result, which resolves to plain free() here.
char* FNAStubDup(const char* str) {
  const size_t len = str ? std::strlen(str) : 0;
  char* out = static_cast<char*>(std::malloc(len + 1));
  if (out) {
    std::memcpy(out, str ? str : "", len + 1);
  }
  return out;
}

// SDL_QUERY/SDL_IGNORE/SDL_DISABLE/SDL_ENABLE bookkeeping for the
// *_EventState functions. All event sources are inert, so the state only
// needs to round-trip.
int FNAStubEventState(int state, int& current) {
  if (state != SDL_QUERY) {
    current = state;
  }
  return current;
}

int FNAJoystickEvents = SDL_ENABLE;
int FNAGameControllerEvents = SDL_ENABLE;

// Unsigned integer -> string, radix 2..36, lowercase digits (matches SDL).
char* FNAStubUtoa(Uint64 value, char* str, int radix) {
  static const char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  char tmp[64];
  int n = 0;
  do {
    tmp[n++] = kDigits[value % static_cast<Uint64>(radix)];
    value /= static_cast<Uint64>(radix);
  } while (value && n < static_cast<int>(sizeof(tmp)));
  char* out = str;
  while (n > 0) {
    *out++ = tmp[--n];
  }
  *out = '\0';
  return str;
}

wchar_t FNAStubWcsLower(wchar_t c) {
  return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
}

int FNAStubHexNibble(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Length in bytes of the UTF-8 sequence starting at c (1..4).
size_t FNAStubUtf8SeqLen(unsigned char c) {
  if (c < 0x80) {
    return 1;
  }
  if (c < 0xE0) {
    return 2;
  }
  if (c < 0xF0) {
    return 3;
  }
  return 4;
}

} // namespace

// ---- SDL_atomic.h ----------------------------------------------------------
// Real (cheap) implementations on GCC/Clang builtins; SDL2-CS and SDL itself
// call these even when nothing else is running.

extern "C" SDL_bool SDL_AtomicTryLock(SDL_SpinLock *lock) {

  return __sync_bool_compare_and_swap(lock, 0, 1) ? SDL_TRUE : SDL_FALSE;
}

extern "C" void SDL_AtomicLock(SDL_SpinLock *lock) {

  while (!__sync_bool_compare_and_swap(lock, 0, 1)) {
  }
}

extern "C" void SDL_AtomicUnlock(SDL_SpinLock *lock) {

  __sync_lock_release(lock);
}

extern "C" void SDL_MemoryBarrierReleaseFunction(void) {

  __sync_synchronize();
}

extern "C" void SDL_MemoryBarrierAcquireFunction(void) {

  __sync_synchronize();
}

extern "C" SDL_bool SDL_AtomicCAS(SDL_atomic_t *a, int oldval, int newval) {

  return __sync_bool_compare_and_swap(&a->value, oldval, newval) ? SDL_TRUE : SDL_FALSE;
}

extern "C" int SDL_AtomicSet(SDL_atomic_t *a, int v) {

  return __sync_lock_test_and_set(&a->value, v);
}

extern "C" int SDL_AtomicGet(SDL_atomic_t *a) {

  return __sync_fetch_and_add(&a->value, 0);
}

extern "C" int SDL_AtomicAdd(SDL_atomic_t *a, int v) {

  return __sync_fetch_and_add(&a->value, v);
}

extern "C" SDL_bool SDL_AtomicCASPtr(void **a, void *oldval, void *newval) {

  return __sync_bool_compare_and_swap(a, oldval, newval) ? SDL_TRUE : SDL_FALSE;
}

extern "C" void* SDL_AtomicSetPtr(void **a, void* v) {

  return __sync_lock_test_and_set(a, v);
}

extern "C" void* SDL_AtomicGetPtr(void **a) {

  return __atomic_load_n(a, __ATOMIC_SEQ_CST);
}

// ---- SDL_joystick.h --------------------------------------------------------
// One always-attached virtual pad. The on-screen GuestPad and physical
// controllers reach the guest as X11-style records in /tmp/vexa-pointer; the
// pump in the guest core maps them onto CompatPadButtons/CompatPadAxes, which
// the joystick and gamecontroller APIs below read.

namespace {
struct VexaJoyImpl {
  Uint32 Magic;
};
struct VexaGCImpl {
  VexaJoyImpl* Joy;
};
VexaJoyImpl VexaJoyInst {0x56455841u};
VexaGCImpl VexaGCInst {&VexaJoyInst};
bool VexaPadAnnounced = false;
constexpr const char* VexaPadName = "VEXA Virtual Gamepad";
constexpr const char* VexaPadPath = "vexa://virtual-pad";

bool VexaIsJoy(SDL_Joystick* joy) {
  return joy == reinterpret_cast<SDL_Joystick*>(&VexaJoyInst);
}
bool VexaIsGC(SDL_GameController* gc) {
  return gc == reinterpret_cast<SDL_GameController*>(&VexaGCInst);
}
SDL_JoystickGUID VexaPadGuid() {
  SDL_JoystickGUID guid {};
  guid.data[0] = 0x03;
  std::memcpy(guid.data + 1, "VEXAVIRTUAPAD01", 15);
  return guid;
}
void VexaAnnouncePad() {
  if (VexaPadAnnounced) {
    return;
  }
  VexaPadAnnounced = true;
  SDL_Event event {};
  event.type = SDL_JOYDEVICEADDED;
  event.jdevice.which = 0;
  event.jdevice.timestamp = GetTicksMS();
  QueueSyntheticEvent(event);
  event = SDL_Event {};
  event.type = SDL_CONTROLLERDEVICEADDED;
  event.cdevice.which = 0;
  event.cdevice.timestamp = GetTicksMS();
  QueueSyntheticEvent(event);
}
} // namespace

extern "C" void SDL_LockJoysticks(void) { }
extern "C" void SDL_UnlockJoysticks(void) { }
extern "C" int SDL_NumJoysticks(void) {  return 1; }
extern "C" const char* SDL_JoystickNameForIndex(int device_index) {  return device_index == 0 ? VexaPadName : nullptr; }
extern "C" const char* SDL_JoystickPathForIndex(int device_index) {  return device_index == 0 ? VexaPadPath : nullptr; }
extern "C" int SDL_JoystickGetDevicePlayerIndex(int device_index) {  return device_index == 0 ? 0 : -1; }
extern "C" SDL_JoystickGUID SDL_JoystickGetDeviceGUID(int device_index) {
  SDL_JoystickGUID guid {};
  return device_index == 0 ? VexaPadGuid() : guid;
}
extern "C" Uint16 SDL_JoystickGetDeviceVendor(int device_index) {  return 0; }
extern "C" Uint16 SDL_JoystickGetDeviceProduct(int device_index) {  return 0; }
extern "C" Uint16 SDL_JoystickGetDeviceProductVersion(int device_index) {  return 0; }
extern "C" SDL_JoystickType SDL_JoystickGetDeviceType(int device_index) {
  return device_index == 0 ? SDL_JOYSTICK_TYPE_GAMECONTROLLER : SDL_JOYSTICK_TYPE_UNKNOWN;
}
extern "C" SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int device_index) {  return device_index == 0 ? 0 : -1; }
extern "C" SDL_Joystick* SDL_JoystickOpen(int device_index) {
  if (device_index != 0) {
    return nullptr;
  }
  VexaAnnouncePad();
  return reinterpret_cast<SDL_Joystick*>(&VexaJoyInst);
}
extern "C" SDL_Joystick* SDL_JoystickFromInstanceID(SDL_JoystickID instance_id) {
  return instance_id == 0 ? reinterpret_cast<SDL_Joystick*>(&VexaJoyInst) : nullptr;
}
extern "C" SDL_Joystick* SDL_JoystickFromPlayerIndex(int player_index) {
  return player_index == 0 ? reinterpret_cast<SDL_Joystick*>(&VexaJoyInst) : nullptr;
}
extern "C" int SDL_JoystickAttachVirtual(SDL_JoystickType type, int naxes, int nbuttons, int nhats) {  return -1; }
extern "C" int SDL_JoystickAttachVirtualEx(const SDL_VirtualJoystickDesc *desc) {  return -1; }
extern "C" int SDL_JoystickDetachVirtual(int device_index) {  return -1; }
extern "C" SDL_bool SDL_JoystickIsVirtual(int device_index) {  return SDL_FALSE; }
extern "C" int SDL_JoystickSetVirtualAxis(SDL_Joystick *joystick, int axis, Sint16 value) {  return -1; }
extern "C" int SDL_JoystickSetVirtualButton(SDL_Joystick *joystick, int button, Uint8 value) {  return -1; }
extern "C" int SDL_JoystickSetVirtualHat(SDL_Joystick *joystick, int hat, Uint8 value) {  return -1; }
extern "C" const char* SDL_JoystickName(SDL_Joystick *joystick) {  return VexaIsJoy(joystick) ? VexaPadName : nullptr; }
extern "C" const char* SDL_JoystickPath(SDL_Joystick *joystick) {  return VexaIsJoy(joystick) ? VexaPadPath : nullptr; }
extern "C" int SDL_JoystickGetPlayerIndex(SDL_Joystick *joystick) {  return VexaIsJoy(joystick) ? 0 : -1; }
extern "C" void SDL_JoystickSetPlayerIndex(SDL_Joystick *joystick, int player_index) { }
extern "C" SDL_JoystickGUID SDL_JoystickGetGUID(SDL_Joystick *joystick) {
  SDL_JoystickGUID guid {};
  return VexaIsJoy(joystick) ? VexaPadGuid() : guid;
}
extern "C" Uint16 SDL_JoystickGetVendor(SDL_Joystick *joystick) {  return 0; }
extern "C" Uint16 SDL_JoystickGetProduct(SDL_Joystick *joystick) {  return 0; }
extern "C" Uint16 SDL_JoystickGetProductVersion(SDL_Joystick *joystick) {  return 0; }
extern "C" Uint16 SDL_JoystickGetFirmwareVersion(SDL_Joystick *joystick) {  return 0; }
extern "C" const char* SDL_JoystickGetSerial(SDL_Joystick *joystick) {  return nullptr; }
extern "C" SDL_JoystickType SDL_JoystickGetType(SDL_Joystick *joystick) {
  return VexaIsJoy(joystick) ? SDL_JOYSTICK_TYPE_GAMECONTROLLER : SDL_JOYSTICK_TYPE_UNKNOWN;
}

extern "C" void SDL_JoystickGetGUIDString(SDL_JoystickGUID guid, char *pszGUID, int cbGUID) {

  static const char kHex[] = "0123456789abcdef";
  if (!pszGUID || cbGUID <= 0) {
    return;
  }
  int out = 0;
  for (int i = 0; i < 16 && out + 2 < cbGUID; ++i) {
    pszGUID[out++] = kHex[guid.data[i] >> 4];
    pszGUID[out++] = kHex[guid.data[i] & 0xF];
  }
  pszGUID[out] = '\0';
}

extern "C" SDL_JoystickGUID SDL_JoystickGetGUIDFromString(const char *pchGUID) {

  SDL_JoystickGUID guid {};
  if (!pchGUID) {
    return guid;
  }
  for (int i = 0; i < 16; ++i) {
    const int hi = FNAStubHexNibble(pchGUID[i * 2]);
    const int lo = FNAStubHexNibble(pchGUID[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      break;
    }
    guid.data[i] = static_cast<Uint8>((hi << 4) | lo);
  }
  return guid;
}

extern "C" void SDL_GetJoystickGUIDInfo(SDL_JoystickGUID guid, Uint16 *vendor, Uint16 *product, Uint16 *version, Uint16 *crc16) {

  if (vendor) {
    *vendor = 0;
  }
  if (product) {
    *product = 0;
  }
  if (version) {
    *version = 0;
  }
  if (crc16) {
    *crc16 = 0;
  }
}

extern "C" SDL_bool SDL_JoystickGetAttached(SDL_Joystick *joystick) {  return VexaIsJoy(joystick) ? SDL_TRUE : SDL_FALSE; }
extern "C" SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *joystick) {  return VexaIsJoy(joystick) ? 0 : -1; }
extern "C" int SDL_JoystickNumAxes(SDL_Joystick *joystick) {  return VexaIsJoy(joystick) ? SDL_CONTROLLER_AXIS_MAX : 0; }
extern "C" int SDL_JoystickNumBalls(SDL_Joystick *joystick) {  return 0; }
extern "C" int SDL_JoystickNumHats(SDL_Joystick *joystick) {  return VexaIsJoy(joystick) ? 1 : 0; }
extern "C" int SDL_JoystickNumButtons(SDL_Joystick *joystick) {  return VexaIsJoy(joystick) ? SDL_CONTROLLER_BUTTON_MAX : 0; }
extern "C" void SDL_JoystickUpdate(void) {  VexaSDL2PollPointerEvents(); }
extern "C" int SDL_JoystickEventState(int state) {  return FNAStubEventState(state, FNAJoystickEvents); }
extern "C" Sint16 SDL_JoystickGetAxis(SDL_Joystick *joystick, int axis) {
  VexaSDL2PollPointerEvents();
  if (!VexaIsJoy(joystick) || axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX) {
    return 0;
  }
  return CompatPadAxes[axis];
}
extern "C" SDL_bool SDL_JoystickGetAxisInitialState(SDL_Joystick *joystick, int axis, Sint16 *state) {

  if (state) {
    *state = 0;
  }
  return VexaIsJoy(joystick) && axis >= 0 && axis < SDL_CONTROLLER_AXIS_MAX ? SDL_TRUE : SDL_FALSE;
}
extern "C" Uint8 SDL_JoystickGetHat(SDL_Joystick *joystick, int hat) {
  VexaSDL2PollPointerEvents();
  if (!VexaIsJoy(joystick) || hat != 0) {
    return SDL_HAT_CENTERED;
  }
  Uint8 value = SDL_HAT_CENTERED;
  if (CompatPadButtons[SDL_CONTROLLER_BUTTON_DPAD_UP]) {
    value |= SDL_HAT_UP;
  }
  if (CompatPadButtons[SDL_CONTROLLER_BUTTON_DPAD_DOWN]) {
    value |= SDL_HAT_DOWN;
  }
  if (CompatPadButtons[SDL_CONTROLLER_BUTTON_DPAD_LEFT]) {
    value |= SDL_HAT_LEFT;
  }
  if (CompatPadButtons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT]) {
    value |= SDL_HAT_RIGHT;
  }
  return value;
}
extern "C" int SDL_JoystickGetBall(SDL_Joystick *joystick, int ball, int *dx, int *dy) {

  if (dx) {
    *dx = 0;
  }
  if (dy) {
    *dy = 0;
  }
  return -1;
}
extern "C" Uint8 SDL_JoystickGetButton(SDL_Joystick *joystick, int button) {
  VexaSDL2PollPointerEvents();
  if (!VexaIsJoy(joystick) || button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX) {
    return 0;
  }
  return CompatPadButtons[button];
}
extern "C" int SDL_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble, Uint32 duration_ms) {  return -1; }
extern "C" int SDL_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble, Uint32 duration_ms) {  return -1; }
extern "C" SDL_bool SDL_JoystickHasLED(SDL_Joystick *joystick) {  return SDL_FALSE; }
extern "C" SDL_bool SDL_JoystickHasRumble(SDL_Joystick *joystick) {  return SDL_FALSE; }
extern "C" SDL_bool SDL_JoystickHasRumbleTriggers(SDL_Joystick *joystick) {  return SDL_FALSE; }
extern "C" int SDL_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue) {  return -1; }
extern "C" int SDL_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size) {  return -1; }
extern "C" void SDL_JoystickClose(SDL_Joystick *joystick) { }
extern "C" SDL_JoystickPowerLevel SDL_JoystickCurrentPowerLevel(SDL_Joystick *joystick) {  return SDL_JOYSTICK_POWER_UNKNOWN; }

// ---- SDL_gamecontroller.h ---------------------------------------------------
// The same virtual pad behind a gamecontroller face. Every query accepts the
// VexaGCInst handle; state comes from CompatPadButtons/CompatPadAxes.
// Note: SDL_GameControllerAddMappingsFromFile is a macro alias for
// SDL_GameControllerAddMappingsFromRW, so it needs no symbol of its own.

namespace {
constexpr const char* VexaPadMappingString =
    "03564558415649525455415041443031,VEXA Virtual Gamepad,"
    "a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,"
    "leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,"
    "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
    "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,"
    "platform:Android,";

constexpr const char* PadAxisNames[] = {
    "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger",
};
constexpr const char* PadButtonNames[] = {
    "a",           "b",        "x",        "y",          "back",
    "guide",       "start",    "leftstick","rightstick", "leftshoulder",
    "rightshoulder","dpup",    "dpdown",   "dpleft",     "dpright",
    "misc1",       "paddle1",  "paddle2",  "paddle3",    "paddle4",
    "touchpad",
};
} // namespace

extern "C" int SDL_GameControllerAddMappingsFromRW(SDL_RWops *rw, int freerw) {

  // freerw semantics: close the stream even though the read "failed". Call the
  // close callback directly so this file has no dependency on SDL_RWclose.
  if (rw && freerw) {
    rw->close(rw);
  }
  return -1;
}
extern "C" int SDL_GameControllerAddMapping(const char* mappingString) {  return 1; }
extern "C" int SDL_GameControllerNumMappings(void) {  return 1; }
extern "C" char* SDL_GameControllerMappingForIndex(int mapping_index) {
  return mapping_index == 0 ? FNAStubDup(VexaPadMappingString) : nullptr;
}
extern "C" char* SDL_GameControllerMappingForGUID(SDL_JoystickGUID guid) {
  return FNAStubDup(VexaPadMappingString);
}
extern "C" char* SDL_GameControllerMapping(SDL_GameController *gamecontroller) {
  return VexaIsGC(gamecontroller) ? FNAStubDup(VexaPadMappingString) : nullptr;
}
extern "C" SDL_bool SDL_IsGameController(int joystick_index) {  return joystick_index == 0 ? SDL_TRUE : SDL_FALSE; }
extern "C" const char* SDL_GameControllerNameForIndex(int joystick_index) {  return joystick_index == 0 ? VexaPadName : nullptr; }
extern "C" const char* SDL_GameControllerPathForIndex(int joystick_index) {  return joystick_index == 0 ? VexaPadPath : nullptr; }
extern "C" SDL_GameControllerType SDL_GameControllerTypeForIndex(int joystick_index) {
  return joystick_index == 0 ? SDL_CONTROLLER_TYPE_XBOX360 : SDL_CONTROLLER_TYPE_UNKNOWN;
}
extern "C" char* SDL_GameControllerMappingForDeviceIndex(int joystick_index) {
  return joystick_index == 0 ? FNAStubDup(VexaPadMappingString) : nullptr;
}
extern "C" SDL_GameController* SDL_GameControllerOpen(int joystick_index) {
  if (joystick_index != 0) {
    return nullptr;
  }
  VexaAnnouncePad();
  return reinterpret_cast<SDL_GameController*>(&VexaGCInst);
}
extern "C" SDL_GameController* SDL_GameControllerFromInstanceID(SDL_JoystickID joyid) {
  return joyid == 0 ? reinterpret_cast<SDL_GameController*>(&VexaGCInst) : nullptr;
}
extern "C" SDL_GameController* SDL_GameControllerFromPlayerIndex(int player_index) {
  return player_index == 0 ? reinterpret_cast<SDL_GameController*>(&VexaGCInst) : nullptr;
}
extern "C" const char* SDL_GameControllerName(SDL_GameController *gamecontroller) {  return VexaIsGC(gamecontroller) ? VexaPadName : nullptr; }
extern "C" const char* SDL_GameControllerPath(SDL_GameController *gamecontroller) {  return VexaIsGC(gamecontroller) ? VexaPadPath : nullptr; }
extern "C" SDL_GameControllerType SDL_GameControllerGetType(SDL_GameController *gamecontroller) {
  return VexaIsGC(gamecontroller) ? SDL_CONTROLLER_TYPE_XBOX360 : SDL_CONTROLLER_TYPE_UNKNOWN;
}
extern "C" int SDL_GameControllerGetPlayerIndex(SDL_GameController *gamecontroller) {  return VexaIsGC(gamecontroller) ? 0 : -1; }
extern "C" void SDL_GameControllerSetPlayerIndex(SDL_GameController *gamecontroller, int player_index) { }
extern "C" Uint16 SDL_GameControllerGetVendor(SDL_GameController *gamecontroller) {  return 0; }
extern "C" Uint16 SDL_GameControllerGetProduct(SDL_GameController *gamecontroller) {  return 0; }
extern "C" Uint16 SDL_GameControllerGetProductVersion(SDL_GameController *gamecontroller) {  return 0; }
extern "C" Uint16 SDL_GameControllerGetFirmwareVersion(SDL_GameController *gamecontroller) {  return 0; }
extern "C" const char* SDL_GameControllerGetSerial(SDL_GameController *gamecontroller) {  return nullptr; }
extern "C" Uint64 SDL_GameControllerGetSteamHandle(SDL_GameController *gamecontroller) {  return 0; }
extern "C" SDL_bool SDL_GameControllerGetAttached(SDL_GameController *gamecontroller) {  return VexaIsGC(gamecontroller) ? SDL_TRUE : SDL_FALSE; }
extern "C" SDL_Joystick* SDL_GameControllerGetJoystick(SDL_GameController *gamecontroller) {
  return VexaIsGC(gamecontroller) ? reinterpret_cast<SDL_Joystick*>(&VexaJoyInst) : nullptr;
}
extern "C" int SDL_GameControllerEventState(int state) {  return FNAStubEventState(state, FNAGameControllerEvents); }
extern "C" void SDL_GameControllerUpdate(void) {  VexaSDL2PollPointerEvents(); }
extern "C" SDL_GameControllerAxis SDL_GameControllerGetAxisFromString(const char *str) {
  if (!str) {
    return SDL_CONTROLLER_AXIS_INVALID;
  }
  for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; ++i) {
    if (SDL_strcasecmp(str, PadAxisNames[i]) == 0) {
      return static_cast<SDL_GameControllerAxis>(i);
    }
  }
  return SDL_CONTROLLER_AXIS_INVALID;
}
extern "C" const char* SDL_GameControllerGetStringForAxis(SDL_GameControllerAxis axis) {
  return axis >= 0 && axis < SDL_CONTROLLER_AXIS_MAX ? PadAxisNames[axis] : nullptr;
}
extern "C" SDL_GameControllerButtonBind SDL_GameControllerGetBindForAxis(SDL_GameController *gamecontroller, SDL_GameControllerAxis axis) {

  SDL_GameControllerButtonBind bind {};
  if (VexaIsGC(gamecontroller) && axis >= 0 && axis < SDL_CONTROLLER_AXIS_MAX) {
    bind.bindType = SDL_CONTROLLER_BINDTYPE_AXIS;
    bind.value.axis = axis;
  }
  return bind;
}
extern "C" SDL_bool SDL_GameControllerHasAxis(SDL_GameController *gamecontroller, SDL_GameControllerAxis axis) {
  return VexaIsGC(gamecontroller) && axis >= 0 && axis < SDL_CONTROLLER_AXIS_MAX ? SDL_TRUE : SDL_FALSE;
}
extern "C" Sint16 SDL_GameControllerGetAxis(SDL_GameController *gamecontroller, SDL_GameControllerAxis axis) {
  VexaSDL2PollPointerEvents();
  if (!VexaIsGC(gamecontroller) || axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX) {
    return 0;
  }
  return CompatPadAxes[axis];
}
extern "C" SDL_GameControllerButton SDL_GameControllerGetButtonFromString(const char *str) {
  if (!str) {
    return SDL_CONTROLLER_BUTTON_INVALID;
  }
  for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; ++i) {
    if (SDL_strcasecmp(str, PadButtonNames[i]) == 0) {
      return static_cast<SDL_GameControllerButton>(i);
    }
  }
  return SDL_CONTROLLER_BUTTON_INVALID;
}
extern "C" const char* SDL_GameControllerGetStringForButton(SDL_GameControllerButton button) {
  return button >= 0 && button < SDL_CONTROLLER_BUTTON_MAX ? PadButtonNames[button] : nullptr;
}
extern "C" SDL_GameControllerButtonBind SDL_GameControllerGetBindForButton(SDL_GameController *gamecontroller, SDL_GameControllerButton button) {

  SDL_GameControllerButtonBind bind {};
  if (VexaIsGC(gamecontroller) && button >= 0 && button < SDL_CONTROLLER_BUTTON_MAX) {
    bind.bindType = SDL_CONTROLLER_BINDTYPE_BUTTON;
    bind.value.button = button;
  }
  return bind;
}
extern "C" SDL_bool SDL_GameControllerHasButton(SDL_GameController *gamecontroller, SDL_GameControllerButton button) {
  return VexaIsGC(gamecontroller) && button >= 0 && button < SDL_CONTROLLER_BUTTON_MAX ? SDL_TRUE : SDL_FALSE;
}
extern "C" Uint8 SDL_GameControllerGetButton(SDL_GameController *gamecontroller, SDL_GameControllerButton button) {
  VexaSDL2PollPointerEvents();
  if (!VexaIsGC(gamecontroller) || button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX) {
    return 0;
  }
  return CompatPadButtons[button];
}
extern "C" int SDL_GameControllerGetNumTouchpads(SDL_GameController *gamecontroller) {  return 0; }
extern "C" int SDL_GameControllerGetNumTouchpadFingers(SDL_GameController *gamecontroller, int touchpad) {  return 0; }
extern "C" int SDL_GameControllerGetTouchpadFinger(SDL_GameController *gamecontroller, int touchpad, int finger, Uint8 *state, float *x, float *y, float *pressure) {

  if (state) {
    *state = 0;
  }
  if (x) {
    *x = 0.0f;
  }
  if (y) {
    *y = 0.0f;
  }
  if (pressure) {
    *pressure = 0.0f;
  }
  return -1;
}
extern "C" SDL_bool SDL_GameControllerHasSensor(SDL_GameController *gamecontroller, SDL_SensorType type) {  return SDL_FALSE; }
extern "C" int SDL_GameControllerSetSensorEnabled(SDL_GameController *gamecontroller, SDL_SensorType type, SDL_bool enabled) {  return -1; }
extern "C" SDL_bool SDL_GameControllerIsSensorEnabled(SDL_GameController *gamecontroller, SDL_SensorType type) {  return SDL_FALSE; }
extern "C" float SDL_GameControllerGetSensorDataRate(SDL_GameController *gamecontroller, SDL_SensorType type) {  return 0.0f; }
extern "C" int SDL_GameControllerGetSensorData(SDL_GameController *gamecontroller, SDL_SensorType type, float *data, int num_values) {

  if (data && num_values > 0) {
    std::memset(data, 0, static_cast<size_t>(num_values) * sizeof(float));
  }
  return -1;
}
extern "C" int SDL_GameControllerGetSensorDataWithTimestamp(SDL_GameController *gamecontroller, SDL_SensorType type, Uint64 *timestamp, float *data, int num_values) {

  if (timestamp) {
    *timestamp = 0;
  }
  if (data && num_values > 0) {
    std::memset(data, 0, static_cast<size_t>(num_values) * sizeof(float));
  }
  return -1;
}
extern "C" int SDL_GameControllerRumble(SDL_GameController *gamecontroller, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble, Uint32 duration_ms) {  return -1; }
extern "C" int SDL_GameControllerRumbleTriggers(SDL_GameController *gamecontroller, Uint16 left_rumble, Uint16 right_rumble, Uint32 duration_ms) {  return -1; }
extern "C" SDL_bool SDL_GameControllerHasLED(SDL_GameController *gamecontroller) {  return SDL_FALSE; }
extern "C" SDL_bool SDL_GameControllerHasRumble(SDL_GameController *gamecontroller) {  return SDL_FALSE; }
extern "C" SDL_bool SDL_GameControllerHasRumbleTriggers(SDL_GameController *gamecontroller) {  return SDL_FALSE; }
extern "C" int SDL_GameControllerSetLED(SDL_GameController *gamecontroller, Uint8 red, Uint8 green, Uint8 blue) {  return -1; }
extern "C" int SDL_GameControllerSendEffect(SDL_GameController *gamecontroller, const void *data, int size) {  return -1; }
extern "C" void SDL_GameControllerClose(SDL_GameController *gamecontroller) { }
extern "C" const char* SDL_GameControllerGetAppleSFSymbolsNameForButton(SDL_GameController *gamecontroller, SDL_GameControllerButton button) {  return nullptr; }
extern "C" const char* SDL_GameControllerGetAppleSFSymbolsNameForAxis(SDL_GameController *gamecontroller, SDL_GameControllerAxis axis) {  return nullptr; }

// ---- SDL_haptic.h -----------------------------------------------------------
// No haptic devices exist; opens fail and all effect calls report errors.

extern "C" int SDL_NumHaptics(void) {  return 0; }
extern "C" const char* SDL_HapticName(int device_index) {  return nullptr; }
extern "C" SDL_Haptic* SDL_HapticOpen(int device_index) {  return nullptr; }
extern "C" int SDL_HapticOpened(int device_index) {  return 0; }
extern "C" int SDL_HapticIndex(SDL_Haptic *haptic) {  return -1; }
extern "C" int SDL_MouseIsHaptic(void) {  return 0; }
extern "C" SDL_Haptic* SDL_HapticOpenFromMouse(void) {  return nullptr; }
extern "C" int SDL_JoystickIsHaptic(SDL_Joystick *joystick) {  return 0; }
extern "C" SDL_Haptic* SDL_HapticOpenFromJoystick(SDL_Joystick *joystick) {  return nullptr; }
extern "C" void SDL_HapticClose(SDL_Haptic *haptic) { }
extern "C" int SDL_HapticNumEffects(SDL_Haptic *haptic) {  return 0; }
extern "C" int SDL_HapticNumEffectsPlaying(SDL_Haptic *haptic) {  return 0; }
extern "C" unsigned int SDL_HapticQuery(SDL_Haptic *haptic) {  return 0; }
extern "C" int SDL_HapticNumAxes(SDL_Haptic *haptic) {  return 0; }
extern "C" int SDL_HapticEffectSupported(SDL_Haptic *haptic, SDL_HapticEffect *effect) {  return 0; }
extern "C" int SDL_HapticNewEffect(SDL_Haptic *haptic, SDL_HapticEffect *effect) {  return -1; }
extern "C" int SDL_HapticUpdateEffect(SDL_Haptic *haptic, int effect, SDL_HapticEffect *data) {  return -1; }
extern "C" int SDL_HapticRunEffect(SDL_Haptic *haptic, int effect, Uint32 iterations) {  return -1; }
extern "C" int SDL_HapticStopEffect(SDL_Haptic *haptic, int effect) {  return -1; }
extern "C" void SDL_HapticDestroyEffect(SDL_Haptic *haptic, int effect) { }
extern "C" int SDL_HapticGetEffectStatus(SDL_Haptic *haptic, int effect) {  return -1; }
extern "C" int SDL_HapticSetGain(SDL_Haptic *haptic, int gain) {  return -1; }
extern "C" int SDL_HapticSetAutocenter(SDL_Haptic *haptic, int autocenter) {  return -1; }
extern "C" int SDL_HapticPause(SDL_Haptic *haptic) {  return -1; }
extern "C" int SDL_HapticUnpause(SDL_Haptic *haptic) {  return -1; }
extern "C" int SDL_HapticStopAll(SDL_Haptic *haptic) {  return -1; }
extern "C" int SDL_HapticRumbleSupported(SDL_Haptic *haptic) {  return 0; }
extern "C" int SDL_HapticRumbleInit(SDL_Haptic *haptic) {  return -1; }
extern "C" int SDL_HapticRumblePlay(SDL_Haptic *haptic, float strength, Uint32 length) {  return -1; }
extern "C" int SDL_HapticRumbleStop(SDL_Haptic *haptic) {  return -1; }

// ---- SDL_sensor.h -----------------------------------------------------------
// No sensors exist.

extern "C" void SDL_LockSensors(void) { }
extern "C" void SDL_UnlockSensors(void) { }
extern "C" int SDL_NumSensors(void) {  return 0; }
extern "C" const char* SDL_SensorGetDeviceName(int device_index) {  return nullptr; }
extern "C" SDL_SensorType SDL_SensorGetDeviceType(int device_index) {  return SDL_SENSOR_INVALID; }
extern "C" int SDL_SensorGetDeviceNonPortableType(int device_index) {  return -1; }
extern "C" SDL_SensorID SDL_SensorGetDeviceInstanceID(int device_index) {  return -1; }
extern "C" SDL_Sensor* SDL_SensorOpen(int device_index) {  return nullptr; }
extern "C" SDL_Sensor* SDL_SensorFromInstanceID(SDL_SensorID instance_id) {  return nullptr; }
extern "C" const char* SDL_SensorGetName(SDL_Sensor *sensor) {  return nullptr; }
extern "C" SDL_SensorType SDL_SensorGetType(SDL_Sensor *sensor) {  return SDL_SENSOR_INVALID; }
extern "C" int SDL_SensorGetNonPortableType(SDL_Sensor *sensor) {  return -1; }
extern "C" SDL_SensorID SDL_SensorGetInstanceID(SDL_Sensor *sensor) {  return -1; }
extern "C" int SDL_SensorGetData(SDL_Sensor *sensor, float *data, int num_values) {

  if (data && num_values > 0) {
    std::memset(data, 0, static_cast<size_t>(num_values) * sizeof(float));
  }
  return -1;
}
extern "C" int SDL_SensorGetDataWithTimestamp(SDL_Sensor *sensor, Uint64 *timestamp, float *data, int num_values) {

  if (timestamp) {
    *timestamp = 0;
  }
  if (data && num_values > 0) {
    std::memset(data, 0, static_cast<size_t>(num_values) * sizeof(float));
  }
  return -1;
}
extern "C" void SDL_SensorClose(SDL_Sensor *sensor) { }
extern "C" void SDL_SensorUpdate(void) { }

// ---- SDL_touch.h ------------------------------------------------------------
// No touch devices exist.

extern "C" int SDL_GetNumTouchDevices(void) {  return 0; }
extern "C" SDL_TouchID SDL_GetTouchDevice(int index) {  return 0; }
extern "C" const char* SDL_GetTouchName(int index) {  return nullptr; }
extern "C" SDL_TouchDeviceType SDL_GetTouchDeviceType(SDL_TouchID touchID) {  return SDL_TOUCH_DEVICE_INVALID; }
extern "C" int SDL_GetNumTouchFingers(SDL_TouchID touchID) {  return 0; }
extern "C" SDL_Finger* SDL_GetTouchFinger(SDL_TouchID touchID, int index) {  return nullptr; }

// ---- SDL_mouse.h (leftovers) -------------------------------------------------
// Cursor creation/selection and mouse focus are owned by the guest core; this
// covers only the state queries and warps SDL2-CS may poke at startup.

extern "C" Uint32 SDL_GetMouseState(int *x, int *y) {

  if (x) {
    *x = CompatMouseX.load(std::memory_order_relaxed);
  }
  if (y) {
    *y = CompatMouseY.load(std::memory_order_relaxed);
  }
  return CompatMouseButtons.load(std::memory_order_relaxed);
}
extern "C" Uint32 SDL_GetGlobalMouseState(int *x, int *y) {

  return SDL_GetMouseState(x, y);
}
extern "C" Uint32 SDL_GetRelativeMouseState(int *x, int *y) {

  if (x) {
    *x = CompatMouseRelX.exchange(0, std::memory_order_relaxed);
  }
  if (y) {
    *y = CompatMouseRelY.exchange(0, std::memory_order_relaxed);
  }
  return CompatMouseButtons.load(std::memory_order_relaxed);
}
extern "C" void SDL_WarpMouseInWindow(SDL_Window *window, int x, int y) { }
extern "C" int SDL_WarpMouseGlobal(int x, int y) {  return -1; }
extern "C" int SDL_SetRelativeMouseMode(SDL_bool enabled) {  return 0; }
extern "C" SDL_bool SDL_GetRelativeMouseMode(void) {  return SDL_FALSE; }
extern "C" int SDL_CaptureMouse(SDL_bool enabled) {  return 0; }

// ---- SDL_keyboard.h (leftovers) ----------------------------------------------
// The IME/text-input entry points (SDL_StartTextInput, SDL_StopTextInput,
// SDL_IsTextInputActive, SDL_SetTextInputRect) are owned by the guest core.

extern "C" SDL_Window* SDL_GetKeyboardFocus(void) {  return nullptr; }
extern "C" const Uint8* SDL_GetKeyboardState(int *numkeys) {

  if (numkeys) {
    *numkeys = SDL_NUM_SCANCODES;
  }
  return CompatKeyStates;
}
extern "C" void SDL_ResetKeyboard(void) {
  std::memset(CompatKeyStates, 0, sizeof(CompatKeyStates));
}
extern "C" SDL_Keymod SDL_GetModState(void) {
  return static_cast<SDL_Keymod>(CurrentModState.load(std::memory_order_acquire));
}
extern "C" void SDL_SetModState(SDL_Keymod modstate) {
  CurrentModState.store(static_cast<Uint16>(modstate), std::memory_order_release);
}
extern "C" SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode scancode) {
  for (const auto& entry : KeyLookupTable) {
    if (entry.Scancode == scancode) {
      return entry.Keycode;
    }
  }
  if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
    return static_cast<SDL_Keycode>(SDLK_a + (scancode - SDL_SCANCODE_A));
  }
  if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
    return static_cast<SDL_Keycode>(SDLK_1 + (scancode - SDL_SCANCODE_1));
  }
  if (scancode == SDL_SCANCODE_0) {
    return SDLK_0;
  }
  return SDLK_UNKNOWN;
}
extern "C" SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key) {  return CompatScancodeFromKeycode(key); }
extern "C" const char* SDL_GetScancodeName(SDL_Scancode scancode) {  return ""; }
extern "C" SDL_Scancode SDL_GetScancodeFromName(const char *name) {  return SDL_SCANCODE_UNKNOWN; }
extern "C" const char* SDL_GetKeyName(SDL_Keycode key) {  return ""; }
extern "C" SDL_Keycode SDL_GetKeyFromName(const char *name) {  return SDLK_UNKNOWN; }
extern "C" void SDL_ClearComposition(void) { }
extern "C" SDL_bool SDL_IsTextInputShown(void) {  return SDL_FALSE; }
extern "C" SDL_bool SDL_HasScreenKeyboardSupport(void) {  return SDL_FALSE; }
extern "C" SDL_bool SDL_IsScreenKeyboardShown(SDL_Window *window) {  return SDL_FALSE; }

// ---- SDL_clipboard.h ----------------------------------------------------------
// The clipboard is empty; setters "succeed" so callers move on.

extern "C" int SDL_SetClipboardText(const char *text) {  return 0; }
extern "C" char* SDL_GetClipboardText(void) {  return FNAStubDup(""); }
extern "C" SDL_bool SDL_HasClipboardText(void) {  return SDL_FALSE; }
extern "C" int SDL_SetPrimarySelectionText(const char *text) {  return 0; }
extern "C" char* SDL_GetPrimarySelectionText(void) {  return FNAStubDup(""); }
extern "C" SDL_bool SDL_HasPrimarySelectionText(void) {  return SDL_FALSE; }

// ---- SDL_messagebox.h ---------------------------------------------------------
// No UI to show; pretend the user dismissed the first button.

extern "C" int SDL_ShowMessageBox(const SDL_MessageBoxData *messageboxdata, int *buttonid) {

  if (buttonid) {
    *buttonid = (messageboxdata && messageboxdata->buttons && messageboxdata->numbuttons > 0)
      ? messageboxdata->buttons[0].buttonid
      : 0;
  }
  return 0;
}
extern "C" int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window) {  return 0; }

// ---- SDL_vulkan.h -------------------------------------------------------------
// Everything fails so FNA3D falls back to OpenGL.

extern "C" int SDL_Vulkan_LoadLibrary(const char *path) {  return -1; }
extern "C" void* SDL_Vulkan_GetVkGetInstanceProcAddr(void) {  return nullptr; }
extern "C" void SDL_Vulkan_UnloadLibrary(void) { }
extern "C" SDL_bool SDL_Vulkan_GetInstanceExtensions(SDL_Window *window, unsigned int *pCount, const char **pNames) {

  if (pCount) {
    *pCount = 0;
  }
  return SDL_FALSE;
}
extern "C" SDL_bool SDL_Vulkan_CreateSurface(SDL_Window *window, VkInstance instance, VkSurfaceKHR *surface) {

  if (surface) {
    *surface = static_cast<VkSurfaceKHR>(0);
  }
  return SDL_FALSE;
}
extern "C" void SDL_Vulkan_GetDrawableSize(SDL_Window *window, int *w, int *h) {

  if (w) {
    *w = 1280;
  }
  if (h) {
    *h = 720;
  }
}

// ---- SDL_power.h --------------------------------------------------------------

extern "C" SDL_PowerState SDL_GetPowerInfo(int *seconds, int *percent) {

  if (seconds) {
    *seconds = -1;
  }
  if (percent) {
    *percent = -1;
  }
  return SDL_POWERSTATE_UNKNOWN;
}

// ---- SDL_gesture.h ------------------------------------------------------------

extern "C" int SDL_RecordGesture(SDL_TouchID touchId) {  return -1; }
extern "C" int SDL_SaveAllDollarTemplates(SDL_RWops *dst) {  return -1; }
extern "C" int SDL_SaveDollarTemplate(SDL_GestureID gestureId, SDL_RWops *dst) {  return -1; }
extern "C" int SDL_LoadDollarTemplates(SDL_TouchID touchId, SDL_RWops *src) {  return -1; }

// ---- SDL_system.h -------------------------------------------------------------
// Platform probes report the plainest device; Android JNI hooks return null so
// SDL2-CS feature checks degrade instead of crashing. The Android block is only
// declared when the toolchain defines __ANDROID__, but exporting the symbols is
// harmless either way.

extern "C" SDL_bool SDL_IsTablet(void) {  return SDL_FALSE; }
extern "C" int SDL_LinuxSetThreadPriority(Sint64 threadID, int priority) {  return -1; }
extern "C" int SDL_LinuxSetThreadPriorityAndPolicy(Sint64 threadID, int sdlPriority, int schedPolicy) {  return -1; }
extern "C" void* SDL_AndroidGetJNIEnv(void) {  return nullptr; }
extern "C" void* SDL_AndroidGetActivity(void) {  return nullptr; }
extern "C" int SDL_GetAndroidSDKVersion(void) {  return 0; }
extern "C" SDL_bool SDL_IsAndroidTV(void) {  return SDL_FALSE; }
extern "C" SDL_bool SDL_IsChromebook(void) {  return SDL_FALSE; }
extern "C" SDL_bool SDL_IsDeXMode(void) {  return SDL_FALSE; }
extern "C" void SDL_AndroidBackButton(void) { }
extern "C" const char* SDL_AndroidGetInternalStoragePath(void) {  return nullptr; }
extern "C" int SDL_AndroidGetExternalStorageState(void) {  return 0; }
extern "C" const char* SDL_AndroidGetExternalStoragePath(void) {  return nullptr; }
extern "C" SDL_bool SDL_AndroidRequestPermission(const char *permission) {  return SDL_FALSE; }
extern "C" int SDL_AndroidShowToast(const char *message, int duration, int gravity, int xoffset, int yoffset) {  return -1; }
extern "C" int SDL_AndroidSendMessage(Uint32 command, int param) {  return -1; }
extern "C" void SDL_OnApplicationWillTerminate(void) { }
extern "C" void SDL_OnApplicationDidReceiveMemoryWarning(void) { }
extern "C" void SDL_OnApplicationWillResignActive(void) { }
extern "C" void SDL_OnApplicationDidEnterBackground(void) { }
extern "C" void SDL_OnApplicationWillEnterForeground(void) { }
extern "C" void SDL_OnApplicationDidBecomeActive(void) { }
extern "C" void SDL_OnApplicationDidChangeStatusBarOrientation(void) { }

// ---- SDL_main.h ---------------------------------------------------------------
// SDL2 does export SDL_SetMainReady (SDL_main.h); SDL2-CS can call it during
// init. It only matters on platforms where SDL wraps main(), so a no-op here.

extern "C" void SDL_SetMainReady(void) { }

// ---- SDL_surface.h BMP leftovers -----------------------------------------------
// BMP load/save unsupported; SDL_LoadBMP/SDL_SaveBMP macros funnel here.

extern "C" SDL_Surface* SDL_LoadBMP_RW(SDL_RWops *src, int freesrc) {

  if (src && freesrc) {
    src->close(src);
  }
  return nullptr;
}
extern "C" int SDL_SaveBMP_RW(SDL_Surface *surface, SDL_RWops *dst, int freedst) {

  if (dst && freedst) {
    dst->close(dst);
  }
  return -1;
}

// ---- SDL_guid.h ---------------------------------------------------------------
// Trivial hex formatting, real enough for round-tripping.

extern "C" void SDL_GUIDToString(SDL_GUID guid, char *pszGUID, int cbGUID) {

  static const char kHex[] = "0123456789abcdef";
  if (!pszGUID || cbGUID <= 0) {
    return;
  }
  int out = 0;
  for (int i = 0; i < 16 && out + 2 < cbGUID; ++i) {
    pszGUID[out++] = kHex[guid.data[i] >> 4];
    pszGUID[out++] = kHex[guid.data[i] & 0xF];
  }
  pszGUID[out] = '\0';
}
extern "C" SDL_GUID SDL_GUIDFromString(const char *pchGUID) {

  SDL_GUID guid {};
  if (!pchGUID) {
    return guid;
  }
  for (int i = 0; i < 16; ++i) {
    const int hi = FNAStubHexNibble(pchGUID[i * 2]);
    const int lo = FNAStubHexNibble(pchGUID[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      break;
    }
    guid.data[i] = static_cast<Uint8>((hi << 4) | lo);
  }
  return guid;
}

// ---- SDL_misc.h ---------------------------------------------------------------

extern "C" int SDL_OpenURL(const char *url) {  return -1; }

// ---- SDL_shape.h ---------------------------------------------------------------
// Shaped windows unsupported.

extern "C" SDL_Window* SDL_CreateShapedWindow(const char *title, unsigned int x, unsigned int y, unsigned int w, unsigned int h, Uint32 flags) {  return nullptr; }
extern "C" SDL_bool SDL_IsShapedWindow(const SDL_Window *window) {  return SDL_FALSE; }
extern "C" int SDL_SetWindowShape(SDL_Window *window, SDL_Surface *shape, SDL_WindowShapeMode *shape_mode) {  return -1; }
extern "C" int SDL_GetShapedWindowMode(SDL_Window *window, SDL_WindowShapeMode *shape_mode) {

  if (shape_mode) {
    std::memset(shape_mode, 0, sizeof(*shape_mode));
  }
  return -1;
}

// ---- SDL_metal.h ---------------------------------------------------------------
// No Metal on this platform; callers always fail.

extern "C" SDL_MetalView SDL_Metal_CreateView(SDL_Window *window) {  return static_cast<SDL_MetalView>(nullptr); }
extern "C" void SDL_Metal_DestroyView(SDL_MetalView view) { }
extern "C" void* SDL_Metal_GetLayer(SDL_MetalView view) {  return nullptr; }
extern "C" void SDL_Metal_GetDrawableSize(SDL_Window *window, int *w, int *h) {

  if (w) {
    *w = 1280;
  }
  if (h) {
    *h = 720;
  }
}
