#pragma once

// ABI-identical Linux ARB shader-object calls. Route through core wrappers so
// shader translation, context state and deferred deletion remain consistent.
#define VEXA_ARB_SHADER_CORE_ALIASES(OP) \
  OP(glColorMaskIndexedEXT, glColorMaski, void, (GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a), (index, r, g, b, a)) \
  OP(glEnableIndexedEXT, glEnablei, void, (GLenum cap, GLuint index), (cap, index)) \
  OP(glDisableIndexedEXT, glDisablei, void, (GLenum cap, GLuint index), (cap, index)) \
  OP(glIsEnabledIndexedEXT, glIsEnabledi, GLboolean, (GLenum cap, GLuint index), (cap, index)) \
  OP(glGetBooleanIndexedvEXT, glGetBooleani_v, void, (GLenum pname, GLuint index, GLboolean* value), (pname, index, value)) \
  OP(glGetIntegerIndexedvEXT, glGetIntegeri_v, void, (GLenum pname, GLuint index, GLint* value), (pname, index, value)) \
  OP(glBeginQueryARB, glBeginQuery, void, (GLenum target, GLuint query), (target, query)) \
  OP(glEndQueryARB, glEndQuery, void, (GLenum target), (target)) \
  OP(glGetQueryObjectivARB, glGetQueryObjectiv, void, (GLuint query, GLenum pname, GLint* value), (query, pname, value)) \
  OP(glGetQueryObjectuivARB, glGetQueryObjectuiv, void, (GLuint query, GLenum pname, GLuint* value), (query, pname, value)) \
  OP(glGenQueriesARB, glGenQueries, void, (GLsizei count, GLuint* queries), (count, queries)) \
  OP(glDeleteQueriesARB, glDeleteQueries, void, (GLsizei count, const GLuint* queries), (count, queries)) \
  OP(glAttachObjectARB, glAttachShader, void, (GLuint program, GLuint shader), (program, shader)) \
  OP(glDetachObjectARB, glDetachShader, void, (GLuint program, GLuint shader), (program, shader)) \
  OP(glCreateShaderObjectARB, glCreateShader, GLuint, (GLenum type), (type)) \
  OP(glCreateProgramObjectARB, glCreateProgram, GLuint, (void), ()) \
  OP(glCompileShaderARB, glCompileShader, void, (GLuint shader), (shader)) \
  OP(glLinkProgramARB, glLinkProgram, void, (GLuint program), (program)) \
  OP(glUseProgramObjectARB, glUseProgram, void, (GLuint program), (program)) \
  OP(glValidateProgramARB, glValidateProgram, void, (GLuint program), (program)) \
  OP(glUniform1fARB, glUniform1f, void, (GLint location, GLfloat v0), (location, v0)) \
  OP(glUniform1fvARB, glUniform1fv, void, (GLint location, GLsizei count, const GLfloat* value), (location, count, value)) \
  OP(glUniform2fARB, glUniform2f, void, (GLint location, GLfloat v0, GLfloat v1), (location, v0, v1)) \
  OP(glUniform2fvARB, glUniform2fv, void, (GLint location, GLsizei count, const GLfloat* value), (location, count, value)) \
  OP(glUniform3fARB, glUniform3f, void, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2), (location, v0, v1, v2)) \
  OP(glUniform3fvARB, glUniform3fv, void, (GLint location, GLsizei count, const GLfloat* value), (location, count, value)) \
  OP(glUniform4fARB, glUniform4f, void, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3), (location, v0, v1, v2, v3)) \
  OP(glUniform4fvARB, glUniform4fv, void, (GLint location, GLsizei count, const GLfloat* value), (location, count, value)) \
  OP(glUniform1iARB, glUniform1i, void, (GLint location, GLint v0), (location, v0)) \
  OP(glUniform1ivARB, glUniform1iv, void, (GLint location, GLsizei count, const GLint* value), (location, count, value)) \
  OP(glUniform2iARB, glUniform2i, void, (GLint location, GLint v0, GLint v1), (location, v0, v1)) \
  OP(glUniform2ivARB, glUniform2iv, void, (GLint location, GLsizei count, const GLint* value), (location, count, value)) \
  OP(glUniform3iARB, glUniform3i, void, (GLint location, GLint v0, GLint v1, GLint v2), (location, v0, v1, v2)) \
  OP(glUniform3ivARB, glUniform3iv, void, (GLint location, GLsizei count, const GLint* value), (location, count, value)) \
  OP(glUniform4iARB, glUniform4i, void, (GLint location, GLint v0, GLint v1, GLint v2, GLint v3), (location, v0, v1, v2, v3)) \
  OP(glUniform4ivARB, glUniform4iv, void, (GLint location, GLsizei count, const GLint* value), (location, count, value)) \
  OP(glUniformMatrix2fvARB, glUniformMatrix2fv, void, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value), (location, count, transpose, value)) \
  OP(glUniformMatrix3fvARB, glUniformMatrix3fv, void, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value), (location, count, transpose, value)) \
  OP(glUniformMatrix4fvARB, glUniformMatrix4fv, void, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value), (location, count, transpose, value)) \
  OP(glGetUniformLocationARB, glGetUniformLocation, GLint, (GLuint program, const GLchar* name), (program, name)) \
  OP(glGetActiveUniformARB, glGetActiveUniform, void, (GLuint program, GLuint index, GLsizei size, GLsizei* length, GLint* count, GLenum* type, GLchar* name), (program, index, size, length, count, type, name)) \
  OP(glGetUniformfvARB, glGetUniformfv, void, (GLuint program, GLint location, GLfloat* value), (program, location, value)) \
  OP(glGetUniformivARB, glGetUniformiv, void, (GLuint program, GLint location, GLint* value), (program, location, value)) \
  OP(glGetAttachedObjectsARB, glGetAttachedShaders, void, (GLuint program, GLsizei maximum, GLsizei* count, GLuint* shaders), (program, maximum, count, shaders)) \
  OP(glGetShaderSourceARB, glGetShaderSource, void, (GLuint shader, GLsizei size, GLsizei* length, GLchar* source), (shader, size, length, source)) \
  OP(glBindAttribLocationARB, glBindAttribLocation, void, (GLuint program, GLuint index, const GLchar* name), (program, index, name)) \
  OP(glGetAttribLocationARB, glGetAttribLocation, GLint, (GLuint program, const GLchar* name), (program, name)) \
  OP(glGetActiveAttribARB, glGetActiveAttrib, void, (GLuint program, GLuint index, GLsizei size, GLsizei* length, GLint* count, GLenum* type, GLchar* name), (program, index, size, length, count, type, name))

#define VEXA_ARB_SHADER_GENERIC(OP) \
  OP(glDeleteObjectARB, void, (GLuint object)) \
  OP(glGetHandleARB, GLuint, (GLenum pname)) \
  OP(glGetObjectParameterivARB, void, (GLuint object, GLenum pname, GLint* value)) \
  OP(glGetObjectParameterfvARB, void, (GLuint object, GLenum pname, GLfloat* value)) \
  OP(glGetInfoLogARB, void, (GLuint object, GLsizei maximum, GLsizei* length, GLchar* log))
