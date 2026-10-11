// VEXA_FIXES: Guest-local SDL2 services (memory/stdinc, strings, math, iconv,
// environment/locale, error/log, hints, threads/sync/TLS, RWops, filesystem,
// cpuinfo and audio) needed by SDL2 consumers such as FMOD (libfmod_SDL.so) and
// FNA3D. These run entirely inside the guest; no host round trip is required.
// Audio playback mirrors the SDL3 compat: it goes through the guest OpenAL
// thunk (host openal-soft) when available and is silent otherwise, while still
// running the application's audio callback in real time.
// Included from libSDL2_Guest.cpp, before LOAD_LIB.

#include <SDL_stdinc.h>
#include <SDL_audio.h>
#include <SDL_cpuinfo.h>
#include <SDL_error.h>
#include <SDL_filesystem.h>
#include <SDL_hints.h>
#include <SDL_locale.h>
#include <SDL_log.h>
#include <SDL_mutex.h>
#include <SDL_rwops.h>
#include <SDL_thread.h>

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <iconv.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <fcntl.h>
#include <limits.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

// SDL2 headers map a few SDL_* helpers onto other names via macros; drop them
// so the definitions below export real SDL_* symbols instead of redefining
// libc or tripping over macro expansion (SDL_LoadWAV -> SDL_LoadWAV_RW, plus
// the __clang_analyzer__ libc aliases in SDL_stdinc.h).
#undef SDL_LoadWAV
#undef SDL_abs
#undef SDL_acos
#undef SDL_acosf
#undef SDL_asin
#undef SDL_asinf
#undef SDL_atan
#undef SDL_atanf
#undef SDL_atan2
#undef SDL_atan2f
#undef SDL_atoi
#undef SDL_atof
#undef SDL_ceil
#undef SDL_ceilf
#undef SDL_copysign
#undef SDL_copysignf
#undef SDL_cos
#undef SDL_cosf
#undef SDL_exp
#undef SDL_expf
#undef SDL_fabs
#undef SDL_fabsf
#undef SDL_floor
#undef SDL_floorf
#undef SDL_fmod
#undef SDL_fmodf
#undef SDL_free
#undef SDL_getenv
#undef SDL_iconv
#undef SDL_iconv_close
#undef SDL_iconv_open
#undef SDL_isalnum
#undef SDL_isalpha
#undef SDL_isdigit
#undef SDL_isspace
#undef SDL_log
#undef SDL_log10
#undef SDL_malloc
#undef SDL_calloc
#undef SDL_realloc
#undef SDL_memcmp
#undef SDL_memcpy
#undef SDL_memmove
#undef SDL_memset
#undef SDL_pow
#undef SDL_powf
#undef SDL_round
#undef SDL_roundf
#undef SDL_scalbn
#undef SDL_scalbnf
#undef SDL_setenv
#undef SDL_sin
#undef SDL_sinf
#undef SDL_snprintf
#undef SDL_sqrt
#undef SDL_sqrtf
#undef SDL_sscanf
#undef SDL_strcasecmp
#undef SDL_strncasecmp
#undef SDL_strchr
#undef SDL_strrchr
#undef SDL_strcmp
#undef SDL_strlcat
#undef SDL_strlcpy
#undef SDL_strlen
#undef SDL_strncmp
#undef SDL_strstr
#undef SDL_strtokr
#undef SDL_strtod
#undef SDL_strtol
#undef SDL_strtoul
#undef SDL_tan
#undef SDL_tanf
#undef SDL_tolower
#undef SDL_toupper
#undef SDL_vsnprintf
#undef SDL_vsscanf
#undef SDL_wcscasecmp
#undef SDL_wcscmp
#undef SDL_wcsdup
#undef SDL_wcslcat
#undef SDL_wcslcpy
#undef SDL_wcslen
#undef SDL_wcsncasecmp
#undef SDL_wcsncmp
#undef SDL_wcsstr

// ---- error ---------------------------------------------------------------
//
// Thread-local error string, filled by SDL_SetError/SDL_Error and read back by
// SDL_GetError/SDL_GetErrorMsg. Log functions live further below and share the
// same "format once, print to stderr" policy as the SDL3 compat.

namespace {
thread_local char CompatErrorBuffer[512] = {};

void CompatSetErrorV(const char* fmt, va_list ap) {
  std::vsnprintf(CompatErrorBuffer, sizeof(CompatErrorBuffer), fmt ? fmt : "", ap);
}
} // namespace

extern "C" int SDL_SetError(SDL_PRINTF_FORMAT_STRING const char* fmt, ...) {

  va_list ap;
  va_start(ap, fmt);
  CompatSetErrorV(fmt, ap);
  va_end(ap);
  return -1;
}

extern "C" const char* SDL_GetError(void) {  return CompatErrorBuffer; }

extern "C" char* SDL_GetErrorMsg(char* errstr, int maxlen) {

  if (errstr && maxlen > 0) {
    std::strncpy(errstr, CompatErrorBuffer, static_cast<size_t>(maxlen) - 1);
    errstr[maxlen - 1] = '\0';
  }
  return errstr;
}

extern "C" void SDL_ClearError(void) {  CompatErrorBuffer[0] = '\0'; }

extern "C" int SDL_Error(SDL_errorcode code) {

  switch (code) {
  case SDL_ENOMEM: return SDL_SetError("Out of memory");
  case SDL_EFREAD: return SDL_SetError("Error reading from datastream");
  case SDL_EFWRITE: return SDL_SetError("Error writing to datastream");
  case SDL_EFSEEK: return SDL_SetError("Error seeking in datastream");
  case SDL_UNSUPPORTED: return SDL_SetError("That operation is not supported");
  default: return SDL_SetError("Unknown SDL error %d", static_cast<int>(code));
  }
}

// ---- memory --------------------------------------------------------------

namespace {
SDL_malloc_func CompatMalloc = std::malloc;
SDL_calloc_func CompatCalloc = std::calloc;
SDL_realloc_func CompatRealloc = std::realloc;
SDL_free_func CompatFree = std::free;
std::atomic<long> CompatNumAllocations {0};
} // namespace

extern "C" void* SDL_malloc(size_t size) {
  void* ptr = CompatMalloc(size);

  if (ptr) {
    ++CompatNumAllocations;
  }
  return ptr;
}

extern "C" void* SDL_calloc(size_t nmemb, size_t size) {
  void* ptr = CompatCalloc(nmemb, size);

  if (ptr) {
    ++CompatNumAllocations;
  }
  return ptr;
}

extern "C" void* SDL_realloc(void* mem, size_t size) {
  void* ptr = CompatRealloc(mem, size);

  if (!mem && ptr) {
    ++CompatNumAllocations;
  }
  return ptr;
}

extern "C" void SDL_free(void* mem) {

  if (mem) {
    --CompatNumAllocations;
    CompatFree(mem);
  }
}

extern "C" void SDL_GetOriginalMemoryFunctions(SDL_malloc_func* malloc_func, SDL_calloc_func* calloc_func,
                                               SDL_realloc_func* realloc_func, SDL_free_func* free_func) {

  if (malloc_func) { *malloc_func = std::malloc; }
  if (calloc_func) { *calloc_func = std::calloc; }
  if (realloc_func) { *realloc_func = std::realloc; }
  if (free_func) { *free_func = std::free; }
}

extern "C" void SDL_GetMemoryFunctions(SDL_malloc_func* malloc_func, SDL_calloc_func* calloc_func,
                                       SDL_realloc_func* realloc_func, SDL_free_func* free_func) {

  if (malloc_func) { *malloc_func = CompatMalloc; }
  if (calloc_func) { *calloc_func = CompatCalloc; }
  if (realloc_func) { *realloc_func = CompatRealloc; }
  if (free_func) { *free_func = CompatFree; }
}

extern "C" int SDL_SetMemoryFunctions(SDL_malloc_func malloc_func, SDL_calloc_func calloc_func,
                                      SDL_realloc_func realloc_func, SDL_free_func free_func) {

  if (!malloc_func || !calloc_func || !realloc_func || !free_func) {
    return SDL_SetError("Memory functions must not be null");
  }
  CompatMalloc = malloc_func;
  CompatCalloc = calloc_func;
  CompatRealloc = realloc_func;
  CompatFree = free_func;
  return 0;
}

extern "C" int SDL_GetNumAllocations(void) {

  return static_cast<int>(CompatNumAllocations.load());
}

extern "C" void* SDL_memset(void* dst, int c, size_t len) {  return std::memset(dst, c, len); }
extern "C" void* SDL_memcpy(void* dst, const void* src, size_t len) {  return std::memcpy(dst, src, len); }
extern "C" void* SDL_memmove(void* dst, const void* src, size_t len) {  return std::memmove(dst, src, len); }
extern "C" int SDL_memcmp(const void* s1, const void* s2, size_t len) {  return std::memcmp(s1, s2, len); }

namespace {
constexpr size_t CompatSIMAlignment = 16;
}

extern "C" size_t SDL_SIMDGetAlignment(void) {  return CompatSIMAlignment; }

extern "C" void* SDL_SIMDAlloc(const size_t len) {
  // VEXA_FIXES: match real SDL2 semantics exactly: over-allocate and stash the
  // base malloc pointer at ((void**)retval)[-1]; callers (FNA3D/mono) replicate
  // this layout and pass aligned pointers back to SDL_SIMDFree.
  const size_t alignment = CompatSIMAlignment;
  const size_t padding = alignment - (len % alignment);
  const size_t padded = padding != alignment ? len + padding : len + alignment;
  void* mem = CompatMalloc(padded + alignment + sizeof(void*));
  Uint8* retval = nullptr;
  if (mem) {
    retval = reinterpret_cast<Uint8*>(
        (reinterpret_cast<uintptr_t>(mem) + alignment - 1 + sizeof(void*)) &
        ~static_cast<uintptr_t>(alignment - 1));
    reinterpret_cast<void**>(retval)[-1] = mem;
    ++CompatNumAllocations;
  }

  return retval;
}

extern "C" void* SDL_SIMDRealloc(void* mem, const size_t len) {

  if (!mem) {
    return SDL_SIMDAlloc(len);
  }
  const size_t alignment = CompatSIMAlignment;
  void* realmem = reinterpret_cast<void**>(mem)[-1];
  const size_t padding = alignment - (len % alignment);
  const size_t padded = padding != alignment ? len + padding : len + alignment;
  void* newmem = CompatRealloc(realmem, padded + alignment + sizeof(void*));
  if (!newmem) {
    return nullptr;
  }
  Uint8* retval = reinterpret_cast<Uint8*>(
      (reinterpret_cast<uintptr_t>(newmem) + alignment - 1 + sizeof(void*)) &
      ~static_cast<uintptr_t>(alignment - 1));
  reinterpret_cast<void**>(retval)[-1] = newmem;
  return retval;
}

extern "C" void SDL_SIMDFree(void* ptr) {

  if (ptr) {
    --CompatNumAllocations;
    void** realptr = static_cast<void**>(ptr);
    --realptr;
    CompatFree(*realptr);
  }
}

// ---- iconv ---------------------------------------------------------------

extern "C" SDL_iconv_t SDL_iconv_open(const char* tocode, const char* fromcode) {

  iconv_t cd = iconv_open(tocode, fromcode);
  return cd == reinterpret_cast<iconv_t>(-1) ? reinterpret_cast<SDL_iconv_t>(-1)
                                             : reinterpret_cast<SDL_iconv_t>(cd);
}

extern "C" int SDL_iconv_close(SDL_iconv_t cd) {

  if (!cd || cd == reinterpret_cast<SDL_iconv_t>(-1)) {
    return 0;
  }
  return iconv_close(reinterpret_cast<iconv_t>(cd));
}

extern "C" size_t SDL_iconv(SDL_iconv_t cd, const char** inbuf, size_t* inbytesleft, char** outbuf,
                            size_t* outbytesleft) {

  if (!cd || cd == reinterpret_cast<SDL_iconv_t>(-1)) {
    return SDL_ICONV_ERROR;
  }
  const size_t rc = iconv(reinterpret_cast<iconv_t>(cd), const_cast<char**>(inbuf), inbytesleft, outbuf,
                          outbytesleft);
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

extern "C" char* SDL_iconv_string(const char* tocode, const char* fromcode, const char* inbuf,
                                  size_t inbytesleft) {

  if (!inbuf) {
    return nullptr;
  }
  SDL_iconv_t cd = SDL_iconv_open(tocode, fromcode);
  if (cd == reinterpret_cast<SDL_iconv_t>(-1)) {
    return nullptr;
  }
  size_t outcap = inbytesleft * 4 + 64;
  char* out = static_cast<char*>(SDL_malloc(outcap));
  if (!out) {
    SDL_iconv_close(cd);
    return nullptr;
  }
  const char* src = inbuf;
  size_t srcleft = inbytesleft;
  char* dst = out;
  size_t dstleft = outcap;
  bool ok = true;
  while (srcleft > 0) {
    const size_t rc = SDL_iconv(cd, &src, &srcleft, &dst, &dstleft);
    if (rc != SDL_ICONV_ERROR && rc != SDL_ICONV_E2BIG && rc != SDL_ICONV_EILSEQ && rc != SDL_ICONV_EINVAL) {
      continue; // progress, keep going
    }
    if (rc == SDL_ICONV_E2BIG) {
      const size_t used = static_cast<size_t>(dst - out);
      outcap *= 2;
      char* bigger = static_cast<char*>(SDL_realloc(out, outcap));
      if (!bigger) {
        ok = false;
        break;
      }
      out = bigger;
      dst = out + used;
      dstleft = outcap - used;
      continue;
    }
    if (rc == SDL_ICONV_EINVAL) {
      break; // truncated multibyte char at end of input; done
    }
    if (rc == SDL_ICONV_EILSEQ) {
      // SDL2 skips over invalid sequences; copy the offending byte through.
      if (dstleft < 2) {
        const size_t used = static_cast<size_t>(dst - out);
        outcap *= 2;
        char* bigger = static_cast<char*>(SDL_realloc(out, outcap));
        if (!bigger) {
          ok = false;
          break;
        }
        out = bigger;
        dst = out + used;
        dstleft = outcap - used;
      }
      *dst++ = *src++;
      --dstleft;
      --srcleft;
      continue;
    }
    ok = false;
    break;
  }
  if (!ok) {
    SDL_free(out);
    SDL_iconv_close(cd);
    return nullptr;
  }
  *dst = '\0';
  SDL_iconv_close(cd);
  return out;
}

// ---- strings / chars -----------------------------------------------------

extern "C" size_t SDL_strlen(const char* str) {  return std::strlen(str); }
extern "C" int SDL_strcmp(const char* str1, const char* str2) {  return std::strcmp(str1, str2); }
extern "C" int SDL_strncmp(const char* str1, const char* str2, size_t maxlen) {

  return std::strncmp(str1, str2, maxlen);
}
extern "C" int SDL_strcasecmp(const char* str1, const char* str2) {  return strcasecmp(str1, str2); }
extern "C" int SDL_strncasecmp(const char* str1, const char* str2, size_t len) {

  return strncasecmp(str1, str2, len);
}
extern "C" char* SDL_strchr(const char* str, int c) {  return const_cast<char*>(std::strchr(str, c)); }
extern "C" char* SDL_strrchr(const char* str, int c) {  return const_cast<char*>(std::strrchr(str, c)); }
extern "C" char* SDL_strstr(const char* haystack, const char* needle) {

  return const_cast<char*>(std::strstr(haystack, needle));
}
extern "C" char* SDL_strtokr(char* s1, const char* s2, char** saveptr) {

  return strtok_r(s1, s2, saveptr);
}
extern "C" long SDL_strtol(const char* str, char** endp, int base) {  return std::strtol(str, endp, base); }
extern "C" unsigned long SDL_strtoul(const char* str, char** endp, int base) {

  return std::strtoul(str, endp, base);
}
extern "C" Sint64 SDL_strtoll(const char* str, char** endp, int base) {

  return static_cast<Sint64>(std::strtoll(str, endp, base));
}
extern "C" Uint64 SDL_strtoull(const char* str, char** endp, int base) {

  return static_cast<Uint64>(std::strtoull(str, endp, base));
}
extern "C" double SDL_strtod(const char* str, char** endp) {  return std::strtod(str, endp); }
extern "C" int SDL_atoi(const char* str) {  return std::atoi(str); }
extern "C" double SDL_atof(const char* str) {  return std::atof(str); }

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

extern "C" char* SDL_strdup(const char* str) {

  const size_t len = std::strlen(str) + 1;
  char* copy = static_cast<char*>(SDL_malloc(len));
  if (copy) {
    std::memcpy(copy, str, len);
  }
  return copy;
}

extern "C" char* SDL_strrev(char* str) {

  const size_t len = std::strlen(str);
  for (size_t i = 0; i < len / 2; ++i) {
    const char tmp = str[i];
    str[i] = str[len - 1 - i];
    str[len - 1 - i] = tmp;
  }
  return str;
}

extern "C" char* SDL_strupr(char* str) {

  for (char* p = str; *p; ++p) {
    *p = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
  }
  return str;
}

extern "C" char* SDL_strlwr(char* str) {

  for (char* p = str; *p; ++p) {
    *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
  }
  return str;
}

extern "C" char* SDL_strcasestr(const char* haystack, const char* needle) {

  if (!*needle) {
    return const_cast<char*>(haystack);
  }
  const size_t needlelen = std::strlen(needle);
  for (const char* p = haystack; *p; ++p) {
    if (strncasecmp(p, needle, needlelen) == 0) {
      return const_cast<char*>(p);
    }
  }
  return nullptr;
}

extern "C" int SDL_tolower(int x) {  return std::tolower(x); }
extern "C" int SDL_toupper(int x) {  return std::toupper(x); }
extern "C" int SDL_isdigit(int x) {  return std::isdigit(x); }
extern "C" int SDL_isalpha(int x) {  return std::isalpha(x); }
extern "C" int SDL_isalnum(int x) {  return std::isalnum(x); }
extern "C" int SDL_isspace(int x) {  return std::isspace(x); }
extern "C" int SDL_isblank(int x) {  return std::isblank(x); }
extern "C" int SDL_iscntrl(int x) {  return std::iscntrl(x); }
extern "C" int SDL_isxdigit(int x) {  return std::isxdigit(x); }
extern "C" int SDL_ispunct(int x) {  return std::ispunct(x); }
extern "C" int SDL_isupper(int x) {  return std::isupper(x); }
extern "C" int SDL_islower(int x) {  return std::islower(x); }
extern "C" int SDL_isprint(int x) {  return std::isprint(x); }
extern "C" int SDL_isgraph(int x) {  return std::isgraph(x); }

extern "C" int SDL_vsnprintf(char* text, size_t maxlen, const char* fmt, va_list ap) {

  return vsnprintf(text, maxlen, fmt, ap);
}

extern "C" int SDL_snprintf(char* text, size_t maxlen, SDL_PRINTF_FORMAT_STRING const char* fmt, ...) {

  va_list ap;
  va_start(ap, fmt);
  const int rc = vsnprintf(text, maxlen, fmt, ap);
  va_end(ap);
  return rc;
}

extern "C" int SDL_vsscanf(const char* text, const char* fmt, va_list ap) {

  return vsscanf(text, fmt, ap);
}

extern "C" int SDL_sscanf(const char* text, SDL_SCANF_FORMAT_STRING const char* fmt, ...) {

  va_list ap;
  va_start(ap, fmt);
  const int rc = vsscanf(text, fmt, ap);
  va_end(ap);
  return rc;
}

extern "C" int SDL_vasprintf(char** strp, const char* fmt, va_list ap) {

  if (!strp) {
    return -1;
  }
  va_list copy;
  va_copy(copy, ap);
  const int len = vsnprintf(nullptr, 0, fmt, copy);
  va_end(copy);
  if (len < 0) {
    *strp = nullptr;
    return -1;
  }
  *strp = static_cast<char*>(SDL_malloc(static_cast<size_t>(len) + 1));
  if (!*strp) {
    return -1;
  }
  return vsnprintf(*strp, static_cast<size_t>(len) + 1, fmt, ap);
}

extern "C" int SDL_asprintf(char** strp, SDL_PRINTF_FORMAT_STRING const char* fmt, ...) {

  va_list ap;
  va_start(ap, fmt);
  const int rc = SDL_vasprintf(strp, fmt, ap);
  va_end(ap);
  return rc;
}

extern "C" void SDL_qsort(void* base, size_t nmemb, size_t size, SDL_CompareCallback compare) {

  std::qsort(base, nmemb, size, compare);
}

extern "C" void* SDL_bsearch(const void* key, const void* base, size_t nmemb, size_t size,
                             SDL_CompareCallback compare) {

  return std::bsearch(key, base, nmemb, size, compare);
}

namespace {
size_t CompatUTF8CodepointSize(Uint8 first) {
  if ((first & 0x80) == 0) { return 1; }
  if ((first & 0xE0) == 0xC0) { return 2; }
  if ((first & 0xF0) == 0xE0) { return 3; }
  if ((first & 0xF8) == 0xF0) { return 4; }
  return 1; // invalid; treat as single byte so we always make progress
}
} // namespace

extern "C" size_t SDL_utf8strlen(const char* str) {

  size_t count = 0;
  const auto* p = reinterpret_cast<const Uint8*>(str);
  while (*p) {
    p += CompatUTF8CodepointSize(*p);
    ++count;
  }
  return count;
}

extern "C" size_t SDL_utf8strnlen(const char* str, size_t bytes) {

  size_t count = 0;
  size_t consumed = 0;
  const auto* p = reinterpret_cast<const Uint8*>(str);
  while (consumed < bytes && *p) {
    const size_t size = CompatUTF8CodepointSize(*p);
    p += size;
    consumed += size;
    ++count;
  }
  return count;
}

extern "C" size_t SDL_utf8strlcpy(char* dst, const char* src, size_t dst_bytes) {

  const size_t srclen = std::strlen(src);
  if (dst_bytes == 0) {
    return srclen;
  }
  size_t written = 0;
  const auto* p = reinterpret_cast<const Uint8*>(src);
  while (*p) {
    const size_t size = CompatUTF8CodepointSize(*p);
    if (written + size >= dst_bytes) {
      break;
    }
    std::memcpy(dst + written, p, size);
    written += size;
    p += size;
  }
  dst[written] = '\0';
  return srclen;
}

extern "C" size_t SDL_wcslen(const wchar_t* wstr) {  return std::wcslen(wstr); }
extern "C" int SDL_wcscmp(const wchar_t* str1, const wchar_t* str2) {  return std::wcscmp(str1, str2); }
extern "C" int SDL_wcsncmp(const wchar_t* str1, const wchar_t* str2, size_t maxlen) {

  return std::wcsncmp(str1, str2, maxlen);
}
extern "C" wchar_t* SDL_wcsstr(const wchar_t* haystack, const wchar_t* needle) {

  return const_cast<wchar_t*>(std::wcsstr(haystack, needle));
}

extern "C" size_t SDL_wcslcpy(wchar_t* dst, const wchar_t* src, size_t maxlen) {

  const size_t srclen = std::wcslen(src);
  if (maxlen > 0) {
    const size_t len = srclen < maxlen - 1 ? srclen : maxlen - 1;
    std::wmemcpy(dst, src, len);
    dst[len] = L'\0';
  }
  return srclen;
}

extern "C" size_t SDL_wcslcat(wchar_t* dst, const wchar_t* src, size_t maxlen) {

  const size_t dstlen = std::wcslen(dst);
  if (dstlen >= maxlen) {
    return maxlen + std::wcslen(src);
  }
  return dstlen + SDL_wcslcpy(dst + dstlen, src, maxlen - dstlen);
}

extern "C" wchar_t* SDL_wcsdup(const wchar_t* wstr) {

  const size_t len = (std::wcslen(wstr) + 1) * sizeof(wchar_t);
  wchar_t* copy = static_cast<wchar_t*>(SDL_malloc(len));
  if (copy) {
    std::memcpy(copy, wstr, len);
  }
  return copy;
}

extern "C" int SDL_wcscasecmp(const wchar_t* str1, const wchar_t* str2) {

  while (*str1 && *str2) {
    const wchar_t a = towlower(*str1);
    const wchar_t b = towlower(*str2);
    if (a != b) {
      return a < b ? -1 : 1;
    }
    ++str1;
    ++str2;
  }
  return static_cast<int>(*str1) - static_cast<int>(*str2);
}

extern "C" int SDL_wcsncasecmp(const wchar_t* str1, const wchar_t* str2, size_t len) {

  for (size_t i = 0; i < len && (str1[i] || str2[i]); ++i) {
    const wchar_t a = towlower(str1[i]);
    const wchar_t b = towlower(str2[i]);
    if (a != b) {
      return a < b ? -1 : 1;
    }
  }
  return 0;
}

namespace {
template <typename T>
char* CompatIntToString(T value, char* str, int radix, bool is_signed) {
  char tmp[66];
  unsigned count = 0;
  bool negative = false;
  Uint64 uv;
  if (is_signed && value < 0) {
    negative = true;
    uv = static_cast<Uint64>(-(value + 1)) + 1;
  } else {
    uv = static_cast<Uint64>(value);
  }
  do {
    const unsigned d = static_cast<unsigned>(uv % static_cast<unsigned>(radix));
    tmp[count++] = static_cast<char>(d < 10 ? '0' + d : 'a' + (d - 10));
    uv /= static_cast<unsigned>(radix);
  } while (uv);
  char* out = str;
  if (negative) {
    *out++ = '-';
  }
  while (count) {
    *out++ = tmp[--count];
  }
  *out = '\0';
  return str;
}
} // namespace

extern "C" char* SDL_itoa(int value, char* str, int radix) {

  return CompatIntToString<long>(value, str, radix, true);
}
extern "C" char* SDL_uitoa(unsigned int value, char* str, int radix) {

  return CompatIntToString<unsigned long>(value, str, radix, false);
}
extern "C" char* SDL_ltoa(long value, char* str, int radix) {

  return CompatIntToString<long>(value, str, radix, true);
}
extern "C" char* SDL_ultoa(unsigned long value, char* str, int radix) {

  return CompatIntToString<unsigned long>(value, str, radix, false);
}
extern "C" char* SDL_lltoa(Sint64 value, char* str, int radix) {

  return CompatIntToString<Sint64>(value, str, radix, true);
}
extern "C" char* SDL_ulltoa(Uint64 value, char* str, int radix) {

  return CompatIntToString<Uint64>(value, str, radix, false);
}

extern "C" Uint16 SDL_crc16(Uint16 crc, const void* data, size_t len) {

  static Uint16 table[256];
  static bool initialized = false;
  if (!initialized) {
    for (unsigned i = 0; i < 256; ++i) {
      Uint16 v = static_cast<Uint16>(i);
      for (int bit = 0; bit < 8; ++bit) {
        v = (v & 1) ? static_cast<Uint16>((v >> 1) ^ 0x8408) : static_cast<Uint16>(v >> 1);
      }
      table[i] = v;
    }
    initialized = true;
  }
  const auto* bytes = static_cast<const Uint8*>(data);
  while (len--) {
    crc = static_cast<Uint16>((crc >> 8) ^ table[(crc ^ *bytes++) & 0xFF]);
  }
  return crc;
}

extern "C" Uint32 SDL_crc32(Uint32 crc, const void* data, size_t len) {

  static Uint32 table[256];
  static bool initialized = false;
  if (!initialized) {
    for (unsigned i = 0; i < 256; ++i) {
      Uint32 v = i;
      for (int bit = 0; bit < 8; ++bit) {
        v = (v & 1) ? (v >> 1) ^ 0xEDB88320u : (v >> 1);
      }
      table[i] = v;
    }
    initialized = true;
  }
  const auto* bytes = static_cast<const Uint8*>(data);
  while (len--) {
    crc = (crc >> 8) ^ table[(crc ^ *bytes++) & 0xFF];
  }
  return crc;
}

// ---- math ----------------------------------------------------------------

extern "C" int SDL_abs(int x) {  return x < 0 ? -x : x; }

extern "C" double SDL_acos(double x) {  return ::acos(x); }
extern "C" float SDL_acosf(float x) {  return ::acosf(x); }
extern "C" double SDL_asin(double x) {  return ::asin(x); }
extern "C" float SDL_asinf(float x) {  return ::asinf(x); }
extern "C" double SDL_atan(double x) {  return ::atan(x); }
extern "C" float SDL_atanf(float x) {  return ::atanf(x); }
extern "C" double SDL_atan2(double y, double x) {  return ::atan2(y, x); }
extern "C" float SDL_atan2f(float y, float x) {  return ::atan2f(y, x); }
extern "C" double SDL_ceil(double x) {  return ::ceil(x); }
extern "C" float SDL_ceilf(float x) {  return ::ceilf(x); }
extern "C" double SDL_copysign(double x, double y) {  return ::copysign(x, y); }
extern "C" float SDL_copysignf(float x, float y) {  return ::copysignf(x, y); }
extern "C" double SDL_cos(double x) {  return ::cos(x); }
extern "C" float SDL_cosf(float x) {  return ::cosf(x); }
extern "C" double SDL_exp(double x) {  return ::exp(x); }
extern "C" float SDL_expf(float x) {  return ::expf(x); }
extern "C" double SDL_fabs(double x) {  return ::fabs(x); }
extern "C" float SDL_fabsf(float x) {  return ::fabsf(x); }
extern "C" double SDL_floor(double x) {  return ::floor(x); }
extern "C" float SDL_floorf(float x) {  return ::floorf(x); }
extern "C" double SDL_trunc(double x) {  return ::trunc(x); }
extern "C" float SDL_truncf(float x) {  return ::truncf(x); }
extern "C" double SDL_fmod(double x, double y) {  return ::fmod(x, y); }
extern "C" float SDL_fmodf(float x, float y) {  return ::fmodf(x, y); }
extern "C" double SDL_log(double x) {  return ::log(x); }
extern "C" float SDL_logf(float x) {  return ::logf(x); }
extern "C" double SDL_log10(double x) {  return ::log10(x); }
extern "C" float SDL_log10f(float x) {  return ::log10f(x); }
extern "C" double SDL_pow(double x, double y) {  return ::pow(x, y); }
extern "C" float SDL_powf(float x, float y) {  return ::powf(x, y); }
extern "C" double SDL_round(double x) {  return ::round(x); }
extern "C" float SDL_roundf(float x) {  return ::roundf(x); }
extern "C" long SDL_lround(double x) {  return ::lround(x); }
extern "C" long SDL_lroundf(float x) {  return ::lroundf(x); }
extern "C" double SDL_scalbn(double x, int n) {  return ::scalbn(x, n); }
extern "C" float SDL_scalbnf(float x, int n) {  return ::scalbnf(x, n); }
extern "C" double SDL_sin(double x) {  return ::sin(x); }
extern "C" float SDL_sinf(float x) {  return ::sinf(x); }
extern "C" double SDL_sqrt(double x) {  return ::sqrt(x); }
extern "C" float SDL_sqrtf(float x) {  return ::sqrtf(x); }
extern "C" double SDL_tan(double x) {  return ::tan(x); }
extern "C" float SDL_tanf(float x) {  return ::tanf(x); }

// ---- environment / locale --------------------------------------------------

extern "C" char* SDL_getenv(const char* name) {  return name ? std::getenv(name) : nullptr; }

extern "C" int SDL_setenv(const char* name, const char* value, int overwrite) {

  if (!name || !*name || !value) {
    return SDL_SetError("SDL_setenv: invalid argument");
  }
  return setenv(name, value, overwrite);
}

extern "C" SDL_Locale* SDL_GetPreferredLocales(void) {

  // Single allocation: two SDL_Locale structs followed by the strings they point
  // at, matching SDL2's "free the whole thing with one SDL_free" contract.
  char* block = static_cast<char*>(SDL_malloc(sizeof(SDL_Locale) * 2 + 8));
  if (!block) {
    return nullptr;
  }
  char* strings = block + sizeof(SDL_Locale) * 2;
  std::memcpy(strings, "en\0US\0\0\0", 8);
  auto* locales = reinterpret_cast<SDL_Locale*>(block);
  locales[0].language = strings;
  locales[0].country = strings + 3;
  locales[1].language = nullptr;
  locales[1].country = nullptr;
  return locales;
}

// ---- logging ---------------------------------------------------------------

namespace {
constexpr int CompatLogCategoryCount = 64;
SDL_LogPriority CompatLogPriorities[CompatLogCategoryCount];
SDL_LogOutputFunction CompatLogOutput = nullptr;
void* CompatLogUserdata = nullptr;
std::mutex CompatLogMutex;

int CompatLogCategoryIndex(int category) {
  if (category < 0) {
    return 0;
  }
  return category < CompatLogCategoryCount ? category : SDL_LOG_CATEGORY_CUSTOM;
}

void CompatInitLogPriorities() {
  for (int i = 0; i < CompatLogCategoryCount; ++i) {
    CompatLogPriorities[i] = SDL_LOG_PRIORITY_ERROR;
  }
  CompatLogPriorities[SDL_LOG_CATEGORY_APPLICATION] = SDL_LOG_PRIORITY_INFO;
  CompatLogPriorities[SDL_LOG_CATEGORY_ASSERT] = SDL_LOG_PRIORITY_WARN;
}

bool CompatLogInitOnce() {
  static std::once_flag once;
  std::call_once(once, CompatInitLogPriorities);
  return true;
}

void CompatLogDefaultOutput(void*, int, SDL_LogPriority, const char* message) {
  fprintf(stderr, "%s\n", message ? message : "");
}
} // namespace

extern "C" void SDL_LogSetAllPriority(SDL_LogPriority priority) {

  CompatLogInitOnce();
  std::lock_guard<std::mutex> lock(CompatLogMutex);
  for (int i = 0; i < CompatLogCategoryCount; ++i) {
    CompatLogPriorities[i] = priority;
  }
}

extern "C" void SDL_LogSetPriority(int category, SDL_LogPriority priority) {

  CompatLogInitOnce();
  std::lock_guard<std::mutex> lock(CompatLogMutex);
  CompatLogPriorities[CompatLogCategoryIndex(category)] = priority;
}

extern "C" SDL_LogPriority SDL_LogGetPriority(int category) {

  CompatLogInitOnce();
  std::lock_guard<std::mutex> lock(CompatLogMutex);
  return CompatLogPriorities[CompatLogCategoryIndex(category)];
}

extern "C" void SDL_LogResetPriorities(void) {

  std::lock_guard<std::mutex> lock(CompatLogMutex);
  CompatInitLogPriorities();
}

extern "C" void SDL_LogGetOutputFunction(SDL_LogOutputFunction* callback, void** userdata) {

  std::lock_guard<std::mutex> lock(CompatLogMutex);
  if (callback) {
    *callback = CompatLogOutput ? CompatLogOutput : CompatLogDefaultOutput;
  }
  if (userdata) {
    *userdata = CompatLogUserdata;
  }
}

extern "C" void SDL_LogSetOutputFunction(SDL_LogOutputFunction callback, void* userdata) {

  std::lock_guard<std::mutex> lock(CompatLogMutex);
  CompatLogOutput = callback;
  CompatLogUserdata = userdata;
}

extern "C" void SDL_LogMessageV(int category, SDL_LogPriority priority,
                                SDL_PRINTF_FORMAT_STRING const char* fmt, va_list ap) {

  CompatLogInitOnce();
  {
    std::lock_guard<std::mutex> lock(CompatLogMutex);
    if (priority < CompatLogPriorities[CompatLogCategoryIndex(category)]) {
      return;
    }
  }
  char stackbuf[512];
  va_list copy;
  va_copy(copy, ap);
  const int needed = vsnprintf(stackbuf, sizeof(stackbuf), fmt ? fmt : "", copy);
  va_end(copy);
  const char* message = stackbuf;
  std::vector<char> heap;
  if (needed >= static_cast<int>(sizeof(stackbuf))) {
    heap.resize(static_cast<size_t>(needed) + 1);
    vsnprintf(heap.data(), heap.size(), fmt ? fmt : "", ap);
    message = heap.data();
  }
  SDL_LogOutputFunction output;
  void* userdata;
  {
    std::lock_guard<std::mutex> lock(CompatLogMutex);
    output = CompatLogOutput ? CompatLogOutput : CompatLogDefaultOutput;
    userdata = CompatLogUserdata;
  }
  output(userdata, category, priority, message);
}

extern "C" void SDL_Log(SDL_PRINTF_FORMAT_STRING const char* fmt, ...) {

  va_list ap;
  va_start(ap, fmt);
  SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO, fmt, ap);
  va_end(ap);
}

extern "C" void SDL_LogVerbose(int category, SDL_PRINTF_FORMAT_STRING const char* fmt, ...) {

  va_list ap;
  va_start(ap, fmt);
  SDL_LogMessageV(category, SDL_LOG_PRIORITY_VERBOSE, fmt, ap);
  va_end(ap);
}

extern "C" void SDL_LogDebug(int category, SDL_PRINTF_FORMAT_STRING const char* fmt, ...) {

  va_list ap;
  va_start(ap, fmt);
  SDL_LogMessageV(category, SDL_LOG_PRIORITY_DEBUG, fmt, ap);
  va_end(ap);
}

extern "C" void SDL_LogInfo(int category, SDL_PRINTF_FORMAT_STRING const char* fmt, ...) {

  va_list ap;
  va_start(ap, fmt);
  SDL_LogMessageV(category, SDL_LOG_PRIORITY_INFO, fmt, ap);
  va_end(ap);
}

extern "C" void SDL_LogWarn(int category, SDL_PRINTF_FORMAT_STRING const char* fmt, ...) {

  va_list ap;
  va_start(ap, fmt);
  SDL_LogMessageV(category, SDL_LOG_PRIORITY_WARN, fmt, ap);
  va_end(ap);
}

extern "C" void SDL_LogError(int category, SDL_PRINTF_FORMAT_STRING const char* fmt, ...) {

  va_list ap;
  va_start(ap, fmt);
  SDL_LogMessageV(category, SDL_LOG_PRIORITY_ERROR, fmt, ap);
  va_end(ap);
}

extern "C" void SDL_LogCritical(int category, SDL_PRINTF_FORMAT_STRING const char* fmt, ...) {

  va_list ap;
  va_start(ap, fmt);
  SDL_LogMessageV(category, SDL_LOG_PRIORITY_CRITICAL, fmt, ap);
  va_end(ap);
}

extern "C" void SDL_LogMessage(int category, SDL_LogPriority priority,
                               SDL_PRINTF_FORMAT_STRING const char* fmt, ...) {

  va_list ap;
  va_start(ap, fmt);
  SDL_LogMessageV(category, priority, fmt, ap);
  va_end(ap);
}

// ---- hints -----------------------------------------------------------------

namespace {
struct CompatHintEntry {
  std::string Value;
  SDL_HintPriority Priority {SDL_HINT_DEFAULT};
};
struct CompatHintCallbackEntry {
  SDL_HintCallback Callback {};
  void* Userdata {};
};

// Recursive like SDL's real hint mutex so hint callbacks can query hints.
std::recursive_mutex CompatHintsMutex;
std::map<std::string, CompatHintEntry> CompatHints;
std::map<std::string, std::vector<CompatHintCallbackEntry>> CompatHintCallbacks;

const char* CompatLookupHint(const std::string& name) {
  auto it = CompatHints.find(name);
  if (it != CompatHints.end()) {
    return it->second.Value.c_str();
  }
  return std::getenv(name.c_str());
}
} // namespace

extern "C" SDL_bool SDL_SetHintWithPriority(const char* name, const char* value, SDL_HintPriority priority) {

  if (!name || !value) {
    return SDL_FALSE;
  }
  std::lock_guard<std::recursive_mutex> lock(CompatHintsMutex);
  auto it = CompatHints.find(name);
  if (it != CompatHints.end() && priority < it->second.Priority) {
    return SDL_FALSE;
  }
  const std::string key(name);
  const std::string old = it != CompatHints.end() ? it->second.Value : std::string {};
  if (it != CompatHints.end() && old == value) {
    it->second.Priority = priority;
    return SDL_TRUE;
  }
  CompatHints[key] = CompatHintEntry {value, priority};
  auto cb = CompatHintCallbacks.find(key);
  if (cb != CompatHintCallbacks.end()) {
    // Copy: callbacks may call SDL_AddHintCallback/SDL_DelHintCallback.
    const auto callbacks = cb->second;
    for (const auto& entry : callbacks) {
      entry.Callback(entry.Userdata, name, it != CompatHints.end() && !old.empty() ? old.c_str() : nullptr, value);
    }
  }
  return SDL_TRUE;
}

extern "C" SDL_bool SDL_SetHint(const char* name, const char* value) {

  return SDL_SetHintWithPriority(name, value, SDL_HINT_NORMAL);
}

extern "C" SDL_bool SDL_ResetHint(const char* name) {

  if (!name) {
    return SDL_FALSE;
  }
  std::lock_guard<std::recursive_mutex> lock(CompatHintsMutex);
  auto it = CompatHints.find(name);
  if (it == CompatHints.end()) {
    return SDL_FALSE;
  }
  const std::string old = it->second.Value;
  const char* env = std::getenv(name);
  CompatHints.erase(it);
  auto cb = CompatHintCallbacks.find(name);
  if (cb != CompatHintCallbacks.end()) {
    for (const auto& entry : cb->second) {
      entry.Callback(entry.Userdata, name, old.c_str(), env);
    }
  }
  return SDL_TRUE;
}

extern "C" void SDL_ResetHints(void) {

  std::lock_guard<std::recursive_mutex> lock(CompatHintsMutex);
  CompatHints.clear();
}

extern "C" void SDL_ClearHints(void) {

  std::lock_guard<std::recursive_mutex> lock(CompatHintsMutex);
  CompatHints.clear();
}

extern "C" const char* SDL_GetHint(const char* name) {

  if (!name) {
    return nullptr;
  }
  std::lock_guard<std::recursive_mutex> lock(CompatHintsMutex);
  return CompatLookupHint(name);
}

extern "C" SDL_bool SDL_GetHintBoolean(const char* name, SDL_bool default_value) {

  const char* value = SDL_GetHint(name);
  if (!value || !*value) {
    return default_value;
  }
  return !(value[0] == '0' || strcasecmp(value, "false") == 0) ? SDL_TRUE : SDL_FALSE;
}

extern "C" void SDL_AddHintCallback(const char* name, SDL_HintCallback callback, void* userdata) {

  if (!name || !callback) {
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(CompatHintsMutex);
  CompatHintCallbacks[name].push_back(CompatHintCallbackEntry {callback, userdata});
  const char* current = CompatLookupHint(name);
  if (current) {
    callback(userdata, name, nullptr, current);
  }
}

extern "C" void SDL_DelHintCallback(const char* name, SDL_HintCallback callback, void* userdata) {

  if (!name || !callback) {
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(CompatHintsMutex);
  auto it = CompatHintCallbacks.find(name);
  if (it == CompatHintCallbacks.end()) {
    return;
  }
  auto& vec = it->second;
  for (auto entry = vec.begin(); entry != vec.end(); ++entry) {
    if (entry->Callback == callback && entry->Userdata == userdata) {
      vec.erase(entry);
      break;
    }
  }
  if (vec.empty()) {
    CompatHintCallbacks.erase(it);
  }
}

// ---- threads / mutex / semaphore / cond / TLS -------------------------------

struct SDL_mutex {
  pthread_mutex_t Handle;
};

struct SDL_semaphore {
  sem_t Handle;
};

struct SDL_cond {
  pthread_cond_t Handle;
};

struct SDL_Thread {
  pthread_t Handle {};
  SDL_ThreadFunction Fn {};
  void* Data {};
  int Status {};
  std::atomic<SDL_threadID> Tid {0};
  std::atomic<bool> Finished {false};
  std::atomic<bool> Detached {false};
  std::string Name;
};

extern "C" SDL_mutex* SDL_CreateMutex(void) {

  auto* mutex = new (std::nothrow) SDL_mutex;
  if (!mutex) {
    return nullptr;
  }
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex->Handle, &attr);
  pthread_mutexattr_destroy(&attr);
  return mutex;
}

extern "C" int SDL_LockMutex(SDL_mutex* mutex) {

  if (!mutex) {
    return SDL_SetError("Parameter 'mutex' is invalid");
  }
  return pthread_mutex_lock(&mutex->Handle) == 0 ? 0 : -1;
}

extern "C" int SDL_TryLockMutex(SDL_mutex* mutex) {

  if (!mutex) {
    return SDL_SetError("Parameter 'mutex' is invalid");
  }
  const int rc = pthread_mutex_trylock(&mutex->Handle);
  if (rc == 0) {
    return 0;
  }
  return rc == EBUSY ? SDL_MUTEX_TIMEDOUT : -1;
}

extern "C" int SDL_UnlockMutex(SDL_mutex* mutex) {

  if (!mutex) {
    return SDL_SetError("Parameter 'mutex' is invalid");
  }
  return pthread_mutex_unlock(&mutex->Handle) == 0 ? 0 : -1;
}

extern "C" void SDL_DestroyMutex(SDL_mutex* mutex) {

  if (mutex) {
    pthread_mutex_destroy(&mutex->Handle);
    delete mutex;
  }
}

extern "C" SDL_sem* SDL_CreateSemaphore(Uint32 initial_value) {

  auto* sem = new (std::nothrow) SDL_sem;
  if (!sem) {
    return nullptr;
  }
  if (sem_init(&sem->Handle, 0, initial_value) != 0) {
    delete sem;
    return nullptr;
  }
  return sem;
}

extern "C" void SDL_DestroySemaphore(SDL_sem* sem) {

  if (sem) {
    sem_destroy(&sem->Handle);
    delete sem;
  }
}

extern "C" int SDL_SemWait(SDL_sem* sem) {

  if (!sem) {
    return SDL_SetError("Parameter 'sem' is invalid");
  }
  while (sem_wait(&sem->Handle) == -1) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return 0;
}

extern "C" int SDL_SemTryWait(SDL_sem* sem) {

  if (!sem) {
    return SDL_SetError("Parameter 'sem' is invalid");
  }
  if (sem_trywait(&sem->Handle) == 0) {
    return 0;
  }
  return errno == EAGAIN ? SDL_MUTEX_TIMEDOUT : -1;
}

extern "C" int SDL_SemWaitTimeout(SDL_sem* sem, Uint32 timeout) {

  if (!sem) {
    return SDL_SetError("Parameter 'sem' is invalid");
  }
  if (timeout == SDL_MUTEX_MAXWAIT) {
    return SDL_SemWait(sem);
  }
  timespec ts {};
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += timeout / 1000;
  ts.tv_nsec += static_cast<long>(timeout % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000L;
  }
  while (sem_timedwait(&sem->Handle, &ts) == -1) {
    if (errno == EINTR) {
      continue;
    }
    return errno == ETIMEDOUT ? SDL_MUTEX_TIMEDOUT : -1;
  }
  return 0;
}

extern "C" int SDL_SemPost(SDL_sem* sem) {

  if (!sem) {
    return SDL_SetError("Parameter 'sem' is invalid");
  }
  return sem_post(&sem->Handle) == 0 ? 0 : -1;
}

extern "C" Uint32 SDL_SemValue(SDL_sem* sem) {

  if (!sem) {
    return 0;
  }
  int value = 0;
  sem_getvalue(&sem->Handle, &value);
  return value > 0 ? static_cast<Uint32>(value) : 0;
}

extern "C" SDL_cond* SDL_CreateCond(void) {

  auto* cond = new (std::nothrow) SDL_cond;
  if (!cond) {
    return nullptr;
  }
  pthread_cond_init(&cond->Handle, nullptr);
  return cond;
}

extern "C" void SDL_DestroyCond(SDL_cond* cond) {

  if (cond) {
    pthread_cond_destroy(&cond->Handle);
    delete cond;
  }
}

extern "C" int SDL_CondSignal(SDL_cond* cond) {

  if (!cond) {
    return SDL_SetError("Parameter 'cond' is invalid");
  }
  return pthread_cond_signal(&cond->Handle) == 0 ? 0 : -1;
}

extern "C" int SDL_CondBroadcast(SDL_cond* cond) {

  if (!cond) {
    return SDL_SetError("Parameter 'cond' is invalid");
  }
  return pthread_cond_broadcast(&cond->Handle) == 0 ? 0 : -1;
}

extern "C" int SDL_CondWait(SDL_cond* cond, SDL_mutex* mutex) {

  if (!cond || !mutex) {
    return SDL_SetError("Parameter 'cond'/'mutex' is invalid");
  }
  return pthread_cond_wait(&cond->Handle, &mutex->Handle) == 0 ? 0 : -1;
}

extern "C" int SDL_CondWaitTimeout(SDL_cond* cond, SDL_mutex* mutex, Uint32 ms) {

  if (!cond || !mutex) {
    return SDL_SetError("Parameter 'cond'/'mutex' is invalid");
  }
  timespec ts {};
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += static_cast<long>(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000L;
  }
  const int rc = pthread_cond_timedwait(&cond->Handle, &mutex->Handle, &ts);
  if (rc == 0) {
    return 0;
  }
  return rc == ETIMEDOUT ? SDL_MUTEX_TIMEDOUT : -1;
}

namespace {
void* CompatThreadProc(void* arg) {
  auto* self = static_cast<SDL_Thread*>(arg);
  self->Tid = static_cast<SDL_threadID>(syscall(SYS_gettid));
  if (!self->Name.empty()) {
    char truncated[16] {};
    std::strncpy(truncated, self->Name.c_str(), sizeof(truncated) - 1);
    pthread_setname_np(pthread_self(), truncated);
  }
  const int status = self->Fn(self->Data);
  self->Status = status;
  // Store Finished before reading Detached (seq_cst): pairs with the
  // store-then-load in SDL_DetachThread so exactly one side frees the struct.
  self->Finished.store(true);
  if (self->Detached.load()) {
    delete self;
  }
  return nullptr;
}
} // namespace

static SDL_Thread* CompatCreateThread(SDL_ThreadFunction fn, const char* name, size_t stacksize, void* data) {
  if (!fn) {
    SDL_SetError("Thread function is null");
    return nullptr;
  }
  auto* thread = new (std::nothrow) SDL_Thread;
  if (!thread) {
    return nullptr;
  }
  thread->Fn = fn;
  thread->Data = data;
  thread->Name = name ? name : "";
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  if (stacksize) {
    pthread_attr_setstacksize(&attr, stacksize);
  }
  const int rc = pthread_create(&thread->Handle, &attr, CompatThreadProc, thread);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    SDL_SetError("pthread_create failed: %d", rc);
    delete thread;
    return nullptr;
  }
  return thread;
}

extern "C" SDL_Thread* SDL_CreateThread(SDL_ThreadFunction fn, const char* name, void* data) {

  return CompatCreateThread(fn, name, 0, data);
}

extern "C" SDL_Thread* SDL_CreateThreadWithStackSize(SDL_ThreadFunction fn, const char* name,
                                                     const size_t stacksize, void* data) {

  return CompatCreateThread(fn, name, stacksize, data);
}

extern "C" const char* SDL_GetThreadName(SDL_Thread* thread) {

  if (!thread || thread->Name.empty()) {
    return nullptr;
  }
  return thread->Name.c_str();
}

extern "C" SDL_threadID SDL_ThreadID(void) {

  return static_cast<SDL_threadID>(syscall(SYS_gettid));
}

extern "C" SDL_threadID SDL_GetThreadID(SDL_Thread* thread) {

  return thread ? thread->Tid.load() : 0;
}

extern "C" int SDL_SetThreadPriority(SDL_ThreadPriority) {

  // Priority changes usually fail without CAP_SYS_NICE; not an error for callers.
  return 0;
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

extern "C" void SDL_DetachThread(SDL_Thread* thread) {

  if (!thread) {
    return;
  }
  pthread_detach(thread->Handle);
  // Pair with the store-then-load in CompatThreadProc: if the thread already
  // exited we free it here, otherwise the worker frees itself on exit.
  thread->Detached.store(true);
  if (thread->Finished.load()) {
    delete thread;
  }
}

namespace {
struct CompatTLSValue {
  const void* Value {};
  SDL_TLSDestructorCallback Destructor {};
};

void CompatTLSDestroy(void* ptr) {
  auto* value = static_cast<CompatTLSValue*>(ptr);
  if (value) {
    if (value->Destructor && value->Value) {
      value->Destructor(const_cast<void*>(value->Value));
    }
    delete value;
  }
}

std::mutex CompatTLSMutex;
std::vector<pthread_key_t> CompatTLSKeys;

bool CompatTLSGetKey(SDL_TLSID id, pthread_key_t* out) {
  if (id == 0 || id > CompatTLSKeys.size()) {
    return false;
  }
  *out = CompatTLSKeys[id - 1];
  return true;
}
} // namespace

extern "C" SDL_TLSID SDL_TLSCreate(void) {

  pthread_key_t key;
  if (pthread_key_create(&key, CompatTLSDestroy) != 0) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(CompatTLSMutex);
  CompatTLSKeys.push_back(key);
  return static_cast<SDL_TLSID>(CompatTLSKeys.size());
}

extern "C" void* SDL_TLSGet(SDL_TLSID id) {

  pthread_key_t key;
  {
    std::lock_guard<std::mutex> lock(CompatTLSMutex);
    if (!CompatTLSGetKey(id, &key)) {
      return nullptr;
    }
  }
  auto* value = static_cast<CompatTLSValue*>(pthread_getspecific(key));
  return value ? const_cast<void*>(value->Value) : nullptr;
}

extern "C" int SDL_TLSSet(SDL_TLSID id, const void* value, SDL_TLSDestructorCallback destructor) {

  pthread_key_t key;
  {
    std::lock_guard<std::mutex> lock(CompatTLSMutex);
    if (!CompatTLSGetKey(id, &key)) {
      return SDL_SetError("Parameter 'id' is invalid");
    }
  }
  auto* entry = static_cast<CompatTLSValue*>(pthread_getspecific(key));
  if (!value) {
    if (entry) {
      pthread_setspecific(key, nullptr);
      CompatTLSDestroy(entry);
    }
    return 0;
  }
  if (!entry) {
    entry = new (std::nothrow) CompatTLSValue;
    if (!entry) {
      return SDL_SetError("Out of memory");
    }
  }
  entry->Value = value;
  entry->Destructor = destructor;
  pthread_setspecific(key, entry);
  return 0;
}

extern "C" void SDL_TLSCleanup(void) {

  std::lock_guard<std::mutex> lock(CompatTLSMutex);
  for (pthread_key_t& key : CompatTLSKeys) {
    auto* entry = static_cast<CompatTLSValue*>(pthread_getspecific(key));
    if (entry) {
      pthread_setspecific(key, nullptr);
      CompatTLSDestroy(entry);
    }
  }
}

// ---- RWops -----------------------------------------------------------------
//
// SDL_RWops is a public struct in SDL2; this build's config (minimal) has no
// HAVE_STDIO_H, so hidden.stdio does not exist — FILE* is stashed in
// hidden.unknown.data1 with the autoclose flag in hidden.unknown.data2.

namespace {
Sint64 SDLCALL CompatRWFileSize(SDL_RWops* context) {
  auto* file = static_cast<FILE*>(context->hidden.unknown.data1);
  const off_t pos = ftello(file);
  if (fseeko(file, 0, SEEK_END) != 0) {
    return -1;
  }
  const off_t size = ftello(file);
  fseeko(file, pos, SEEK_SET);
  return static_cast<Sint64>(size);
}

Sint64 SDLCALL CompatRWFileSeek(SDL_RWops* context, Sint64 offset, int whence) {
  auto* file = static_cast<FILE*>(context->hidden.unknown.data1);
  const int origin = whence == RW_SEEK_SET ? SEEK_SET : whence == RW_SEEK_CUR ? SEEK_CUR : SEEK_END;
  if (fseeko(file, static_cast<off_t>(offset), origin) != 0) {
    return -1;
  }
  return static_cast<Sint64>(ftello(file));
}

size_t SDLCALL CompatRWFileRead(SDL_RWops* context, void* ptr, size_t size, size_t maxnum) {
  auto* file = static_cast<FILE*>(context->hidden.unknown.data1);
  return std::fread(ptr, size, maxnum, file);
}

size_t SDLCALL CompatRWFileWrite(SDL_RWops* context, const void* ptr, size_t size, size_t num) {
  auto* file = static_cast<FILE*>(context->hidden.unknown.data1);
  return std::fwrite(ptr, size, num, file);
}

int SDLCALL CompatRWFileClose(SDL_RWops* context) {
  auto* file = static_cast<FILE*>(context->hidden.unknown.data1);
  const bool autoclose = context->hidden.unknown.data2 != nullptr;
  int rc = 0;
  if (autoclose && file) {
    rc = std::fclose(file) == 0 ? 0 : -1;
  }
  SDL_free(context);
  return rc;
}

Sint64 SDLCALL CompatRWMemSize(SDL_RWops* context) {
  return static_cast<Sint64>(context->hidden.mem.stop - context->hidden.mem.base);
}

Sint64 SDLCALL CompatRWMemSeek(SDL_RWops* context, Sint64 offset, int whence) {
  Uint8* base = context->hidden.mem.base;
  Sint64 target = 0;
  switch (whence) {
  case RW_SEEK_SET: target = offset; break;
  case RW_SEEK_CUR: target = static_cast<Sint64>(context->hidden.mem.here - base) + offset; break;
  case RW_SEEK_END: target = static_cast<Sint64>(context->hidden.mem.stop - base) + offset; break;
  default: return -1;
  }
  if (target < 0 || target > static_cast<Sint64>(context->hidden.mem.stop - base)) {
    return -1;
  }
  context->hidden.mem.here = base + target;
  return target;
}

size_t SDLCALL CompatRWMemRead(SDL_RWops* context, void* ptr, size_t size, size_t maxnum) {
  const size_t avail = static_cast<size_t>(context->hidden.mem.stop - context->hidden.mem.here);
  if (size == 0 || maxnum == 0) {
    return 0;
  }
  size_t count = avail / size;
  if (count > maxnum) {
    count = maxnum;
  }
  const size_t bytes = count * size;
  std::memcpy(ptr, context->hidden.mem.here, bytes);
  context->hidden.mem.here += bytes;
  return count;
}

size_t SDLCALL CompatRWMemWrite(SDL_RWops* context, const void* ptr, size_t size, size_t num) {
  const size_t avail = static_cast<size_t>(context->hidden.mem.stop - context->hidden.mem.here);
  if (size == 0 || num == 0) {
    return 0;
  }
  size_t count = avail / size;
  if (count > num) {
    count = num;
  }
  const size_t bytes = count * size;
  std::memcpy(context->hidden.mem.here, ptr, bytes);
  context->hidden.mem.here += bytes;
  return count;
}

size_t SDLCALL CompatRWConstMemWrite(SDL_RWops*, const void*, size_t, size_t) {
  SDL_SetError("Can't write to read-only memory stream");
  return 0;
}

int SDLCALL CompatRWMemClose(SDL_RWops* context) {
  SDL_free(context);
  return 0;
}
} // namespace

extern "C" SDL_RWops* SDL_AllocRW(void) {

  auto* area = static_cast<SDL_RWops*>(SDL_malloc(sizeof(SDL_RWops)));
  if (area) {
    std::memset(area, 0, sizeof(*area));
  }
  return area;
}

extern "C" void SDL_FreeRW(SDL_RWops* area) {  SDL_free(area); }

extern "C" SDL_RWops* SDL_RWFromFile(const char* file, const char* mode) {

  if (!file || !mode) {
    SDL_SetError("Parameter 'file'/'mode' is invalid");
    return nullptr;
  }
  FILE* handle = std::fopen(file, mode);
  if (!handle) {
    SDL_SetError("Couldn't open %s: %s", file, strerror(errno));
    return nullptr;
  }
  SDL_RWops* rwops = SDL_AllocRW();
  if (!rwops) {
    std::fclose(handle);
    return nullptr;
  }
  rwops->size = CompatRWFileSize;
  rwops->seek = CompatRWFileSeek;
  rwops->read = CompatRWFileRead;
  rwops->write = CompatRWFileWrite;
  rwops->close = CompatRWFileClose;
  rwops->type = SDL_RWOPS_STDFILE;
  rwops->hidden.unknown.data1 = handle;
  rwops->hidden.unknown.data2 = reinterpret_cast<void*>(1); // autoclose
  return rwops;
}

extern "C" SDL_RWops* SDL_RWFromFP(void* fp, SDL_bool autoclose) {

  if (!fp) {
    SDL_SetError("Parameter 'fp' is invalid");
    return nullptr;
  }
  SDL_RWops* rwops = SDL_AllocRW();
  if (!rwops) {
    return nullptr;
  }
  rwops->size = CompatRWFileSize;
  rwops->seek = CompatRWFileSeek;
  rwops->read = CompatRWFileRead;
  rwops->write = CompatRWFileWrite;
  rwops->close = CompatRWFileClose;
  rwops->type = SDL_RWOPS_STDFILE;
  rwops->hidden.unknown.data1 = fp;
  rwops->hidden.unknown.data2 = autoclose ? reinterpret_cast<void*>(1) : nullptr;
  return rwops;
}

extern "C" SDL_RWops* SDL_RWFromMem(void* mem, int size) {

  if (!mem || size < 0) {
    SDL_SetError("Parameter 'mem' is invalid");
    return nullptr;
  }
  SDL_RWops* rwops = SDL_AllocRW();
  if (!rwops) {
    return nullptr;
  }
  rwops->size = CompatRWMemSize;
  rwops->seek = CompatRWMemSeek;
  rwops->read = CompatRWMemRead;
  rwops->write = CompatRWMemWrite;
  rwops->close = CompatRWMemClose;
  rwops->type = SDL_RWOPS_MEMORY;
  rwops->hidden.mem.base = static_cast<Uint8*>(mem);
  rwops->hidden.mem.here = rwops->hidden.mem.base;
  rwops->hidden.mem.stop = rwops->hidden.mem.base + size;
  return rwops;
}

extern "C" SDL_RWops* SDL_RWFromConstMem(const void* mem, int size) {

  if (!mem || size < 0) {
    SDL_SetError("Parameter 'mem' is invalid");
    return nullptr;
  }
  SDL_RWops* rwops = SDL_AllocRW();
  if (!rwops) {
    return nullptr;
  }
  rwops->size = CompatRWMemSize;
  rwops->seek = CompatRWMemSeek;
  rwops->read = CompatRWMemRead;
  rwops->write = CompatRWConstMemWrite;
  rwops->close = CompatRWMemClose;
  rwops->type = SDL_RWOPS_MEMORY_RO;
  rwops->hidden.mem.base = const_cast<Uint8*>(static_cast<const Uint8*>(mem));
  rwops->hidden.mem.here = rwops->hidden.mem.base;
  rwops->hidden.mem.stop = rwops->hidden.mem.base + size;
  return rwops;
}

extern "C" Sint64 SDL_RWsize(SDL_RWops* context) {

  return context && context->size ? context->size(context) : -1;
}

extern "C" Sint64 SDL_RWseek(SDL_RWops* context, Sint64 offset, int whence) {

  if (!context || !context->seek) {
    return -1;
  }
  return context->seek(context, offset, whence);
}

extern "C" Sint64 SDL_RWtell(SDL_RWops* context) {

  return SDL_RWseek(context, 0, RW_SEEK_CUR);
}

extern "C" size_t SDL_RWread(SDL_RWops* context, void* ptr, size_t size, size_t maxnum) {

  if (!context || !context->read) {
    return 0;
  }
  return context->read(context, ptr, size, maxnum);
}

extern "C" size_t SDL_RWwrite(SDL_RWops* context, const void* ptr, size_t size, size_t num) {

  if (!context || !context->write) {
    return 0;
  }
  return context->write(context, ptr, size, num);
}

extern "C" int SDL_RWclose(SDL_RWops* context) {

  if (!context || !context->close) {
    return -1;
  }
  return context->close(context);
}

extern "C" void* SDL_LoadFile_RW(SDL_RWops* src, size_t* datasize, int freesrc) {

  if (datasize) {
    *datasize = 0;
  }
  if (!src) {
    SDL_SetError("Parameter 'src' is invalid");
    return nullptr;
  }
  Sint64 size = SDL_RWsize(src);
  std::vector<Uint8> data;
  if (size >= 0) {
    data.resize(static_cast<size_t>(size));
    size_t got = SDL_RWread(src, data.data(), 1, data.size());
    if (got != data.size()) {
      data.resize(got);
    }
  } else {
    Uint8 chunk[8192];
    size_t got;
    while ((got = SDL_RWread(src, chunk, 1, sizeof(chunk))) > 0) {
      data.insert(data.end(), chunk, chunk + got);
    }
  }
  if (freesrc) {
    SDL_RWclose(src);
  }
  Uint8* out = static_cast<Uint8*>(SDL_malloc(data.size() + 1));
  if (!out) {
    return nullptr;
  }
  if (!data.empty()) {
    std::memcpy(out, data.data(), data.size());
  }
  out[data.size()] = 0;
  if (datasize) {
    *datasize = data.size();
  }
  return out;
}

extern "C" void* SDL_LoadFile(const char* file, size_t* datasize) {

  SDL_RWops* src = SDL_RWFromFile(file, "rb");
  if (!src) {
    if (datasize) {
      *datasize = 0;
    }
    return nullptr;
  }
  return SDL_LoadFile_RW(src, datasize, 1);
}

extern "C" Uint8 SDL_ReadU8(SDL_RWops* src) {

  Uint8 value = 0;
  SDL_RWread(src, &value, 1, 1);
  return value;
}

extern "C" Uint16 SDL_ReadLE16(SDL_RWops* src) {

  Uint8 b[2] {};
  SDL_RWread(src, b, 1, 2);
  return static_cast<Uint16>(b[0] | (static_cast<Uint16>(b[1]) << 8));
}

extern "C" Uint16 SDL_ReadBE16(SDL_RWops* src) {

  Uint8 b[2] {};
  SDL_RWread(src, b, 1, 2);
  return static_cast<Uint16>((static_cast<Uint16>(b[0]) << 8) | b[1]);
}

extern "C" Uint32 SDL_ReadLE32(SDL_RWops* src) {

  Uint8 b[4] {};
  SDL_RWread(src, b, 1, 4);
  return static_cast<Uint32>(b[0]) | (static_cast<Uint32>(b[1]) << 8) | (static_cast<Uint32>(b[2]) << 16) |
         (static_cast<Uint32>(b[3]) << 24);
}

extern "C" Uint32 SDL_ReadBE32(SDL_RWops* src) {

  Uint8 b[4] {};
  SDL_RWread(src, b, 1, 4);
  return (static_cast<Uint32>(b[0]) << 24) | (static_cast<Uint32>(b[1]) << 16) |
         (static_cast<Uint32>(b[2]) << 8) | static_cast<Uint32>(b[3]);
}

extern "C" Uint64 SDL_ReadLE64(SDL_RWops* src) {

  Uint8 b[8] {};
  SDL_RWread(src, b, 1, 8);
  Uint64 v = 0;
  for (int i = 7; i >= 0; --i) {
    v = (v << 8) | b[i];
  }
  return v;
}

extern "C" Uint64 SDL_ReadBE64(SDL_RWops* src) {

  Uint8 b[8] {};
  SDL_RWread(src, b, 1, 8);
  Uint64 v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8) | b[i];
  }
  return v;
}

extern "C" size_t SDL_WriteU8(SDL_RWops* dst, Uint8 value) {

  return SDL_RWwrite(dst, &value, 1, 1);
}

extern "C" size_t SDL_WriteLE16(SDL_RWops* dst, Uint16 value) {

  const Uint8 b[2] {static_cast<Uint8>(value), static_cast<Uint8>(value >> 8)};
  return SDL_RWwrite(dst, b, 1, 2);
}

extern "C" size_t SDL_WriteBE16(SDL_RWops* dst, Uint16 value) {

  const Uint8 b[2] {static_cast<Uint8>(value >> 8), static_cast<Uint8>(value)};
  return SDL_RWwrite(dst, b, 1, 2);
}

extern "C" size_t SDL_WriteLE32(SDL_RWops* dst, Uint32 value) {

  const Uint8 b[4] {static_cast<Uint8>(value), static_cast<Uint8>(value >> 8),
                    static_cast<Uint8>(value >> 16), static_cast<Uint8>(value >> 24)};
  return SDL_RWwrite(dst, b, 1, 4);
}

extern "C" size_t SDL_WriteBE32(SDL_RWops* dst, Uint32 value) {

  const Uint8 b[4] {static_cast<Uint8>(value >> 24), static_cast<Uint8>(value >> 16),
                    static_cast<Uint8>(value >> 8), static_cast<Uint8>(value)};
  return SDL_RWwrite(dst, b, 1, 4);
}

extern "C" size_t SDL_WriteLE64(SDL_RWops* dst, Uint64 value) {

  Uint8 b[8];
  for (int i = 0; i < 8; ++i) {
    b[i] = static_cast<Uint8>(value >> (i * 8));
  }
  return SDL_RWwrite(dst, b, 1, 8);
}

extern "C" size_t SDL_WriteBE64(SDL_RWops* dst, Uint64 value) {

  Uint8 b[8];
  for (int i = 0; i < 8; ++i) {
    b[i] = static_cast<Uint8>(value >> ((7 - i) * 8));
  }
  return SDL_RWwrite(dst, b, 1, 8);
}

// ---- filesystem paths ------------------------------------------------------

extern "C" char* SDL_GetBasePath(void) {

  char path[PATH_MAX] {};
  const ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len <= 0) {
    return SDL_strdup("./");
  }
  path[len] = '\0';
  char* slash = std::strrchr(path, '/');
  if (slash) {
    slash[1] = '\0';
  }
  return SDL_strdup(path);
}

extern "C" char* SDL_GetPrefPath(const char* org, const char* app) {

  if (!app || !*app) {
    SDL_SetError("Parameter 'app' is invalid");
    return nullptr;
  }
  char base[PATH_MAX];
  const char* xdg = std::getenv("XDG_DATA_HOME");
  if (xdg && *xdg) {
    std::snprintf(base, sizeof(base), "%s", xdg);
  } else {
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
      SDL_SetError("HOME is not set");
      return nullptr;
    }
    std::snprintf(base, sizeof(base), "%s/.local/share", home);
  }
  char path[PATH_MAX];
  if (org && *org) {
    std::snprintf(path, sizeof(path), "%s/%s/%s/", base, org, app);
  } else {
    std::snprintf(path, sizeof(path), "%s/%s/", base, app);
  }
  // mkdir -p: create each intermediate component.
  for (char* p = path + 1; *p; ++p) {
    if (*p == '/') {
      *p = '\0';
      mkdir(path, 0755);
      *p = '/';
    }
  }
  return SDL_strdup(path);
}

// ---- CPU info ----------------------------------------------------------------

extern "C" int SDL_GetCPUCount(void) {

  const long count = sysconf(_SC_NPROCESSORS_ONLN);
  return count > 0 ? static_cast<int>(count) : 1;
}

extern "C" int SDL_GetCPUCacheLineSize(void) {  return 64; }

extern "C" SDL_bool SDL_HasRDTSC(void) {  return SDL_TRUE; }
extern "C" SDL_bool SDL_HasAltiVec(void) {  return SDL_FALSE; }
extern "C" SDL_bool SDL_HasMMX(void) {  return SDL_TRUE; }
extern "C" SDL_bool SDL_Has3DNow(void) {  return SDL_FALSE; }

extern "C" SDL_bool SDL_HasSSE(void) {

#if defined(__x86_64__) || defined(__i386__)
  return __builtin_cpu_supports("sse") ? SDL_TRUE : SDL_FALSE;
#else
  return SDL_FALSE;
#endif
}

extern "C" SDL_bool SDL_HasSSE2(void) {

#if defined(__x86_64__)
  return SDL_TRUE;
#elif defined(__i386__)
  return __builtin_cpu_supports("sse2") ? SDL_TRUE : SDL_FALSE;
#else
  return SDL_FALSE;
#endif
}

extern "C" SDL_bool SDL_HasSSE3(void) {

#if defined(__x86_64__) || defined(__i386__)
  return __builtin_cpu_supports("sse3") ? SDL_TRUE : SDL_FALSE;
#else
  return SDL_FALSE;
#endif
}

extern "C" SDL_bool SDL_HasSSE41(void) {

#if defined(__x86_64__) || defined(__i386__)
  return __builtin_cpu_supports("sse4.1") ? SDL_TRUE : SDL_FALSE;
#else
  return SDL_FALSE;
#endif
}

extern "C" SDL_bool SDL_HasSSE42(void) {

#if defined(__x86_64__) || defined(__i386__)
  return __builtin_cpu_supports("sse4.2") ? SDL_TRUE : SDL_FALSE;
#else
  return SDL_FALSE;
#endif
}

extern "C" SDL_bool SDL_HasAVX(void) {

#if defined(__x86_64__) || defined(__i386__)
  return __builtin_cpu_supports("avx") ? SDL_TRUE : SDL_FALSE;
#else
  return SDL_FALSE;
#endif
}

extern "C" SDL_bool SDL_HasAVX2(void) {

#if defined(__x86_64__) || defined(__i386__)
  return __builtin_cpu_supports("avx2") ? SDL_TRUE : SDL_FALSE;
#else
  return SDL_FALSE;
#endif
}

extern "C" SDL_bool SDL_HasAVX512F(void) {

#if defined(__x86_64__) || defined(__i386__)
  return __builtin_cpu_supports("avx512f") ? SDL_TRUE : SDL_FALSE;
#else
  return SDL_FALSE;
#endif
}

extern "C" SDL_bool SDL_HasARMSIMD(void) {  return SDL_FALSE; }
extern "C" SDL_bool SDL_HasNEON(void) {  return SDL_FALSE; }
extern "C" SDL_bool SDL_HasLSX(void) {  return SDL_FALSE; }
extern "C" SDL_bool SDL_HasLASX(void) {  return SDL_FALSE; }

extern "C" int SDL_GetSystemRAM(void) {

  const long pages = sysconf(_SC_PHYS_PAGES);
  const long pagesize = sysconf(_SC_PAGESIZE);
  if (pages <= 0 || pagesize <= 0) {
    return 4096;
  }
  const Sint64 mb = (static_cast<Sint64>(pages) * pagesize) / (1024 * 1024);
  return mb > INT_MAX ? INT_MAX : static_cast<int>(mb);
}

// ---- audio ------------------------------------------------------------------
//
// SDL2's device model: SDL_OpenAudioDevice() returns a device ID and the device
// starts paused; SDL_PauseAudioDevice(dev, 0) unpauses. Callback devices pull
// PCM via spec->callback each period; non-callback devices get fed via
// SDL_QueueAudio(). Output goes through the guest OpenAL thunk when available
// and is dropped (still paced in real time) otherwise, same as the SDL3 compat.

namespace {
constexpr int CompatMixFramesFallback = 1024;

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
  fprintf(stderr, "[SDL2 audio] %s\n", message);
}

int CompatBytesPerSample(SDL_AudioFormat format) {
  return SDL_AUDIO_BITSIZE(format) / 8;
}

int CompatBytesPerFrame(const SDL_AudioSpec& spec) {
  const int channels = spec.channels > 0 ? spec.channels : 1;
  return CompatBytesPerSample(spec.format) * channels;
}

// Playback goes through the guest OpenAL thunk (host OpenAL Soft). If it cannot
// be loaded the stream still runs in real time and its data is dropped.
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
      fprintf(stderr, "[SDL2 thunk] audio: OpenAL not available (%s); output is silent\n", dlerror());
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
      fprintf(stderr, "[SDL2 thunk] audio: OpenAL thunk is missing entry points; output is silent\n");
      return;
    }
    void* device = al.alcOpenDevice(nullptr);
    void* context = device ? al.alcCreateContext(device, nullptr) : nullptr;
    CompatAudioLog("audio: OpenAL device=%p context=%p", device, context);
    if (!context || !al.alcMakeContextCurrent(context)) {
      fprintf(stderr, "[SDL2 thunk] audio: OpenAL device/context creation failed; output is silent\n");
      return;
    }
    al.Device = device;
    al.Context = context;
    al.Ready = true;
    CompatAudioLog("audio: OpenAL ready device=%p context=%p alcError=0x%x", device, context,
                   al.alcGetError(device));
    fprintf(stderr, "[SDL2 thunk] audio: OpenAL output ready\n");
  });
  return al;
}

// Convert whatever the guest mixed into interleaved stereo/mono signed 16-bit.
std::vector<int16_t> CompatToS16(const uint8_t* in, size_t inBytes, const SDL_AudioSpec& spec,
                               int* outChannels) {
  const int channels = spec.channels > 0 ? spec.channels : 2;
  const int outCh = channels == 1 ? 1 : 2;
  *outChannels = outCh;
  const int bytes = CompatBytesPerSample(spec.format);
  const size_t frames = bytes > 0 ? inBytes / (static_cast<size_t>(bytes) * channels) : 0;
  std::vector<int16_t> out(frames * outCh);
  auto sample = [&](size_t frame, int ch) -> int16_t {
    const uint8_t* p = in + (frame * channels + ch) * bytes;
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
} // namespace

struct CompatAudioDevice {
  SDL_AudioDeviceID Id {};
  SDL_AudioSpec Spec {};
  std::atomic<bool> Paused {true}; // SDL2 devices open in the paused state
  std::atomic<bool> Stop {false};
  std::thread Worker;
  std::recursive_mutex Lock;   // SDL_LockAudioDevice; held by worker while callback runs
  std::mutex QueueLock;        // protects Pending for SDL_QueueAudio devices
  std::vector<Uint8> Pending;
};

namespace {
std::mutex AudioDeviceMutex;
std::map<SDL_AudioDeviceID, CompatAudioDevice*> AudioDevices;
SDL_AudioDeviceID NextAudioDeviceId = 2; // 1 is reserved for legacy SDL_OpenAudio

void CompatAudioLoop(CompatAudioDevice* dev) {
  const int freq = dev->Spec.freq > 0 ? dev->Spec.freq : 48000;
  const int frames = dev->Spec.samples > 0 ? dev->Spec.samples : CompatMixFramesFallback;
  const int bytesPerFrame = CompatBytesPerFrame(dev->Spec);
  const size_t wanted = static_cast<size_t>(frames) * bytesPerFrame;
  const auto period = std::chrono::microseconds(1000000LL * frames / freq);
  CompatAudioLog("audio worker starting dev=%u freq=%d format=0x%x channels=%d samples=%d callback=%p",
                 dev->Id, dev->Spec.freq, dev->Spec.format, dev->Spec.channels, dev->Spec.samples,
                 reinterpret_cast<void*>(dev->Spec.callback));
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
  std::vector<Uint8> mixbuf(wanted);
  auto next = std::chrono::steady_clock::now();
  Uint64 loops = 0;
  Uint64 callbackCalls = 0;
  Uint64 dataBytes = 0;
  Uint64 queuedBuffers = 0;
  Uint64 playStarts = 0;
  Uint64 nonzeroSamples = 0;
  int lastPeak = 0;
  int lastAlError = 0;
  while (!dev->Stop.load()) {
    if (dev->Paused.load()) {
      next = std::chrono::steady_clock::now();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    next += period;
    const Uint8* data = mixbuf.data();
    std::vector<Uint8> drained;
    if (dev->Spec.callback) {
      std::lock_guard<std::recursive_mutex> lock(dev->Lock);
      std::memset(mixbuf.data(), dev->Spec.silence, wanted);
      dev->Spec.callback(dev->Spec.userdata, mixbuf.data(), static_cast<int>(wanted));
      ++callbackCalls;
    } else {
      std::lock_guard<std::mutex> lock(dev->QueueLock);
      const size_t take = dev->Pending.size() < wanted ? dev->Pending.size() : wanted;
      drained.assign(dev->Pending.begin(), dev->Pending.begin() + take);
      dev->Pending.erase(dev->Pending.begin(), dev->Pending.begin() + take);
      if (drained.size() < wanted) {
        drained.resize(wanted, dev->Spec.silence);
      }
      data = drained.data();
    }
    dataBytes += wanted;
    if (al.Ready && source) {
      int processed = 0;
      al.alGetSourcei(source, 0x1016 /* AL_BUFFERS_PROCESSED */, &processed);
      while (processed-- > 0) {
        unsigned done = 0;
        al.alSourceUnqueueBuffers(source, 1, &done);
        freeBuffers.push_back(done);
      }
      if (!freeBuffers.empty()) {
        int outCh = 2;
        const std::vector<int16_t> pcm = CompatToS16(data, wanted, dev->Spec, &outCh);
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
          al.alBufferData(buf, outCh == 1 ? 0x1101 : 0x1103, pcm.data(),
                          static_cast<int>(pcm.size() * sizeof(int16_t)), freq);
          al.alSourceQueueBuffers(source, 1, &buf);
          const int alErr = al.alGetError();
          if (alErr != 0) {
            lastAlError = alErr;
          }
          ++queuedBuffers;
          if (queuedBuffers <= 3 || queuedBuffers % 64 == 0) {
            CompatAudioLog("audio queued buffer=%u count=%llu samples=%zu nonzero=%zu peak=%d alErr=0x%x "
                           "channels=%d freq=%d",
                           buf, static_cast<unsigned long long>(queuedBuffers), pcm.size(), nonzero, peak,
                           alErr, outCh, freq);
          }
        }
      }
      int queued = 0;
      int state = 0;
      al.alGetSourcei(source, 0x1015 /* AL_BUFFERS_QUEUED */, &queued);
      al.alGetSourcei(source, 0x1010 /* AL_SOURCE_STATE */, &state);
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
      CompatAudioLog("audio worker loop=%llu callbacks=%llu dataBytes=%llu queued=%llu nonzero=%llu "
                     "peak=%d alErr=0x%x",
                     static_cast<unsigned long long>(loops), static_cast<unsigned long long>(callbackCalls),
                     static_cast<unsigned long long>(dataBytes),
                     static_cast<unsigned long long>(queuedBuffers),
                     static_cast<unsigned long long>(nonzeroSamples), lastPeak, lastAlError);
    }
    std::this_thread::sleep_until(next);
  }
  CompatAudioLog("audio worker exiting dev=%u loops=%llu callbacks=%llu dataBytes=%llu queued=%llu plays=%llu "
                 "nonzero=%llu alErr=0x%x",
                 dev->Id, static_cast<unsigned long long>(loops),
                 static_cast<unsigned long long>(callbackCalls), static_cast<unsigned long long>(dataBytes),
                 static_cast<unsigned long long>(queuedBuffers),
                 static_cast<unsigned long long>(playStarts),
                 static_cast<unsigned long long>(nonzeroSamples), lastAlError);
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

CompatAudioDevice* CompatFindDevice(SDL_AudioDeviceID dev) {
  auto it = AudioDevices.find(dev);
  return it != AudioDevices.end() ? it->second : nullptr;
}

void CompatFillSpec(SDL_AudioSpec* spec) {
  // Normalize unset fields: real SDL lets the driver fill these, and callers
  // (fmod_SDL) leave channels=0 expecting a sane default in `obtained`.
  if (spec->freq <= 0) {
    spec->freq = 48000;
  }
  if (spec->format == 0) {
    spec->format = AUDIO_F32SYS;
  }
  if (spec->channels <= 0) {
    spec->channels = 2;
  }
  spec->silence = SDL_AUDIO_ISUNSIGNED(spec->format) ? 0x80 : 0;
  if (spec->samples == 0) {
    spec->samples = CompatMixFramesFallback;
  }
  spec->size = static_cast<Uint32>(CompatBytesPerFrame(*spec)) * spec->samples;
  spec->padding = 0;
}

CompatAudioDevice* CompatOpenDeviceLocked(const SDL_AudioSpec* desired, SDL_AudioSpec* obtained) {
  auto* dev = new (std::nothrow) CompatAudioDevice;
  if (!dev) {
    return nullptr;
  }
  dev->Spec = *desired;
  CompatFillSpec(&dev->Spec);
  if (obtained) {
    *obtained = dev->Spec;
  }
  dev->Worker = std::thread(CompatAudioLoop, dev);
  return dev;
}

void CompatCloseDevice(CompatAudioDevice* dev) {
  if (!dev) {
    return;
  }
  dev->Stop = true;
  if (dev->Worker.joinable()) {
    dev->Worker.join();
  }
  delete dev;
}
} // namespace

extern "C" int SDL_GetNumAudioDrivers(void) {  return 1; }

extern "C" const char* SDL_GetAudioDriver(int index) {  return index == 0 ? "android" : nullptr; }

extern "C" const char* SDL_GetCurrentAudioDriver(void) {  return "android"; }

extern "C" int SDL_AudioInit(const char* driver_name) {

  (void)driver_name;
  return 0;
}

extern "C" void SDL_AudioQuit(void) {

  std::vector<CompatAudioDevice*> dead;
  {
    std::lock_guard<std::mutex> lock(AudioDeviceMutex);
    for (auto& entry : AudioDevices) {
      dead.push_back(entry.second);
    }
    AudioDevices.clear();
  }
  for (CompatAudioDevice* dev : dead) {
    CompatCloseDevice(dev);
  }
}

extern "C" int SDL_GetNumAudioDevices(int iscapture) {

  return iscapture ? 0 : 1;
}

extern "C" const char* SDL_GetAudioDeviceName(int index, int iscapture) {

  if (iscapture || index != 0) {
    SDL_SetError("No such audio device");
    return nullptr;
  }
  return "VEXA OpenAL";
}

extern "C" int SDL_GetAudioDeviceSpec(int index, int iscapture, SDL_AudioSpec* spec) {

  if (iscapture || index != 0 || !spec) {
    return SDL_SetError("No such audio device");
  }
  std::memset(spec, 0, sizeof(*spec));
  spec->freq = 48000;
  spec->format = AUDIO_F32SYS;
  spec->channels = 2;
  spec->samples = CompatMixFramesFallback;
  CompatFillSpec(spec);
  return 0;
}

extern "C" int SDL_GetDefaultAudioInfo(char** name, SDL_AudioSpec* spec, int iscapture) {

  if (iscapture) {
    return SDL_SetError("No default capture device");
  }
  if (name) {
    *name = SDL_strdup("VEXA OpenAL");
  }
  if (spec) {
    std::memset(spec, 0, sizeof(*spec));
    spec->freq = 48000;
    spec->format = AUDIO_F32SYS;
    spec->channels = 2;
    spec->samples = CompatMixFramesFallback;
    CompatFillSpec(spec);
  }
  return 0;
}

extern "C" SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int iscapture,
                                                 const SDL_AudioSpec* desired, SDL_AudioSpec* obtained,
                                                 int allowed_changes) {

  (void)allowed_changes; // we satisfy whatever was requested; nothing to change
  if (iscapture) {
    SDL_SetError("Audio capture is not supported");
    return 0;
  }
  if (!desired) {
    SDL_SetError("Parameter 'desired' is invalid");
    return 0;
  }
  if (device && *device && std::strcmp(device, "VEXA OpenAL") != 0) {
    CompatAudioLog("SDL_OpenAudioDevice: unknown device name '%s'; using default", device);
  }
  std::lock_guard<std::mutex> lock(AudioDeviceMutex);
  CompatAudioDevice* dev = CompatOpenDeviceLocked(desired, obtained);
  if (!dev) {
    SDL_SetError("Out of memory");
    return 0;
  }
  dev->Id = NextAudioDeviceId++;
  AudioDevices[dev->Id] = dev;
  CompatAudioLog("SDL_OpenAudioDevice dev=%u freq=%d format=0x%x channels=%d samples=%d callback=%p",
                 dev->Id, dev->Spec.freq, dev->Spec.format, dev->Spec.channels, dev->Spec.samples,
                 reinterpret_cast<void*>(dev->Spec.callback));
  return dev->Id;
}

extern "C" int SDL_OpenAudio(SDL_AudioSpec* desired, SDL_AudioSpec* obtained) {

  if (!desired) {
    return SDL_SetError("Parameter 'desired' is invalid");
  }
  std::lock_guard<std::mutex> lock(AudioDeviceMutex);
  if (AudioDevices.count(1)) {
    return SDL_SetError("Audio device is already opened");
  }
  CompatAudioDevice* dev = CompatOpenDeviceLocked(desired, obtained);
  if (!dev) {
    return SDL_SetError("Out of memory");
  }
  dev->Id = 1;
  AudioDevices[1] = dev;
  return 0;
}

extern "C" void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on) {

  std::lock_guard<std::mutex> lock(AudioDeviceMutex);
  CompatAudioDevice* device = CompatFindDevice(dev);
  if (device) {
    device->Paused = pause_on != 0;
    CompatAudioLog("SDL_PauseAudioDevice dev=%u paused=%d", dev, pause_on);
  }
}

extern "C" void SDL_PauseAudio(int pause_on) {  SDL_PauseAudioDevice(1, pause_on); }

extern "C" SDL_AudioStatus SDL_GetAudioDeviceStatus(SDL_AudioDeviceID dev) {

  std::lock_guard<std::mutex> lock(AudioDeviceMutex);
  CompatAudioDevice* device = CompatFindDevice(dev);
  if (!device) {
    return SDL_AUDIO_STOPPED;
  }
  return device->Paused.load() ? SDL_AUDIO_PAUSED : SDL_AUDIO_PLAYING;
}

extern "C" SDL_AudioStatus SDL_GetAudioStatus(void) {  return SDL_GetAudioDeviceStatus(1); }

extern "C" void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) {

  CompatAudioDevice* device = nullptr;
  {
    std::lock_guard<std::mutex> lock(AudioDeviceMutex);
    auto it = AudioDevices.find(dev);
    if (it != AudioDevices.end()) {
      device = it->second;
      AudioDevices.erase(it);
    }
  }
  CompatAudioLog("SDL_CloseAudioDevice dev=%u device=%p", dev, device);
  CompatCloseDevice(device);
}

extern "C" void SDL_CloseAudio(void) {  SDL_CloseAudioDevice(1); }

extern "C" void SDL_LockAudioDevice(SDL_AudioDeviceID dev) {

  // Look up under the map lock, then release it before taking the device lock:
  // the audio worker holds the device lock while the app callback runs, and the
  // callback may call SDL_QueueAudio & friends which need the map lock.
  CompatAudioDevice* device;
  {
    std::lock_guard<std::mutex> lock(AudioDeviceMutex);
    device = CompatFindDevice(dev);
  }
  if (device) {
    device->Lock.lock();
  }
}

extern "C" void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev) {

  CompatAudioDevice* device;
  {
    std::lock_guard<std::mutex> lock(AudioDeviceMutex);
    device = CompatFindDevice(dev);
  }
  if (device) {
    device->Lock.unlock();
  }
}

extern "C" void SDL_LockAudio(void) {  SDL_LockAudioDevice(1); }
extern "C" void SDL_UnlockAudio(void) {  SDL_UnlockAudioDevice(1); }

extern "C" int SDL_QueueAudio(SDL_AudioDeviceID dev, const void* data, Uint32 len) {

  if (!data || len == 0) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(AudioDeviceMutex);
  CompatAudioDevice* device = CompatFindDevice(dev);
  if (!device) {
    return SDL_SetError("Invalid audio device ID");
  }
  std::lock_guard<std::mutex> qlock(device->QueueLock);
  const auto* bytes = static_cast<const Uint8*>(data);
  device->Pending.insert(device->Pending.end(), bytes, bytes + len);
  return 0;
}

extern "C" Uint32 SDL_DequeueAudio(SDL_AudioDeviceID dev, void* data, Uint32 len) {

  // Only capture devices produce dequeued audio; capture is unsupported.
  (void)dev;
  (void)data;
  (void)len;
  return 0;
}

extern "C" Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID dev) {

  std::lock_guard<std::mutex> lock(AudioDeviceMutex);
  CompatAudioDevice* device = CompatFindDevice(dev);
  if (!device) {
    return 0;
  }
  std::lock_guard<std::mutex> qlock(device->QueueLock);
  return static_cast<Uint32>(device->Pending.size());
}

extern "C" void SDL_ClearQueuedAudio(SDL_AudioDeviceID dev) {

  std::lock_guard<std::mutex> lock(AudioDeviceMutex);
  CompatAudioDevice* device = CompatFindDevice(dev);
  if (device) {
    std::lock_guard<std::mutex> qlock(device->QueueLock);
    device->Pending.clear();
  }
}

// ---- audio conversion / mixing -----------------------------------------------

namespace {
void SDLCALL CompatCvtS16ToF32(SDL_AudioCVT* cvt, SDL_AudioFormat) {
  // In-place expansion: walk backwards converting Sint16 samples to float.
  const int samples = cvt->len / 2;
  const auto* src = reinterpret_cast<const Sint16*>(cvt->buf);
  auto* dst = reinterpret_cast<float*>(cvt->buf);
  for (int i = samples - 1; i >= 0; --i) {
    dst[i] = static_cast<float>(src[i]) * (1.0f / 32768.0f);
  }
  cvt->len_cvt = samples * 4;
}

void SDLCALL CompatCvtF32ToS16(SDL_AudioCVT* cvt, SDL_AudioFormat) {
  const int samples = cvt->len / 4;
  const auto* src = reinterpret_cast<const float*>(cvt->buf);
  auto* dst = reinterpret_cast<Sint16*>(cvt->buf);
  for (int i = 0; i < samples; ++i) {
    float f = src[i];
    f = f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f);
    dst[i] = static_cast<Sint16>(f * 32767.0f);
  }
  cvt->len_cvt = samples * 2;
}

bool CompatIsS16Format(SDL_AudioFormat f) {
  return f == AUDIO_S16LSB || f == AUDIO_S16MSB;
}

bool CompatIsF32Format(SDL_AudioFormat f) {
  return f == AUDIO_F32LSB || f == AUDIO_F32MSB;
}
} // namespace

extern "C" int SDL_BuildAudioCVT(SDL_AudioCVT* cvt, SDL_AudioFormat src_format, Uint8 src_channels,
                                 int src_rate, SDL_AudioFormat dst_format, Uint8 dst_channels, int dst_rate) {

  if (!cvt) {
    return SDL_SetError("Parameter 'cvt' is invalid");
  }
  std::memset(cvt, 0, sizeof(*cvt));
  cvt->src_format = src_format;
  cvt->dst_format = dst_format;
  cvt->rate_incr = 1.0;
  cvt->len_mult = 1;
  cvt->len_ratio = 1.0;
  if (src_format == dst_format && src_channels == dst_channels && src_rate == dst_rate) {
    cvt->needed = 0;
    return 0; // no conversion needed
  }
  if (src_channels == dst_channels && src_rate == dst_rate && CompatIsS16Format(src_format) &&
      CompatIsF32Format(dst_format) && SDL_AUDIO_ISBIGENDIAN(src_format) == SDL_AUDIO_ISBIGENDIAN(dst_format)) {
    cvt->needed = 1;
    cvt->len_mult = 2;
    cvt->len_ratio = 2.0;
    cvt->filters[0] = CompatCvtS16ToF32;
    return 1;
  }
  if (src_channels == dst_channels && src_rate == dst_rate && CompatIsF32Format(src_format) &&
      CompatIsS16Format(dst_format) && SDL_AUDIO_ISBIGENDIAN(src_format) == SDL_AUDIO_ISBIGENDIAN(dst_format)) {
    cvt->needed = 1;
    cvt->len_mult = 1;
    cvt->len_ratio = 0.5;
    cvt->filters[0] = CompatCvtF32ToS16;
    return 1;
  }
  return SDL_SetError("Unsupported audio conversion (0x%x %dch %dHz -> 0x%x %dch %dHz)", src_format,
                      src_channels, src_rate, dst_format, dst_channels, dst_rate);
}

extern "C" int SDL_ConvertAudio(SDL_AudioCVT* cvt) {

  if (!cvt || !cvt->buf) {
    return SDL_SetError("Parameter 'cvt' is invalid");
  }
  if (!cvt->needed) {
    cvt->len_cvt = cvt->len;
    return 0;
  }
  for (int i = 0; i < SDL_AUDIOCVT_MAX_FILTERS + 1 && cvt->filters[i]; ++i) {
    cvt->filter_index = i;
    cvt->filters[i](cvt, cvt->src_format);
  }
  return 0;
}

namespace {
template <typename T>
void CompatMixSamples(Uint8* dst, const Uint8* src, Uint32 len, int volume) {
  auto* dstp = reinterpret_cast<T*>(dst);
  const auto* srcp = reinterpret_cast<const T*>(src);
  const size_t samples = len / sizeof(T);
  const float scale = static_cast<float>(volume) / static_cast<float>(SDL_MIX_MAXVOLUME);
  for (size_t i = 0; i < samples; ++i) {
    const float mixed = static_cast<float>(dstp[i]) + static_cast<float>(srcp[i]) * scale;
    if constexpr (std::is_floating_point<T>::value) {
      dstp[i] = mixed > 1.0f ? 1.0f : (mixed < -1.0f ? -1.0f : mixed);
    } else {
      const float lo = -static_cast<float>(std::numeric_limits<T>::max()) - 1.0f;
      const float hi = static_cast<float>(std::numeric_limits<T>::max());
      dstp[i] = static_cast<T>(mixed > hi ? hi : (mixed < lo ? lo : mixed));
    }
  }
}

void CompatMixU8(Uint8* dst, const Uint8* src, Uint32 len, int volume) {
  const float scale = static_cast<float>(volume) / static_cast<float>(SDL_MIX_MAXVOLUME);
  for (Uint32 i = 0; i < len; ++i) {
    const int d = static_cast<int>(dst[i]) - 128;
    const int s = static_cast<int>(src[i]) - 128;
    int mixed = d + static_cast<int>(s * scale);
    mixed = mixed > 127 ? 127 : (mixed < -128 ? -128 : mixed);
    dst[i] = static_cast<Uint8>(mixed + 128);
  }
}
} // namespace

extern "C" void SDL_MixAudioFormat(Uint8* dst, const Uint8* src, SDL_AudioFormat format, Uint32 len,
                                   int volume) {

  if (!dst || !src || !len) {
    return;
  }
  if (volume <= 0) {
    return;
  }
  if (volume > SDL_MIX_MAXVOLUME) {
    volume = SDL_MIX_MAXVOLUME;
  }
  switch (format) {
  case AUDIO_U8: CompatMixU8(dst, src, len, volume); break;
  case AUDIO_S8: CompatMixSamples<Sint8>(dst, src, len, volume); break;
  case AUDIO_S16LSB:
  case AUDIO_S16MSB: CompatMixSamples<Sint16>(dst, src, len, volume); break;
  case AUDIO_S32LSB:
  case AUDIO_S32MSB: CompatMixSamples<Sint32>(dst, src, len, volume); break;
  case AUDIO_F32LSB:
  case AUDIO_F32MSB: CompatMixSamples<float>(dst, src, len, volume); break;
  case AUDIO_U16LSB:
  case AUDIO_U16MSB: {
    // Unsigned 16: bias to signed, mix, bias back.
    const size_t samples = len / 2;
    auto* dstp = reinterpret_cast<Uint16*>(dst);
    const auto* srcp = reinterpret_cast<const Uint16*>(src);
    const float scale = static_cast<float>(volume) / static_cast<float>(SDL_MIX_MAXVOLUME);
    for (size_t i = 0; i < samples; ++i) {
      const float mixed = (static_cast<float>(dstp[i]) - 32768.0f) +
                          (static_cast<float>(srcp[i]) - 32768.0f) * scale;
      dstp[i] = static_cast<Uint16>(mixed > 32767.0f ? 65535 : (mixed < -32768.0f ? 0 : mixed + 32768.0f));
    }
    break;
  }
  default: break;
  }
}

extern "C" void SDL_MixAudio(Uint8* dst, const Uint8* src, Uint32 len, int volume) {

  SDL_MixAudioFormat(dst, src, AUDIO_S16SYS, len, volume);
}

// ---- WAV ---------------------------------------------------------------------

extern "C" SDL_AudioSpec* SDL_LoadWAV_RW(SDL_RWops* src, int freesrc, SDL_AudioSpec* spec,
                                         Uint8** audio_buf, Uint32* audio_len) {

  (void)spec;
  if (audio_buf) {
    *audio_buf = nullptr;
  }
  if (audio_len) {
    *audio_len = 0;
  }
  if (freesrc && src) {
    SDL_RWclose(src);
  }
  SDL_SetError("SDL_LoadWAV_RW is not supported by the VEXA SDL2 thunk");
  return nullptr;
}

extern "C" SDL_AudioSpec* SDL_LoadWAV(const char* file, SDL_AudioSpec* spec, Uint8** audio_buf,
                                      Uint32* audio_len) {

  return SDL_LoadWAV_RW(SDL_RWFromFile(file, "rb"), 1, spec, audio_buf, audio_len);
}

extern "C" void SDL_FreeWAV(Uint8* audio_buf) {  SDL_free(audio_buf); }
