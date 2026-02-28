/*
$info$
tags: thunklibs|libc
$end_info$
*/

extern "C" {
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
}

#include "common/Host.h"
#include <dlfcn.h>

#if defined(__BIONIC__)
using _IO_FILE = FILE;
using __off64_t = off64_t;
using __compar_fn_t = int (*)(const void*, const void*);
#endif

#include "thunkgen_host_libc.inl"

#if defined(__BIONIC__)
extern "C" int* __errno(void);
#endif

static int fexfn_impl_libc_FEX_open(const char* path, int flags, mode_t mode) {
  if (flags & O_CREAT) {
    return ::open(path, flags, mode);
  }
  return ::open(path, flags);
}

static int fexfn_impl_libc_FEX_open64(const char* path, int flags, mode_t mode) {
#if defined(__BIONIC__)
  return fexfn_impl_libc_FEX_open(path, flags, mode);
#else
  if (flags & O_CREAT) {
    return ::open64(path, flags, mode);
  }
  return ::open64(path, flags);
#endif
}

static int8_t* fexfn_impl_libc_FEX_strcpy(int8_t* dst, const int8_t* src) {
  return reinterpret_cast<int8_t*>(::strcpy(reinterpret_cast<char*>(dst), reinterpret_cast<const char*>(src)));
}

static int8_t* fexfn_impl_libc_FEX_strncpy(int8_t* dst, const int8_t* src, size_t n) {
  return reinterpret_cast<int8_t*>(::strncpy(reinterpret_cast<char*>(dst), reinterpret_cast<const char*>(src), n));
}

static int8_t* fexfn_impl_libc_FEX_strcat(int8_t* dst, const int8_t* src) {
  return reinterpret_cast<int8_t*>(::strcat(reinterpret_cast<char*>(dst), reinterpret_cast<const char*>(src)));
}

static int8_t* fexfn_impl_libc_FEX_strncat(int8_t* dst, const int8_t* src, size_t n) {
  return reinterpret_cast<int8_t*>(::strncat(reinterpret_cast<char*>(dst), reinterpret_cast<const char*>(src), n));
}

static int8_t* fexfn_impl_libc_FEX_strchr(const int8_t* s, int c) {
  return reinterpret_cast<int8_t*>(const_cast<char*>(::strchr(reinterpret_cast<const char*>(s), c)));
}

static int8_t* fexfn_impl_libc_FEX_strrchr(const int8_t* s, int c) {
  return reinterpret_cast<int8_t*>(const_cast<char*>(::strrchr(reinterpret_cast<const char*>(s), c)));
}

static int8_t* fexfn_impl_libc_FEX_strstr(const int8_t* haystack, const int8_t* needle) {
  return reinterpret_cast<int8_t*>(
    const_cast<char*>(::strstr(reinterpret_cast<const char*>(haystack), reinterpret_cast<const char*>(needle))));
}

static int8_t* fexfn_impl_libc_FEX_strdup(const int8_t* s) {
  return reinterpret_cast<int8_t*>(::strdup(reinterpret_cast<const char*>(s)));
}

static int8_t* fexfn_impl_libc_FEX_stpcpy(int8_t* dst, const int8_t* src) {
  return reinterpret_cast<int8_t*>(::stpcpy(reinterpret_cast<char*>(dst), reinterpret_cast<const char*>(src)));
}

static void* fexfn_impl_libc_FEX_memchr(const void* s, int c, size_t n) {
  return const_cast<void*>(::memchr(s, c, n));
}

static int8_t* fexfn_impl_libc_FEX_getenv(const int8_t* name) {
  return reinterpret_cast<int8_t*>(::getenv(reinterpret_cast<const char*>(name)));
}

static int8_t* fexfn_impl_libc_FEX_strerror(int errnum) {
  return reinterpret_cast<int8_t*>(::strerror(errnum));
}

static int8_t* fexfn_impl_libc_FEX_dlerror() {
  return reinterpret_cast<int8_t*>(::dlerror());
}

static int* fexfn_impl_libc___errno_location() {
#if defined(__BIONIC__)
  return __errno();
#else
  return ::__errno_location();
#endif
}

static int fexfn_impl_libc_FEX_pthread_create(
  uint64_t* thread, const void* attr, guest_layout<void* (*)(void*)> start_routine, void* arg) {
  pthread_t native_thread {};
  auto result = ::pthread_create(
    &native_thread, reinterpret_cast<const pthread_attr_t*>(attr), start_routine.force_get_host_pointer(), arg);
  if (result == 0 && thread) {
    *thread = static_cast<uint64_t>(native_thread);
  }
  return result;
}

static int fexfn_impl_libc_FEX_pthread_join(uint64_t thread, void** retval) {
  return ::pthread_join(static_cast<pthread_t>(thread), retval);
}

static uint64_t fexfn_impl_libc_FEX_pthread_self() {
  return static_cast<uint64_t>(::pthread_self());
}

static int fexfn_impl_libc_FEX_pthread_detach(uint64_t thread) {
  return ::pthread_detach(static_cast<pthread_t>(thread));
}

static int fexfn_impl_libc_pthread_once(pthread_once_t* once_control, guest_layout<void (*)(void)> init_routine) {
  return fexldr_ptr_libc_pthread_once(once_control, init_routine.force_get_host_pointer());
}

static void fexfn_impl_libc_qsort(void* base, size_t nmemb, size_t size, guest_layout<__compar_fn_t> compar) {
  fexldr_ptr_libc_qsort(base, nmemb, size, compar.force_get_host_pointer());
}

EXPORTS(libc)
