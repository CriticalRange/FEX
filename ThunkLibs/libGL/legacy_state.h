#pragma once
#define VEXA_LEGACY_STATE_HOST_PROCS(OP) \
  OP(glVertexAttribPointer, void, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer)) \
  OP(glVertexAttribPointerARB, void, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer)) \
  OP(glEnableVertexAttribArray, void, (GLuint index)) \
  OP(glDisableVertexAttribArray, void, (GLuint index)) \
  OP(glGetVertexAttribiv, void, (GLuint index, GLenum pname, GLint* value)) \
  OP(glGetVertexAttribfv, void, (GLuint index, GLenum pname, GLfloat* value)) \
  OP(glDeleteVertexArrays, void, (GLsizei count, const GLuint* arrays)) \
  OP(glPushAttrib, void, (GLbitfield mask)) \
  OP(glPushClientAttrib, void, (GLbitfield mask)) \
  OP(glPopClientAttrib, void, (void)) \
  OP(glClientActiveTexture, void, (GLenum texture)) \
  OP(glVertexPointer, void, (GLint size, GLenum type, GLsizei stride, const void* pointer)) \
  OP(glTexCoordPointer, void, (GLint size, GLenum type, GLsizei stride, const void* pointer)) \
  OP(glColorPointer, void, (GLint size, GLenum type, GLsizei stride, const void* pointer)) \
  OP(glEnableClientState, void, (GLenum array)) \
  OP(glDisableClientState, void, (GLenum array))
#define VEXA_LEGACY_STATE_GUEST_PROCS(OP) \
  OP(glPopAttrib, void, (void)) \
  OP(glColor4f, void, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
  OP(glColor4sv, void, (const GLshort* values)) \
  OP(glTexCoord2f, void, (GLfloat s, GLfloat t))
