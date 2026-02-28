#include <common/GeneratorInterface.h>

extern "C" {
#include <dlfcn.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

int FEX_open(const char* path, int flags, mode_t mode);
int FEX_open64(const char* path, int flags, mode_t mode);
int8_t* FEX_strcpy(int8_t* dst, const int8_t* src);
int8_t* FEX_strncpy(int8_t* dst, const int8_t* src, size_t n);
int8_t* FEX_strcat(int8_t* dst, const int8_t* src);
int8_t* FEX_strncat(int8_t* dst, const int8_t* src, size_t n);
int8_t* FEX_strchr(const int8_t* s, int c);
int8_t* FEX_strrchr(const int8_t* s, int c);
int8_t* FEX_strstr(const int8_t* haystack, const int8_t* needle);
int8_t* FEX_strdup(const int8_t* s);
int8_t* FEX_stpcpy(int8_t* dst, const int8_t* src);
void* FEX_memchr(const void* s, int c, size_t n);
int8_t* FEX_getenv(const int8_t* name);
int8_t* FEX_strerror(int errnum);
int8_t* FEX_dlerror();
int FEX_pthread_create(uint64_t* thread, const void* attr, void* (*start_routine)(void*), void* arg);
int FEX_pthread_join(uint64_t thread, void** retval);
uint64_t FEX_pthread_self(void);
int FEX_pthread_detach(uint64_t thread);
int* __errno_location(void);
}

template<auto>
struct fex_gen_config {
  unsigned version = 1;
};

template<typename>
struct fex_gen_type {};

template<auto, int, typename = void>
struct fex_gen_param {};

template<>
struct fex_gen_type<FILE> : fexgen::opaque_type {};
template<>
struct fex_gen_type<pthread_attr_t> : fexgen::opaque_type {};
template<>
struct fex_gen_type<pthread_mutex_t> : fexgen::opaque_type {};
template<>
struct fex_gen_type<pthread_cond_t> : fexgen::opaque_type {};

template<>
struct fex_gen_config<malloc> {};
template<>
struct fex_gen_config<free> {};
template<>
struct fex_gen_config<calloc> {};
template<>
struct fex_gen_config<realloc> {};
template<>
struct fex_gen_config<memalign> {};
template<>
struct fex_gen_config<posix_memalign> {};
template<>
struct fex_gen_config<aligned_alloc> {};
template<>
struct fex_gen_config<malloc_usable_size> {};

template<>
struct fex_gen_config<strlen> {};
template<>
struct fex_gen_config<strnlen> {};
template<>
struct fex_gen_config<strcmp> {};
template<>
struct fex_gen_config<strncmp> {};
template<>
struct fex_gen_config<strcasecmp> {};
template<>
struct fex_gen_config<strncasecmp> {};
template<>
struct fex_gen_config<FEX_strcpy> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_strncpy> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_strcat> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_strncat> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_strchr> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_strrchr> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_strstr> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_strdup> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_stpcpy> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<memcpy> {};
template<>
struct fex_gen_config<memmove> {};
template<>
struct fex_gen_config<memset> {};
template<>
struct fex_gen_config<memcmp> {};
template<>
struct fex_gen_config<FEX_memchr> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<bcmp> {};

template<>
struct fex_gen_config<FEX_open> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_open64> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<close> {};
template<>
struct fex_gen_config<read> {};
template<>
struct fex_gen_config<write> {};
template<>
struct fex_gen_config<lseek> {};
template<>
struct fex_gen_config<lseek64> {};
template<>
struct fex_gen_config<pread64> {};
template<>
struct fex_gen_config<pwrite64> {};

template<>
struct fex_gen_config<mmap> {};
template<>
struct fex_gen_config<mmap64> {};
template<>
struct fex_gen_config<munmap> {};
template<>
struct fex_gen_config<mprotect> {};
template<>
struct fex_gen_config<madvise> {};
template<>
struct fex_gen_config<msync> {};

template<>
struct fex_gen_config<FEX_getenv> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<setenv> {};
template<>
struct fex_gen_config<FEX_strerror> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<perror> {};
template<>
struct fex_gen_config<__errno_location> : fexgen::custom_host_impl {};

template<>
struct fex_gen_config<getpid> {};
template<>
struct fex_gen_config<gettid> {};
template<>
struct fex_gen_config<sched_yield> {};
template<>
struct fex_gen_config<sleep> {};
template<>
struct fex_gen_config<usleep> {};
template<>
struct fex_gen_config<nanosleep> {};
template<>
struct fex_gen_config<time> {};
template<>
struct fex_gen_config<gettimeofday> {};
template<>
struct fex_gen_config<clock_gettime> {};
template<>
struct fex_gen_config<getpagesize> {};
template<>
struct fex_gen_config<sysconf> {};
template<>
struct fex_gen_config<getrandom> {};

template<>
struct fex_gen_config<dlopen> {};
template<>
struct fex_gen_config<dlclose> {};
template<>
struct fex_gen_config<FEX_dlerror> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<dlsym> : fexgen::custom_guest_entrypoint {};

template<>
struct fex_gen_config<FEX_pthread_create> : fexgen::custom_host_impl, fexgen::custom_guest_entrypoint {};
template<>
struct fex_gen_param<FEX_pthread_create, 2, void* (*)(void*)> : fexgen::ptr_passthrough {};
template<>
struct fex_gen_config<FEX_pthread_join> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_pthread_self> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<FEX_pthread_detach> : fexgen::custom_host_impl {};
template<>
struct fex_gen_config<pthread_once> : fexgen::custom_host_impl, fexgen::custom_guest_entrypoint {};
template<>
struct fex_gen_param<pthread_once, 1, void (*)()> : fexgen::ptr_passthrough {};
template<>
struct fex_gen_config<pthread_mutex_init> {};
template<>
struct fex_gen_config<pthread_mutex_destroy> {};
template<>
struct fex_gen_config<pthread_mutex_lock> {};
template<>
struct fex_gen_config<pthread_mutex_unlock> {};
template<>
struct fex_gen_config<pthread_cond_init> {};
template<>
struct fex_gen_config<pthread_cond_destroy> {};
template<>
struct fex_gen_config<pthread_cond_wait> {};
template<>
struct fex_gen_config<pthread_cond_signal> {};
template<>
struct fex_gen_config<pthread_cond_broadcast> {};

template<>
struct fex_gen_config<qsort> : fexgen::custom_host_impl, fexgen::custom_guest_entrypoint {};
template<>
struct fex_gen_param<qsort, 3, int (*)(const void*, const void*)> : fexgen::ptr_passthrough {};
