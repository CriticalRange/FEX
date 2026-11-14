// Integration hooks, implemented by libGL_Guest.cpp before this include:
//   vexa_pixel::PixelTransferState& VexaPixelState();
//   bool VexaPixelRecord(std::function<void()> callback);
//   bool VexaPixelInsideBegin();
//   bool VexaPixelMapRead(const void* guest_pointer_or_offset, void* out, size_t bytes);
// VexaPixelMapRead must honor GL_PIXEL_UNPACK_BUFFER_BINDING and validate/read
// the entire array; when no unpack buffer is bound it copies guest memory.
// VexaPixelRecord returns true if the value-capturing command was recorded.
static bool VexaPixelCheckBegin() {
  if (!VexaPixelInsideBegin()) return true;
  fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return false;
}
static void VexaPixelSetTransfer(GLenum pname, double value) {
  if (!VexaPixelCheckBegin()) return;
  auto probe = VexaPixelState();
  if (!vexa_pixel::transfer_value(probe, pname, value)) {
    fexfn_pack_VEXA_CompatError(GL_INVALID_ENUM); return;
  }
  auto apply = [pname, value]() { vexa_pixel::transfer_value(VexaPixelState(), pname, value); };
  if (!VexaPixelRecord(apply)) apply();
}
VEXA_GL_GUEST_EXPORT void glPixelTransferf(GLenum pname, GLfloat value) { VexaPixelSetTransfer(pname, value); }
VEXA_GL_GUEST_EXPORT void glPixelTransferi(GLenum pname, GLint value) { VexaPixelSetTransfer(pname, value); }
VEXA_GL_GUEST_EXPORT void glPixelZoom(GLfloat xfactor, GLfloat yfactor) {
  if (!VexaPixelCheckBegin()) return;
  auto apply = [xfactor, yfactor]() { auto& state=VexaPixelState(); state.zoom_x=xfactor; state.zoom_y=yfactor; };
  if (!VexaPixelRecord(apply)) apply();
}

template<typename T> static void VexaPixelMap(GLenum map, GLsizei size, const T* values) {
  if (!VexaPixelCheckBegin()) return;
  const int index = vexa_pixel::map_index(map);
  if (index < 0) { fexfn_pack_VEXA_CompatError(GL_INVALID_ENUM); return; }
  if (!vexa_pixel::map_size_valid(index, size)) { fexfn_pack_VEXA_CompatError(GL_INVALID_VALUE); return; }
  std::vector<T> raw(static_cast<size_t>(size));
  if (!VexaPixelMapRead(values, raw.data(), raw.size() * sizeof(T))) return;
  std::vector<float> converted(raw.size());
  for (size_t i=0; i<raw.size(); ++i) {
    double value = static_cast<double>(raw[i]);
    if constexpr (!std::is_floating_point_v<T>) {
      if (index >= 2) value /= static_cast<double>(std::numeric_limits<T>::max());
    }
    if (index >= 2) value = std::clamp(value, 0.0, 1.0);
    else if (index == 1 && std::is_floating_point_v<T>) value = std::round(value);
    converted[i] = static_cast<float>(value);
  }
  auto apply = [index, converted=std::move(converted)]() { VexaPixelState().maps[index] = converted; };
  if (!VexaPixelRecord(apply)) apply();
}
VEXA_GL_GUEST_EXPORT void glPixelMapfv(GLenum map, GLsizei size, const GLfloat* values) { VexaPixelMap(map,size,values); }
VEXA_GL_GUEST_EXPORT void glPixelMapuiv(GLenum map, GLsizei size, const GLuint* values) { VexaPixelMap(map,size,values); }
VEXA_GL_GUEST_EXPORT void glPixelMapusv(GLenum map, GLsizei size, const GLushort* values) { VexaPixelMap(map,size,values); }

template<typename T> static void VexaGetPixelMap(GLenum map, T* values) {
  if (!VexaPixelCheckBegin()) return;
  const int index=vexa_pixel::map_index(map);
  if (index < 0) { fexfn_pack_VEXA_CompatError(GL_INVALID_ENUM); return; }
  if (!values) return;
  const auto& source=VexaPixelState().maps[index];
  for (size_t i=0; i<source.size(); ++i) {
    double value = source[i];
    if constexpr (!std::is_floating_point_v<T>) {
      if (index >= 2) value = std::clamp(value, 0.0, 1.0) * static_cast<double>(std::numeric_limits<T>::max());
      value = std::clamp(std::round(value), 0.0, static_cast<double>(std::numeric_limits<T>::max()));
    }
    values[i] = static_cast<T>(value);
  }
}
VEXA_GL_GUEST_EXPORT void glGetPixelMapfv(GLenum map, GLfloat* values) { VexaGetPixelMap(map,values); }
VEXA_GL_GUEST_EXPORT void glGetPixelMapuiv(GLenum map, GLuint* values) { VexaGetPixelMap(map,values); }
VEXA_GL_GUEST_EXPORT void glGetPixelMapusv(GLenum map, GLushort* values) { VexaGetPixelMap(map,values); }

// Hook for glGet{Boolean,Double,Float,Integer}v: returns one scalar.
static bool VexaPixelQuery(unsigned pname, double* value, unsigned& count) {
  if (!vexa_pixel::query_transfer(VexaPixelState(),pname,*value) &&
      !vexa_pixel::query_map_size(VexaPixelState(),pname,*value)) return false;
  count=1; return true;
}
