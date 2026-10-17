// SPDX-License-Identifier: MIT
#pragma once

#include <cerrno>
#include <deque>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace FEX::HLE {
// Android app policy may reject openat2. Plain openat does not implement
// RESOLVE_IN_ROOT: absolute symlinks would escape into Android's filesystem.
// Walk from directory descriptors, resolving absolute links against RootFD and
// clamping '..' there. No process-global chdir or rootfs mutation is involved.
inline int AndroidOpenInRoot(int RootFD, const char* Path, int Flags, mode_t Mode) {
  if (!Path || !*Path) { errno = Path ? ENOENT : EFAULT; return -1; }
  struct Directories {
    std::vector<int> FDs;
    ~Directories() { const int Error = errno; for (int FD : FDs) ::close(FD); errno = Error; }
    void Reset() { while (FDs.size() > 1) { ::close(FDs.back()); FDs.pop_back(); } }
  } Dirs;
  const int Root = RootFD == AT_FDCWD ? ::open(".", O_PATH | O_DIRECTORY | O_CLOEXEC) : ::fcntl(RootFD, F_DUPFD_CLOEXEC, 0);
  if (Root < 0) return -1;
  Dirs.FDs.push_back(Root);
  std::deque<std::string> Pending;
  auto Prepend = [&](const std::string& Name) {
    std::vector<std::string> Parts;
    size_t Start = 0;
    while (Start < Name.size()) {
      const auto End = Name.find('/', Start);
      auto Part = Name.substr(Start, End - Start);
      if (!Part.empty()) Parts.push_back(std::move(Part));
      if (End == std::string::npos) break;
      Start = End + 1;
    }
    for (auto I = Parts.rbegin(); I != Parts.rend(); ++I) Pending.push_front(*I);
  };
  const std::string Input {Path};
  bool TrailingSlash = Input.back() == '/';
  Prepend(Input);
  unsigned Links = 0;
  while (!Pending.empty()) {
    const auto Part = std::move(Pending.front());
    Pending.pop_front();
    if (Part == ".") continue;
    if (Part == "..") {
      if (Dirs.FDs.size() > 1) { ::close(Dirs.FDs.back()); Dirs.FDs.pop_back(); }
      continue;
    }
    const bool Last = Pending.empty();
    const int Parent = Dirs.FDs.back();
    // An O_PATH descriptor pins the inspected object while resolving a link.
    const int Entry = ::openat(Parent, Part.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (Entry < 0) {
      if (Last && errno == ENOENT)
        return ::openat(Parent, Part.c_str(), Flags | O_NOFOLLOW | (TrailingSlash ? O_DIRECTORY : 0), Mode);
      return -1;
    }
    struct stat Info {};
    if (::fstat(Entry, &Info) < 0) { const int Error = errno; ::close(Entry); errno = Error; return -1; }
    if (S_ISLNK(Info.st_mode)) {
      if (Last && ((Flags & O_NOFOLLOW) || ((Flags & O_CREAT) && (Flags & O_EXCL)))) {
        ::close(Entry);
        return ::openat(Parent, Part.c_str(), Flags | (TrailingSlash ? O_DIRECTORY : 0), Mode);
      }
      if (++Links > 40) { ::close(Entry); errno = ELOOP; return -1; }
      char Target[4096];
      const auto Length = ::readlinkat(Entry, "", Target, sizeof(Target));
      const int Error = errno;
      ::close(Entry);
      if (Length < 0) { errno = Error; return -1; }
      if (Length == sizeof(Target)) { errno = ENAMETOOLONG; return -1; }
      if (!Length) { errno = ENOENT; return -1; }
      const std::string Link {Target, static_cast<size_t>(Length)};
      if (Last && Link.back() == '/') TrailingSlash = true;
      if (Link.front() == '/') Dirs.Reset();
      Prepend(Link);
      continue;
    }
    if (Last) {
      ::close(Entry);
      // Refuse a raced-in symlink instead of following it outside RootFD.
      return ::openat(Parent, Part.c_str(), Flags | O_NOFOLLOW | (TrailingSlash ? O_DIRECTORY : 0), Mode);
    }
    if (!S_ISDIR(Info.st_mode)) { ::close(Entry); errno = ENOTDIR; return -1; }
    Dirs.FDs.push_back(Entry);
  }
  return ::openat(Dirs.FDs.back(), ".", Flags | (TrailingSlash ? O_DIRECTORY : 0), Mode);
}
} // namespace FEX::HLE
