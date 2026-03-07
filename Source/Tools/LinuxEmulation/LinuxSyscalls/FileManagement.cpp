// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|common
desc: Rootfs overlay logic
$end_info$
*/

#include "Common/Config.h"
#include "Common/FDUtils.h"
#include "Common/JSONPool.h"

#include "FEXCore/Config/Config.h"
#include "LinuxSyscalls/FileManagement.h"
#include "LinuxSyscalls/EmulatedFiles/EmulatedFiles.h"
#include "LinuxSyscalls/Syscalls.h"
#include "LinuxSyscalls/x64/Syscalls.h"

#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/FileLoading.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/list.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>
#include <FEXHeaderUtils/Filesystem.h>
#include <FEXHeaderUtils/SymlinkChecks.h>
#include <FEXHeaderUtils/Syscalls.h>

#include <algorithm>
#include <errno.h>
#include <cstring>
#include <linux/memfd.h>
#include <linux/openat2.h>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdio.h>
#include <string_view>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/xattr.h>
#include <syscall.h>
#include <system_error>
#include <unistd.h>
#include <utility>

#include <tiny-json.h>

namespace FEX::HLE {
#ifdef __ANDROID__
namespace {
static bool ContainsControllerToken(std::string_view value, std::string_view token) {
  size_t start = 0;
  while (start <= value.size()) {
    const size_t comma = value.find(',', start);
    const size_t end = comma == std::string_view::npos ? value.size() : comma;
    if (value.substr(start, end - start) == token) {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return false;
}

struct AndroidCgroupMountInfo {
  fextl::string UnifiedMount;
  fextl::string CPUMount;
  fextl::string MemoryMount;
};

static bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

static bool EndsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::optional<fextl::string> ReadSmallTextFile(const char* path) {
  FILE* fp = fopen(path, "re");
  if (!fp) {
    return std::nullopt;
  }

  char buffer[512] {};
  const size_t read = fread(buffer, 1, sizeof(buffer) - 1, fp);
  const bool hadError = ferror(fp) != 0;
  fclose(fp);
  if (read == 0 && hadError) {
    return std::nullopt;
  }

  buffer[read] = '\0';
  while (buffer[0] != '\0' && (buffer[strlen(buffer) - 1] == '\n' || buffer[strlen(buffer) - 1] == '\r')) {
    buffer[strlen(buffer) - 1] = '\0';
  }
  return fextl::string {buffer};
}

static bool GetSelfCgroupPath(std::string_view controller, fextl::string* outPath) {
  if (!outPath) {
    return false;
  }

  FILE* fp = fopen("/proc/self/cgroup", "re");
  if (!fp) {
    return false;
  }

  char line[512] {};
  bool found = false;
  while (fgets(line, sizeof(line), fp)) {
    char* first = strchr(line, ':');
    if (!first) {
      continue;
    }
    char* second = strchr(first + 1, ':');
    if (!second) {
      continue;
    }

    *first = '\0';
    *second = '\0';
    const char* controllers = first + 1;
    char* path = second + 1;
    path[strcspn(path, "\r\n")] = '\0';

    if (controller.empty()) {
      if (controllers[0] == '\0') {
        *outPath = path;
        found = true;
        break;
      }
      continue;
    }

    std::string_view controllersView {controllers};
    size_t start = 0;
    while (start <= controllersView.size()) {
      const size_t comma = controllersView.find(',', start);
      const size_t end = comma == std::string_view::npos ? controllersView.size() : comma;
      if (controllersView.substr(start, end - start) == controller) {
        *outPath = path;
        found = true;
        break;
      }
      if (comma == std::string_view::npos) {
        break;
      }
      start = comma + 1;
    }

    if (found) {
      break;
    }
  }

  fclose(fp);
  return found;
}

static AndroidCgroupMountInfo GetAndroidCgroupMountInfo() {
  AndroidCgroupMountInfo info {};
  FILE* fp = fopen("/proc/self/mountinfo", "re");
  if (!fp) {
    return info;
  }

  char line[1024] {};
  while (fgets(line, sizeof(line), fp)) {
    char* separator = strstr(line, " - ");
    if (!separator) {
      continue;
    }

    *separator = '\0';
    char* right = separator + 3;

    char mountID[32] {};
    char parentID[32] {};
    char majorMinor[32] {};
    char root[PATH_MAX] {};
    char mountPoint[PATH_MAX] {};
    if (sscanf(line, "%31s %31s %31s %1023s %1023s", mountID, parentID, majorMinor, root, mountPoint) < 5) {
      continue;
    }

    if (StartsWith(right, "cgroup2 ")) {
      info.UnifiedMount = mountPoint;
      continue;
    }

    if (!StartsWith(right, "cgroup ")) {
      continue;
    }

    const char* superOptions = strrchr(right, ' ');
    if (!superOptions) {
      continue;
    }
    superOptions += 1;

    if (ContainsControllerToken(superOptions, "cpu")) {
      info.CPUMount = mountPoint;
    }
    if (ContainsControllerToken(superOptions, "memory")) {
      info.MemoryMount = mountPoint;
    }
  }

  fclose(fp);
  return info;
}

static fextl::string JoinMountPath(const fextl::string& mount, const fextl::string& relative, std::string_view leaf) {
  if (mount.empty()) {
    return {};
  }

  fextl::string path = mount;
  if (relative.empty() || relative == "/") {
    path += "/";
  } else if (relative.front() != '/') {
    path += "/";
    path += relative;
    path += "/";
  } else {
    path += relative;
    if (path.back() != '/') {
      path += "/";
    }
  }
  path += leaf;
  return path;
}

static std::optional<int64_t> ParseSignedInt(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }

  errno = 0;
  char* end {};
  const auto value = strtoll(text.data(), &end, 10);
  if (errno != 0 || end == text.data()) {
    return std::nullopt;
  }
  return value;
}

static fextl::string BuildMemoryMaxContent(const fextl::string& legacyValue) {
  constexpr int64_t UnlimitedThreshold = INT64_MAX - 4096;
  if (auto parsed = ParseSignedInt(legacyValue); parsed.has_value() && *parsed >= UnlimitedThreshold) {
    return "max\n";
  }
  return legacyValue + "\n";
}

static fextl::string BuildCPUMaxContent(const fextl::string& cpuMount, const fextl::string& cpuPath) {
  const auto quotaPath = JoinMountPath(cpuMount, cpuPath, "cpu.cfs_quota_us");
  const auto periodPath = JoinMountPath(cpuMount, cpuPath, "cpu.cfs_period_us");
  const auto quotaText = ReadSmallTextFile(quotaPath.c_str());
  const auto periodText = ReadSmallTextFile(periodPath.c_str());

  if (quotaText && periodText) {
    const auto quota = ParseSignedInt(*quotaText);
    const auto period = ParseSignedInt(*periodText);
    if (quota && period && *period > 0) {
      if (*quota < 0) {
        return fextl::fmt::format("max {}\n", *period);
      }
      return fextl::fmt::format("{} {}\n", *quota, *period);
    }
  }

  return "max 100000\n";
}

static std::optional<fextl::string> CreateCompatMemfdPath(std::string_view name, const fextl::string& contents) {
  const std::string memfdName {name};
  const int fd = static_cast<int>(::syscall(SYS_memfd_create, memfdName.c_str(), MFD_CLOEXEC));
  if (fd == -1) {
    const char* tmpdir = getenv("TMPDIR");
    if (!tmpdir || tmpdir[0] == '\0') {
      tmpdir = "/data/local/tmp";
    }

    char pathTemplate[PATH_MAX] {};
    snprintf(pathTemplate, sizeof(pathTemplate), "%s/%s-XXXXXX", tmpdir, memfdName.c_str());
    const int tmpfd = mkstemp(pathTemplate);
    if (tmpfd == -1) {
      return std::nullopt;
    }

    if (!contents.empty()) {
      const ssize_t written = write(tmpfd, contents.data(), contents.size());
      if (written != static_cast<ssize_t>(contents.size())) {
        close(tmpfd);
        unlink(pathTemplate);
        return std::nullopt;
      }
    }

    close(tmpfd);
    return fextl::string {pathTemplate};
  }

  if (ftruncate(fd, contents.size()) != 0) {
    close(fd);
    return std::nullopt;
  }

  if (!contents.empty()) {
    const ssize_t written = write(fd, contents.data(), contents.size());
    if (written != static_cast<ssize_t>(contents.size())) {
      close(fd);
      return std::nullopt;
    }
  }

  return fextl::fmt::format("/proc/self/fd/{}", fd);
}

static int CreateCompatContentsFD(std::string_view name, const fextl::string& contents, int flags) {
  const std::string memfdName {name};
  int fd = static_cast<int>(::syscall(SYS_memfd_create, memfdName.c_str(), 0));
  if (fd == -1) {
    const char* tmpdir = getenv("TMPDIR");
    if (!tmpdir || tmpdir[0] == '\0') {
      tmpdir = "/data/local/tmp";
    }

    char pathTemplate[PATH_MAX] {};
    snprintf(pathTemplate, sizeof(pathTemplate), "%s/%s-XXXXXX", tmpdir, memfdName.c_str());
    fd = mkstemp(pathTemplate);
    if (fd == -1) {
      return -1;
    }
    unlink(pathTemplate);
  }

  if (ftruncate(fd, contents.size()) != 0) {
    close(fd);
    return -1;
  }

  if (!contents.empty()) {
    const ssize_t written = write(fd, contents.data(), contents.size());
    if (written != static_cast<ssize_t>(contents.size())) {
      close(fd);
      return -1;
    }
  }

  if (lseek(fd, 0, SEEK_SET) == -1) {
    close(fd);
    return -1;
  }

  if (flags & O_CLOEXEC) {
    const int current = fcntl(fd, F_GETFD);
    if (current != -1) {
      fcntl(fd, F_SETFD, current | FD_CLOEXEC);
    }
  }

  return fd;
}

static int OpenAndroidCompatCgroupFile(const char* pathname, int flags) {
  if (!pathname || pathname[0] != '/' || !StartsWith(pathname, "/sys/fs/cgroup/")) {
    errno = ENOENT;
    return -1;
  }

  const bool wantsCPUMax = EndsWith(pathname, "/cpu.max");
  const bool wantsMemoryMax = EndsWith(pathname, "/memory.max");
  if (!wantsCPUMax && !wantsMemoryMax) {
    errno = ENOENT;
    return -1;
  }

  const auto mounts = GetAndroidCgroupMountInfo();
  fextl::string unifiedPath {};
  if (GetSelfCgroupPath("", &unifiedPath) && !mounts.UnifiedMount.empty()) {
    const auto v2Path = JoinMountPath(mounts.UnifiedMount, unifiedPath, wantsCPUMax ? "cpu.max" : "memory.max");
    if (!v2Path.empty() && access(v2Path.c_str(), F_OK) == 0) {
      return ::syscall(SYSCALL_DEF(openat), AT_FDCWD, v2Path.c_str(), flags, 0);
    }
  }

  if (wantsMemoryMax) {
    fextl::string memoryPath {};
    if (GetSelfCgroupPath("memory", &memoryPath) && !mounts.MemoryMount.empty()) {
      const auto legacyPath = JoinMountPath(mounts.MemoryMount, memoryPath, "memory.limit_in_bytes");
      if (auto legacyValue = ReadSmallTextFile(legacyPath.c_str()); legacyValue.has_value()) {
        return CreateCompatContentsFD("android-memory.max", BuildMemoryMaxContent(*legacyValue), flags);
      }
    }
    errno = ENOENT;
    return -1;
  }

  fextl::string cpuPath {};
  if (GetSelfCgroupPath("cpu", &cpuPath) && !mounts.CPUMount.empty()) {
    return CreateCompatContentsFD("android-cpu.max", BuildCPUMaxContent(mounts.CPUMount, cpuPath), flags);
  }

  errno = ENOENT;
  return -1;
}

static std::optional<fextl::string> GetAndroidCompatCgroupPath(const char* pathname) {
  if (!pathname || pathname[0] != '/' || !StartsWith(pathname, "/sys/fs/cgroup/")) {
    return std::nullopt;
  }

  const bool wantsCPUMax = EndsWith(pathname, "/cpu.max");
  const bool wantsMemoryMax = EndsWith(pathname, "/memory.max");
  if (!wantsCPUMax && !wantsMemoryMax) {
    return std::nullopt;
  }

  const auto mounts = GetAndroidCgroupMountInfo();
  fextl::string unifiedPath {};
  if (GetSelfCgroupPath("", &unifiedPath) && !mounts.UnifiedMount.empty()) {
    const auto v2Path = JoinMountPath(mounts.UnifiedMount, unifiedPath, wantsCPUMax ? "cpu.max" : "memory.max");
    if (!v2Path.empty() && access(v2Path.c_str(), F_OK) == 0) {
      return v2Path;
    }
  }

  if (wantsMemoryMax) {
    fextl::string memoryPath {};
    if (GetSelfCgroupPath("memory", &memoryPath) && !mounts.MemoryMount.empty()) {
      const auto legacyPath = JoinMountPath(mounts.MemoryMount, memoryPath, "memory.limit_in_bytes");
      if (auto legacyValue = ReadSmallTextFile(legacyPath.c_str()); legacyValue.has_value()) {
        return CreateCompatMemfdPath("android-memory.max", BuildMemoryMaxContent(*legacyValue));
      }
    }
    return std::nullopt;
  }

  fextl::string cpuPath {};
  if (GetSelfCgroupPath("cpu", &cpuPath) && !mounts.CPUMount.empty()) {
    return CreateCompatMemfdPath("android-cpu.max", BuildCPUMaxContent(mounts.CPUMount, cpuPath));
  }

  return std::nullopt;
}
}
#else
static std::optional<fextl::string> GetAndroidCompatCgroupPath(const char*) {
  return std::nullopt;
}
#endif

bool FileManager::RootFSPathExists(const char* Filepath) const {
  LOGMAN_THROW_A_FMT(Filepath && Filepath[0] == '/', "Filepath needs to be absolute");
  return FHU::Filesystem::ExistsAt(RootFSFD, Filepath + 1);
}

void FileManager::LoadThunkDatabase(fextl::unordered_map<fextl::string, ThunkDBObject>& ThunkDB, bool Global) {
  auto ThunkDBPath = FEXCore::Config::GetConfigDirectory(Global) + "ThunksDB.json";
  fextl::vector<char> FileData;
  if (FEXCore::FileLoading::LoadFile(FileData, ThunkDBPath)) {

    // If the thunksDB file exists then we need to check if the rootfs supports multi-arch or not.
    const bool RootFSIsMultiarch = RootFSPathExists("/usr/lib/x86_64-linux-gnu/") || RootFSPathExists("/usr/lib/i386-linux-gnu/");

    fextl::vector<fextl::string> PathPrefixes {};
    if (RootFSIsMultiarch) {
      // Multi-arch debian distros have a fairly complex arrangement of filepaths.
      // These fractal out to the combination of library prefixes with arch suffixes.
      constexpr static std::array<std::string_view, 4> LibPrefixes = {
        "/usr/lib",
        "/usr/local/lib",
        "/lib",
        "/usr/lib/pressure-vessel/overrides/lib",
      };

      // We only need to generate 32-bit or 64-bit depending on the operating mode.
      const auto ArchPrefix = Is64BitMode() ? "x86_64-linux-gnu" : "i386-linux-gnu";

      for (auto Prefix : LibPrefixes) {
        PathPrefixes.emplace_back(fextl::fmt::format("{}/{}", Prefix, ArchPrefix));
      }
    } else {
      // Non multi-arch supporting distros like Fedora and Debian have a much more simple layout.
      // lib/ folders refer to 32-bit library folders.
      // li64/ folders refer to 64-bit library folders.
      constexpr static std::array<std::string_view, 4> LibPrefixes = {
        "/usr",
        "/usr/local",
        "", // root, the '/' will be appended in the next step.
        "/usr/lib/pressure-vessel/overrides",
      };

      // We only need to generate 32-bit or 64-bit depending on the operating mode.
      const auto ArchPrefix = Is64BitMode() ? "lib64" : "lib";

      for (auto Prefix : LibPrefixes) {
        PathPrefixes.emplace_back(fextl::fmt::format("{}/{}", Prefix, ArchPrefix));
      }
    }

    FEX::JSON::JsonAllocator Pool {};
    const json_t* json = FEX::JSON::CreateJSON(FileData, Pool);

    if (!json) {
      ERROR_AND_DIE_FMT("Failed to parse JSON from ThunkDB file '{}' - invalid JSON format", ThunkDBPath);
    }

    const json_t* DB = json_getProperty(json, "DB");
    if (!DB || JSON_OBJ != json_getType(DB)) {
      return;
    }

    auto HomeDirectory = FEX::Config::GetHomeDirectory();

    for (const json_t* Library = json_getChild(DB); Library != nullptr; Library = json_getSibling(Library)) {
      // Get the user defined name for the library
      const char* LibraryName = json_getName(Library);
      auto DBObject = ThunkDB.insert_or_assign(LibraryName, ThunkDBObject {}).first;

      // Walk the libraries items to get the data
      for (const json_t* LibraryItem = json_getChild(Library); LibraryItem != nullptr; LibraryItem = json_getSibling(LibraryItem)) {
        std::string_view ItemName = json_getName(LibraryItem);

        if (ItemName == "Library") {
          // "Library": "libGL-guest.so"
          DBObject->second.LibraryName = json_getValue(LibraryItem);
        } else if (ItemName == "Depends") {
          jsonType_t PropertyType = json_getType(LibraryItem);
          if (PropertyType == JSON_TEXT) {
            DBObject->second.Depends.emplace(json_getValue(LibraryItem));
          } else if (PropertyType == JSON_ARRAY) {
            for (const json_t* Depend = json_getChild(LibraryItem); Depend != nullptr; Depend = json_getSibling(Depend)) {
              DBObject->second.Depends.emplace(json_getValue(Depend));
            }
          }
        } else if (ItemName == "Overlay") {
          auto AddWithReplacement = [HomeDirectory, &PathPrefixes](ThunkDBObject& DBObject, std::string_view LibraryItem) {
            // Walk through template string and fill in prefixes from right to left

            using namespace std::string_view_literals;
            const std::pair PrefixHome {"@HOME@"sv, LibraryItem.find("@HOME@")};
            const std::pair PrefixLib {"@PREFIX_LIB@"sv, LibraryItem.find("@PREFIX_LIB@")};

            fextl::string::size_type PrefixPositions[] = {
              PrefixHome.second,
              PrefixLib.second,
            };
            // Sort offsets in descending order to enable safe in-place replacement
            std::sort(std::begin(PrefixPositions), std::end(PrefixPositions), std::greater<> {});

            for (const auto& LibPrefix : PathPrefixes) {
              fextl::string Replacement(LibraryItem);
              for (auto PrefixPos : PrefixPositions) {
                if (PrefixPos == fextl::string::npos) {
                  continue;
                } else if (PrefixPos == PrefixHome.second) {
                  Replacement.replace(PrefixPos, PrefixHome.first.size(), HomeDirectory);
                } else if (PrefixPos == PrefixLib.second) {
                  Replacement.replace(PrefixPos, PrefixLib.first.size(), LibPrefix);
                }
              }
              DBObject.Overlays.emplace_back(std::move(Replacement));

              if (PrefixLib.second == fextl::string::npos) {
                // Don't repeat for other LibPrefixes entries if the prefix wasn't used
                break;
              }
            }
          };

          jsonType_t PropertyType = json_getType(LibraryItem);
          if (PropertyType == JSON_TEXT) {
            AddWithReplacement(DBObject->second, json_getValue(LibraryItem));
          } else if (PropertyType == JSON_ARRAY) {
            for (const json_t* Overlay = json_getChild(LibraryItem); Overlay != nullptr; Overlay = json_getSibling(Overlay)) {
              AddWithReplacement(DBObject->second, json_getValue(Overlay));
            }
          }
        }
      }
    }
  }
}

FileManager::FileManager(FEXCore::Context::Context* ctx)
  : EmuFD {ctx} {
  const auto& ThunkConfigFile = ThunkConfig();

  // We try to load ThunksDB from:
  // - FEX global config
  // - FEX user config
  // - Defined ThunksConfig option
  // - Steam AppConfig Global
  // - AppConfig Global
  // - Steam AppConfig Local
  // - AppConfig Local
  // - AppConfig override
  // This doesn't support the classic thunks interface.

  const auto& AppName = AppConfigName();
  fextl::vector<fextl::string> ConfigPaths {
    FEXCore::Config::GetConfigFileLocation(true),
    FEXCore::Config::GetConfigFileLocation(false),
    ThunkConfigFile,
  };

  auto SteamID = getenv("SteamAppId");
  if (SteamID) {
    // If a SteamID exists then let's search for Steam application configs as well.
    // We want to key off both the SteamAppId number /and/ the executable since we may not want to thunk all binaries.
    fextl::string SteamAppName = fextl::fmt::format("Steam_{}_{}", SteamID, AppName);

    // Steam application configs interleaved with non-steam for priority sorting.
    ConfigPaths.emplace_back(FEXCore::Config::GetApplicationConfig(SteamAppName, true));
    ConfigPaths.emplace_back(FEXCore::Config::GetApplicationConfig(AppName, true));
    ConfigPaths.emplace_back(FEXCore::Config::GetApplicationConfig(SteamAppName, false));
    ConfigPaths.emplace_back(FEXCore::Config::GetApplicationConfig(AppName, false));
  } else {
    ConfigPaths.emplace_back(FEXCore::Config::GetApplicationConfig(AppName, true));
    ConfigPaths.emplace_back(FEXCore::Config::GetApplicationConfig(AppName, false));
  }

  const char* AppConfig = getenv("FEX_APP_CONFIG");
  if (AppConfig) {
    ConfigPaths.emplace_back(AppConfig);
  }

  if (!LDPath().empty()) {
    RootFSFD = open(LDPath().c_str(), O_DIRECTORY | O_PATH | O_CLOEXEC);
    if (RootFSFD == -1) {
      RootFSFD = AT_FDCWD;
    } else {
      TrackFEXFD(RootFSFD);
    }
  }

  fextl::unordered_map<fextl::string, ThunkDBObject> ThunkDB;
  LoadThunkDatabase(ThunkDB, true);
  LoadThunkDatabase(ThunkDB, false);

  for (const auto& Path : ConfigPaths) {
    fextl::vector<char> FileData;
    if (FEXCore::FileLoading::LoadFile(FileData, Path)) {
      FEX::JSON::JsonAllocator Pool {};

      // If a thunks DB property exists then we pull in data from the thunks database
      const json_t* json = FEX::JSON::CreateJSON(FileData, Pool);
      if (!json) {
        continue;
      }

      const json_t* ThunksDB = json_getProperty(json, "ThunksDB");
      if (!ThunksDB) {
        continue;
      }

      for (const json_t* Item = json_getChild(ThunksDB); Item != nullptr; Item = json_getSibling(Item)) {
        const char* LibraryName = json_getName(Item);
        bool LibraryEnabled = json_getInteger(Item) != 0;
        // If the library is enabled then find it in the DB
        auto DBObject = ThunkDB.find(LibraryName);
        if (DBObject != ThunkDB.end()) {
          DBObject->second.Enabled = LibraryEnabled;
        }
      }
    }
  }

  // Now that we loaded the thunks object, walk through and ensure dependencies are enabled as well
  auto ThunkGuestPath = ThunkGuestLibs();
  while (ThunkGuestPath.ends_with('/')) {
    ThunkGuestPath.pop_back();
  }
  if (!Is64BitMode()) {
    ThunkGuestPath += "_32";
  }
  for (const auto& DBObject : ThunkDB) {
    if (!DBObject.second.Enabled) {
      continue;
    }

    // Recursively add paths for this thunk library and its dependencies to ThunkOverlays.
    // Using a local struct for this is slightly less ugly than using self-capturing lambdas
    struct {
      decltype(FileManager::ThunkOverlays)& ThunkOverlays;
      decltype(ThunkDB)& ThunkDB;
      const fextl::string& ThunkGuestPath;
      bool Is64BitMode;

      void SetupOverlay(const ThunkDBObject& DBDepend) {
        auto ThunkPath = fextl::fmt::format("{}/{}", ThunkGuestPath, DBDepend.LibraryName);
        if (!FHU::Filesystem::Exists(ThunkPath)) {
          if (!Is64BitMode) {
            // Guest libraries not existing is expected since not all libraries are thunked on 32-bit
            return;
          }
          ERROR_AND_DIE_FMT("Requested thunking via guest library \"{}\" that does not exist", ThunkPath);
        }

        for (const auto& Overlay : DBDepend.Overlays) {
          // Direct full path in guest RootFS to our overlay file
          ThunkOverlays.emplace(Overlay, ThunkPath);
        }
      };

      void InsertDependencies(const fextl::unordered_set<fextl::string>& Depends) {
        for (const auto& Depend : Depends) {
          auto& DBDepend = ThunkDB.at(Depend);
          if (DBDepend.Enabled) {
            continue;
          }

          SetupOverlay(DBDepend);

          // Mark enabled and recurse into dependencies
          DBDepend.Enabled = true;
          InsertDependencies(DBDepend.Depends);
        }
      };
    } DBObjectHandler {ThunkOverlays, ThunkDB, ThunkGuestPath, Is64BitMode()};

    DBObjectHandler.SetupOverlay(DBObject.second);
    DBObjectHandler.InsertDependencies(DBObject.second.Depends);
  }

  if (false) {
    // Useful for debugging
    if (ThunkOverlays.size()) {
      LogMan::Msg::IFmt("Thunk Overlays:");
      for (const auto& [Overlay, ThunkPath] : ThunkOverlays) {
        LogMan::Msg::IFmt("\t{} -> {}", Overlay, ThunkPath);
      }
    }
  }

  // Keep an fd open for /proc, to bypass chroot-style sandboxes
  ProcFD = open("/proc", O_RDONLY | O_CLOEXEC);
  if (ProcFD != -1) {
    // Track the st_dev of /proc, to check for inode equality
    struct stat Buffer;
    auto Result = fstat(ProcFD, &Buffer);
    if (Result >= 0) {
      ProcFSDev = Buffer.st_dev;
    }
  } else {
    LogMan::Msg::EFmt("Couldn't open `/proc`. Is ProcFS mounted? FEX won't be able to track FD conflicts");
  }

  UpdatePID(::getpid());
}

FileManager::~FileManager() {
  close(RootFSFD);
}

size_t FileManager::GetRootFSPrefixLen(const char* pathname, size_t len, bool AliasedOnly) const {
  if (len < 2 ||            // If no pathname or root
      pathname[0] != '/') { // If we are getting root
    return 0;
  }

  const auto& RootFSPath = LDPath();
  if (RootFSPath.empty()) { // If RootFS doesn't exist
    return 0;
  }

  auto RootFSLen = RootFSPath.length();
  if (RootFSPath.ends_with("/")) {
    RootFSLen -= 1;
  }

  if (RootFSLen > len) {
    return 0;
  }

  if (memcmp(pathname, RootFSPath.c_str(), RootFSLen) || (len > RootFSLen && pathname[RootFSLen] != '/')) {
    return 0; // If the path is not within the RootFS
  }

  if (AliasedOnly) {
    fextl::string Path(pathname, len); // Need to nul-terminate so copy

    struct stat HostStat {};
    struct stat RootFSStat {};
    if (lstat(Path.c_str(), &RootFSStat)) {
      LogMan::Msg::DFmt("GetRootFSPrefixLen: lstat on RootFS path failed: {}", std::string_view(pathname, len));
      return 0; // RootFS path does not exist?
    }
    if (lstat(Path.c_str() + RootFSLen, &HostStat)) {
      return 0; // Host path does not exist or not accessible
    }
    // Note: We do not check st_dev, since the RootFS might be
    // an overlayfs mount that changes it. This means there could
    // be false positives. However, since we check the size too,
    // this is highly unlikely (an overlaid file would need to
    // have the same exact size and coincidentally the same
    // inode number as on the host, which is implausible for things
    // like binaries and libraries).
    if (RootFSStat.st_size != HostStat.st_size || RootFSStat.st_ino != HostStat.st_ino || RootFSStat.st_mode != HostStat.st_mode) {
      return 0; // Host path is a different file
    }
  }

  return RootFSLen;
}

ssize_t FileManager::StripRootFSPrefix(char* pathname, ssize_t len, bool leaky) const {
  if (len < 0) {
    return len;
  }

  auto Prefix = GetRootFSPrefixLen(pathname, len, false);
  if (Prefix == 0) {
    return len;
  }

  if (Prefix == len) {
    if (leaky) {
      // Getting the root, without a trailing /. This is a hack pressure-vessel uses to get the FEX RootFS,
      // so we have to leak it here...
      LogMan::Msg::DFmt("Leaking RootFS path for pressure-vessel");
      return len;
    } else {
      ::strcpy(pathname, "/");
      return 1;
    }
  }

  ::memmove(pathname, pathname + Prefix, len - Prefix);
  pathname[len - Prefix] = '\0';

  return len - Prefix;
}

fextl::string FileManager::GetHostPath(fextl::string& Path, bool AliasedOnly) const {
  auto Prefix = GetRootFSPrefixLen(Path.c_str(), Path.length(), AliasedOnly);

  if (Prefix == 0) {
    return {};
  }

  auto ret = Path.substr(Prefix);
  if (ret.empty()) { // Getting the root
    ret = "/";
  }

  return ret;
}

fextl::string FileManager::GetEmulatedPath(const char* pathname, bool FollowSymlink) const {
  if (!pathname ||                  // If no pathname
      pathname[0] != '/' ||         // If relative
      strcmp(pathname, "/") == 0) { // If we are getting root
    return {};
  }

  auto thunkOverlay = ThunkOverlays.find(pathname);
  if (thunkOverlay != ThunkOverlays.end()) {
    return thunkOverlay->second;
  }

  const auto& RootFSPath = LDPath();
  if (RootFSPath.empty()) { // If RootFS doesn't exist
    return {};
  }

  fextl::string Path = RootFSPath + pathname;
  if (FollowSymlink) {
    char Filename[PATH_MAX];
    while (FEX::HLE::IsSymlink(AT_FDCWD, Path.c_str())) {
      auto SymlinkSize = FEX::HLE::GetSymlink(AT_FDCWD, Path.c_str(), Filename, PATH_MAX - 1);
      if (SymlinkSize > 0 && Filename[0] == '/') {
        Path = RootFSPath;
        Path += std::string_view(Filename, SymlinkSize);
      } else {
        break;
      }
    }
  }
  return Path;
}

FileManager::EmulatedFDPathResult
FileManager::GetEmulatedFDPath(int dirfd, const char* pathname, bool FollowSymlink, FDPathTmpData& TmpFilename) const {
  constexpr auto NoEntry = EmulatedFDPathResult {-1, nullptr};

  if (!pathname) {
    // No pathname.
    return NoEntry;
  }

  if (pathname[0] == '/') {
    // If the path is absolute then dirfd is ignored.
    dirfd = AT_FDCWD;
  }

  if (pathname[0] != '/' || // If relative
      pathname[1] == 0 ||   // If we are getting root
      dirfd != AT_FDCWD) {  // If dirfd isn't special FDCWD
    return NoEntry;
  }

  auto thunkOverlay = ThunkOverlays.find(pathname);
  if (thunkOverlay != ThunkOverlays.end()) {
    return EmulatedFDPathResult {AT_FDCWD, thunkOverlay->second.c_str()};
  }

  if (RootFSFD == AT_FDCWD) {
    // If RootFS doesn't exist
    return NoEntry;
  }

  // Starting subpath is the pathname passed in.
  const char* SubPath = pathname;

  // Current index for the temporary path to use.
  uint32_t CurrentIndex {};

  // The two temporary paths.
  const std::array<char*, 2> TmpPaths = {
    TmpFilename[0],
    TmpFilename[1],
  };

  if (FollowSymlink) {
    // Check if the combination of RootFS FD and subpath with the front '/' stripped off is a symlink.
    bool HadAtLeastOne {};
    struct stat Buffer {};
    for (;;) {
      // We need to check if the filepath exists and is a symlink.
      // If the initial filepath doesn't exist then early exit.
      // If it did exist at some state then trace it all all the way to the final link.
      int Result = fstatat(RootFSFD, &SubPath[1], &Buffer, AT_SYMLINK_NOFOLLOW);
      if (Result != 0 && errno == ENOENT && !HadAtLeastOne) {
        // Initial file didn't exist at all
        return NoEntry;
      }

      const bool IsLink = Result == 0 && S_ISLNK(Buffer.st_mode);

      HadAtLeastOne = true;

      if (IsLink) {
        // Choose the current temporary working path.
        auto CurrentTmp = TmpPaths[CurrentIndex];

        // Get the symlink of RootFS FD + stripped subpath.
        auto SymlinkSize = FEX::HLE::GetSymlink(RootFSFD, &SubPath[1], CurrentTmp, PATH_MAX - 1);

        // This might be a /proc symlink into the RootFS, so strip it in that case.
        SymlinkSize = StripRootFSPrefix(CurrentTmp, SymlinkSize, false);

        if (SymlinkSize > 1 && CurrentTmp[0] == '/') {
          // If the symlink is absolute and not the root:
          // 1) Zero terminate it.
          // 2) Set the path as our current subpath.
          // 3) Switch to the next temporary index. (We don't want to overwrite the current one on the next loop iteration).
          // 4) Run the loop again.
          CurrentTmp[SymlinkSize] = 0;
          SubPath = CurrentTmp;
          CurrentIndex ^= 1;
        } else {
          // If the path wasn't a symlink or wasn't absolute.
          // 1) Break early, returning the previous found result.
          // 2) If first iteration then we return `pathname`.
          break;
        }
      } else {
        break;
      }
    }
  }

  // Return the pair of rootfs FD plus relative subpath by stripping off the front '/'
  return EmulatedFDPathResult {RootFSFD, &SubPath[1]};
}

///< Returns true if the pathname is self and symlink flags are set NOFOLLOW.
bool FileManager::IsSelfNoFollow(const char* Pathname, int flags) const {
  const bool Follow = (flags & AT_SYMLINK_NOFOLLOW) == 0;
  if (Follow) {
    // If we are following the self symlink then we don't care about this.
    return false;
  }

  if (!Pathname) {
    return false;
  }

  char PidSelfPath[50];
  snprintf(PidSelfPath, sizeof(PidSelfPath), "/proc/%i/exe", CurrentPID);

  return strcmp(Pathname, "/proc/self/exe") == 0 || strcmp(Pathname, "/proc/thread-self/exe") == 0 || strcmp(Pathname, PidSelfPath) == 0;
}

std::optional<std::string_view> FileManager::GetSelf(const char* Pathname) const {
  if (!Pathname) {
    return std::nullopt;
  }

  char PidSelfPath[50];
  snprintf(PidSelfPath, sizeof(PidSelfPath), "/proc/%i/exe", CurrentPID);

  if (strcmp(Pathname, "/proc/self/exe") == 0 || strcmp(Pathname, "/proc/thread-self/exe") == 0 || strcmp(Pathname, PidSelfPath) == 0) {
    return Filename();
  }

  return Pathname;
}

static bool ShouldSkipOpenInEmu(int flags) {
  if (flags & O_CREAT) {
    // If trying to create a file then skip checking in emufd
    return true;
  }

  if (flags & O_WRONLY) {
    // If the file is trying to be open with write permissions then skip.
    return true;
  }

  if (flags & O_APPEND) {
    // If the file is trying to be open with append options then skip.
    return true;
  }

  return false;
}

bool FileManager::ReplaceEmuFd(int fd, int flags, uint32_t mode) {
  char Tmp[PATH_MAX + 1];

  if (fd < 0) {
    return false;
  }

  // Get the path of the file we just opened
  auto PathLength = FEX::get_fdpath(fd, Tmp);
  if (PathLength == -1) {
    return false;
  }
  Tmp[PathLength] = '\0';

  // And try to open via EmuFD
  auto EmuFd = EmuFD.Open(Tmp, flags, mode);
  if (EmuFd == -1) {
    return false;
  }

  // If we succeeded, swap out the fd
  ::dup2(EmuFd, fd);
  ::close(EmuFd);
  return true;
}

uint64_t FileManager::Open(const char* pathname, int flags, uint32_t mode) {
  if (const int compatFD = OpenAndroidCompatCgroupFile(pathname, flags); compatFD != -1) {
    int fd = compatFD;
    ReplaceEmuFd(fd, flags, mode);
    return fd;
  }
  const auto CompatPath = GetAndroidCompatCgroupPath(pathname);
  const char* InputPath = CompatPath ? CompatPath->c_str() : pathname;
  auto NewPath = GetSelf(InputPath);
  const char* SelfPath = NewPath ? NewPath->data() : InputPath;
  int fd = -1;

  if (!ShouldSkipOpenInEmu(flags)) {
    FDPathTmpData TmpFilename;
    auto Path = GetEmulatedFDPath(AT_FDCWD, SelfPath, false, TmpFilename);
    if (Path.FD != -1) {
#if defined(__ANDROID__)
      // TODO(Android): Re-enable openat2 + RESOLVE_IN_ROOT when app seccomp policy allows syscall 437.
      fd = ::syscall(SYSCALL_DEF(openat), Path.FD, Path.Path, flags, mode);
#else
      FEX::HLE::open_how how = {
        .flags = (uint64_t)flags,
        .mode = (flags & (O_CREAT | O_TMPFILE)) ? mode & 07777 : 0, // openat2() is stricter about this
        .resolve = (Path.FD == AT_FDCWD) ? 0u : RESOLVE_IN_ROOT,    // AT_FDCWD means it's a thunk and not via RootFS
      };
      fd = ::syscall(SYSCALL_DEF(openat2), Path.FD, Path.Path, &how, sizeof(how));

      if (fd == -1 && errno == EXDEV) {
        // This means a magic symlink (/proc/foo) was involved. In this case we
        // just punt and do the access without RESOLVE_IN_ROOT.
        fd = ::syscall(SYSCALL_DEF(openat), Path.FD, Path.Path, flags, mode);
      }
#endif
    }

    // Open through RootFS failed (probably nonexistent), so open directly.
    if (fd == -1) {
      fd = ::open(SelfPath, flags, mode);
    }

    ReplaceEmuFd(fd, flags, mode);
  } else {
    fd = ::open(SelfPath, flags, mode);
  }

  return fd;
}

uint64_t FileManager::Close(int fd) {
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
  if (CheckIfFDInTrackedSet(fd)) {
    LogMan::Msg::EFmt("{} closing FEX FD {}", __func__, fd);
    RemoveFEXFD(fd);
  }
#endif

  return ::close(fd);
}

uint64_t FileManager::CloseRange(unsigned int first, unsigned int last, unsigned int flags) {
#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
  if (!(flags & CLOSE_RANGE_CLOEXEC) && CheckIfFDRangeInTrackedSet(first, last)) {
    LogMan::Msg::EFmt("{} closing FEX FDs in range ({}, {})", __func__, first, last);
    RemoveFEXFDRange(first, last);
  }
#endif

  return ::syscall(SYSCALL_DEF(close_range), first, last, flags);
}

uint64_t FileManager::Stat(const char* pathname, void* buf) {
  const auto CompatPath = GetAndroidCompatCgroupPath(pathname);
  if (CompatPath) {
    return ::stat(CompatPath->c_str(), reinterpret_cast<struct stat*>(buf));
  }
  const char* InputPath = CompatPath ? CompatPath->c_str() : pathname;
  auto NewPath = GetSelf(InputPath);
  const char* SelfPath = NewPath ? NewPath->data() : InputPath;

  // Stat follows symlinks
  FDPathTmpData TmpFilename;
  auto Path = GetEmulatedFDPath(AT_FDCWD, SelfPath, true, TmpFilename);
  if (Path.FD != -1) {
    uint64_t Result = ::fstatat(Path.FD, Path.Path, reinterpret_cast<struct stat*>(buf), 0);
    if (Result != -1) {
      return Result;
    }
  }
  return ::stat(SelfPath, reinterpret_cast<struct stat*>(buf));
}

uint64_t FileManager::Lstat(const char* pathname, void* buf) {
  const auto CompatPath = GetAndroidCompatCgroupPath(pathname);
  if (CompatPath) {
    return ::lstat(CompatPath->c_str(), reinterpret_cast<struct stat*>(buf));
  }
  const char* InputPath = CompatPath ? CompatPath->c_str() : pathname;
  auto NewPath = GetSelf(InputPath);
  const char* SelfPath = NewPath ? NewPath->data() : InputPath;

  // lstat does not follow symlinks
  FDPathTmpData TmpFilename;
  auto Path = GetEmulatedFDPath(AT_FDCWD, SelfPath, false, TmpFilename);
  if (Path.FD != -1) {
    uint64_t Result = ::fstatat(Path.FD, Path.Path, reinterpret_cast<struct stat*>(buf), AT_SYMLINK_NOFOLLOW);
    if (Result != -1) {
      return Result;
    }
  }

  return ::lstat(SelfPath, reinterpret_cast<struct stat*>(buf));
}

uint64_t FileManager::Access(const char* pathname, [[maybe_unused]] int mode) {
  const auto CompatPath = GetAndroidCompatCgroupPath(pathname);
  if (CompatPath) {
    return ::access(CompatPath->c_str(), mode);
  }
  const char* InputPath = CompatPath ? CompatPath->c_str() : pathname;
  auto NewPath = GetSelf(InputPath);
  const char* SelfPath = NewPath ? NewPath->data() : InputPath;

  // Access follows symlinks
  FDPathTmpData TmpFilename;
  auto Path = GetEmulatedFDPath(AT_FDCWD, SelfPath, true, TmpFilename);
  if (Path.FD != -1) {
    uint64_t Result = ::faccessat(Path.FD, Path.Path, mode, 0);
    if (Result != -1) {
      return Result;
    }
  }
  return ::access(SelfPath, mode);
}

uint64_t FileManager::FAccessat(int dirfd, const char* pathname, int mode) {
  const auto CompatPath = GetAndroidCompatCgroupPath(pathname);
  if (CompatPath) {
    return ::syscall(SYS_faccessat, AT_FDCWD, CompatPath->c_str(), mode);
  }
  const char* InputPath = CompatPath ? CompatPath->c_str() : pathname;
  auto NewPath = GetSelf(InputPath);
  const char* SelfPath = NewPath ? NewPath->data() : InputPath;

  FDPathTmpData TmpFilename;
  auto Path = GetEmulatedFDPath(dirfd, SelfPath, true, TmpFilename);
  if (Path.FD != -1) {
    uint64_t Result = ::syscall(SYSCALL_DEF(faccessat), Path.FD, Path.Path, mode);
    if (Result != -1) {
      return Result;
    }
  }

  return ::syscall(SYS_faccessat, dirfd, SelfPath, mode);
}

uint64_t FileManager::FAccessat2(int dirfd, const char* pathname, int mode, int flags) {
  const auto CompatPath = GetAndroidCompatCgroupPath(pathname);
  if (CompatPath) {
    return ::syscall(SYSCALL_DEF(faccessat2), AT_FDCWD, CompatPath->c_str(), mode, flags);
  }
  const char* InputPath = CompatPath ? CompatPath->c_str() : pathname;
  auto NewPath = GetSelf(InputPath);
  const char* SelfPath = NewPath ? NewPath->data() : InputPath;

  FDPathTmpData TmpFilename;
  auto Path = GetEmulatedFDPath(dirfd, SelfPath, (flags & AT_SYMLINK_NOFOLLOW) == 0, TmpFilename);
  if (Path.FD != -1) {
    uint64_t Result = ::syscall(SYSCALL_DEF(faccessat2), Path.FD, Path.Path, mode, flags);
    if (Result != -1) {
      return Result;
    }
  }

  return ::syscall(SYSCALL_DEF(faccessat2), dirfd, SelfPath, mode, flags);
}

uint64_t FileManager::Readlink(const char* pathname, char* buf, size_t bufsiz) {
  // calculate the non-self link to exe
  // Some executables do getpid, stat("/proc/$pid/exe")
  char PidSelfPath[50];
  snprintf(PidSelfPath, 50, "/proc/%i/exe", CurrentPID);

  const auto CompatPath = GetAndroidCompatCgroupPath(pathname);
  if (CompatPath) {
    return ::readlink(CompatPath->c_str(), buf, bufsiz);
  }
  const char* InputPath = CompatPath ? CompatPath->c_str() : pathname;

  if (strcmp(InputPath, "/proc/self/exe") == 0 || strcmp(InputPath, "/proc/thread-self/exe") == 0 || strcmp(InputPath, PidSelfPath) == 0) {
    const auto& App = Filename();
    strncpy(buf, App.c_str(), bufsiz);
    return std::min(bufsiz, App.size());
  }

  FDPathTmpData TmpFilename;
  auto Path = GetEmulatedFDPath(AT_FDCWD, InputPath, false, TmpFilename);
  uint64_t Result = -1;
  if (Path.FD != -1) {
    Result = ::readlinkat(Path.FD, Path.Path, buf, bufsiz);

    if (Result == -1 && errno == EINVAL) {
      // This means that the file wasn't a symlink
      // This is expected behaviour
      return -1;
    }
  }
  if (Result == -1) {
    Result = ::readlink(InputPath, buf, bufsiz);
  }

  // We might have read a /proc/self/fd/* link. If so, strip the RootFS prefix from it.
  return StripRootFSPrefix(buf, Result, true);
}

uint64_t FileManager::Chmod(const char* pathname, mode_t mode) {
  auto NewPath = GetSelf(pathname);
  const char* SelfPath = NewPath ? NewPath->data() : nullptr;

  FDPathTmpData TmpFilename;
  auto Path = GetEmulatedFDPath(AT_FDCWD, SelfPath, false, TmpFilename);
  if (Path.FD != -1) {
    uint64_t Result = ::fchmodat(Path.FD, Path.Path, mode, 0);
    if (Result != -1) {
      return Result;
    }
  }
  return ::chmod(SelfPath, mode);
}

uint64_t FileManager::Readlinkat(int dirfd, const char* pathname, char* buf, size_t bufsiz) {
  // calculate the non-self link to exe
  // Some executables do getpid, stat("/proc/$pid/exe")
  // Can't use `GetSelf` directly here since readlink{at,} returns EINVAL if it isn't a symlink
  // Self is always a symlink and isn't expected to fail

  fextl::string Path {};
  if (((pathname && pathname[0] != '/') || // If pathname exists then it must not be absolute
       !pathname) &&
      dirfd != AT_FDCWD) {
    // Passed in a dirfd that isn't magic FDCWD
    // We need to get the path from the fd now
    char Tmp[PATH_MAX] = "";
    auto PathLength = FEX::get_fdpath(dirfd, Tmp);
    if (PathLength != -1) {
      Path = fextl::string(Tmp, PathLength);
    }

    if (pathname) {
      if (!Path.empty()) {
        // If the path returned empty then we don't need a separator
        Path += "/";
      }
      Path += pathname;
    }
  } else {
    if (!pathname || strlen(pathname) == 0) {
      return -1;
    } else if (pathname) {
      Path = pathname;
    }
  }

  char PidSelfPath[50];
  snprintf(PidSelfPath, 50, "/proc/%i/exe", CurrentPID);

  if (Path == "/proc/self/exe" || Path == "/proc/thread-self/exe" || Path == PidSelfPath) {
    const auto& App = Filename();
    strncpy(buf, App.c_str(), bufsiz);
    return std::min(bufsiz, App.size());
  }

  FDPathTmpData TmpFilename;
  auto NewPath = GetEmulatedFDPath(dirfd, pathname, false, TmpFilename);
  uint64_t Result = -1;

  if (NewPath.FD != -1) {
    Result = ::readlinkat(NewPath.FD, NewPath.Path, buf, bufsiz);

    if (Result == -1 && errno == EINVAL) {
      // This means that the file wasn't a symlink
      // This is expected behaviour
      return -1;
    }
  }

  if (Result == -1) {
    Result = ::readlinkat(dirfd, pathname, buf, bufsiz);
  }

  // We might have read a /proc/self/fd/* link. If so, strip the RootFS prefix from it.
  return StripRootFSPrefix(buf, Result, true);
}

uint64_t FileManager::Openat([[maybe_unused]] int dirfs, const char* pathname, int flags, uint32_t mode) {
  if (const int compatFD = OpenAndroidCompatCgroupFile(pathname, flags); compatFD != -1) {
    int32_t fd = compatFD;
    ReplaceEmuFd(fd, flags, mode);
    return fd;
  }
  const auto CompatPath = GetAndroidCompatCgroupPath(pathname);
  const char* InputPath = CompatPath ? CompatPath->c_str() : pathname;
  auto NewPath = GetSelf(InputPath);
  const char* SelfPath = NewPath ? NewPath->data() : InputPath;

  int32_t fd = -1;

  if (!ShouldSkipOpenInEmu(flags)) {
    FDPathTmpData TmpFilename;
    auto Path = GetEmulatedFDPath(dirfs, SelfPath, false, TmpFilename);
    if (Path.FD != -1) {
#if defined(__ANDROID__)
      // TODO(Android): Re-enable openat2 + RESOLVE_IN_ROOT when app seccomp policy allows syscall 437.
      fd = ::syscall(SYSCALL_DEF(openat), Path.FD, Path.Path, flags, mode);
#else
      FEX::HLE::open_how how = {
        .flags = (uint64_t)flags,
        .mode = (flags & (O_CREAT | O_TMPFILE)) ? mode & 07777 : 0, // openat2() is stricter about this,
        .resolve = (Path.FD == AT_FDCWD) ? 0u : RESOLVE_IN_ROOT,    // AT_FDCWD means it's a thunk and not via RootFS
      };
      fd = ::syscall(SYSCALL_DEF(openat2), Path.FD, Path.Path, &how, sizeof(how));
      if (fd == -1 && errno == EXDEV) {
        // This means a magic symlink (/proc/foo) was involved. In this case we
        // just punt and do the access without RESOLVE_IN_ROOT.
        fd = ::syscall(SYSCALL_DEF(openat), Path.FD, Path.Path, flags, mode);
      }
#endif
    }

    // Open through RootFS failed (probably nonexistent), so open directly.
    if (fd == -1) {
      fd = ::syscall(SYSCALL_DEF(openat), dirfs, SelfPath, flags, mode);
    }

    ReplaceEmuFd(fd, flags, mode);
  } else {
    fd = ::syscall(SYSCALL_DEF(openat), dirfs, SelfPath, flags, mode);
  }

  return fd;
}

uint64_t FileManager::Openat2(int dirfs, const char* pathname, FEX::HLE::open_how* how, size_t usize) {
  if (const int compatFD = OpenAndroidCompatCgroupFile(pathname, static_cast<int>(how->flags)); compatFD != -1) {
    int32_t fd = compatFD;
    ReplaceEmuFd(fd, how->flags, how->mode);
    return fd;
  }
  const auto CompatPath = GetAndroidCompatCgroupPath(pathname);
  const char* InputPath = CompatPath ? CompatPath->c_str() : pathname;
  auto NewPath = GetSelf(InputPath);
  const char* SelfPath = NewPath ? NewPath->data() : InputPath;
  const int open_flags = static_cast<int>(how->flags);
#if defined(__ANDROID__)
  (void)usize;
#endif

  int32_t fd = -1;

  if (!ShouldSkipOpenInEmu(open_flags)) {
    FDPathTmpData TmpFilename;
    auto Path = GetEmulatedFDPath(dirfs, SelfPath, false, TmpFilename);
#if defined(__ANDROID__)
    // TODO(Android): Re-enable openat2 + RESOLVE_IN_ROOT when app seccomp policy allows syscall 437.
    if (Path.FD != -1) {
      fd = ::syscall(SYSCALL_DEF(openat), Path.FD, Path.Path, open_flags, how->mode);
    }

    // Open through RootFS failed (probably nonexistent), so open directly.
    if (fd == -1) {
      fd = ::syscall(SYSCALL_DEF(openat), dirfs, SelfPath, open_flags, how->mode);
    }
#else
    if (Path.FD != -1 && !(how->resolve & RESOLVE_IN_ROOT)) {
      // AT_FDCWD means it's a thunk and not via RootFS
      if (Path.FD != AT_FDCWD) {
        how->resolve |= RESOLVE_IN_ROOT;
      }
      fd = ::syscall(SYSCALL_DEF(openat2), Path.FD, Path.Path, how, usize);
      how->resolve &= ~RESOLVE_IN_ROOT;
      if (fd == -1 && errno == EXDEV) {
        // This means a magic symlink (/proc/foo) was involved. In this case we
        // just punt and do the access without RESOLVE_IN_ROOT.
        fd = ::syscall(SYSCALL_DEF(openat2), Path.FD, Path.Path, how, usize);
      }
    }

    // Open through RootFS failed (probably nonexistent), so open directly.
    if (fd == -1) {
      fd = ::syscall(SYSCALL_DEF(openat2), dirfs, SelfPath, how, usize);
    }
#endif

    ReplaceEmuFd(fd, how->flags, how->mode);
  } else {
#if defined(__ANDROID__)
    fd = ::syscall(SYSCALL_DEF(openat), dirfs, SelfPath, open_flags, how->mode);
#else
    fd = ::syscall(SYSCALL_DEF(openat2), dirfs, SelfPath, how, usize);
#endif
  }

  return fd;
}

uint64_t FileManager::Statx(int dirfd, const char* pathname, int flags, uint32_t mask, struct statx* statxbuf) {
  const auto CompatPath = GetAndroidCompatCgroupPath(pathname);
  if (CompatPath) {
    return FHU::Syscalls::statx(AT_FDCWD, CompatPath->c_str(), flags, mask, statxbuf);
  }
  const char* InputPath = CompatPath ? CompatPath->c_str() : pathname;

  if (IsSelfNoFollow(InputPath, flags)) {
    // If we aren't following the symlink for self then we need to return data about the symlink itself.
    // Let's just /actually/ return FEX symlink information in this case.
    return FHU::Syscalls::statx(dirfd, InputPath, flags, mask, statxbuf);
  }

  auto NewPath = GetSelf(InputPath);
  const char* SelfPath = NewPath ? NewPath->data() : InputPath;

  FDPathTmpData TmpFilename;
  auto Path = GetEmulatedFDPath(dirfd, SelfPath, (flags & AT_SYMLINK_NOFOLLOW) == 0, TmpFilename);
  if (Path.FD != -1) {
    uint64_t Result = FHU::Syscalls::statx(Path.FD, Path.Path, flags, mask, statxbuf);
    if (Result != -1) {
      return Result;
    }
  }
  return FHU::Syscalls::statx(dirfd, SelfPath, flags, mask, statxbuf);
}

uint64_t FileManager::Mknod(const char* pathname, mode_t mode, dev_t dev) {
  const auto CompatPath = GetAndroidCompatCgroupPath(pathname);
  const char* InputPath = CompatPath ? CompatPath->c_str() : pathname;
  auto NewPath = GetSelf(InputPath);
  const char* SelfPath = NewPath ? NewPath->data() : InputPath;

  FDPathTmpData TmpFilename;
  auto Path = GetEmulatedFDPath(AT_FDCWD, SelfPath, false, TmpFilename);
  if (Path.FD != -1) {
    uint64_t Result = ::mknodat(Path.FD, Path.Path, mode, dev);
    if (Result != -1) {
      return Result;
    }
  }
  return ::mknod(SelfPath, mode, dev);
}

uint64_t FileManager::Statfs(const char* path, void* buf) {
  const auto CompatPath = GetAndroidCompatCgroupPath(path);
  if (CompatPath) {
    return ::statfs(CompatPath->c_str(), reinterpret_cast<struct statfs*>(buf));
  }
  const char* InputPath = CompatPath ? CompatPath->c_str() : path;
  auto Path = GetEmulatedPath(InputPath);
  if (!Path.empty()) {
    uint64_t Result = ::statfs(Path.c_str(), reinterpret_cast<struct statfs*>(buf));
    if (Result != -1) {
      return Result;
    }
  }
  return ::statfs(InputPath, reinterpret_cast<struct statfs*>(buf));
}

uint64_t FileManager::NewFSStatAt(int dirfd, const char* pathname, struct stat* buf, int flag) {
  const auto CompatPath = GetAndroidCompatCgroupPath(pathname);
  if (CompatPath) {
    return ::fstatat(AT_FDCWD, CompatPath->c_str(), buf, flag);
  }
  const char* InputPath = CompatPath ? CompatPath->c_str() : pathname;

  if (IsSelfNoFollow(InputPath, flag)) {
    // See Statx
    return ::fstatat(dirfd, InputPath, buf, flag);
  }

  auto NewPath = GetSelf(InputPath);
  const char* SelfPath = NewPath ? NewPath->data() : InputPath;

  FDPathTmpData TmpFilename;
  auto Path = GetEmulatedFDPath(dirfd, SelfPath, (flag & AT_SYMLINK_NOFOLLOW) == 0, TmpFilename);
  if (Path.FD != -1) {
    uint64_t Result = ::fstatat(Path.FD, Path.Path, buf, flag);
    if (Result != -1) {
      return Result;
    }
  }
  return ::fstatat(dirfd, SelfPath, buf, flag);
}

uint64_t FileManager::NewFSStatAt64(int dirfd, const char* pathname, struct stat64* buf, int flag) {
  const auto CompatPath = GetAndroidCompatCgroupPath(pathname);
  if (CompatPath) {
    return ::fstatat64(AT_FDCWD, CompatPath->c_str(), buf, flag);
  }
  const char* InputPath = CompatPath ? CompatPath->c_str() : pathname;

  if (IsSelfNoFollow(InputPath, flag)) {
    // See Statx
    return ::fstatat64(dirfd, InputPath, buf, flag);
  }

  auto NewPath = GetSelf(InputPath);
  const char* SelfPath = NewPath ? NewPath->data() : InputPath;

  FDPathTmpData TmpFilename;
  auto Path = GetEmulatedFDPath(dirfd, SelfPath, (flag & AT_SYMLINK_NOFOLLOW) == 0, TmpFilename);
  if (Path.FD != -1) {
    uint64_t Result = ::fstatat64(Path.FD, Path.Path, buf, flag);
    if (Result != -1) {
      return Result;
    }
  }
  return ::fstatat64(dirfd, SelfPath, buf, flag);
}

uint64_t FileManager::Setxattr(const char* path, const char* name, const void* value, size_t size, int flags) {
  auto NewPath = GetSelf(path);
  const char* SelfPath = NewPath ? NewPath->data() : nullptr;

  auto Path = GetEmulatedPath(SelfPath, true);
  if (!Path.empty()) {
    uint64_t Result = ::setxattr(Path.c_str(), name, value, size, flags);
    if (Result != -1 || errno != ENOENT) {
      return Result;
    }
  }

  return ::setxattr(SelfPath, name, value, size, flags);
}

uint64_t FileManager::LSetxattr(const char* path, const char* name, const void* value, size_t size, int flags) {
  auto NewPath = GetSelf(path);
  const char* SelfPath = NewPath ? NewPath->data() : nullptr;

  auto Path = GetEmulatedPath(SelfPath, false);
  if (!Path.empty()) {
    uint64_t Result = ::lsetxattr(Path.c_str(), name, value, size, flags);
    if (Result != -1 || errno != ENOENT) {
      return Result;
    }
  }

  return ::lsetxattr(SelfPath, name, value, size, flags);
}

uint64_t FileManager::Getxattr(const char* path, const char* name, void* value, size_t size) {
  auto NewPath = GetSelf(path);
  const char* SelfPath = NewPath ? NewPath->data() : nullptr;

  auto Path = GetEmulatedPath(SelfPath, true);
  if (!Path.empty()) {
    uint64_t Result = ::getxattr(Path.c_str(), name, value, size);
    if (Result != -1 || errno != ENOENT) {
      return Result;
    }
  }

  return ::getxattr(SelfPath, name, value, size);
}

uint64_t FileManager::LGetxattr(const char* path, const char* name, void* value, size_t size) {
  auto NewPath = GetSelf(path);
  const char* SelfPath = NewPath ? NewPath->data() : nullptr;

  auto Path = GetEmulatedPath(SelfPath, false);
  if (!Path.empty()) {
    uint64_t Result = ::lgetxattr(Path.c_str(), name, value, size);
    if (Result != -1 || errno != ENOENT) {
      return Result;
    }
  }

  return ::lgetxattr(SelfPath, name, value, size);
}

uint64_t FileManager::Listxattr(const char* path, char* list, size_t size) {
  auto NewPath = GetSelf(path);
  const char* SelfPath = NewPath ? NewPath->data() : nullptr;

  auto Path = GetEmulatedPath(SelfPath, true);
  if (!Path.empty()) {
    uint64_t Result = ::listxattr(Path.c_str(), list, size);
    if (Result != -1 || errno != ENOENT) {
      return Result;
    }
  }

  return ::listxattr(SelfPath, list, size);
}

uint64_t FileManager::LListxattr(const char* path, char* list, size_t size) {
  auto NewPath = GetSelf(path);
  const char* SelfPath = NewPath ? NewPath->data() : nullptr;

  auto Path = GetEmulatedPath(SelfPath, false);
  if (!Path.empty()) {
    uint64_t Result = ::llistxattr(Path.c_str(), list, size);
    if (Result != -1 || errno != ENOENT) {
      return Result;
    }
  }

  return ::llistxattr(SelfPath, list, size);
}

uint64_t FileManager::Removexattr(const char* path, const char* name) {
  auto NewPath = GetSelf(path);
  const char* SelfPath = NewPath ? NewPath->data() : nullptr;

  auto Path = GetEmulatedPath(SelfPath, true);
  if (!Path.empty()) {
    uint64_t Result = ::removexattr(Path.c_str(), name);
    if (Result != -1 || errno != ENOENT) {
      return Result;
    }
  }

  return ::removexattr(SelfPath, name);
}

uint64_t FileManager::LRemovexattr(const char* path, const char* name) {
  auto NewPath = GetSelf(path);
  const char* SelfPath = NewPath ? NewPath->data() : nullptr;

  auto Path = GetEmulatedPath(SelfPath, false);
  if (!Path.empty()) {
    uint64_t Result = ::lremovexattr(Path.c_str(), name);
    if (Result != -1 || errno != ENOENT) {
      return Result;
    }
  }

  return ::lremovexattr(SelfPath, name);
}

uint64_t FileManager::SetxattrAt(int dfd, const char* pathname, uint32_t at_flags, const char* name, const xattr_args* uargs, size_t usize) {
  if (IsSelfNoFollow(pathname, at_flags)) {
    // See Statx
    return syscall(SYSCALL_DEF(setxattrat), dfd, pathname, at_flags, name, uargs, usize);
  }

  auto NewPath = GetSelf(pathname);
  const char* SelfPath = NewPath ? NewPath->data() : nullptr;

  FDPathTmpData TmpFilename;
  auto Path = GetEmulatedFDPath(dfd, SelfPath, (at_flags & AT_SYMLINK_NOFOLLOW) == 0, TmpFilename);
  if (Path.FD != -1) {
    uint64_t Result = syscall(SYSCALL_DEF(setxattrat), Path.FD, Path.Path, at_flags, name, uargs, usize);
    if (Result != -1) {
      return Result;
    }
  }
  return syscall(SYSCALL_DEF(setxattrat), dfd, SelfPath, at_flags, name, uargs, usize);
}

uint64_t FileManager::GetxattrAt(int dfd, const char* pathname, uint32_t at_flags, const char* name, const xattr_args* uargs, size_t usize) {
  if (IsSelfNoFollow(pathname, at_flags)) {
    // See Statx
    return syscall(SYSCALL_DEF(getxattrat), dfd, pathname, at_flags, name, uargs, usize);
  }

  auto NewPath = GetSelf(pathname);
  const char* SelfPath = NewPath ? NewPath->data() : nullptr;

  FDPathTmpData TmpFilename;
  auto Path = GetEmulatedFDPath(dfd, SelfPath, (at_flags & AT_SYMLINK_NOFOLLOW) == 0, TmpFilename);
  if (Path.FD != -1) {
    uint64_t Result = syscall(SYSCALL_DEF(getxattrat), Path.FD, Path.Path, at_flags, name, uargs, usize);
    if (Result != -1) {
      return Result;
    }
  }
  return syscall(SYSCALL_DEF(getxattrat), dfd, SelfPath, at_flags, name, uargs, usize);
}

uint64_t FileManager::ListxattrAt(int dfd, const char* pathname, uint32_t at_flags, char* list, size_t size) {
  if (IsSelfNoFollow(pathname, at_flags)) {
    // See Statx
    return syscall(SYSCALL_DEF(listxattrat), dfd, pathname, at_flags, list, size);
  }

  auto NewPath = GetSelf(pathname);
  const char* SelfPath = NewPath ? NewPath->data() : nullptr;

  FDPathTmpData TmpFilename;
  auto Path = GetEmulatedFDPath(dfd, SelfPath, (at_flags & AT_SYMLINK_NOFOLLOW) == 0, TmpFilename);
  if (Path.FD != -1) {
    uint64_t Result = syscall(SYSCALL_DEF(listxattrat), Path.FD, Path.Path, at_flags, list, size);
    if (Result != -1) {
      return Result;
    }
  }
  return syscall(SYSCALL_DEF(listxattrat), dfd, SelfPath, at_flags, list, size);
}

uint64_t FileManager::RemovexattrAt(int dfd, const char* pathname, uint32_t at_flags, const char* name) {
  if (IsSelfNoFollow(pathname, at_flags)) {
    // See Statx
    return syscall(SYSCALL_DEF(removexattrat), dfd, pathname, at_flags, name);
  }

  auto NewPath = GetSelf(pathname);
  const char* SelfPath = NewPath ? NewPath->data() : nullptr;

  FDPathTmpData TmpFilename;
  auto Path = GetEmulatedFDPath(dfd, SelfPath, (at_flags & AT_SYMLINK_NOFOLLOW) == 0, TmpFilename);
  if (Path.FD != -1) {
    uint64_t Result = syscall(SYSCALL_DEF(removexattrat), Path.FD, Path.Path, at_flags, name);
    if (Result != -1) {
      return Result;
    }
  }
  return syscall(SYSCALL_DEF(removexattrat), dfd, SelfPath, at_flags, name);
}

void FileManager::UpdatePID(uint32_t PID) {
  CurrentPID = PID;

  // Track the inode of /proc/self/fd/<RootFSFD>, to be able to hide it
  auto FDpath = fextl::fmt::format("self/fd/{}", RootFSFD);
  struct stat Buffer {};
  int Result = fstatat(ProcFD, FDpath.c_str(), &Buffer, AT_SYMLINK_NOFOLLOW);
  if (Result >= 0) {
    RootFSFDInode = Buffer.st_ino;
  } else {
    // Probably in a strict sandbox
    RootFSFDInode = 0;
    ProcFDInode = 0;
    return;
  }

  // And track the ProcFSFD itself
  FDpath = fextl::fmt::format("self/fd/{}", ProcFD);
  Result = fstatat(ProcFD, FDpath.c_str(), &Buffer, AT_SYMLINK_NOFOLLOW);
  if (Result >= 0) {
    ProcFDInode = Buffer.st_ino;
  } else {
    // ??
    ProcFDInode = 0;
    return;
  }
}

bool FileManager::IsProtectedFile(int ParentDirFD, uint64_t inode) const {
  // Check if we have to hide this entry
  const char* Match = nullptr;
  if (inode == RootFSFDInode) {
    Match = "RootFS";
  } else if (inode == ProcFDInode) {
    Match = "/proc";
  } else if (inode == CodeMapInode) {
    Match = "code map";
  }
  if (Match) {
    struct stat Buffer;
    if (fstat(ParentDirFD, &Buffer) >= 0) {
      if (Buffer.st_dev == ProcFSDev) {
        LogMan::Msg::DFmt("Hiding directory entry for {} FD", Match);
        return true;
      }
    }
  }
  return false;
}

void FileManager::SetProtectedCodeMapFD(int FD) {
  if (FD == -1) {
    CodeMapInode = 0;
    return;
  }

  auto FDPath = fextl::fmt::format("self/fd/{}", FD);
  struct stat Buffer {};
  auto Result = fstatat(ProcFD, FDPath.c_str(), &Buffer, AT_SYMLINK_NOFOLLOW);
  if (Result >= 0) {
    CodeMapInode = Buffer.st_ino;
  } else {
    CodeMapInode = 0;
  }
}

} // namespace FEX::HLE
