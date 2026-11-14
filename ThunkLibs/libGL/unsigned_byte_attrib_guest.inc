// Desktop unsigned-byte generic attributes are absent from GLES. Convert in
// the guest so vector pointers never cross the 32/64-bit thunk boundary.
VEXA_GL_GUEST_EXPORT void glVertexAttrib4ubv(GLuint index, const GLubyte* v) {
  fexfn_pack_glVertexAttrib4f(index, GLfloat(v[0]), GLfloat(v[1]), GLfloat(v[2]), GLfloat(v[3]));
}
VEXA_GL_GUEST_EXPORT void glVertexAttrib4Nub(GLuint index, GLubyte x, GLubyte y, GLubyte z, GLubyte w) {
  fexfn_pack_glVertexAttrib4f(index, GLfloat(x)/255.0f, GLfloat(y)/255.0f, GLfloat(z)/255.0f, GLfloat(w)/255.0f);
}
VEXA_GL_GUEST_EXPORT void glVertexAttrib4Nubv(GLuint index, const GLubyte* v) {
  glVertexAttrib4Nub(index, v[0], v[1], v[2], v[3]);
}
VEXA_GL_GUEST_EXPORT void glVertexAttrib4ubvARB(GLuint index, const GLubyte* v) { glVertexAttrib4ubv(index, v); }
VEXA_GL_GUEST_EXPORT void glVertexAttrib4NubARB(GLuint index, GLubyte x, GLubyte y, GLubyte z, GLubyte w) {
  glVertexAttrib4Nub(index, x, y, z, w);
}
VEXA_GL_GUEST_EXPORT void glVertexAttrib4NubvARB(GLuint index, const GLubyte* v) { glVertexAttrib4Nubv(index, v); }
static uintptr_t VexaUnsignedByteAttribProc(std::string_view name) {
  if (name == "glVertexAttrib4ubv" || name == "glVertexAttrib4ubvARB") return reinterpret_cast<uintptr_t>(glVertexAttrib4ubv);
  if (name == "glVertexAttrib4Nub" || name == "glVertexAttrib4NubARB") return reinterpret_cast<uintptr_t>(glVertexAttrib4Nub);
  if (name == "glVertexAttrib4Nubv" || name == "glVertexAttrib4NubvARB") return reinterpret_cast<uintptr_t>(glVertexAttrib4Nubv);
  return 0;
}
