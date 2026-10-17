// SPDX-License-Identifier: MIT
#pragma once
#include "LinuxSyscalls/Syscalls.h"
#include <cstdlib>
#include <cstring>
#include <string>

namespace FEX::HLE {
// Wine's Linux build hardcodes /tmp for its server directory. Rootfs reads
// already use FileManager; these pathname-mutating syscalls need the same
// location when the Android Proton supervisor explicitly enables the session.
inline std::string VexaPrivateTmpPath(const char* path) {
#if defined(__ANDROID__)
  const char* session = getenv("VEXA_COMPAT_SESSION");
  if (session && *session && path &&
      (strcmp(path, "/tmp") == 0 || strncmp(path, "/tmp/", 5) == 0)) {
    const auto root = _SyscallHandler->RootFSPath();
    if (!root.empty()) return std::string(root.c_str()) + path;
  }
#endif
  return {};
}
}
