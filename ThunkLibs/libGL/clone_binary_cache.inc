// Per-thread byte cache avoids locks on render threads. Binary loading is
// validated by the driver, including after an EGL context/device change.
static thread_local VexaCloneBinaryCache g_vexa_clone_binaries;

static bool VexaCloneCacheEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("VEXA_GL_CLONE_CACHE");
    return !value || std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

static GLuint VexaLoadCloneBinary(const std::string& source) {
  if (!VexaCloneCacheEnabled()) return 0;
  const auto* entry = g_vexa_clone_binaries.Find(source);
  if (!entry) return 0;
  const GLuint program = ::glCreateProgram();
  if (!program) return 0;
  ::glProgramParameteri(program, GL_PROGRAM_SEPARABLE, GL_TRUE);
  ::glProgramBinary(program, entry->Format, entry->Bytes.data(), static_cast<GLsizei>(entry->Bytes.size()));
  GLint linked = GL_FALSE;
  ::glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == GL_TRUE) return program;
  ::glDeleteProgram(program);
  g_vexa_clone_binaries.Erase(source);
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
  g_vexa_clone_binaries.Insert(source, format, std::move(bytes));
}
