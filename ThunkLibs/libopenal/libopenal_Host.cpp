#define AL_DISABLE_NOEXCEPT
#include <AL/al.h>
#include <AL/alc.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <unordered_map>

#include "common/Host.h"

#if defined(__ANDROID__)
#include <stdlib.h>
__attribute__((constructor)) static void vexa_openal_drivers(void) {
  /* AerialX picks the first backend that initializes. Force OpenSL and leave
     the real-time priority at its default of 0. */
  setenv("ALSOFT_DRIVERS", "opensl", 1);
  setenv("ALSOFT_LOGLEVEL", "2", 0);
}
#endif

static std::mutex g_alc_mutex;
static uint32_t g_next_alc_handle = 1;
static std::unordered_map<uint32_t, ALCdevice*> g_id_to_dev;
static std::unordered_map<ALCdevice*, uint32_t> g_dev_to_id;
static std::unordered_map<uint32_t, ALCcontext*> g_id_to_ctx;
static std::unordered_map<ALCcontext*, uint32_t> g_ctx_to_id;

static char* g_low_str_pool = nullptr;
static size_t g_low_str_offset = 0;
static constexpr size_t kLowStrPoolSize = 65536;
static std::unordered_map<const char*, const char*> g_host_to_low_str;

static const char* VexaLowString(const char* s, bool double_null = false) {
  if (!s) return nullptr;
  uintptr_t addr = reinterpret_cast<uintptr_t>(s);
  if (addr < 0x100000000ULL) {
    return s;
  }
  std::lock_guard<std::mutex> lock(g_alc_mutex);
  auto it = g_host_to_low_str.find(s);
  if (it != g_host_to_low_str.end()) {
    return it->second;
  }
  if (!g_low_str_pool) {
    for (uintptr_t hint = 0x50000000ULL; hint < 0xef000000ULL; hint += 0x04000000ULL) {
      void* p = mmap(reinterpret_cast<void*>(hint), kLowStrPoolSize,
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (p != MAP_FAILED) {
        if (reinterpret_cast<uintptr_t>(p) + kLowStrPoolSize <= 0x100000000ULL) {
          g_low_str_pool = static_cast<char*>(p);
          break;
        }
        munmap(p, kLowStrPoolSize);
      }
    }
  }
  if (!g_low_str_pool) {
    return s;
  }
  size_t len = 0;
  if (double_null) {
    while (s[len] != '\0' || s[len + 1] != '\0') {
      len++;
    }
    len += 2;
  } else {
    len = strlen(s) + 1;
  }
  if (g_low_str_offset + len > kLowStrPoolSize) {
    g_low_str_offset = 0;
  }
  char* dst = g_low_str_pool + g_low_str_offset;
  memcpy(dst, s, len);
  g_low_str_offset += (len + 7) & ~size_t(7);
  g_host_to_low_str[s] = dst;
  return dst;
}

static ALCdevice* VexaWrapDev(ALCdevice* real_dev) {
  if (!real_dev) return nullptr;
  std::lock_guard<std::mutex> lock(g_alc_mutex);
  auto it = g_dev_to_id.find(real_dev);
  if (it != g_dev_to_id.end()) {
    return reinterpret_cast<ALCdevice*>(static_cast<uintptr_t>(it->second));
  }
  uint32_t id = g_next_alc_handle++;
  if (id == 0) id = g_next_alc_handle++;
  g_dev_to_id[real_dev] = id;
  g_id_to_dev[id] = real_dev;
  return reinterpret_cast<ALCdevice*>(static_cast<uintptr_t>(id));
}

static ALCdevice* VexaUnwrapDev(ALCdevice* guest_dev) {
  if (!guest_dev) return nullptr;
  uint32_t id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(guest_dev));
  std::lock_guard<std::mutex> lock(g_alc_mutex);
  auto it = g_id_to_dev.find(id);
  if (it != g_id_to_dev.end()) {
    return it->second;
  }
  return guest_dev;
}

static ALCcontext* VexaWrapCtx(ALCcontext* real_ctx) {
  if (!real_ctx) return nullptr;
  std::lock_guard<std::mutex> lock(g_alc_mutex);
  auto it = g_ctx_to_id.find(real_ctx);
  if (it != g_ctx_to_id.end()) {
    return reinterpret_cast<ALCcontext*>(static_cast<uintptr_t>(it->second));
  }
  uint32_t id = g_next_alc_handle++;
  if (id == 0) id = g_next_alc_handle++;
  g_ctx_to_id[real_ctx] = id;
  g_id_to_ctx[id] = real_ctx;
  return reinterpret_cast<ALCcontext*>(static_cast<uintptr_t>(id));
}

static ALCcontext* VexaUnwrapCtx(ALCcontext* guest_ctx) {
  if (!guest_ctx) return nullptr;
  uint32_t id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(guest_ctx));
  std::lock_guard<std::mutex> lock(g_alc_mutex);
  auto it = g_id_to_ctx.find(id);
  if (it != g_id_to_ctx.end()) {
    return it->second;
  }
  return guest_ctx;
}

extern "C" {
static ALCdevice* fexfn_impl_libopenal_alcOpenDevice(const ALCchar* devicename);
static ALCboolean fexfn_impl_libopenal_alcCloseDevice(ALCdevice* device);
static ALCcontext* fexfn_impl_libopenal_alcCreateContext(ALCdevice* device, const ALCint* attrlist);
static ALCboolean fexfn_impl_libopenal_alcMakeContextCurrent(ALCcontext* context);
static void fexfn_impl_libopenal_alcProcessContext(ALCcontext* context);
static void fexfn_impl_libopenal_alcSuspendContext(ALCcontext* context);
static void fexfn_impl_libopenal_alcDestroyContext(ALCcontext* context);
static ALCcontext* fexfn_impl_libopenal_alcGetCurrentContext();
static ALCdevice* fexfn_impl_libopenal_alcGetContextsDevice(ALCcontext* context);
static ALCenum fexfn_impl_libopenal_alcGetError(ALCdevice* device);
static ALCboolean fexfn_impl_libopenal_alcIsExtensionPresent(ALCdevice* device, const ALCchar* extname);
static void* fexfn_impl_libopenal_alcGetProcAddress(ALCdevice* device, const ALCchar* funcname);
static ALCenum fexfn_impl_libopenal_alcGetEnumValue(ALCdevice* device, const ALCchar* enumname);
static const ALCchar* fexfn_impl_libopenal_alcGetString(ALCdevice* device, ALCenum param);
static void fexfn_impl_libopenal_alcGetIntegerv(ALCdevice* device, ALCenum param, ALCsizei size, ALCint* values);
static ALCdevice* fexfn_impl_libopenal_alcCaptureOpenDevice(const ALCchar* devicename, ALCuint frequency, ALCenum format, ALCsizei buffersize);
static ALCboolean fexfn_impl_libopenal_alcCaptureCloseDevice(ALCdevice* device);
static void fexfn_impl_libopenal_alcCaptureStart(ALCdevice* device);
static void fexfn_impl_libopenal_alcCaptureStop(ALCdevice* device);
static void fexfn_impl_libopenal_alcCaptureSamples(ALCdevice* device, ALCvoid* buffer, ALCsizei samples);
static const ALchar* fexfn_impl_libopenal_alGetString(ALenum param);
}

#include "thunkgen_host_libopenal.inl"

static ALCdevice* fexfn_impl_libopenal_alcOpenDevice(const ALCchar* devicename) {
  return VexaWrapDev(fexldr_ptr_libopenal_alcOpenDevice(devicename));
}

static ALCboolean fexfn_impl_libopenal_alcCloseDevice(ALCdevice* device) {
  ALCdevice* real_dev = VexaUnwrapDev(device);
  return fexldr_ptr_libopenal_alcCloseDevice(real_dev);
}

static ALCcontext* fexfn_impl_libopenal_alcCreateContext(ALCdevice* device, const ALCint* attrlist) {
  return VexaWrapCtx(fexldr_ptr_libopenal_alcCreateContext(VexaUnwrapDev(device), attrlist));
}

static ALCboolean fexfn_impl_libopenal_alcMakeContextCurrent(ALCcontext* context) {
  return fexldr_ptr_libopenal_alcMakeContextCurrent(VexaUnwrapCtx(context));
}

static void fexfn_impl_libopenal_alcProcessContext(ALCcontext* context) {
  fexldr_ptr_libopenal_alcProcessContext(VexaUnwrapCtx(context));
}

static void fexfn_impl_libopenal_alcSuspendContext(ALCcontext* context) {
  fexldr_ptr_libopenal_alcSuspendContext(VexaUnwrapCtx(context));
}

static void fexfn_impl_libopenal_alcDestroyContext(ALCcontext* context) {
  fexldr_ptr_libopenal_alcDestroyContext(VexaUnwrapCtx(context));
}

static ALCcontext* fexfn_impl_libopenal_alcGetCurrentContext() {
  return VexaWrapCtx(fexldr_ptr_libopenal_alcGetCurrentContext());
}

static ALCdevice* fexfn_impl_libopenal_alcGetContextsDevice(ALCcontext* context) {
  return VexaWrapDev(fexldr_ptr_libopenal_alcGetContextsDevice(VexaUnwrapCtx(context)));
}

static ALCenum fexfn_impl_libopenal_alcGetError(ALCdevice* device) {
  return fexldr_ptr_libopenal_alcGetError(VexaUnwrapDev(device));
}

static ALCboolean fexfn_impl_libopenal_alcIsExtensionPresent(ALCdevice* device, const ALCchar* extname) {
  return fexldr_ptr_libopenal_alcIsExtensionPresent(VexaUnwrapDev(device), extname);
}

static void* fexfn_impl_libopenal_alcGetProcAddress(ALCdevice* device, const ALCchar* funcname) {
  return fexldr_ptr_libopenal_alcGetProcAddress(VexaUnwrapDev(device), funcname);
}

static ALCenum fexfn_impl_libopenal_alcGetEnumValue(ALCdevice* device, const ALCchar* enumname) {
  return fexldr_ptr_libopenal_alcGetEnumValue(VexaUnwrapDev(device), enumname);
}

static const ALCchar* fexfn_impl_libopenal_alcGetString(ALCdevice* device, ALCenum param) {
  bool double_null = (param == ALC_ALL_DEVICES_SPECIFIER || param == ALC_DEVICE_SPECIFIER || param == ALC_CAPTURE_DEVICE_SPECIFIER);
  return VexaLowString(fexldr_ptr_libopenal_alcGetString(VexaUnwrapDev(device), param), double_null);
}

static void fexfn_impl_libopenal_alcGetIntegerv(ALCdevice* device, ALCenum param, ALCsizei size, ALCint* values) {
  fexldr_ptr_libopenal_alcGetIntegerv(VexaUnwrapDev(device), param, size, values);
}

static ALCdevice* fexfn_impl_libopenal_alcCaptureOpenDevice(const ALCchar* devicename, ALCuint frequency, ALCenum format, ALCsizei buffersize) {
  return VexaWrapDev(fexldr_ptr_libopenal_alcCaptureOpenDevice(devicename, frequency, format, buffersize));
}

static ALCboolean fexfn_impl_libopenal_alcCaptureCloseDevice(ALCdevice* device) {
  return fexldr_ptr_libopenal_alcCaptureCloseDevice(VexaUnwrapDev(device));
}

static void fexfn_impl_libopenal_alcCaptureStart(ALCdevice* device) {
  fexldr_ptr_libopenal_alcCaptureStart(VexaUnwrapDev(device));
}

static void fexfn_impl_libopenal_alcCaptureStop(ALCdevice* device) {
  fexldr_ptr_libopenal_alcCaptureStop(VexaUnwrapDev(device));
}

static void fexfn_impl_libopenal_alcCaptureSamples(ALCdevice* device, ALCvoid* buffer, ALCsizei samples) {
  fexldr_ptr_libopenal_alcCaptureSamples(VexaUnwrapDev(device), buffer, samples);
}

static const ALchar* fexfn_impl_libopenal_alGetString(ALenum param) {
  return VexaLowString(fexldr_ptr_libopenal_alGetString(param), false);
}

EXPORTS(libopenal)