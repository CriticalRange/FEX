// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/Utils/LogManager.h>

#include <mutex>
#include <shared_mutex>

#ifndef _WIN32
#include <errno.h>
#include <pthread.h>
#endif

namespace FEXCore {
#ifndef _WIN32
class ForkableUniqueMutex final {
public:
  ForkableUniqueMutex()
    : Mutex(PTHREAD_MUTEX_INITIALIZER) {}

  // Move-only type
  ForkableUniqueMutex(const ForkableUniqueMutex&) = delete;
  ForkableUniqueMutex& operator=(const ForkableUniqueMutex&) = delete;
  ForkableUniqueMutex(ForkableUniqueMutex&& rhs) = default;
  ForkableUniqueMutex& operator=(ForkableUniqueMutex&&) = default;

  void lock() {
    const auto Result = pthread_mutex_lock(&Mutex);
    LOGMAN_THROW_A_FMT(Result == 0, "{} failed to lock with {}", __func__, Result);
  }
  void unlock() {
    const auto Result = pthread_mutex_unlock(&Mutex);
    LOGMAN_THROW_A_FMT(Result == 0, "{} failed to unlock with {}", __func__, Result);
  }

  // Initialize the internal pthread object to its default initializer state.
  // Should only ever be used in the child process when a Linux fork() has occured.
  void StealAndDropActiveLocks() {
    Mutex = PTHREAD_MUTEX_INITIALIZER;
  }

  // Asserts that the mutex isn't exclusively owned by the calling thread.
  void check_lock_owned_by_self() {
    const auto Result = pthread_mutex_lock(&Mutex);
    LOGMAN_THROW_A_FMT(Result == EDEADLK, "User of unique lock must have already locked mutex as write!");
  }

private:
  pthread_mutex_t Mutex;
};

class ForkableSharedMutex final {
public:
  ForkableSharedMutex()
    : Mutex(PTHREAD_RWLOCK_INITIALIZER) {}

  // Move-only type
  ForkableSharedMutex(const ForkableSharedMutex&) = delete;
  ForkableSharedMutex& operator=(const ForkableSharedMutex&) = delete;
  ForkableSharedMutex(ForkableSharedMutex&& rhs) = default;
  ForkableSharedMutex& operator=(ForkableSharedMutex&&) = default;

  void lock() {
    const auto Result = pthread_rwlock_wrlock(&Mutex);
    LOGMAN_THROW_A_FMT(Result == 0, "{} failed to lock with {}", __func__, Result);
  }
  void unlock() {
    const auto Result = pthread_rwlock_unlock(&Mutex);
    LOGMAN_THROW_A_FMT(Result == 0, "{} failed to unlock with {}", __func__, Result);
  }
  void lock_shared() {
    const auto Result = pthread_rwlock_rdlock(&Mutex);
    LOGMAN_THROW_A_FMT(Result == 0, "{} failed to lock with {}", __func__, Result);
  }

  void unlock_shared() {
    unlock();
  }

  bool try_lock() {
    const auto Result = pthread_rwlock_trywrlock(&Mutex);
    return Result == 0;
  }

  bool try_lock_shared() {
    const auto Result = pthread_rwlock_tryrdlock(&Mutex);
    return Result == 0;
  }

  // Asserts that the rwlock isn't exclusively owned by the calling thread.
  void check_lock_owned_by_self_as_write() {
    const auto Result = pthread_rwlock_wrlock(&Mutex);
    LOGMAN_THROW_A_FMT(Result == EDEADLK, "User of rwlock must have already locked mutex as write!");
  }

  // Initialize the internal pthread object to its default initializer state.
  // Should only ever be used in the child process when a Linux fork() has occured.
  void StealAndDropActiveLocks() {
    Mutex = PTHREAD_RWLOCK_INITIALIZER;
  }
private:
  pthread_rwlock_t Mutex;
};

#else

// Dummy implementations as Windows doesn't support forking or async signals.
class ForkableUniqueMutex final : public std::mutex {
public:
  void StealAndDropActiveLocks() {
    LogMan::Msg::AFmt("{} is unsupported on WIN32 builds!", __func__);
  }
};

class ForkableSharedMutex final : public std::shared_mutex {
public:
  void StealAndDropActiveLocks() {
    LogMan::Msg::AFmt("{} is unsupported on WIN32 builds!", __func__);
  }
};
#endif
} // namespace FEXCore
