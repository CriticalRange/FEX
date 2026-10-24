#pragma once
// Separate ARB assembly-program namespace; never aliases a GLSL program.
#define VEXA_ARB_ASSEMBLY_PROCS(OP) \
  OP(glProgramEnvParameters4fvEXT, void, (GLenum target, GLuint index, GLsizei count, const GLfloat* values)) \
  OP(glBindProgramARB, void, (GLenum target, GLuint program)) \
  OP(glGenProgramsARB, void, (GLsizei count, GLuint* programs)) \
  OP(glDeleteProgramsARB, void, (GLsizei count, const GLuint* programs)) \
  OP(glIsProgramARB, GLboolean, (GLuint program)) \
  OP(glProgramStringARB, void, (GLenum target, GLenum format, GLsizei length, const void* text)) \
  OP(glGetProgramivARB, void, (GLenum target, GLenum pname, GLint* value)) \
  OP(glGetProgramStringARB, void, (GLenum target, GLenum pname, void* text)) \
  OP(glProgramLocalParameter4fARB, void, (GLenum target, GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w)) \
  OP(glProgramLocalParameter4fvARB, void, (GLenum target, GLuint index, const GLfloat* value)) \
  OP(glGetProgramLocalParameterfvARB, void, (GLenum target, GLuint index, GLfloat* value)) \
  OP(glProgramLocalParameter4dARB, void, (GLenum target, GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w)) \
  OP(glProgramLocalParameter4dvARB, void, (GLenum target, GLuint index, const GLdouble* value)) \
  OP(glGetProgramLocalParameterdvARB, void, (GLenum target, GLuint index, GLdouble* value)) \
  OP(glProgramEnvParameter4fARB, void, (GLenum target, GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w)) \
  OP(glProgramEnvParameter4fvARB, void, (GLenum target, GLuint index, const GLfloat* value)) \
  OP(glGetProgramEnvParameterfvARB, void, (GLenum target, GLuint index, GLfloat* value)) \
  OP(glProgramEnvParameter4dARB, void, (GLenum target, GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w)) \
  OP(glProgramEnvParameter4dvARB, void, (GLenum target, GLuint index, const GLdouble* value)) \
  OP(glGetProgramEnvParameterdvARB, void, (GLenum target, GLuint index, GLdouble* value))
