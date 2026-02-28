/*
$info$
tags: thunklibs|libc
$end_info$
*/

extern "C" {
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
}

#include "common/Guest.h"
#include "thunkgen_guest_libc.inl"

#ifdef open
#undef open
#endif

#ifdef open64
#undef open64
#endif

namespace {
int8_t* ToFEXChar(char* value) {
  return reinterpret_cast<int8_t*>(value);
}

const int8_t* ToFEXChar(const char* value) {
  return reinterpret_cast<const int8_t*>(value);
}

char* ToGuestChar(int8_t* value) {
  return reinterpret_cast<char*>(value);
}
int OpenWithOptionalMode(bool use_open64, const char* path, int flags, va_list ap) {
  mode_t mode {};
  if (flags & O_CREAT) {
    mode = static_cast<mode_t>(va_arg(ap, int));
  }
  return use_open64 ? fexfn_pack_FEX_open64(path, flags, mode) : fexfn_pack_FEX_open(path, flags, mode);
}
}

extern "C" int guest_open(const char* path, int flags, ...) __asm__("open");
extern "C" int guest_open(const char* path, int flags, ...) {
  va_list ap;
  va_start(ap, flags);
  auto result = OpenWithOptionalMode(false, path, flags, ap);
  va_end(ap);
  return result;
}

extern "C" int guest_open64(const char* path, int flags, ...) __asm__("open64");
extern "C" int guest_open64(const char* path, int flags, ...) {
  va_list ap;
  va_start(ap, flags);
  auto result = OpenWithOptionalMode(true, path, flags, ap);
  va_end(ap);
  return result;
}

extern "C" char* strcpy(char* dst, const char* src) noexcept {
  return ToGuestChar(fexfn_pack_FEX_strcpy(ToFEXChar(dst), ToFEXChar(src)));
}

extern "C" char* strncpy(char* dst, const char* src, size_t n) noexcept {
  return ToGuestChar(fexfn_pack_FEX_strncpy(ToFEXChar(dst), ToFEXChar(src), n));
}

extern "C" char* strcat(char* dst, const char* src) noexcept {
  return ToGuestChar(fexfn_pack_FEX_strcat(ToFEXChar(dst), ToFEXChar(src)));
}

extern "C" char* strncat(char* dst, const char* src, size_t n) noexcept {
  return ToGuestChar(fexfn_pack_FEX_strncat(ToFEXChar(dst), ToFEXChar(src), n));
}

extern "C" char* strchr(const char* s, int c) noexcept {
  return ToGuestChar(fexfn_pack_FEX_strchr(ToFEXChar(s), c));
}

extern "C" char* strrchr(const char* s, int c) noexcept {
  return ToGuestChar(fexfn_pack_FEX_strrchr(ToFEXChar(s), c));
}

extern "C" char* strstr(const char* haystack, const char* needle) noexcept {
  return ToGuestChar(fexfn_pack_FEX_strstr(ToFEXChar(haystack), ToFEXChar(needle)));
}

extern "C" char* strdup(const char* s) noexcept {
  return ToGuestChar(fexfn_pack_FEX_strdup(ToFEXChar(s)));
}

extern "C" char* stpcpy(char* dst, const char* src) noexcept {
  return ToGuestChar(fexfn_pack_FEX_stpcpy(ToFEXChar(dst), ToFEXChar(src)));
}

extern "C" void* memchr(const void* s, int c, size_t n) noexcept {
  return fexfn_pack_FEX_memchr(s, c, n);
}

extern "C" char* getenv(const char* name) noexcept {
  return ToGuestChar(fexfn_pack_FEX_getenv(ToFEXChar(name)));
}

extern "C" char* strerror(int errnum) noexcept {
  return ToGuestChar(fexfn_pack_FEX_strerror(errnum));
}

extern "C" char* dlerror(void) noexcept {
  return ToGuestChar(fexfn_pack_FEX_dlerror());
}

extern "C" int pthread_create(
  pthread_t* thread, const pthread_attr_t* attr,
  void* (*start_routine)(void*), void* arg) {
  auto start_callback = start_routine;
  if (!start_callback) {
    return EINVAL;
  }
  auto host_start = AllocateHostTrampolineForGuestFunction(start_callback);
  uint64_t thread_handle {};
  auto result = fexfn_pack_FEX_pthread_create(&thread_handle, attr, host_start, arg);
  if (result == 0 && thread) {
    *thread = static_cast<pthread_t>(thread_handle);
  }
  return result;
}

extern "C" int pthread_join(pthread_t thread, void** retval) {
  return fexfn_pack_FEX_pthread_join(static_cast<uint64_t>(thread), retval);
}

extern "C" pthread_t pthread_self(void) {
  return static_cast<pthread_t>(fexfn_pack_FEX_pthread_self());
}

extern "C" int pthread_detach(pthread_t thread) {
  return fexfn_pack_FEX_pthread_detach(static_cast<uint64_t>(thread));
}

extern "C" int pthread_once(pthread_once_t* once_control, void (*init_routine)(void)) {
  auto init_callback = init_routine;
  auto host_init = init_callback ? AllocateHostTrampolineForGuestFunction(init_callback) : nullptr;
  return fexfn_pack_pthread_once(once_control, host_init);
}

extern "C" void qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*)) {
  auto compare_callback = compar;
  auto host_compar = compare_callback ? AllocateHostTrampolineForGuestFunction(compare_callback) : nullptr;
  fexfn_pack_qsort(base, nmemb, size, host_compar);
}

static void* ResolveGuestLibCSymbol(const char* symbol) {
  if (!symbol) return nullptr;
  if (__builtin_strcmp(symbol, "malloc") == 0) return reinterpret_cast<void*>(&malloc);
  if (__builtin_strcmp(symbol, "free") == 0) return reinterpret_cast<void*>(&free);
  if (__builtin_strcmp(symbol, "calloc") == 0) return reinterpret_cast<void*>(&calloc);
  if (__builtin_strcmp(symbol, "realloc") == 0) return reinterpret_cast<void*>(&realloc);
  if (__builtin_strcmp(symbol, "open") == 0) return reinterpret_cast<void*>(&guest_open);
  if (__builtin_strcmp(symbol, "open64") == 0) return reinterpret_cast<void*>(&guest_open64);
  if (__builtin_strcmp(symbol, "strcpy") == 0) return reinterpret_cast<void*>(&strcpy);
  if (__builtin_strcmp(symbol, "strncpy") == 0) return reinterpret_cast<void*>(&strncpy);
  if (__builtin_strcmp(symbol, "strcat") == 0) return reinterpret_cast<void*>(&strcat);
  if (__builtin_strcmp(symbol, "strncat") == 0) return reinterpret_cast<void*>(&strncat);
  if (__builtin_strcmp(symbol, "strchr") == 0) return reinterpret_cast<void*>(&strchr);
  if (__builtin_strcmp(symbol, "strrchr") == 0) return reinterpret_cast<void*>(&strrchr);
  if (__builtin_strcmp(symbol, "strstr") == 0) return reinterpret_cast<void*>(&strstr);
  if (__builtin_strcmp(symbol, "strdup") == 0) return reinterpret_cast<void*>(&strdup);
  if (__builtin_strcmp(symbol, "stpcpy") == 0) return reinterpret_cast<void*>(&stpcpy);
  if (__builtin_strcmp(symbol, "memchr") == 0) return reinterpret_cast<void*>(&memchr);
  if (__builtin_strcmp(symbol, "getenv") == 0) return reinterpret_cast<void*>(&getenv);
  if (__builtin_strcmp(symbol, "strerror") == 0) return reinterpret_cast<void*>(&strerror);
  if (__builtin_strcmp(symbol, "dlerror") == 0) return reinterpret_cast<void*>(&dlerror);
  if (__builtin_strcmp(symbol, "pthread_create") == 0) return reinterpret_cast<void*>(&pthread_create);
  if (__builtin_strcmp(symbol, "pthread_join") == 0) return reinterpret_cast<void*>(&pthread_join);
  if (__builtin_strcmp(symbol, "pthread_self") == 0) return reinterpret_cast<void*>(&pthread_self);
  if (__builtin_strcmp(symbol, "pthread_detach") == 0) return reinterpret_cast<void*>(&pthread_detach);
  if (__builtin_strcmp(symbol, "pthread_once") == 0) return reinterpret_cast<void*>(&pthread_once);
  if (__builtin_strcmp(symbol, "qsort") == 0) return reinterpret_cast<void*>(&qsort);
  return nullptr;
}

extern "C" void* dlsym(void* handle, const char* symbol) {
  if (void* local = ResolveGuestLibCSymbol(symbol)) {
    return local;
  }
  return fexfn_pack_dlsym(handle, symbol);
}

LOAD_LIB(libc)
