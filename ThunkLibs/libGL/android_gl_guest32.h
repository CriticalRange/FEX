// Android allocations and driver handles are LP64 even for an i386 guest.
// This file is included after GuestMalloc/GuestFree have been declared.
#pragma once

static const GLubyte* VexaGuestString(const GLubyte* text) {
#ifdef IS_32BIT_THUNK
  if (!text) return nullptr;
  static std::mutex mutex;
  static std::unordered_map<std::string, const GLubyte*> strings;
  std::lock_guard lock(mutex);
  const std::string key {reinterpret_cast<const char*>(text)};
  if (auto it = strings.find(key); it != strings.end()) return it->second;
  if (!GuestMalloc || key.size() >= UINT32_MAX) return nullptr;
  auto copy = static_cast<GLubyte*>(GuestMalloc(static_cast<uint32_t>(key.size() + 1)));
  if (!copy) return nullptr;
  std::memcpy(copy, text, key.size() + 1);
  strings.emplace(key, copy);
  return copy;
#else
  return text;
#endif
}

static const char* VexaGuestString(const char* text) {
  return reinterpret_cast<const char*>(VexaGuestString(reinterpret_cast<const GLubyte*>(text)));
}

#ifdef IS_32BIT_THUNK
static std::vector<const GLchar*> VexaExpandStrings32(GLsizei count, const void* data) {
  std::vector<const GLchar*> result;
  if (count > 0 && data) {
    result.reserve(count);
    auto pointers = static_cast<const uint32_t*>(data);
    for (GLsizei i = 0; i < count; ++i)
      result.push_back(reinterpret_cast<const GLchar*>(uintptr_t {pointers[i]}));
  }
  return result;
}
static void VexaTransformFeedbackVaryings32(GLuint program, GLsizei count, const GLchar* const* names, GLenum mode) {
  const auto expanded = VexaExpandStrings32(count, names);
  ::glTransformFeedbackVaryings(program, count, expanded.data(), mode);
}
static std::mutex VexaSyncMutex;
static std::unordered_map<uint32_t, GLsync> VexaSyncs;
static GLsync VexaExportSync(GLsync sync) {
  if (!sync) return nullptr;
  std::lock_guard lock(VexaSyncMutex);
  static uint32_t next = 1;
  uint32_t id;
  do { id = next++; } while (!id || VexaSyncs.contains(id));
  VexaSyncs.emplace(id, sync);
  return reinterpret_cast<GLsync>(uintptr_t {id});
}
static GLsync VexaImportSync(GLsync id, bool erase = false) {
  std::lock_guard lock(VexaSyncMutex);
  auto it = VexaSyncs.find(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(id)));
  if (it == VexaSyncs.end()) return nullptr;
  auto sync = it->second;
  if (erase) VexaSyncs.erase(it);
  return sync;
}

struct VexaMappedBuffer {
  void* guest;
  size_t length;
  GLbitfield access;
};
static std::mutex VexaMapMutex;
static std::unordered_map<void*, VexaMappedBuffer> VexaMappedBuffers;

static void VexaDeleteBuffers32(GLsizei count, const GLuint* buffers) {
  std::vector<void*> released;
  if (count > 0 && buffers) {
    GLint saved = 0;
    ::glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &saved);
    for (GLsizei i = 0; i < count; ++i) {
      if (!::glIsBuffer(buffers[i])) continue;
      ::glBindBuffer(GL_COPY_READ_BUFFER, buffers[i]);
      void* host = nullptr;
      ::glGetBufferPointerv(GL_COPY_READ_BUFFER, GL_BUFFER_MAP_POINTER, &host);
      std::lock_guard lock(VexaMapMutex);
      auto it = VexaMappedBuffers.find(host);
      if (it != VexaMappedBuffers.end()) {
        released.push_back(it->second.guest);
        VexaMappedBuffers.erase(it);
      }
    }
    ::glBindBuffer(GL_COPY_READ_BUFFER, saved);
  }
  ::glDeleteBuffers(count, buffers);
  for (void* guest : released) if (GuestFree) GuestFree(guest);
}

static void VexaGetBufferPointer32(GLenum target, GLenum pname, void** output) {
  void* host = nullptr;
  ::glGetBufferPointerv(target, pname, &host);
  if (!output) return;
  std::lock_guard lock(VexaMapMutex);
  auto it = VexaMappedBuffers.find(host);
  *reinterpret_cast<uint32_t*>(output) = it == VexaMappedBuffers.end() ? 0 :
    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(it->second.guest));
}

static void* fexfn_impl_libGL_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
  void* host = ::glMapBufferRange(target, offset, length, access);
  if (!host) return nullptr;
  // GLES rejects persistent/coherent mappings. Copy ordinary maps into guest
  // memory and copy writes back on explicit flush or unmap.
  void* guest = GuestMalloc && length > 0 && uint64_t(length) <= UINT32_MAX
                  ? GuestMalloc(static_cast<uint32_t>(length)) : nullptr;
  if (!guest) { ::glUnmapBuffer(target); return nullptr; }
  if (access & GL_MAP_READ_BIT) std::memcpy(guest, host, length);
  std::lock_guard lock(VexaMapMutex);
  VexaMappedBuffers.emplace(host, VexaMappedBuffer {guest, size_t(length), access});
  return guest;
}

static void fexfn_impl_libGL_glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
  void* host = nullptr;
  ::glGetBufferPointerv(target, GL_BUFFER_MAP_POINTER, &host);
  {
    std::lock_guard lock(VexaMapMutex);
    auto it = VexaMappedBuffers.find(host);
    if (it != VexaMappedBuffers.end() && offset >= 0 && length >= 0 &&
        size_t(offset) <= it->second.length && size_t(length) <= it->second.length - size_t(offset) &&
        (it->second.access & GL_MAP_WRITE_BIT) && (it->second.access & GL_MAP_FLUSH_EXPLICIT_BIT)) {
      std::memcpy(static_cast<char*>(host) + offset, static_cast<char*>(it->second.guest) + offset, length);
    }
  }
  ::glFlushMappedBufferRange(target, offset, length);
}

static GLboolean fexfn_impl_libGL_glUnmapBuffer(GLenum target) {
  void* host = nullptr;
  void* guest = nullptr;
  ::glGetBufferPointerv(target, GL_BUFFER_MAP_POINTER, &host);
  {
    std::lock_guard lock(VexaMapMutex);
    auto it = VexaMappedBuffers.find(host);
    if (it != VexaMappedBuffers.end()) {
      const auto& map = it->second;
      if ((map.access & GL_MAP_WRITE_BIT) && !(map.access & GL_MAP_FLUSH_EXPLICIT_BIT))
        std::memcpy(host, map.guest, map.length);
      guest = map.guest;
      VexaMappedBuffers.erase(it);
    }
  }
  const auto result = ::glUnmapBuffer(target);
  if (guest && GuestFree) GuestFree(guest);
  return result;
}
#else
static GLsync VexaExportSync(GLsync sync) { return sync; }
static GLsync VexaImportSync(GLsync sync, bool = false) { return sync; }
#endif
