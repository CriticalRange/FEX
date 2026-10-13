#define AL_DISABLE_NOEXCEPT
#include <AL/al.h>
#include <AL/alc.h>
#include <cstring>

#include "common/Guest.h"
#include "thunkgen_guest_libopenal.inl"
LOAD_LIB(libopenal)

// Extension lookup must return x86 guest code, never a raw ARM host address.
// Use generated entrypoints so ALC calls still apply host handle conversion.
static void* GuestOpenALProc(const char* name) {
  if (!name) return nullptr;
  if (!std::strcmp(name, "alGetProcAddress")) return reinterpret_cast<void*>(alGetProcAddress);
  if (!std::strcmp(name, "alcGetProcAddress")) return reinterpret_cast<void*>(alcGetProcAddress);
#define MATCH_PROC(symbol, unused) \
  if (!std::strcmp(name, #symbol)) return reinterpret_cast<void*>(fexfn_pack_##symbol);
  FOREACH_SYMBOL(MATCH_PROC)
#undef MATCH_PROC
  return nullptr;
}

extern "C" void* alGetProcAddress(const ALchar* name) {
  if (!name || !fexfn_pack_alGetProcAddress(name)) return nullptr;
  return GuestOpenALProc(name);
}

extern "C" void* alcGetProcAddress(ALCdevice* device, const ALCchar* name) {
  if (!name || !fexfn_pack_alcGetProcAddress(device, name)) return nullptr;
  return GuestOpenALProc(name);
}
