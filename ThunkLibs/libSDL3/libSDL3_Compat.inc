// VEXA_FIXES: Guest-local SDL3 services (audio, threads, mutex/semaphore, IO, hints,
// libc-style helpers) needed by SDL3 consumers such as FAudio and FNA3D. These run
// entirely inside the guest; no host round trip is required.
// Included from libSDL3_Guest.cpp, before LOAD_LIB.

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_touch.h>

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <strings.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" {
void* iconv_open(const char* tocode, const char* fromcode);
size_t iconv(void* cd, char** inbuf, size_t* inbytesleft, char** outbuf, size_t* outbytesleft);
int iconv_close(void* cd);
}

#include <map>
#include <stdarg.h>

// SDL_stdinc.h maps many SDL_* helpers onto libc names via macros; drop them so the
// definitions below export real SDL_* symbols instead of redefining libc.
#undef SDL_BlitSurfaceScaled
#undef SDL_CloseAudioDevice
#undef SDL_CloseIO
#undef SDL_DestroyAudioStream
#undef SDL_DestroyMutex
#undef SDL_DestroySemaphore
#undef SDL_GetAudioDeviceFormat
#undef SDL_GetAudioDeviceName
#undef SDL_GetAudioDriver
#undef SDL_GetAudioPlaybackDevices
#undef SDL_GetAudioStreamDevice
#undef SDL_GetCurrentThreadID
#undef SDL_GetCurrentVideoDriver
#undef SDL_GetHint
#undef SDL_GetHintBoolean
#undef SDL_GetIOProperties
#undef SDL_GetNumAudioDrivers
#undef SDL_HasNEON
#undef SDL_HasSSE2
#undef SDL_IOFromFile
#undef SDL_IOFromMem
#undef SDL_LockMutex
#undef SDL_OpenAudioDeviceStream
#undef SDL_PutAudioStreamData
#undef SDL_ReadIO
#undef SDL_ResumeAudioDevice
#undef SDL_SeekIO
#undef SDL_SetCurrentThreadPriority
#undef SDL_SetHint
#undef SDL_SetMainReady
#undef SDL_SetHintWithPriority
#undef SDL_SetLogPriority
#undef SDL_SetSurfaceBlendMode
#undef SDL_SignalSemaphore
#undef SDL_UnlockMutex
#undef SDL_WaitSemaphore
#undef SDL_WaitThread
#undef SDL_abs
#undef SDL_acos
#undef SDL_acosf
#undef SDL_asin
#undef SDL_atan
#undef SDL_atan2
#undef SDL_atan2f
#undef SDL_atoi
#undef SDL_ceil
#undef SDL_cos
#undef SDL_cosf
#undef SDL_exp
#undef SDL_fabsf
#undef SDL_floor
#undef SDL_free
#undef SDL_getenv
#undef SDL_iconv
#undef SDL_iconv_close
#undef SDL_iconv_open
#undef SDL_isalnum
#undef SDL_log
#undef SDL_log10
#undef SDL_malloc
#undef SDL_memcmp
#undef SDL_memset
#undef SDL_pow
#undef SDL_powf
#undef SDL_qsort
#undef SDL_realloc
#undef SDL_roundf
#undef SDL_scalbn
#undef SDL_setenv_unsafe
#undef SDL_sin
#undef SDL_sinf
#undef SDL_snprintf
#undef SDL_sqrt
#undef SDL_sqrtf
#undef SDL_sscanf
#undef SDL_strcasecmp
#undef SDL_strchr
#undef SDL_strcmp
#undef SDL_strlcat
#undef SDL_strlcpy
#undef SDL_strlen
#undef SDL_strncmp
#undef SDL_strstr
#undef SDL_tan
#undef SDL_vsnprintf


// ---- hints ----------------------------------------------------------------

namespace {
std::mutex HintsMutex;
std::map<std::string, std::string> Hints;

const char* CompatLookupHint(const char* name) {
  auto it = Hints.find(name);
  if (it != Hints.end()) {
    return it->second.c_str();
  }
  return std::getenv(name);
}
} // namespace

extern "C" const char* SDL_GetHint(const char* name) {
  if (!name) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(HintsMutex);
  return CompatLookupHint(name);
}

extern "C" bool SDL_SetHintWithPriority(const char* name, const char* value, SDL_HintPriority priority) {
  if (!name || !value) {
    return false;
  }
  std::lock_guard<std::mutex> lock(HintsMutex);
  if (priority != SDL_HINT_OVERRIDE && Hints.count(name) && priority == SDL_HINT_DEFAULT) {
    return false;
  }
  Hints[name] = value;
  return true;
}

extern "C" bool SDL_SetHint(const char* name, const char* value) {
  return SDL_SetHintWithPriority(name, value, SDL_HINT_NORMAL);
}

extern "C" bool SDL_GetHintBoolean(const char* name, bool default_value) {
  const char* value = SDL_GetHint(name);
  if (!value || !*value) {
    return default_value;
  }
  return !(value[0] == '0' || strcasecmp(value, "false") == 0);
}

extern "C" void SDL_SetLogPriority(int, SDL_LogPriority) {}

extern "C" bool SDL_HasSSE2() { return true; }
extern "C" bool SDL_HasNEON() { return false; }

extern "C" void SDL_SetMainReady() {}

extern "C" const char* SDL_GetCurrentVideoDriver() { return "android"; }

// ---- threads / mutex / semaphore -------------------------------------------

struct SDL_Mutex {
  pthread_mutex_t Handle;
};

struct SDL_Semaphore {
  sem_t Handle;
};

struct SDL_Thread {
  pthread_t Handle {};
  SDL_ThreadFunction Fn {};
  void* Data {};
  int Status {};
  std::string Name;
};

extern "C" SDL_Mutex* SDL_CreateMutex() {
  auto* mutex = new SDL_Mutex;
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex->Handle, &attr);
  pthread_mutexattr_destroy(&attr);
  return mutex;
}

extern "C" void SDL_DestroyMutex(SDL_Mutex* mutex) {
  if (mutex) {
    pthread_mutex_destroy(&mutex->Handle);
    delete mutex;
  }
}

extern "C" void SDL_LockMutex(SDL_Mutex* mutex) {
  if (mutex) {
    pthread_mutex_lock(&mutex->Handle);
  }
}

extern "C" void SDL_UnlockMutex(SDL_Mutex* mutex) {
  if (mutex) {
    pthread_mutex_unlock(&mutex->Handle);
  }
}

extern "C" SDL_Semaphore* SDL_CreateSemaphore(Uint32 initial_value) {
  auto* sem = new SDL_Semaphore;
  sem_init(&sem->Handle, 0, initial_value);
  return sem;
}

extern "C" void SDL_DestroySemaphore(SDL_Semaphore* sem) {
  if (sem) {
    sem_destroy(&sem->Handle);
    delete sem;
  }
}

extern "C" void SDL_WaitSemaphore(SDL_Semaphore* sem) {
  if (!sem) {
    return;
  }
  while (sem_wait(&sem->Handle) == -1 && errno == EINTR) {
  }
}

extern "C" void SDL_SignalSemaphore(SDL_Semaphore* sem) {
  if (sem) {
    sem_post(&sem->Handle);
  }
}

extern "C" SDL_Thread* SDL_CreateThreadRuntime(SDL_ThreadFunction fn, const char* name, void* data,
                                               SDL_FunctionPointer, SDL_FunctionPointer) {
  if (!fn) {
    SDL_SetError("Thread function is null");
    return nullptr;
  }
  auto* thread = new SDL_Thread;
  thread->Fn = fn;
  thread->Data = data;
  thread->Name = name ? name : "";
  const int rc = pthread_create(
    &thread->Handle, nullptr,
    [](void* arg) -> void* {
      auto* self = static_cast<SDL_Thread*>(arg);
      if (!self->Name.empty()) {
        pthread_setname_np(pthread_self(), self->Name.substr(0, 15).c_str());
      }
      self->Status = self->Fn(self->Data);
      return nullptr;
    },
    thread);
  if (rc != 0) {
    SDL_SetError("pthread_create failed: %d", rc);
    delete thread;
    return nullptr;
  }
  return thread;
}

extern "C" void SDL_WaitThread(SDL_Thread* thread, int* status) {
  if (!thread) {
    return;
  }
  pthread_join(thread->Handle, nullptr);
  if (status) {
    *status = thread->Status;
  }
  delete thread;
}

extern "C" SDL_ThreadID SDL_GetCurrentThreadID() {
  return static_cast<SDL_ThreadID>(syscall(SYS_gettid));
}

extern "C" bool SDL_SetCurrentThreadPriority(SDL_ThreadPriority priority) {
  int nice = 0;
  switch (priority) {
  case SDL_THREAD_PRIORITY_LOW: nice = 19; break;
  case SDL_THREAD_PRIORITY_HIGH: nice = -10; break;
  case SDL_THREAD_PRIORITY_TIME_CRITICAL: nice = -20; break;
  default: break;
  }
  // Raising priority commonly fails without privileges; that is not an error for callers.
  setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), nice);
  return true;
}

// ---- IO ------------------------------------------------------------------

struct SDL_IOStream {
  FILE* File {};
  Uint8* Mem {};
  size_t MemSize {};
  size_t MemPos {};
  SDL_PropertiesID Props {};
};

extern "C" SDL_IOStream* SDL_IOFromFile(const char* file, const char* mode) {
  if (!file || !mode) {
    SDL_SetError("Parameter 'file'/'mode' is invalid");
    return nullptr;
  }
  FILE* handle = std::fopen(file, mode);
  if (!handle) {
    SDL_SetError("Couldn't open %s: %s", file, strerror(errno));
    return nullptr;
  }
  auto* stream = new SDL_IOStream;
  stream->File = handle;
  return stream;
}

extern "C" SDL_IOStream* SDL_IOFromMem(void* mem, size_t size) {
  if (!mem) {
    SDL_SetError("Parameter 'mem' is invalid");
    return nullptr;
  }
  auto* stream = new SDL_IOStream;
  stream->Mem = static_cast<Uint8*>(mem);
  stream->MemSize = size;
  return stream;
}

extern "C" size_t SDL_ReadIO(SDL_IOStream* context, void* ptr, size_t size) {
  if (!context || !ptr) {
    return 0;
  }
  if (context->File) {
    return std::fread(ptr, 1, size, context->File);
  }
  const size_t available = context->MemSize - context->MemPos;
  const size_t count = size < available ? size : available;
  std::memcpy(ptr, context->Mem + context->MemPos, count);
  context->MemPos += count;
  return count;
}

extern "C" Sint64 SDL_SeekIO(SDL_IOStream* context, Sint64 offset, SDL_IOWhence whence) {
  if (!context) {
    return -1;
  }
  if (context->File) {
    const int origin = whence == SDL_IO_SEEK_SET ? SEEK_SET : whence == SDL_IO_SEEK_CUR ? SEEK_CUR : SEEK_END;
    if (fseeko(context->File, static_cast<off_t>(offset), origin) != 0) {
      return -1;
    }
    return static_cast<Sint64>(ftello(context->File));
  }
  Sint64 base = 0;
  if (whence == SDL_IO_SEEK_CUR) {
    base = static_cast<Sint64>(context->MemPos);
  } else if (whence == SDL_IO_SEEK_END) {
    base = static_cast<Sint64>(context->MemSize);
  }
  const Sint64 target = base + offset;
  if (target < 0 || target > static_cast<Sint64>(context->MemSize)) {
    SDL_SetError("Seek out of range");
    return -1;
  }
  context->MemPos = static_cast<size_t>(target);
  return target;
}

extern "C" SDL_PropertiesID SDL_GetIOProperties(SDL_IOStream* context) {
  if (!context) {
    return 0;
  }
  if (!context->Props) {
    context->Props = SDL_CreateProperties();
    if (context->File) {
      SDL_SetPointerProperty(context->Props, SDL_PROP_IOSTREAM_STDIO_FILE_POINTER, context->File);
    } else {
      SDL_SetPointerProperty(context->Props, SDL_PROP_IOSTREAM_MEMORY_POINTER, context->Mem);
      SDL_SetNumberProperty(context->Props, SDL_PROP_IOSTREAM_MEMORY_SIZE_NUMBER, static_cast<Sint64>(context->MemSize));
    }
  }
  return context->Props;
}

extern "C" bool SDL_CloseIO(SDL_IOStream* context) {
  if (!context) {
    return false;
  }
  bool ok = true;
  if (context->File) {
    ok = std::fclose(context->File) == 0;
  }
  if (context->Props) {
    SDL_DestroyProperties(context->Props);
  }
  delete context;
  return ok;
}

// ---- audio ---------------------------------------------------------------
//
// Playback is paced in real time and drained silently: the guest's audio callbacks
// run and produce data on schedule, but no sound is routed to the Android device yet.

namespace {
constexpr SDL_AudioDeviceID CompatDefaultPlaybackId = 1;
constexpr int CompatMixFrames = 1024;

SDL_AudioSpec CompatDeviceSpec() {
  SDL_AudioSpec spec {};
  spec.format = SDL_AUDIO_F32;
  spec.channels = 2;
  spec.freq = 48000;
  return spec;
}

int CompatBytesPerFrame(const SDL_AudioSpec& spec) {
  return (SDL_AUDIO_BITSIZE(spec.format) / 8) * (spec.channels > 0 ? spec.channels : 1);
}
} // namespace

struct SDL_AudioStream {
  SDL_AudioDeviceID Device {};
  SDL_AudioSpec Spec {};
  SDL_AudioStreamCallback Callback {};
  void* UserData {};
  std::mutex Lock;
  std::vector<uint8_t> Pending;
  Uint64 PutCalls {};
  Uint64 PutBytes {};
  std::atomic<bool> Running {false};
  std::atomic<bool> Stop {false};
  std::thread Worker;
};

namespace {
std::mutex AudioMutex;
std::map<SDL_AudioDeviceID, SDL_AudioStream*> AudioStreams;
SDL_AudioDeviceID NextAudioDeviceId = 100;

bool CompatAudioTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("VEXA_AUDIO_TRACE");
    return value && value[0] && value[0] != '0';
  }();
  return enabled;
}

void CompatAudioLog(const char* format, ...) {
  if (!CompatAudioTraceEnabled()) {
    return;
  }
  char message[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  fprintf(stderr, "[SDL3 audio] %s\n", message);
}

// Playback goes through the guest OpenAL thunk (host OpenAL Soft). If it cannot be
// loaded the stream still runs in real time and its data is dropped.
struct CompatOpenAL {
  void* (*alcOpenDevice)(const char*) {};
  void* (*alcCreateContext)(void*, const int*) {};
  signed char (*alcMakeContextCurrent)(void*) {};
  void (*alGenSources)(int, unsigned*) {};
  void (*alGenBuffers)(int, unsigned*) {};
  void (*alBufferData)(unsigned, int, const void*, int, int) {};
  void (*alSourceQueueBuffers)(unsigned, int, const unsigned*) {};
  void (*alSourceUnqueueBuffers)(unsigned, int, unsigned*) {};
  void (*alGetSourcei)(unsigned, int, int*) {};
  void (*alSourcePlay)(unsigned) {};
  void (*alSourceStop)(unsigned) {};
  void (*alSourcei)(unsigned, int, int) {};
  void (*alDeleteSources)(int, const unsigned*) {};
  void (*alDeleteBuffers)(int, const unsigned*) {};
  int (*alGetError)() {};
  int (*alcGetError)(void*) {};
  void* Device {};
  void* Context {};
  bool Ready {};
};

CompatOpenAL& CompatAL() {
  static CompatOpenAL al;
  static std::once_flag once;
  std::call_once(once, [] {
    void* lib = dlopen("libopenal.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
      CompatAudioLog("audio: libopenal.so.1 load failed (%s); trying libopenal.so", dlerror());
      lib = dlopen("libopenal.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!lib) {
      fprintf(stderr, "[SDL3 thunk] audio: OpenAL not available (%s); output is silent\n", dlerror());
      return;
    }
    CompatAudioLog("audio: OpenAL library loaded at %p", lib);
#define VEXA_AL(name) al.name = reinterpret_cast<decltype(al.name)>(dlsym(lib, #name))
    VEXA_AL(alcOpenDevice); VEXA_AL(alcCreateContext); VEXA_AL(alcMakeContextCurrent);
    VEXA_AL(alGenSources); VEXA_AL(alGenBuffers); VEXA_AL(alBufferData);
    VEXA_AL(alSourceQueueBuffers); VEXA_AL(alSourceUnqueueBuffers); VEXA_AL(alGetSourcei);
    VEXA_AL(alSourcePlay); VEXA_AL(alSourceStop); VEXA_AL(alSourcei);
    VEXA_AL(alDeleteSources); VEXA_AL(alDeleteBuffers);
    VEXA_AL(alGetError); VEXA_AL(alcGetError);
#undef VEXA_AL
    if (!al.alcOpenDevice || !al.alcCreateContext || !al.alcMakeContextCurrent || !al.alGenSources ||
        !al.alGenBuffers || !al.alBufferData || !al.alSourceQueueBuffers || !al.alSourceUnqueueBuffers ||
        !al.alGetSourcei || !al.alSourcePlay || !al.alGetError || !al.alcGetError) {
      CompatAudioLog("audio: OpenAL entrypoints alcOpenDevice=%d alcCreateContext=%d alcMakeContextCurrent=%d "
                     "alGenSources=%d alGenBuffers=%d alBufferData=%d alSourceQueueBuffers=%d "
                     "alSourceUnqueueBuffers=%d alGetSourcei=%d alSourcePlay=%d alGetError=%d alcGetError=%d",
                     al.alcOpenDevice != nullptr, al.alcCreateContext != nullptr,
                     al.alcMakeContextCurrent != nullptr, al.alGenSources != nullptr,
                     al.alGenBuffers != nullptr, al.alBufferData != nullptr,
                     al.alSourceQueueBuffers != nullptr, al.alSourceUnqueueBuffers != nullptr,
                     al.alGetSourcei != nullptr, al.alSourcePlay != nullptr,
                     al.alGetError != nullptr, al.alcGetError != nullptr);
      fprintf(stderr, "[SDL3 thunk] audio: OpenAL thunk is missing entry points; output is silent\n");
      return;
    }
    void* device = al.alcOpenDevice(nullptr);
    void* context = device ? al.alcCreateContext(device, nullptr) : nullptr;
    CompatAudioLog("audio: OpenAL device=%p context=%p", device, context);
    if (!context || !al.alcMakeContextCurrent(context)) {
      fprintf(stderr, "[SDL3 thunk] audio: OpenAL device/context creation failed; output is silent\n");
      return;
    }
    al.Device = device;
    al.Context = context;
    al.Ready = true;
    CompatAudioLog("audio: OpenAL ready device=%p context=%p alcError=0x%x", device, context,
                   al.alcGetError(device));
    fprintf(stderr, "[SDL3 thunk] audio: OpenAL output ready\n");
  });
  return al;
}

// Convert whatever the guest mixed into interleaved stereo/mono signed 16-bit.
std::vector<int16_t> CompatToS16(const std::vector<uint8_t>& in, const SDL_AudioSpec& spec, int* outChannels) {
  const int channels = spec.channels > 0 ? spec.channels : 2;
  const int outCh = channels == 1 ? 1 : 2;
  *outChannels = outCh;
  const int bytes = SDL_AUDIO_BITSIZE(spec.format) / 8;
  const size_t frames = bytes > 0 ? in.size() / (static_cast<size_t>(bytes) * channels) : 0;
  std::vector<int16_t> out(frames * outCh);
  auto sample = [&](size_t frame, int ch) -> int16_t {
    const uint8_t* p = in.data() + (frame * channels + ch) * bytes;
    if (SDL_AUDIO_ISFLOAT(spec.format)) {
      float f;
      std::memcpy(&f, p, sizeof(f));
      f = f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f);
      return static_cast<int16_t>(f * 32767.0f);
    }
    if (bytes == 2) {
      int16_t v;
      std::memcpy(&v, p, 2);
      return v;
    }
    if (bytes == 4) {
      int32_t v;
      std::memcpy(&v, p, 4);
      return static_cast<int16_t>(v >> 16);
    }
    return SDL_AUDIO_ISSIGNED(spec.format) ? static_cast<int16_t>(static_cast<int8_t>(p[0]) << 8)
                                           : static_cast<int16_t>((static_cast<int>(p[0]) - 128) << 8);
  };
  for (size_t i = 0; i < frames; ++i) {
    for (int c = 0; c < outCh; ++c) {
      out[i * outCh + c] = sample(i, c);
    }
  }
  return out;
}

void CompatAudioLoop(SDL_AudioStream* stream) {
  const int bytesPerFrame = CompatBytesPerFrame(stream->Spec);
  const int freq = stream->Spec.freq > 0 ? stream->Spec.freq : 48000;
  const auto period = std::chrono::microseconds(1000000LL * CompatMixFrames / freq);
  CompatAudioLog("audio worker starting stream=%p freq=%d format=0x%x channels=%d callback=%p",
                 stream, stream->Spec.freq, stream->Spec.format, stream->Spec.channels,
                 reinterpret_cast<void*>(stream->Callback));
  CompatOpenAL& al = CompatAL();
  constexpr int kBuffers = 6;
  unsigned source = 0;
  unsigned buffers[kBuffers] {};
  std::vector<unsigned> freeBuffers;
  if (al.Ready) {
    al.alGenSources(1, &source);
    al.alGenBuffers(kBuffers, buffers);
    freeBuffers.assign(buffers, buffers + kBuffers);
  }
  CompatAudioLog("audio worker OpenAL ready=%d source=%u buffers=%d", al.Ready, source, kBuffers);
  auto next = std::chrono::steady_clock::now();
  Uint64 loops = 0;
  Uint64 callbackCalls = 0;
  Uint64 dataBytes = 0;
  Uint64 queuedBuffers = 0;
  Uint64 playStarts = 0;
  Uint64 nonzeroSamples = 0;
  int lastPeak = 0;
  int lastAlError = 0;
  int lastQueued = 0;
  int lastState = 0;
  while (!stream->Stop.load()) {
    next += period;
    const int wanted = CompatMixFrames * bytesPerFrame;
    if (stream->Callback) {
      stream->Callback(stream->UserData, stream, wanted, wanted);
      ++callbackCalls;
    }
    std::vector<uint8_t> data;
    Uint64 putCalls = 0;
    Uint64 putBytes = 0;
    {
      std::lock_guard<std::mutex> lock(stream->Lock);
      data.swap(stream->Pending);
      putCalls = stream->PutCalls;
      putBytes = stream->PutBytes;
    }
    dataBytes += data.size();
    if (al.Ready && source) {
      int processed = 0;
      al.alGetSourcei(source, 0x1016 /* AL_BUFFERS_PROCESSED */, &processed);
      while (processed-- > 0) {
        unsigned done = 0;
        al.alSourceUnqueueBuffers(source, 1, &done);
        freeBuffers.push_back(done);
      }
      if (!data.empty() && !freeBuffers.empty()) {
        int outCh = 2;
        const std::vector<int16_t> pcm = CompatToS16(data, stream->Spec, &outCh);
        if (!pcm.empty()) {
          size_t nonzero = 0;
          int peak = 0;
          for (const int16_t sample : pcm) {
            const int amplitude = sample == -32768 ? 32768 : (sample < 0 ? -sample : sample);
            if (amplitude) {
              ++nonzero;
            }
            peak = amplitude > peak ? amplitude : peak;
          }
          nonzeroSamples += nonzero;
          lastPeak = peak;
          const unsigned buf = freeBuffers.back();
          freeBuffers.pop_back();
          al.alBufferData(buf, outCh == 1 ? 0x1101 : 0x1103, pcm.data(), static_cast<int>(pcm.size() * sizeof(int16_t)), freq);
          al.alSourceQueueBuffers(source, 1, &buf);
          const int alErr = al.alGetError();
          if (alErr != 0) {
            lastAlError = alErr;
          }
          ++queuedBuffers;
          if (queuedBuffers <= 3 || queuedBuffers % 64 == 0) {
            CompatAudioLog("audio queued buffer=%u count=%llu samples=%zu nonzero=%zu peak=%d alErr=0x%x channels=%d freq=%d",
                           buf, static_cast<unsigned long long>(queuedBuffers), pcm.size(), nonzero, peak, alErr,
                           outCh, freq);
          }
        }
      }
      int queued = 0;
      int state = 0;
      al.alGetSourcei(source, 0x1015 /* AL_BUFFERS_QUEUED */, &queued);
      al.alGetSourcei(source, 0x1010 /* AL_SOURCE_STATE */, &state);
      lastQueued = queued;
      lastState = state;
      if (queued >= 2 && state != 0x1012 /* AL_PLAYING */) {
        al.alSourcePlay(source);
        const int alErr = al.alGetError();
        if (alErr != 0) {
          lastAlError = alErr;
        }
        ++playStarts;
        CompatAudioLog("audio OpenAL play source=%u queued=%d state=0x%x starts=%llu alErr=0x%x",
                       source, queued, state, static_cast<unsigned long long>(playStarts), alErr);
      }
    }
    ++loops;
    if (loops <= 4 || loops % 128 == 0) {
      CompatAudioLog("audio worker loop=%llu callbacks=%llu puts=%llu putBytes=%llu dataBytes=%llu queued=%llu "
                     "nonzero=%llu peak=%d alQueued=%d alState=0x%x alErr=0x%x last=%zu",
                     static_cast<unsigned long long>(loops), static_cast<unsigned long long>(callbackCalls),
                     static_cast<unsigned long long>(putCalls), static_cast<unsigned long long>(putBytes),
                     static_cast<unsigned long long>(dataBytes), static_cast<unsigned long long>(queuedBuffers),
                     static_cast<unsigned long long>(nonzeroSamples), lastPeak, lastQueued, lastState, lastAlError,
                     data.size());
    }
    std::this_thread::sleep_until(next);
  }
  CompatAudioLog("audio worker exiting stream=%p loops=%llu callbacks=%llu dataBytes=%llu queued=%llu plays=%llu "
                 "nonzero=%llu peak=%d alState=0x%x alErr=0x%x",
                 stream, static_cast<unsigned long long>(loops), static_cast<unsigned long long>(callbackCalls),
                 static_cast<unsigned long long>(dataBytes), static_cast<unsigned long long>(queuedBuffers),
                 static_cast<unsigned long long>(playStarts), static_cast<unsigned long long>(nonzeroSamples),
                 lastPeak, lastState, lastAlError);
  if (al.Ready && source) {
    if (al.alSourceStop) {
      al.alSourceStop(source);
    }
    if (al.alSourcei) {
      al.alSourcei(source, 0x1009 /* AL_BUFFER */, 0);
    }
    if (al.alDeleteSources) {
      al.alDeleteSources(1, &source);
    }
    if (al.alDeleteBuffers) {
      al.alDeleteBuffers(kBuffers, buffers);
    }
  }
}
} // namespace

extern "C" int SDL_GetNumAudioDrivers() { return 1; }

extern "C" const char* SDL_GetAudioDriver(int index) { return index == 0 ? "vexa" : nullptr; }

extern "C" SDL_AudioDeviceID* SDL_GetAudioPlaybackDevices(int* count) {
  auto* devices = static_cast<SDL_AudioDeviceID*>(std::malloc(2 * sizeof(SDL_AudioDeviceID)));
  if (!devices) {
    if (count) {
      *count = 0;
    }
    return nullptr;
  }
  devices[0] = CompatDefaultPlaybackId;
  devices[1] = 0;
  if (count) {
    *count = 1;
  }
  return devices;
}

extern "C" const char* SDL_GetAudioDeviceName(SDL_AudioDeviceID devid) {
  if (devid == CompatDefaultPlaybackId || devid == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) {
    return "Vexa Audio";
  }
  SDL_SetError("Invalid audio device instance ID");
  return nullptr;
}

extern "C" bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID devid, SDL_AudioSpec* spec, int* sample_frames) {
  std::lock_guard<std::mutex> lock(AudioMutex);
  const bool known = devid == CompatDefaultPlaybackId || devid == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ||
                     AudioStreams.count(devid);
  if (!known) {
    SDL_SetError("Invalid audio device instance ID");
    return false;
  }
  if (spec) {
    *spec = CompatDeviceSpec();
  }
  if (sample_frames) {
    *sample_frames = CompatMixFrames;
  }
  return true;
}

extern "C" SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec* spec,
                                                      SDL_AudioStreamCallback callback, void* userdata) {
  if (devid != SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK && devid != CompatDefaultPlaybackId) {
    CompatAudioLog("SDL_OpenAudioDeviceStream rejected devid=%u default=%u compat=%u",
                   devid, SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, CompatDefaultPlaybackId);
    SDL_SetError("Invalid audio device instance ID");
    return nullptr;
  }
  auto* stream = new SDL_AudioStream;
  stream->Spec = spec ? *spec : CompatDeviceSpec();
  stream->Callback = callback;
  stream->UserData = userdata;
  {
    std::lock_guard<std::mutex> lock(AudioMutex);
    stream->Device = NextAudioDeviceId++;
    AudioStreams[stream->Device] = stream;
  }
  CompatAudioLog("SDL_OpenAudioDeviceStream devid=%u stream=%p device=%u callback=%p freq=%d format=0x%x channels=%d",
                 devid, stream, stream->Device, reinterpret_cast<void*>(callback), stream->Spec.freq,
                 stream->Spec.format, stream->Spec.channels);
  return stream;
}

extern "C" SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream* stream) {
  return stream ? stream->Device : 0;
}

extern "C" bool SDL_ResumeAudioDevice(SDL_AudioDeviceID devid) {
  SDL_AudioStream* stream = nullptr;
  {
    std::lock_guard<std::mutex> lock(AudioMutex);
    auto it = AudioStreams.find(devid);
    if (it != AudioStreams.end()) {
      stream = it->second;
    }
  }
  if (!stream) {
    const bool accepted = devid == CompatDefaultPlaybackId || devid == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    CompatAudioLog("SDL_ResumeAudioDevice devid=%u stream=null accepted=%d", devid, accepted);
    return accepted;
  }
  if (!stream->Running.exchange(true)) {
    stream->Stop = false;
    stream->Worker = std::thread(CompatAudioLoop, stream);
    CompatAudioLog("SDL_ResumeAudioDevice devid=%u stream=%p worker=start", devid, stream);
  } else {
    CompatAudioLog("SDL_ResumeAudioDevice devid=%u stream=%p worker=already-running", devid, stream);
  }
  return true;
}

extern "C" bool SDL_PutAudioStreamData(SDL_AudioStream* stream, const void* buf, int len) {
  if (!stream || !buf || len < 0) {
    CompatAudioLog("SDL_PutAudioStreamData rejected stream=%p buf=%p len=%d", stream, buf, len);
    SDL_SetError("Parameter 'stream' is invalid");
    return false;
  }
  std::lock_guard<std::mutex> lock(stream->Lock);
  const auto* bytes = static_cast<const uint8_t*>(buf);
  stream->Pending.insert(stream->Pending.end(), bytes, bytes + len);
  ++stream->PutCalls;
  stream->PutBytes += static_cast<Uint64>(len);
  if (stream->PutCalls <= 3 || stream->PutCalls % 256 == 0) {
    CompatAudioLog("SDL_PutAudioStreamData stream=%p calls=%llu len=%d total=%llu pending=%zu",
                   stream, static_cast<unsigned long long>(stream->PutCalls), len,
                   static_cast<unsigned long long>(stream->PutBytes), stream->Pending.size());
  }
  return true;
}

extern "C" void SDL_CloseAudioDevice(SDL_AudioDeviceID devid) {
  SDL_AudioStream* stream = nullptr;
  {
    std::lock_guard<std::mutex> lock(AudioMutex);
    auto it = AudioStreams.find(devid);
    if (it != AudioStreams.end()) {
      stream = it->second;
      AudioStreams.erase(it);
    }
  }
  if (stream) {
    CompatAudioLog("SDL_CloseAudioDevice devid=%u stream=%p", devid, stream);
    stream->Stop = true;
    if (stream->Worker.joinable()) {
      stream->Worker.join();
    }
    delete stream;
  }
}

extern "C" void SDL_DestroyAudioStream(SDL_AudioStream* stream) {
  if (stream) {
    SDL_CloseAudioDevice(stream->Device);
  }
}

// ---- surface -------------------------------------------------------------

extern "C" bool SDL_BlitSurfaceScaled(SDL_Surface* src, const SDL_Rect* srcrect, SDL_Surface* dst,
                                      const SDL_Rect* dstrect, SDL_ScaleMode scaleMode) {
  return fexfn_pack_FEX_SDL_BlitSurfaceScaled(src, srcrect, dst, dstrect, scaleMode);
}

extern "C" bool SDL_SetSurfaceBlendMode(SDL_Surface* surface, SDL_BlendMode blendMode) {
  return fexfn_pack_FEX_SDL_SetSurfaceBlendMode(surface, blendMode);
}

// ---- iconv ---------------------------------------------------------------

extern "C" SDL_iconv_t SDL_iconv_open(const char* tocode, const char* fromcode) {
  void* cd = iconv_open(tocode, fromcode);
  return cd == reinterpret_cast<void*>(-1) ? reinterpret_cast<SDL_iconv_t>(-1) : reinterpret_cast<SDL_iconv_t>(cd);
}

extern "C" int SDL_iconv_close(SDL_iconv_t cd) {
  if (!cd || cd == reinterpret_cast<SDL_iconv_t>(-1)) {
    return 0;
  }
  return iconv_close(cd);
}

extern "C" size_t SDL_iconv(SDL_iconv_t cd, const char** inbuf, size_t* inbytesleft, char** outbuf, size_t* outbytesleft) {
  if (!cd || cd == reinterpret_cast<SDL_iconv_t>(-1)) {
    return SDL_ICONV_ERROR;
  }
  const size_t rc = iconv(cd, const_cast<char**>(inbuf), inbytesleft, outbuf, outbytesleft);
  if (rc != static_cast<size_t>(-1)) {
    return rc;
  }
  switch (errno) {
  case E2BIG: return SDL_ICONV_E2BIG;
  case EILSEQ: return SDL_ICONV_EILSEQ;
  case EINVAL: return SDL_ICONV_EINVAL;
  default: return SDL_ICONV_ERROR;
  }
}

// ---- libc / math helpers -------------------------------------------------

extern "C" void* SDL_malloc(size_t size) { return std::malloc(size); }
extern "C" void* SDL_realloc(void* mem, size_t size) { return std::realloc(mem, size); }
extern "C" void SDL_free(void* mem) { std::free(mem); }
extern "C" int SDL_memcmp(const void* s1, const void* s2, size_t len) { return std::memcmp(s1, s2, len); }
extern "C" void* SDL_memset(void* dst, int c, size_t len) { return std::memset(dst, c, len); }
extern "C" void SDL_qsort(void* base, size_t nmemb, size_t size, SDL_CompareCallback compare) {
  std::qsort(base, nmemb, size, compare);
}

extern "C" const char* SDL_getenv(const char* name) { return name ? std::getenv(name) : nullptr; }
extern "C" int SDL_setenv_unsafe(const char* name, const char* value, int overwrite) {
  if (!name || !value) {
    return -1;
  }
  return setenv(name, value, overwrite);
}

extern "C" size_t SDL_strlen(const char* str) { return std::strlen(str); }
extern "C" int SDL_strcmp(const char* a, const char* b) { return std::strcmp(a, b); }
extern "C" int SDL_strncmp(const char* a, const char* b, size_t maxlen) { return std::strncmp(a, b, maxlen); }
extern "C" int SDL_strcasecmp(const char* a, const char* b) { return strcasecmp(a, b); }
extern "C" char* SDL_strchr(const char* str, int c) { return const_cast<char*>(std::strchr(str, c)); }
extern "C" char* SDL_strstr(const char* haystack, const char* needle) {
  return const_cast<char*>(std::strstr(haystack, needle));
}

extern "C" size_t SDL_strlcpy(char* dst, const char* src, size_t maxlen) {
  const size_t srclen = std::strlen(src);
  if (maxlen > 0) {
    const size_t len = srclen < maxlen - 1 ? srclen : maxlen - 1;
    std::memcpy(dst, src, len);
    dst[len] = '\0';
  }
  return srclen;
}

extern "C" size_t SDL_strlcat(char* dst, const char* src, size_t maxlen) {
  const size_t dstlen = strnlen(dst, maxlen);
  if (dstlen == maxlen) {
    return maxlen + std::strlen(src);
  }
  return dstlen + SDL_strlcpy(dst + dstlen, src, maxlen - dstlen);
}

extern "C" int SDL_vsnprintf(char* text, size_t maxlen, const char* fmt, va_list ap) {
  return vsnprintf(text, maxlen, fmt, ap);
}

extern "C" int SDL_snprintf(char* text, size_t maxlen, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int rc = vsnprintf(text, maxlen, fmt, ap);
  va_end(ap);
  return rc;
}

extern "C" int SDL_sscanf(const char* text, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int rc = vsscanf(text, fmt, ap);
  va_end(ap);
  return rc;
}

extern "C" int SDL_atoi(const char* str) { return std::atoi(str); }
extern "C" int SDL_abs(int x) { return x < 0 ? -x : x; }
extern "C" int SDL_isalnum(int x) { return std::isalnum(x); }

extern "C" double SDL_acos(double x) { return ::acos(x); }
extern "C" double SDL_asin(double x) { return ::asin(x); }
extern "C" double SDL_atan(double x) { return ::atan(x); }
extern "C" double SDL_atan2(double y, double x) { return ::atan2(y, x); }
extern "C" double SDL_ceil(double x) { return ::ceil(x); }
extern "C" double SDL_cos(double x) { return ::cos(x); }
extern "C" double SDL_exp(double x) { return ::exp(x); }
extern "C" double SDL_floor(double x) { return ::floor(x); }
extern "C" double SDL_pow(double x, double y) { return ::pow(x, y); }
extern "C" double SDL_scalbn(double x, int n) { return ::scalbn(x, n); }
extern "C" double SDL_sin(double x) { return ::sin(x); }
extern "C" double SDL_sqrt(double x) { return ::sqrt(x); }
extern "C" double SDL_tan(double x) { return ::tan(x); }
extern "C" float SDL_acosf(float x) { return ::acosf(x); }
extern "C" float SDL_atan2f(float y, float x) { return ::atan2f(y, x); }
extern "C" float SDL_cosf(float x) { return ::cosf(x); }
extern "C" float SDL_fabsf(float x) { return ::fabsf(x); }
extern "C" float SDL_powf(float x, float y) { return ::powf(x, y); }
extern "C" float SDL_roundf(float x) { return ::roundf(x); }
extern "C" float SDL_sinf(float x) { return ::sinf(x); }
extern "C" float SDL_sqrtf(float x) { return ::sqrtf(x); }

extern "C" double SDL_log(double x) { return ::log(x); }
extern "C" double SDL_log10(double x) { return ::log10(x); }

// ---- SDL GPU (unsupported; see libSDL3_GPUStubs.inc) -----------------------

#include "libSDL3_GPUStubs.inc"

// ---- touch / pointer input -------------------------------------------------
//
// The Android shell appends 16-byte records (type, x, y, state) to <rootfs>/tmp/vexa-pointer
// using X11 event numbers: KeyPress=2, KeyRelease=3, ButtonPress=4, ButtonRelease=5,
// MotionNotify=6. They become SDL mouse and keyboard events here.

namespace {
std::atomic<float> CompatMouseX {0.0f};
std::atomic<float> CompatMouseY {0.0f};
std::atomic<Uint32> CompatMouseButtons {0};
std::mutex PointerMutex;
off_t PointerOffset = 0;
std::chrono::steady_clock::time_point PointerLastPoll {};

constexpr SDL_JoystickID kVexaPadInstanceId = 1;
constexpr int32_t kVexaPadSynthFlag = 0x40000000;
Uint8 CompatPadButtons[SDL_GAMEPAD_BUTTON_COUNT] = {};
Sint16 CompatPadAxes[SDL_GAMEPAD_AXIS_COUNT] = {};
bool VexaPadAnnounced = false;

Uint8 VexaComputePadHat() {
  Uint8 value = SDL_HAT_CENTERED;
  if (CompatPadButtons[SDL_GAMEPAD_BUTTON_DPAD_UP]) {
    value |= SDL_HAT_UP;
  }
  if (CompatPadButtons[SDL_GAMEPAD_BUTTON_DPAD_DOWN]) {
    value |= SDL_HAT_DOWN;
  }
  if (CompatPadButtons[SDL_GAMEPAD_BUTTON_DPAD_LEFT]) {
    value |= SDL_HAT_LEFT;
  }
  if (CompatPadButtons[SDL_GAMEPAD_BUTTON_DPAD_RIGHT]) {
    value |= SDL_HAT_RIGHT;
  }
  return value;
}

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

void CompatPushEvent(const SDL_Event& event) {
  std::scoped_lock lock {SyntheticEventQueueMutex};
  SyntheticEventQueue.push_back(event);
}
} // namespace

void VexaAnnouncePadSDL3() {
  if (VexaPadAnnounced) {
    return;
  }
  VexaPadAnnounced = true;
  const Uint64 ts = SDL_GetTicksNS();
  SDL_Event event {};
  event.type = SDL_EVENT_JOYSTICK_ADDED;
  event.jdevice.timestamp = ts;
  event.jdevice.which = kVexaPadInstanceId;
  CompatPushEvent(event);
  event = SDL_Event {};
  event.type = SDL_EVENT_GAMEPAD_ADDED;
  event.gdevice.timestamp = ts;
  event.gdevice.which = kVexaPadInstanceId;
  CompatPushEvent(event);
}

void VexaPollPointerEvents() {
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
    const Uint64 timestamp = SDL_GetTicksNS();
    while (handled < 64 && read(fd, rec, sizeof(rec)) == static_cast<ssize_t>(sizeof(rec))) {
      PointerOffset += sizeof(rec);
      ++handled;
      int32_t type, x, y, state;
      std::memcpy(&type, rec, 4);
      std::memcpy(&x, rec + 4, 4);
      std::memcpy(&y, rec + 8, 4);
      std::memcpy(&state, rec + 12, 4);
      if (type == 7) {
        VexaAnnouncePadSDL3();
        if (x >= 0 && x < SDL_GAMEPAD_BUTTON_COUNT) {
          const Uint8 new_state = y != 0 ? 1 : 0;
          if (CompatPadButtons[x] != new_state) {
            const Uint8 old_hat = VexaComputePadHat();
            CompatPadButtons[x] = new_state;
            const Uint8 new_hat = VexaComputePadHat();

            SDL_Event jev {};
            jev.type = new_state ? SDL_EVENT_JOYSTICK_BUTTON_DOWN : SDL_EVENT_JOYSTICK_BUTTON_UP;
            jev.jbutton.timestamp = timestamp;
            jev.jbutton.which = kVexaPadInstanceId;
            jev.jbutton.button = static_cast<Uint8>(x);
            jev.jbutton.down = new_state != 0;
            CompatPushEvent(jev);

            if (new_hat != old_hat) {
              SDL_Event hev {};
              hev.type = SDL_EVENT_JOYSTICK_HAT_MOTION;
              hev.jhat.timestamp = timestamp;
              hev.jhat.which = kVexaPadInstanceId;
              hev.jhat.hat = 0;
              hev.jhat.value = new_hat;
              CompatPushEvent(hev);
            }

            SDL_Event gev {};
            gev.type = new_state ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
            gev.gbutton.timestamp = timestamp;
            gev.gbutton.which = kVexaPadInstanceId;
            gev.gbutton.button = static_cast<Uint8>(x);
            gev.gbutton.down = new_state != 0;
            CompatPushEvent(gev);
          }
        } else {
          const int axis = x - 100;
          if (axis >= 0 && axis < SDL_GAMEPAD_AXIS_COUNT) {
            const Sint16 new_val = static_cast<Sint16>(y);
            if (CompatPadAxes[axis] != new_val) {
              CompatPadAxes[axis] = new_val;

              SDL_Event jev {};
              jev.type = SDL_EVENT_JOYSTICK_AXIS_MOTION;
              jev.jaxis.timestamp = timestamp;
              jev.jaxis.which = kVexaPadInstanceId;
              jev.jaxis.axis = static_cast<Uint8>(axis);
              jev.jaxis.value = new_val;
              CompatPushEvent(jev);

              SDL_Event gev {};
              gev.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
              gev.gaxis.timestamp = timestamp;
              gev.gaxis.which = kVexaPadInstanceId;
              gev.gaxis.axis = static_cast<Uint8>(axis);
              gev.gaxis.value = new_val;
              CompatPushEvent(gev);
            }
          }
        }
        continue;
      }
      if ((state & kVexaPadSynthFlag) != 0) {
        continue;
      }
      SDL_Event event {};
      if (type == 6 || type == 4 || type == 5) {
        const float fx = static_cast<float>(x);
        const float fy = static_cast<float>(y);
        const float oldX = CompatMouseX.exchange(fx);
        const float oldY = CompatMouseY.exchange(fy);
        if (type == 4) {
          CompatMouseButtons |= SDL_BUTTON_LMASK;
        } else if (type == 5) {
          CompatMouseButtons &= ~static_cast<Uint32>(SDL_BUTTON_LMASK);
        }
        if (type == 6 || oldX != fx || oldY != fy) {
          event.type = SDL_EVENT_MOUSE_MOTION;
          event.motion.timestamp = timestamp;
          event.motion.windowID = 1;
          event.motion.state = CompatMouseButtons.load();
          event.motion.x = fx;
          event.motion.y = fy;
          event.motion.xrel = fx - oldX;
          event.motion.yrel = fy - oldY;
          CompatPushEvent(event);
        }
        if (type != 6) {
          SDL_Event button {};
          button.type = type == 4 ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
          button.button.timestamp = timestamp;
          button.button.windowID = 1;
          button.button.button = SDL_BUTTON_LEFT;
          button.button.down = type == 4;
          button.button.clicks = 1;
          button.button.x = fx;
          button.button.y = fy;
          CompatPushEvent(button);
        }
      } else if (type == 2 || type == 3) {
        const SDL_Keycode key = CompatKeysymToKeycode(static_cast<uint32_t>(x));
        if (key != SDLK_UNKNOWN) {
          event.type = type == 2 ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
          event.key.timestamp = timestamp;
          event.key.windowID = 1;
          event.key.key = key;
          event.key.scancode = SDL_GetScancodeFromKey(key, nullptr);
          event.key.down = type == 2;
          CompatPushEvent(event);
        }
      }
    }
  }
  close(fd);
}

// ---- FNA SDL3 platform layer entry points ----------------------------------

#include "libSDL3_FNAStubs.inc"
