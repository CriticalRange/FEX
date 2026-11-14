// SPDX-License-Identifier: MIT
#pragma once
#include "LinuxSyscalls/Syscalls.h"
#include <cstdlib>
#include <cstring>
#include <string>

namespace FEX::HLE {
// glibc implements POSIX shm with files in /dev/shm. Android has no such
// mount. Keep names shared between Wine processes in the private Proton rootfs.
// Unlike ordinary rootfs lookup, create, reopen and unlink must all agree.
inline std::string VexaPrivateShmPath(const char* path) {
#if defined(__ANDROID__)
  const char* session = getenv("VEXA_COMPAT_SESSION");
  if (session && *session && path &&
      (strcmp(path, "/dev/shm") == 0 || strncmp(path, "/dev/shm/", 9) == 0)) {
    const auto root = _SyscallHandler->RootFSPath();
    if (!root.empty()) return std::string(root.c_str()) + path;
  }
#endif
  return {};
}
}
