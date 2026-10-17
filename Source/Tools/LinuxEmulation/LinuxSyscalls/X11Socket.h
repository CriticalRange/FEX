// SPDX-License-Identifier: MIT
#pragma once

#include "LinuxSyscalls/Syscalls.h"

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>

namespace FEX::HLE {
// Android cannot expose rootfs /tmp via a real chroot. Only the explicitly
// enabled VEXA real-X11 session maps Xlib's canonical X socket names; Wine's
// own Unix sockets and every other address retain normal kernel semantics.
inline int ConnectVexaX11(int FD, const sockaddr* Address, socklen_t Length) {
  const char* Enabled = getenv("VEXA_REAL_X11");
  if (!Enabled || strcmp(Enabled, "1") != 0 || Length < sizeof(sa_family_t)) {
    return ::connect(FD, Address, Length);
  }
  sa_family_t Family {};
  if (FaultSafeUserMemAccess::CopyFromUser(&Family, Address, sizeof(Family))) {
    errno = EFAULT;
    return -1;
  }
  if (Family != AF_UNIX || Length <= offsetof(sockaddr_un, sun_path) || Length > sizeof(sockaddr_un)) {
    return ::connect(FD, Address, Length);
  }
  sockaddr_un Source {};
  if (FaultSafeUserMemAccess::CopyFromUser(&Source, Address, Length)) {
    errno = EFAULT;
    return -1;
  }
  const bool Abstract = Source.sun_path[0] == '\0';
  const size_t Skip = Abstract ? 1 : 0;
  const size_t Available = Length - offsetof(sockaddr_un, sun_path) - Skip;
  const char* Name = Source.sun_path + Skip;
  const size_t NameLength = strnlen(Name, Available);
  constexpr char Prefix[] = "/tmp/.X11-unix/X";
  constexpr size_t PrefixLength = sizeof(Prefix) - 1;
  if (NameLength <= PrefixLength || memcmp(Name, Prefix, PrefixLength) != 0 ||
      (Abstract && NameLength < Available)) {
    return ::connect(FD, Address, Length);
  }
  for (size_t I = PrefixLength; I < NameLength; ++I) {
    if (Name[I] < '0' || Name[I] > '9') {
      return ::connect(FD, Address, Length);
    }
  }
  const char* ExplicitPath = getenv("VEXA_X11_SOCKET");
  const std::string Target = ExplicitPath && ExplicitPath[0] ? ExplicitPath :
    std::string(_SyscallHandler->RootFSPath().c_str()) + std::string(Name, NameLength);
  sockaddr_un Mapped {};
  Mapped.sun_family = AF_UNIX;
  if (Target.empty() || Target.front() != '/') {
    errno = EINVAL;
    return -1;
  }
  if (Target.size() >= sizeof(Mapped.sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(Mapped.sun_path, Target.c_str(), Target.size() + 1);
  return ::connect(FD, reinterpret_cast<const sockaddr*>(&Mapped),
                   offsetof(sockaddr_un, sun_path) + Target.size() + 1);
}
} // namespace FEX::HLE
