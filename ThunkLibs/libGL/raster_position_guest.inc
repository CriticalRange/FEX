// Include once in libGL_Guest.cpp after the OpenGL typedefs, GL export macro,
// fixed-function state, and the three hooks below have been defined.
// Required hooks (context-owned; never return shared fallback state):
//   vexa_raster::Position& VexaRasterPosition();
//   vexa_raster::Input VexaRasterInput();
//   bool VexaRasterRecord(const vexa_raster::Vec4& object);
// VexaRasterRecord records a display-list command and returns true during
// list compilation; on replay it returns false so the current matrices,
// viewport, depth range, clipping and color are sampled at execution time.
static void VexaRasterApply(double x, double y, double z, double w) {
  vexa_raster::Vec4 object{x, y, z, w};
  if (VexaRasterRecord(object)) return;
  auto input = VexaRasterInput();
  vexa_raster::raster_pos(VexaRasterPosition(), input, object);
}

// Query hook for the guest glGetBooleanv/glGetFloatv/glGetDoublev/
// glGetIntegerv wrappers. Position/color/texture have four components and
// POSITION_VALID has one. Other queries must be forwarded normally.
static bool VexaRasterQuery(unsigned pname, double* values, unsigned& count) {
  const auto& state = VexaRasterPosition();
  switch (pname) {
    case 0x0B08: values[0] = state.valid ? 1.0 : 0.0; count = 1; return true;
    case 0x0B07: std::copy(state.window.begin(), state.window.end(), values); count = 4; return true;
    case 0x0B04: std::copy(state.color.begin(), state.color.end(), values); count = 4; return true;
    case 0x0B06: std::copy(state.texcoord.begin(), state.texcoord.end(), values); count = 4; return true;
    default: return false;
  }
}

VEXA_GL_GUEST_EXPORT void glRasterPos2d(GLdouble x, GLdouble y) { VexaRasterApply(static_cast<double>(x), static_cast<double>(y), 0.0, 1.0); }
VEXA_GL_GUEST_EXPORT void glRasterPos2dv(const GLdouble* v) { if (!v) return; VexaRasterApply(static_cast<double>(v[0]), static_cast<double>(v[1]), 0.0, 1.0); }
VEXA_GL_GUEST_EXPORT void glRasterPos2f(GLfloat x, GLfloat y) { VexaRasterApply(static_cast<double>(x), static_cast<double>(y), 0.0, 1.0); }
VEXA_GL_GUEST_EXPORT void glRasterPos2fv(const GLfloat* v) { if (!v) return; VexaRasterApply(static_cast<double>(v[0]), static_cast<double>(v[1]), 0.0, 1.0); }
VEXA_GL_GUEST_EXPORT void glRasterPos2i(GLint x, GLint y) { VexaRasterApply(static_cast<double>(x), static_cast<double>(y), 0.0, 1.0); }
VEXA_GL_GUEST_EXPORT void glRasterPos2iv(const GLint* v) { if (!v) return; VexaRasterApply(static_cast<double>(v[0]), static_cast<double>(v[1]), 0.0, 1.0); }
VEXA_GL_GUEST_EXPORT void glRasterPos2s(GLshort x, GLshort y) { VexaRasterApply(static_cast<double>(x), static_cast<double>(y), 0.0, 1.0); }
VEXA_GL_GUEST_EXPORT void glRasterPos2sv(const GLshort* v) { if (!v) return; VexaRasterApply(static_cast<double>(v[0]), static_cast<double>(v[1]), 0.0, 1.0); }
VEXA_GL_GUEST_EXPORT void glRasterPos3d(GLdouble x, GLdouble y, GLdouble z) { VexaRasterApply(static_cast<double>(x), static_cast<double>(y), static_cast<double>(z), 1.0); }
VEXA_GL_GUEST_EXPORT void glRasterPos3dv(const GLdouble* v) { if (!v) return; VexaRasterApply(static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]), 1.0); }
VEXA_GL_GUEST_EXPORT void glRasterPos3f(GLfloat x, GLfloat y, GLfloat z) { VexaRasterApply(static_cast<double>(x), static_cast<double>(y), static_cast<double>(z), 1.0); }
VEXA_GL_GUEST_EXPORT void glRasterPos3fv(const GLfloat* v) { if (!v) return; VexaRasterApply(static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]), 1.0); }
VEXA_GL_GUEST_EXPORT void glRasterPos3i(GLint x, GLint y, GLint z) { VexaRasterApply(static_cast<double>(x), static_cast<double>(y), static_cast<double>(z), 1.0); }
VEXA_GL_GUEST_EXPORT void glRasterPos3iv(const GLint* v) { if (!v) return; VexaRasterApply(static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]), 1.0); }
VEXA_GL_GUEST_EXPORT void glRasterPos3s(GLshort x, GLshort y, GLshort z) { VexaRasterApply(static_cast<double>(x), static_cast<double>(y), static_cast<double>(z), 1.0); }
VEXA_GL_GUEST_EXPORT void glRasterPos3sv(const GLshort* v) { if (!v) return; VexaRasterApply(static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]), 1.0); }
VEXA_GL_GUEST_EXPORT void glRasterPos4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w) { VexaRasterApply(static_cast<double>(x), static_cast<double>(y), static_cast<double>(z), static_cast<double>(w)); }
VEXA_GL_GUEST_EXPORT void glRasterPos4dv(const GLdouble* v) { if (!v) return; VexaRasterApply(static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]), static_cast<double>(v[3])); }
VEXA_GL_GUEST_EXPORT void glRasterPos4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { VexaRasterApply(static_cast<double>(x), static_cast<double>(y), static_cast<double>(z), static_cast<double>(w)); }
VEXA_GL_GUEST_EXPORT void glRasterPos4fv(const GLfloat* v) { if (!v) return; VexaRasterApply(static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]), static_cast<double>(v[3])); }
VEXA_GL_GUEST_EXPORT void glRasterPos4i(GLint x, GLint y, GLint z, GLint w) { VexaRasterApply(static_cast<double>(x), static_cast<double>(y), static_cast<double>(z), static_cast<double>(w)); }
VEXA_GL_GUEST_EXPORT void glRasterPos4iv(const GLint* v) { if (!v) return; VexaRasterApply(static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]), static_cast<double>(v[3])); }
VEXA_GL_GUEST_EXPORT void glRasterPos4s(GLshort x, GLshort y, GLshort z, GLshort w) { VexaRasterApply(static_cast<double>(x), static_cast<double>(y), static_cast<double>(z), static_cast<double>(w)); }
VEXA_GL_GUEST_EXPORT void glRasterPos4sv(const GLshort* v) { if (!v) return; VexaRasterApply(static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]), static_cast<double>(v[3])); }
