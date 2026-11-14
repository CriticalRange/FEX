// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/unordered_map.h>
#include <FEXCore/fextl/vector.h>

#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace FEX::HLE {
// Android app policy may reject openat2. Plain openat does not implement
// RESOLVE_IN_ROOT: absolute symlinks would escape into Android's filesystem.
// Paths are therefore resolved from directory descriptors, resolving absolute
// links against the rootfs and clamping '..' there. No process-global chdir or
// rootfs mutation is involved.
//
// That walk costs a few syscalls per path component, and library and asset
// opens repeat the same directories, so resolved directories are cached. A
// cached directory is used only after re-validation: the kernel's own
// resolution of the same prefix (which would follow an escaping symlink out of
// the rootfs) must reach the inode the in-root walk produced, and the cached
// descriptor must still refer to that inode. A hit costs three syscalls.
class AndroidRootFSResolver final {
public:
  // Cached descriptors are FEX-owned; the callback adds or removes them from
  // FileManager's tracked set so guest close()/close_range() keep away from them.
  using TrackFn = void (*)(void* Context, int FD, bool Track);

  AndroidRootFSResolver(TrackFn Track, void* Context)
    : Track {Track}
    , Context {Context} {}

  ~AndroidRootFSResolver() {
    std::unique_lock lk(CacheMutex);
    for (auto& [Dir, Entry] : Cache) {
      Track(Context, Entry.FD, false);
      ::close(Entry.FD);
    }
    Cache.clear();
  }

  AndroidRootFSResolver(const AndroidRootFSResolver&) = delete;
  AndroidRootFSResolver& operator=(const AndroidRootFSResolver&) = delete;

  // In-root resolution against an arbitrary root directory, without the cache.
  static int OpenUncached(int RootFD, const char* Path, int Flags, mode_t Mode) {
    if (!Path || !*Path) {
      errno = Path ? ENOENT : EFAULT;
      return -1;
    }
    return Walk(RootFD, Path, Flags, Mode);
  }

  int Open(int RootFD, const char* Path, int Flags, mode_t Mode) {
    if (!Path || !*Path) {
      errno = Path ? ENOENT : EFAULT;
      return -1;
    }

    const std::string_view Input {Path};
    size_t End = Input.size();
    while (End > 0 && Input[End - 1] == '/') {
      --End;
    }
    const bool TrailingSlash = End != Input.size();
    const size_t Slash = End ? Input.rfind('/', End - 1) : std::string_view::npos;
    const size_t LastBegin = Slash == std::string_view::npos ? 0 : Slash + 1;
    std::string_view Dir = Slash == std::string_view::npos ? std::string_view {} : Input.substr(0, Slash);
    const std::string_view Last = Input.substr(LastBegin, End - LastBegin);
    while (!Dir.empty() && Dir.front() == '/') {
      Dir.remove_prefix(1);
    }
    while (!Dir.empty() && Dir.back() == '/') {
      Dir.remove_suffix(1);
    }

    // O_PATH may legitimately open a symlink, and dot components need the walk's clamping.
    if (Last.empty() || Last == "." || Last == ".." || Last.size() > NAME_MAX || (Flags & O_PATH) || HasDotComponent(Dir) ||
        Dir.size() >= PATH_MAX) {
      return Walk(RootFD, Input, Flags, Mode);
    }

    char Name[NAME_MAX + 1];
    memcpy(Name, Last.data(), Last.size());
    Name[Last.size()] = '\0';

    int Result = -1;
    int Error = 0;
    {
      std::shared_lock lk(CacheMutex, std::defer_lock);
      int DirFD = RootFD;
      if (!Dir.empty()) {
        lk.lock();
        DirFD = LookupValidated(RootFD, Dir);
        if (DirFD < 0) {
          lk.unlock();
          const int Inserted = Insert(RootFD, Dir);
          if (Inserted > 0) {
            // The directory itself doesn't resolve, so neither can anything inside it.
            errno = Inserted;
            return -1;
          }
          if (Inserted < 0) {
            return Walk(RootFD, Input, Flags, Mode);
          }
          lk.lock();
          DirFD = LookupValidated(RootFD, Dir);
          if (DirFD < 0) {
            lk.unlock();
            return Walk(RootFD, Input, Flags, Mode);
          }
        }
      }
      // The shared lock keeps DirFD open while it's in use.
      Result = ::openat(DirFD, Name, Flags | O_NOFOLLOW | (TrailingSlash ? O_DIRECTORY : 0), Mode);
      Error = errno;
    }

    if (Result >= 0) {
      return Result;
    }
    const bool MaybeLink = Error == ELOOP || (Error == ENOTDIR && (TrailingSlash || (Flags & O_DIRECTORY)));
    if (!MaybeLink || (Flags & O_NOFOLLOW)) {
      errno = Error;
      return -1;
    }
    // The final component is (or may be) a symlink: resolve it inside the rootfs.
    return Walk(RootFD, Input, Flags, Mode);
  }

private:
  struct DirEntry {
    int FD;
    dev_t Dev;
    ino_t Ino;
  };

  struct StringViewHash {
    using is_transparent = void;
    size_t operator()(std::string_view Value) const {
      return std::hash<std::string_view> {}(Value);
    }
  };

  static constexpr size_t MaxCachedDirs = 256;
  static constexpr unsigned MaxSymlinks = 40;

  TrackFn Track;
  void* Context;
  std::shared_mutex CacheMutex;
  fextl::unordered_map<fextl::string, DirEntry, StringViewHash, std::equal_to<>> Cache;

  static bool HasDotComponent(std::string_view Path) {
    size_t Begin = 0;
    while (Begin < Path.size()) {
      const size_t Slash = Path.find('/', Begin);
      const auto Part = Path.substr(Begin, Slash == std::string_view::npos ? std::string_view::npos : Slash - Begin);
      if (Part == "." || Part == "..") {
        return true;
      }
      if (Slash == std::string_view::npos) {
        break;
      }
      Begin = Slash + 1;
    }
    return false;
  }

  static bool CopyPath(std::string_view Path, char (&Buffer)[PATH_MAX]) {
    if (Path.size() >= sizeof(Buffer)) {
      return false;
    }
    memcpy(Buffer, Path.data(), Path.size());
    Buffer[Path.size()] = '\0';
    return true;
  }

  // CacheMutex must be held (shared or unique).
  int LookupValidated(int RootFD, std::string_view Dir) {
    const auto It = Cache.find(Dir);
    if (It == Cache.end()) {
      return -1;
    }
    char DirPath[PATH_MAX];
    if (!CopyPath(Dir, DirPath)) {
      return -1;
    }
    struct stat PathInfo {};
    struct stat FDInfo {};
    if (::fstatat(RootFD, DirPath, &PathInfo, 0) != 0 || PathInfo.st_dev != It->second.Dev || PathInfo.st_ino != It->second.Ino) {
      return -1;
    }
    if (::fstat(It->second.FD, &FDInfo) != 0 || FDInfo.st_dev != It->second.Dev || FDInfo.st_ino != It->second.Ino) {
      return -1;
    }
    return It->second.FD;
  }

  // Resolves Dir in-root and caches it when the kernel's own resolution agrees.
  // Returns 0 when cached, the errno when Dir doesn't resolve to a directory, or
  // -1 when it resolves but can't be cached (the caller then walks the full path).
  int Insert(int RootFD, std::string_view Dir) {
    char DirPath[PATH_MAX];
    if (!CopyPath(Dir, DirPath)) {
      return -1;
    }
    const int DirFD = Walk(RootFD, Dir, O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
    if (DirFD < 0) {
      return errno > 0 ? errno : ENOENT;
    }
    struct stat FDInfo {};
    struct stat PathInfo {};
    if (::fstat(DirFD, &FDInfo) != 0 || ::fstatat(RootFD, DirPath, &PathInfo, 0) != 0 || FDInfo.st_dev != PathInfo.st_dev ||
        FDInfo.st_ino != PathInfo.st_ino) {
      // An absolute or escaping symlink: only the full walk resolves it correctly.
      ::close(DirFD);
      return -1;
    }

    std::unique_lock lk(CacheMutex);
    auto It = Cache.find(Dir);
    if (It != Cache.end()) {
      // Replace a stale entry. Readers use descriptors under the shared lock only.
      Track(Context, It->second.FD, false);
      ::close(It->second.FD);
      It->second = DirEntry {DirFD, FDInfo.st_dev, FDInfo.st_ino};
    } else {
      if (Cache.size() >= MaxCachedDirs) {
        ::close(DirFD);
        return -1;
      }
      Cache.emplace(fextl::string(Dir), DirEntry {DirFD, FDInfo.st_dev, FDInfo.st_ino});
    }
    Track(Context, DirFD, true);
    return 0;
  }

  // Component-by-component in-root resolution. Intermediate directories cost one
  // openat and one close; symlinks and non-directories take an extra fstatat.
  static int Walk(int RootFD, std::string_view Input, int Flags, mode_t Mode) {
    struct Directories {
      fextl::vector<int> FDs;
      ~Directories() {
        const int Error = errno;
        for (size_t i = 1; i < FDs.size(); ++i) {
          ::close(FDs[i]);
        }
        errno = Error;
      }
      void ResetToRoot() {
        while (FDs.size() > 1) {
          ::close(FDs.back());
          FDs.pop_back();
        }
      }
    } Dirs;
    Dirs.FDs.reserve(16);
    Dirs.FDs.push_back(RootFD);

    bool TrailingSlash = !Input.empty() && Input.back() == '/';
    fextl::string Buffer;
    std::string_view Rest = Input;
    unsigned Links = 0;

    auto Fail = [](int Error) {
      errno = Error;
      return -1;
    };

    // Replaces the component just consumed with the target of the symlink at Parent/Name.
    auto FollowLink = [&](int Parent, const char* Name, bool Last) -> int {
      if (++Links > MaxSymlinks) {
        return ELOOP;
      }
      char Target[PATH_MAX];
      const auto Length = ::readlinkat(Parent, Name, Target, sizeof(Target));
      if (Length < 0) {
        return errno;
      }
      if (Length == static_cast<ssize_t>(sizeof(Target))) {
        return ENAMETOOLONG;
      }
      if (Length == 0) {
        return ENOENT;
      }
      if (Last && Target[Length - 1] == '/') {
        TrailingSlash = true;
      }
      if (Target[0] == '/') {
        Dirs.ResetToRoot();
      }
      fextl::string Next(Target, static_cast<size_t>(Length));
      Next.push_back('/');
      Next.append(Rest.data(), Rest.size());
      Buffer.swap(Next);
      Rest = Buffer;
      return 0;
    };

    for (;;) {
      while (!Rest.empty() && Rest.front() == '/') {
        Rest.remove_prefix(1);
      }
      if (Rest.empty()) {
        break;
      }
      const size_t Slash = Rest.find('/');
      const std::string_view Part = Rest.substr(0, Slash);
      Rest = Slash == std::string_view::npos ? std::string_view {} : Rest.substr(Slash);
      const bool Last = Rest.find_first_not_of('/') == std::string_view::npos;

      if (Part == ".") {
        continue;
      }
      if (Part == "..") {
        if (Dirs.FDs.size() > 1) {
          ::close(Dirs.FDs.back());
          Dirs.FDs.pop_back();
        }
        continue;
      }
      if (Part.size() > NAME_MAX) {
        return Fail(ENAMETOOLONG);
      }
      char Name[NAME_MAX + 1];
      memcpy(Name, Part.data(), Part.size());
      Name[Part.size()] = '\0';
      const int Parent = Dirs.FDs.back();

      if (!Last) {
        const int Entry = ::openat(Parent, Name, O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
        if (Entry >= 0) {
          Dirs.FDs.push_back(Entry);
          continue;
        }
        if (errno != ENOTDIR) {
          return -1;
        }
        struct stat Info {};
        if (::fstatat(Parent, Name, &Info, AT_SYMLINK_NOFOLLOW) != 0) {
          return -1;
        }
        if (!S_ISLNK(Info.st_mode)) {
          return Fail(ENOTDIR);
        }
        if (const int Error = FollowLink(Parent, Name, false)) {
          return Fail(Error);
        }
        continue;
      }

      // Last component. O_EXCL creation never follows a link.
      const bool NoFollow = (Flags & O_NOFOLLOW) || ((Flags & O_CREAT) && (Flags & O_EXCL));
      const int DirectoryFlag = TrailingSlash ? O_DIRECTORY : 0;
      const int Result = ::openat(Parent, Name, Flags | O_NOFOLLOW | DirectoryFlag, Mode);
      if (Result >= 0) {
        if (!(Flags & O_PATH) || NoFollow) {
          return Result;
        }
        // O_PATH|O_NOFOLLOW opens a symlink itself; follow it as the guest asked.
        struct stat Info {};
        if (::fstat(Result, &Info) != 0 || !S_ISLNK(Info.st_mode)) {
          return Result;
        }
        ::close(Result);
      } else {
        const int Error = errno;
        const bool MaybeLink = Error == ELOOP || (Error == ENOTDIR && (DirectoryFlag || (Flags & O_DIRECTORY)));
        if (!MaybeLink || NoFollow) {
          return Fail(Error);
        }
        struct stat Info {};
        if (::fstatat(Parent, Name, &Info, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISLNK(Info.st_mode)) {
          return Fail(Error);
        }
      }
      if (const int Error = FollowLink(Parent, Name, true)) {
        return Fail(Error);
      }
    }

    // Every component was consumed ("dir/.", "dir/..", or the root itself).
    return ::openat(Dirs.FDs.back(), ".", Flags | (TrailingSlash ? O_DIRECTORY : 0), Mode);
  }
};
} // namespace FEX::HLE
