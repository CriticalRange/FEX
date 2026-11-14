// Color-index attributes are retained for queries, but do not affect RGBA contexts.
static void VexaIndex(double value) {
  vexa_ff::run_or_record_callback([value] { vexa_ff::S().color_index = value; });
}
#define VEXA_INDEX(suffix, type) \
  VEXA_GL_GUEST_EXPORT void glIndex##suffix(type value) { VexaIndex(value); } \
  VEXA_GL_GUEST_EXPORT void glIndex##suffix##v(const type* value) { if (value) VexaIndex(*value); }
VEXA_INDEX(d, GLdouble)
VEXA_INDEX(f, GLfloat)
VEXA_INDEX(i, GLint)
VEXA_INDEX(s, GLshort)
VEXA_INDEX(ub, GLubyte)
#undef VEXA_INDEX
VEXA_GL_GUEST_EXPORT void glClearIndex(GLfloat value) {
  if (vexa_ff::in_begin()) { fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return; }
  vexa_ff::run_or_record_callback([value] { vexa_ff::S().clear_index = value; });
}
VEXA_GL_GUEST_EXPORT void glIndexMask(GLuint mask) {
  if (vexa_ff::in_begin()) { fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return; }
  vexa_ff::run_or_record_callback([mask] { vexa_ff::S().index_mask = mask; });
}
VEXA_GL_GUEST_EXPORT void glEdgeFlag(GLboolean value) {
  vexa_ff::run_or_record_callback([value] { vexa_ff::S().edge_flag = value != 0; });
}
VEXA_GL_GUEST_EXPORT void glEdgeFlagv(const GLboolean* value) { if (value) glEdgeFlag(*value); }
VEXA_GL_GUEST_EXPORT GLboolean glAreTexturesResident(GLsizei n, const GLuint* textures, GLboolean* residences) {
  if (vexa_ff::in_begin()) { fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return GL_FALSE; }
  if (n < 0) { fexfn_pack_VEXA_CompatError(GL_INVALID_VALUE); return GL_FALSE; }
  if (n && (!textures || !residences)) { fexfn_pack_VEXA_CompatError(GL_INVALID_VALUE); return GL_FALSE; }
  for (GLsizei i=0; i<n; ++i) {
    if (!textures[i] || !glIsTexture(textures[i])) { fexfn_pack_VEXA_CompatError(GL_INVALID_VALUE); return GL_FALSE; }
  }
  // GLES manages residency internally; all valid texture objects are resident.
  // When all textures are resident GL leaves the residences array unchanged.
  return GL_TRUE;
}
VEXA_GL_GUEST_EXPORT void glPrioritizeTextures(GLsizei n, const GLuint*, const GLclampf*) {
  if (vexa_ff::in_begin()) { fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return; }
  if (n < 0) fexfn_pack_VEXA_CompatError(GL_INVALID_VALUE);
  // Priority is an implementation hint, and GLES owns memory residency.
}
