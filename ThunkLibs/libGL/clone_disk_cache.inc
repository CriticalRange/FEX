// These files contain driver binaries, never GL object IDs or uniform state.
// The hash names a file only: the complete key and payload checksum are
// checked before loading, so a filename collision cannot select wrong code.
#include <dirent.h>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/syscall.h>

static uint64_t VexaCloneHash(std::string_view bytes) {
  uint64_t value = 14695981039346656037ULL;
  for (unsigned char byte : bytes) { value ^= byte; value *= 1099511628211ULL; }
  return value;
}

static std::string VexaCloneDriverIdentity() {
  std::string key = "vexa-clone-v2\n";
  for (GLenum name : {GL_VENDOR, GL_RENDERER, GL_VERSION, GL_SHADING_LANGUAGE_VERSION}) {
    const auto* value = ::glGetString(name);
    if (!value) return {};
    key += reinterpret_cast<const char*>(value);
    key += '\n';
  }
  return key;
}

static std::string VexaCloneDiskDirectory() {
  const char* enabled = std::getenv("VEXA_GL_CLONE_DISK_CACHE");
  if (enabled && std::strcmp(enabled, "0") == 0) return {};
  const char* override_dir = std::getenv("VEXA_GL_PROGRAM_CACHE_DIR");
  std::string directory = override_dir ? override_dir : "/data/user/0/com.critical.vexaemulator/cache/vexa-gl-programs-v2";
  if (directory.empty()) return {};
  if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) return {};
  return directory;
}

static std::string VexaCloneDiskPath(const std::string& directory, const std::string& key) {
  char name[40];
  std::snprintf(name, sizeof(name), "/clone-%016llx.bin", static_cast<unsigned long long>(VexaCloneHash(key)));
  return directory + name;
}

static bool VexaReadCloneDisk(const std::string& key, uint32_t* format, std::vector<uint8_t>* bytes) {
  const auto directory = VexaCloneDiskDirectory();
  if (directory.empty()) return false;
  const auto path = VexaCloneDiskPath(directory, key);
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return false;
  // Fixed-width host-endian header; cache is local to the Android device.
  uint32_t header[6] = {};
  bool ok = std::fread(header, sizeof(header), 1, file) == 1 && header[0] == 0x56434332 &&
            header[2] == key.size() && header[2] <= 8 * 1024 * 1024 && header[3] && header[3] <= 8 * 1024 * 1024;
  std::string stored_key;
  if (ok) {
    stored_key.resize(header[2]);
    bytes->resize(header[3]);
    ok = std::fread(stored_key.data(), 1, stored_key.size(), file) == stored_key.size() && stored_key == key &&
         std::fread(bytes->data(), 1, bytes->size(), file) == bytes->size() && std::fgetc(file) == EOF;
    const uint64_t checksum = VexaCloneHash(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    ok = ok && checksum == (uint64_t {header[4]} | (uint64_t {header[5]} << 32));
  }
  std::fclose(file);
  if (!ok) { bytes->clear(); ::unlink(path.c_str()); return false; }
  *format = header[1];
  // Disk reads occur only after a RAM miss; refreshing mtime is not a draw
  // hot-path operation. It gives disk eviction an approximate LRU order.
  ::utimensat(AT_FDCWD, path.c_str(), nullptr, 0);
  return true;
}

static void VexaPruneCloneDisk(const std::string& directory) {
  DIR* dir = ::opendir(directory.c_str());
  if (!dir) return;
  struct File { std::string Path; uint64_t Bytes; time_t Time; };
  std::vector<File> files;
  uint64_t total = 0;
  while (auto* entry = ::readdir(dir)) {
    std::string_view name(entry->d_name);
    if (name.size() != 26 || name.substr(0, 6) != "clone-" || name.substr(22) != ".bin") continue;
    std::string path = directory + "/" + entry->d_name;
    struct stat st {};
    if (::stat(path.c_str(), &st) || !S_ISREG(st.st_mode)) continue;
    total += static_cast<uint64_t>(st.st_size);
    files.push_back({std::move(path), static_cast<uint64_t>(st.st_size), st.st_mtime});
  }
  ::closedir(dir);
  std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.Time < b.Time; });
  size_t remaining = files.size();
  for (const auto& file : files) {
    if (total <= 32 * 1024 * 1024 && remaining <= 128) break;
    if (::unlink(file.Path.c_str()) == 0) { total -= file.Bytes; --remaining; }
  }
}

static void VexaWriteCloneDisk(const std::string& key, uint32_t format, const std::vector<uint8_t>& bytes) {
  const auto directory = VexaCloneDiskDirectory();
  if (directory.empty() || key.size() > 8 * 1024 * 1024 || bytes.empty() || bytes.size() > 8 * 1024 * 1024) return;
  const auto path = VexaCloneDiskPath(directory, key);
  const auto temp = path + ".tmp-" + std::to_string(getpid()) + "-" + std::to_string(syscall(SYS_gettid));
  FILE* file = std::fopen(temp.c_str(), "wb");
  if (!file) return;
  const auto checksum = VexaCloneHash(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  const uint32_t header[] = {0x56434332, format, static_cast<uint32_t>(key.size()), static_cast<uint32_t>(bytes.size()),
                            static_cast<uint32_t>(checksum), static_cast<uint32_t>(checksum >> 32)};
  bool ok = std::fwrite(header, sizeof(header), 1, file) == 1 &&
            std::fwrite(key.data(), 1, key.size(), file) == key.size() &&
            std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  if (std::fclose(file)) ok = false;
  if (ok && ::rename(temp.c_str(), path.c_str()) == 0) VexaPruneCloneDisk(directory);
  else ::unlink(temp.c_str());
}
