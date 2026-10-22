// Per-thread byte cache avoids locks on render threads. Binary loading is
// validated by the driver, including after an EGL context/device change.
static thread_local VexaCloneBinaryCache g_vexa_clone_binaries;
static thread_local std::string g_vexa_clone_driver;
static thread_local const char* g_vexa_clone_hit_source = "miss";

#include <fcntl.h>
#include "clone_disk_cache.inc"

static bool VexaCloneCacheEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("VEXA_GL_CLONE_CACHE");
    return (!value || std::strcmp(value, "0") != 0) &&
           access("/data/user/0/com.critical.vexaemulator/files/vexa_gl_clone_cache_disabled.flag", F_OK) != 0;
  }();
  return enabled;
}

static GLuint VexaLoadCloneBinary(const std::string& source) {
  g_vexa_clone_hit_source = "miss";
  if (!VexaCloneCacheEnabled()) return 0;
  const auto driver = VexaCloneDriverIdentity();
  if (driver.empty()) return 0;
  if (driver != g_vexa_clone_driver) {
    g_vexa_clone_binaries = VexaCloneBinaryCache {};
    g_vexa_clone_driver = driver;
  }
  auto* entry = g_vexa_clone_binaries.Find(source);
  const bool ram_hit = entry != nullptr;
  if (!entry) {
    uint32_t format = 0;
    std::vector<uint8_t> bytes;
    if (!VexaReadCloneDisk(driver + source, &format, &bytes)) return 0;
    g_vexa_clone_binaries.Insert(source, format, std::move(bytes));
    entry = g_vexa_clone_binaries.Find(source);
    if (!entry) return 0;
  }
  const GLuint program = ::glCreateProgram();
  if (!program) return 0;
  ::glProgramParameteri(program, GL_PROGRAM_SEPARABLE, GL_TRUE);
  ::glProgramBinary(program, entry->Format, entry->Bytes.data(), static_cast<GLsizei>(entry->Bytes.size()));
  GLint linked = GL_FALSE;
  ::glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == GL_TRUE) {
    g_vexa_clone_hit_source = ram_hit ? "ram" : "disk";
    return program;
  }
  ::glDeleteProgram(program);
  g_vexa_clone_binaries.Erase(source);
  const auto directory = VexaCloneDiskDirectory();
  if (!directory.empty()) ::unlink(VexaCloneDiskPath(directory, driver + source).c_str());
  return 0;
}

static void VexaRememberCloneBinary(GLuint program, const std::string& source) {
  if (!VexaCloneCacheEnabled()) return;
  GLint length = 0;
  ::glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
  if (length <= 0 || static_cast<size_t>(length) + source.size() > 8 * 1024 * 1024) return;
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  GLsizei written = 0;
  GLenum format = 0;
  ::glGetProgramBinary(program, length, &written, &format, bytes.data());
  if (written <= 0 || written > length) return;
  bytes.resize(static_cast<size_t>(written));
  VexaWriteCloneDisk(g_vexa_clone_driver + source, format, bytes);
  g_vexa_clone_binaries.Insert(source, format, std::move(bytes));
}
