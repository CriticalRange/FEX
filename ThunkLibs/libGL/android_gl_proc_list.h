#ifndef FEX_THUNKLIBS_LIBGL_ANDROID_GL_PROC_LIST_H
#define FEX_THUNKLIBS_LIBGL_ANDROID_GL_PROC_LIST_H

// VEXA_FIXES: Canonical Android GL proc allowlist used by the libGL thunk Android path.
// Shared Android GL proc surface used by the guest-side lookup tables.
// Keep this list in sync with the Android-facing libGL thunk surface.
#define FEX_ANDROID_GL_PROC_LIST(OP) \
  OP(glGetString) \
  OP(glGetStringi) \
  OP(glClear) \
  OP(glClearColor) \
  OP(glClearStencil) \
  OP(glClearDepthf) \
  OP(glClearDepthfOES) \
  OP(glClearDepth) \
  OP(glViewport) \
  OP(glEnable) \
  OP(glDisable) \
  OP(glBlendFunc) \
  OP(glBlendFuncSeparate) \
  OP(glBlendEquation) \
  OP(glColorMask) \
  OP(glDepthFunc) \
  OP(glDepthMask) \
  OP(glScissor) \
  OP(glStencilFunc) \
  OP(glStencilMask) \
  OP(glStencilOp) \
  OP(glCullFace) \
  OP(glFrontFace) \
  OP(glCreateShader) \
  OP(glDeleteShader) \
  OP(glShaderSource) \
  OP(glCompileShader) \
  OP(glGetShaderiv) \
  OP(glGetShaderInfoLog) \
  OP(glCreateProgram) \
  OP(glDeleteProgram) \
  OP(glAttachShader) \
  OP(glDetachShader) \
  OP(glLinkProgram) \
  OP(glUseProgram) \
  OP(glGetProgramiv) \
  OP(glGetProgramInfoLog) \
  OP(glBindAttribLocation) \
  OP(glGetActiveAttrib) \
  OP(glGetActiveUniform) \
  OP(glGetUniformBlockIndex) \
  OP(glGetUniformLocation) \
  OP(glGetAttribLocation) \
  OP(glUniform1i) \
  OP(glUniform1f) \
  OP(glUniform1fv) \
  OP(glUniform2f) \
  OP(glUniform3f) \
  OP(glUniform4f) \
  OP(glUniform4fv) \
  OP(glUniformBlockBinding) \
  OP(glUniformMatrix2fv) \
  OP(glUniformMatrix3fv) \
  OP(glUniformMatrix4fv) \
  OP(glGenBuffers) \
  OP(glDeleteBuffers) \
  OP(glBindBuffer) \
  OP(glBindBufferRange) \
  OP(glBindVertexBuffer) \
  OP(glBufferData) \
  OP(glBufferStorage) \
  OP(glBufferSubData) \
  OP(glMapBufferRange) \
  OP(glGetBufferPointerv) \
  OP(glUnmapBuffer) \
  OP(glGenVertexArrays) \
  OP(glDeleteVertexArrays) \
  OP(glGenVertexArraysAPPLE) \
  OP(glDeleteVertexArraysAPPLE) \
  OP(glBindVertexArray) \
  OP(glBindVertexArrayAPPLE) \
  OP(glVertexAttribBinding) \
  OP(glVertexAttribFormat) \
  OP(glVertexAttribPointer) \
  OP(glVertexAttribIPointer) \
  OP(glVertexAttrib2f) \
  OP(glVertexAttrib4f) \
  OP(glVertexAttrib4fv) \
  OP(glEnableVertexAttribArray) \
  OP(glDisableVertexAttribArray) \
  OP(glGetVertexAttribiv) \
  OP(glGenTextures) \
  OP(glDeleteTextures) \
  OP(glBindTexture) \
  OP(glTexImage2D) \
  OP(glTexSubImage2D) \
  OP(glTexParameteri) \
  OP(glTexParameterf) \
  OP(glTexStorage2D) \
  OP(glGetTexParameteriv) \
  OP(glCompressedTexImage2D) \
  OP(glActiveTexture) \
  OP(glGenerateMipmap) \
  OP(glGenFramebuffers) \
  OP(glDeleteFramebuffers) \
  OP(glBindFramebuffer) \
  OP(glFramebufferTexture2D) \
  OP(glFramebufferTextureLayer) \
  OP(glInvalidateFramebuffer) \
  OP(glCheckFramebufferStatus) \
  OP(glBlitFramebuffer) \
  OP(glBlitFramebufferEXT) \
  OP(glGenRenderbuffers) \
  OP(glDeleteRenderbuffers) \
  OP(glBindRenderbuffer) \
  OP(glRenderbufferStorage) \
  OP(glRenderbufferStorageMultisample) \
  OP(glRenderbufferStorageMultisampleEXT) \
  OP(glFramebufferRenderbuffer) \
  OP(glGetRenderbufferParameteriv) \
  OP(glGenSamplers) \
  OP(glDeleteSamplers) \
  OP(glBindSampler) \
  OP(glSamplerParameterf) \
  OP(glSamplerParameteri) \
  OP(glDrawArrays) \
  OP(glDrawElements) \
  OP(glDrawArraysInstanced) \
  OP(glDrawElementsInstanced) \
  OP(glDrawElementsIndirect) \
  OP(glDrawArraysIndirect) \
  OP(glDrawElementsBaseVertex) \
  OP(glDrawElementsInstancedBaseVertex) \
  OP(glDispatchCompute) \
  OP(glDispatchComputeIndirect) \
  OP(glMemoryBarrier) \
  OP(glBindImageTexture) \
  OP(glGetProgramInterfaceiv) \
  OP(glGetProgramResourceIndex) \
  OP(glGetProgramResourceName) \
  OP(glGetProgramResourceiv) \
  OP(glGetIntegeri_v) \
  OP(glCopyBufferSubData) \
  OP(glCopyImageSubData) \
  OP(glGetInternalformativ) \
  OP(glProgramParameteri) \
  OP(glTexStorage3D) \
  OP(glTexStorage2DMultisample) \
  OP(glFramebufferTexture) \
  OP(glUniform1uiv) \
  OP(glUniform2iv) \
  OP(glUniform2uiv) \
  OP(glUniform3iv) \
  OP(glUniform3uiv) \
  OP(glUniform4iv) \
  OP(glUniform4uiv) \
  OP(glGetActiveUniformBlockiv) \
  OP(glClearBufferuiv) \
  OP(glColorMaski) \
  OP(glBlendEquationi) \
  OP(glFlushMappedBufferRange) \
  OP(glVertexAttribDivisor) \
  OP(glReadBuffer) \
  OP(glReadPixels) \
  OP(glFenceSync) \
  OP(glClientWaitSync) \
  OP(glWaitSync) \
  OP(glIsSync) \
  OP(glGetSynciv) \
  OP(glDeleteSync) \
  OP(glFlush) \
  OP(glGetError) \
  OP(glGetBooleanv) \
  OP(glGetIntegerv) \
  OP(glGetFloatv) \
  OP(glIsBuffer) \
  OP(glIsFramebuffer) \
  OP(glIsProgram) \
  OP(glIsRenderbuffer) \
  OP(glIsSampler) \
  OP(glIsShader) \
  OP(glIsVertexArray) \
  OP(glIsVertexArrayAPPLE) \
  OP(glObjectLabel) \
  OP(glPushDebugGroup) \
  OP(glPopDebugGroup) \
  OP(glPushGroupMarkerEXT) \
  OP(glPopGroupMarkerEXT) \
  OP(glBeginQuery) \
  OP(glBeginTransformFeedback) \
  OP(glBindBufferBase) \
  OP(glBlendEquationSeparate) \
  OP(glBlendFuncSeparatei) \
  OP(glBlendFunci) \
  OP(glClearBufferfv) \
  OP(glCopyTexSubImage2D) \
  OP(glDeleteQueries) \
  OP(glDrawBuffers) \
  OP(glEndQuery) \
  OP(glEndTransformFeedback) \
  OP(glGenQueries) \
  OP(glGetQueryObjectui64v) \
  OP(glGetQueryObjectuiv) \
  OP(glHint) \
  OP(glPointSize) \
  OP(glPolygonOffset) \
  OP(glQueryCounter) \
  OP(glTransformFeedbackVaryings) \
  OP(glStencilFuncSeparate) \
  OP(glStencilOpSeparate) \
  OP(glTexBuffer) \
  OP(glTexImage3D) \
  OP(glTexParameterfv) \
  OP(glTexSubImage3D) \
  OP(glUniform1iv) \
  OP(glUniform2fv) \
  OP(glUniform2i) \
  OP(glUniform3fv) \
  OP(glUniform3i) \
  OP(glUniform4i) \
  OP(glPixelStorei) \
  OP(glDisableClientState) \
  OP(glGetUniformiv) \
  OP(glGetUniformIndices) \
  OP(glGetActiveUniformsiv) \
  OP(glGetActiveUniformBlockName) \
  OP(glGetQueryiv) \
  OP(glValidateProgram) \
  OP(glGetShaderSource) \
  OP(glGetShaderPrecisionFormat) \
  OP(glClearBufferfi) \
  OP(glCompressedTexImage3D) \
  OP(glCompressedTexSubImage2D) \
  OP(glCompressedTexSubImage3D) \
  OP(glProgramBinary) \
  OP(glGetProgramBinary) \
  OP(glProgramUniform1iv) \
  OP(glProgramUniform1fv) \
  OP(glProgramUniform1uiv) \
  OP(glProgramUniform2iv) \
  OP(glProgramUniform2fv) \
  OP(glProgramUniform2uiv) \
  OP(glProgramUniform3iv) \
  OP(glProgramUniform3fv) \
  OP(glProgramUniform3uiv) \
  OP(glProgramUniform4iv) \
  OP(glProgramUniform4fv) \
  OP(glProgramUniform4uiv) \
  OP(glProgramUniformMatrix2fv) \
  OP(glProgramUniformMatrix3fv) \
  OP(glProgramUniformMatrix4fv) \
  OP(glProgramUniformMatrix2x3fv) \
  OP(glProgramUniformMatrix3x2fv) \
  OP(glProgramUniformMatrix2x4fv) \
  OP(glProgramUniformMatrix4x2fv) \
  OP(glProgramUniformMatrix3x4fv) \
  OP(glProgramUniformMatrix4x3fv) \
  OP(glTexImage2DMultisample) \
  OP(glTexStorage3DMultisample) \
  OP(glGetTextureParameteriv) \
  OP(glGetTextureLevelParameteriv) \
  OP(glMapBufferOES) \
  OP(glBlendBarrierKHR) \
  OP(glBlendEquationSeparatei) \
  OP(glDebugMessageCallback) \
  OP(glDebugMessageControl) \
  OP(glDebugMessageInsert) \
  OP(glGetObjectLabel) \
  OP(glGetObjectLabelEXT) \
  OP(glLabelObjectEXT) \
  OP(glFramebufferTexture2DMultisampleEXT) \
  OP(glEGLImageTargetTexture2DOES) \
  OP(glDrawRangeElements) \
  OP(glDrawRangeElementsBaseVertex) \
  OP(glDrawArraysInstancedBaseInstance) \
  OP(glDrawElementsInstancedBaseInstance) \
  OP(glDrawElementsInstancedBaseVertexBaseInstance) \
  OP(glMultiDrawArrays) \
  OP(glMultiDrawElements) \
  OP(glMultiDrawElementsBaseVertex) \
  OP(glDrawTransformFeedback) \
  OP(glDrawTransformFeedbackInstanced) \
  OP(glDrawTransformFeedbackStream) \
  OP(glDrawTransformFeedbackStreamInstanced) \
  OP(glResolveMultisampleFramebufferAPPLE)

// VEXA_FIXES: Thunkgen list intentionally excludes custom-bridged entries (for example
// glShaderSource) that are handled via dedicated host implementations.
#define FEX_ANDROID_GL_PROC_LIST_FOR_THUNKGEN(OP) \
  OP(glGetString) \
  OP(glGetStringi) \
  OP(glClearStencil) \
  OP(glClearDepthf) \
  OP(glClearDepthfOES) \
  OP(glClearDepth) \
  OP(glBlendFunc) \
  OP(glBlendFuncSeparate) \
  OP(glBlendEquation) \
  OP(glColorMask) \
  OP(glDepthFunc) \
  OP(glDepthMask) \
  OP(glStencilFunc) \
  OP(glStencilMask) \
  OP(glStencilOp) \
  OP(glCullFace) \
  OP(glFrontFace) \
  OP(glCreateShader) \
  OP(glDeleteShader) \
  OP(glCompileShader) \
  OP(glGetShaderiv) \
  OP(glGetShaderInfoLog) \
  OP(glCreateProgram) \
  OP(glDeleteProgram) \
  OP(glAttachShader) \
  OP(glDetachShader) \
  OP(glLinkProgram) \
  OP(glGetProgramiv) \
  OP(glGetProgramInfoLog) \
  OP(glBindAttribLocation) \
  OP(glGetActiveAttrib) \
  OP(glGetActiveUniform) \
  OP(glGetUniformBlockIndex) \
  OP(glGetUniformLocation) \
  OP(glGetAttribLocation) \
  OP(glUniform1i) \
  OP(glUniform1f) \
  OP(glUniform1fv) \
  OP(glUniform2f) \
  OP(glUniform3f) \
  OP(glUniform4f) \
  OP(glUniform4fv) \
  OP(glUniformBlockBinding) \
  OP(glUniformMatrix2fv) \
  OP(glUniformMatrix3fv) \
  OP(glUniformMatrix4fv) \
  OP(glGenBuffers) \
  OP(glDeleteBuffers) \
  OP(glBindBuffer) \
  OP(glBindBufferRange) \
  OP(glBindVertexBuffer) \
  OP(glBufferData) \
  OP(glBufferStorage) \
  OP(glBufferSubData) \
  OP(glMapBufferRange) \
  OP(glGetBufferPointerv) \
  OP(glWaitSync) \
  OP(glIsSync) \
  OP(glUnmapBuffer) \
  OP(glGenVertexArrays) \
  OP(glDeleteVertexArrays) \
  OP(glGenVertexArraysAPPLE) \
  OP(glDeleteVertexArraysAPPLE) \
  OP(glBindVertexArray) \
  OP(glBindVertexArrayAPPLE) \
  OP(glVertexAttribBinding) \
  OP(glVertexAttribFormat) \
  OP(glVertexAttribPointer) \
  OP(glVertexAttribIPointer) \
  OP(glVertexAttrib2f) \
  OP(glVertexAttrib4f) \
  OP(glVertexAttrib4fv) \
  OP(glEnableVertexAttribArray) \
  OP(glDisableVertexAttribArray) \
  OP(glGetVertexAttribiv) \
  OP(glGenTextures) \
  OP(glDeleteTextures) \
  OP(glBindTexture) \
  OP(glTexImage2D) \
  OP(glTexSubImage2D) \
  OP(glTexStorage2D) \
  OP(glGetTexParameteriv) \
  OP(glCompressedTexImage2D) \
  OP(glActiveTexture) \
  OP(glGenerateMipmap) \
  OP(glGenFramebuffers) \
  OP(glDeleteFramebuffers) \
  OP(glFramebufferTexture2D) \
  OP(glFramebufferTextureLayer) \
  OP(glInvalidateFramebuffer) \
  OP(glCheckFramebufferStatus) \
  OP(glGenRenderbuffers) \
  OP(glDeleteRenderbuffers) \
  OP(glBindRenderbuffer) \
  OP(glRenderbufferStorageMultisampleEXT) \
  OP(glFramebufferRenderbuffer) \
  OP(glGetRenderbufferParameteriv) \
  OP(glGenSamplers) \
  OP(glDeleteSamplers) \
  OP(glBindSampler) \
  OP(glSamplerParameterf) \
  OP(glSamplerParameteri) \
  OP(glBindImageTexture) \
  OP(glGetProgramInterfaceiv) \
  OP(glGetProgramResourceIndex) \
  OP(glGetProgramResourceName) \
  OP(glGetProgramResourceiv) \
  OP(glGetIntegeri_v) \
  OP(glCopyBufferSubData) \
  OP(glCopyImageSubData) \
  OP(glGetInternalformativ) \
  OP(glProgramParameteri) \
  OP(glTexStorage3D) \
  OP(glTexStorage2DMultisample) \
  OP(glFramebufferTexture) \
  OP(glUniform1uiv) \
  OP(glUniform2iv) \
  OP(glUniform2uiv) \
  OP(glUniform3iv) \
  OP(glUniform3uiv) \
  OP(glUniform4iv) \
  OP(glUniform4uiv) \
  OP(glGetActiveUniformBlockiv) \
  OP(glColorMaski) \
  OP(glBlendEquationi) \
  OP(glFlushMappedBufferRange) \
  OP(glVertexAttribDivisor) \
  OP(glReadBuffer) \
  OP(glFlush) \
  OP(glGetError) \
  OP(glGetBooleanv) \
  OP(glGetIntegerv) \
  OP(glGetFloatv) \
  OP(glIsBuffer) \
  OP(glIsFramebuffer) \
  OP(glIsProgram) \
  OP(glIsRenderbuffer) \
  OP(glIsSampler) \
  OP(glIsShader) \
  OP(glIsVertexArray) \
  OP(glIsVertexArrayAPPLE) \
  OP(glObjectLabel) \
  OP(glPushDebugGroup) \
  OP(glPopDebugGroup) \
  OP(glPushGroupMarkerEXT) \
  OP(glPopGroupMarkerEXT) \
  OP(glBeginTransformFeedback) \
  OP(glBindBufferBase) \
  OP(glBlendEquationSeparate) \
  OP(glBlendFuncSeparatei) \
  OP(glBlendFunci) \
  OP(glCopyTexSubImage2D) \
  OP(glDeleteQueries) \
  OP(glDrawBuffers) \
  OP(glEndTransformFeedback) \
  OP(glHint) \
  OP(glPointSize) \
  OP(glPolygonOffset) \
  OP(glQueryCounter) \
  OP(glTransformFeedbackVaryings) \
  OP(glStencilFuncSeparate) \
  OP(glStencilOpSeparate) \
  OP(glTexBuffer) \
  OP(glTexImage3D) \
  OP(glTexParameterfv) \
  OP(glTexSubImage3D) \
  OP(glUniform1iv) \
  OP(glUniform2fv) \
  OP(glUniform2i) \
  OP(glUniform3fv) \
  OP(glUniform3i) \
  OP(glUniform4i) \
  OP(glPixelStorei) \
  OP(glDisableClientState)

#ifdef BUILD_ANDROID
// VEXA_FIXES: Declare EXT/APPLE alias entrypoints used by Android fallback lookups and
// guest expectations even when the underlying GLES driver only exposes core variants.
extern "C" {
void glBindVertexArrayAPPLE(GLuint array);
void glDeleteVertexArraysAPPLE(GLsizei n, const GLuint* arrays);
void glGenVertexArraysAPPLE(GLsizei n, GLuint* arrays);
GLboolean glIsVertexArrayAPPLE(GLuint array);
void glBlitFramebufferEXT(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                          GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                          GLbitfield mask, GLenum filter);
void glRenderbufferStorageMultisampleEXT(GLenum target, GLsizei samples, GLenum internalformat,
                                         GLsizei width, GLsizei height);
void glPushGroupMarkerEXT(GLsizei length, const GLchar* marker);
void glPopGroupMarkerEXT(void);
void glClearDepthfOES(GLfloat depth);
void glDisableClientState(GLenum array);

// VEXA_FIXES: Signatures for guest-visible procs that are absent from the GLES
// headers but thunkable through eglGetProcAddress-backed host shims.
void glGetUniformiv(GLuint program, GLint location, GLint* params);
void glGetUniformIndices(GLuint program, GLsizei uniformCount, const GLchar* const* uniformNames, GLuint* uniformIndices);
void glGetActiveUniformsiv(GLuint program, GLsizei uniformCount, const GLuint* uniformIndices, GLenum pname, GLint* params);
void glGetActiveUniformBlockName(GLuint program, GLuint uniformBlockIndex, GLsizei bufSize, GLsizei* length, GLchar* uniformBlockName);
void glGetQueryiv(GLenum target, GLenum pname, GLint* params);
void glValidateProgram(GLuint program);
void glGetShaderSource(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* source);
void glGetShaderPrecisionFormat(GLenum shaderType, GLenum precisionType, GLint* range, GLint* precision);
void glClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
void glCompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void* data);
void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void* data);
void glCompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void* data);
void glProgramBinary(GLuint program, GLenum binaryFormat, const void* binary, GLsizei length);
void glGetProgramBinary(GLuint program, GLsizei bufSize, GLsizei* length, GLenum* binaryFormat, void* binary);
void glProgramUniform1iv(GLuint program, GLint location, GLsizei count, const GLint* value);
void glProgramUniform1fv(GLuint program, GLint location, GLsizei count, const GLfloat* value);
void glProgramUniform1uiv(GLuint program, GLint location, GLsizei count, const GLuint* value);
void glProgramUniform2iv(GLuint program, GLint location, GLsizei count, const GLint* value);
void glProgramUniform2fv(GLuint program, GLint location, GLsizei count, const GLfloat* value);
void glProgramUniform2uiv(GLuint program, GLint location, GLsizei count, const GLuint* value);
void glProgramUniform3iv(GLuint program, GLint location, GLsizei count, const GLint* value);
void glProgramUniform3fv(GLuint program, GLint location, GLsizei count, const GLfloat* value);
void glProgramUniform3uiv(GLuint program, GLint location, GLsizei count, const GLuint* value);
void glProgramUniform4iv(GLuint program, GLint location, GLsizei count, const GLint* value);
void glProgramUniform4fv(GLuint program, GLint location, GLsizei count, const GLfloat* value);
void glProgramUniform4uiv(GLuint program, GLint location, GLsizei count, const GLuint* value);
void glProgramUniformMatrix2fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
void glProgramUniformMatrix3fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
void glProgramUniformMatrix4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
void glProgramUniformMatrix2x3fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
void glProgramUniformMatrix3x2fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
void glProgramUniformMatrix2x4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
void glProgramUniformMatrix4x2fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
void glProgramUniformMatrix3x4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
void glProgramUniformMatrix4x3fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
void glTexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations);
void glTexStorage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedsamplelocations);
void glGetTextureParameteriv(GLuint texture, GLenum pname, GLint* params);
void glGetTextureLevelParameteriv(GLuint texture, GLint level, GLenum pname, GLint* params);
void* glMapBufferOES(GLenum target, GLenum access);
void glBlendBarrierKHR(void);
void glBlendEquationSeparatei(GLuint buf, GLenum modeRGB, GLenum modeAlpha);
typedef void (*VEXA_GLDEBUGPROC)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam);
void glDebugMessageCallback(VEXA_GLDEBUGPROC callback, const void* userParam);
void glDebugMessageControl(GLenum source, GLenum type, GLenum severity, GLsizei count, const GLuint* ids, GLboolean enabled);
void glDebugMessageInsert(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* buf);
void glGetObjectLabel(GLenum identifier, GLuint name, GLsizei bufSize, GLsizei* length, GLchar* label);
void glGetObjectLabelEXT(GLenum type, GLuint object, GLsizei bufSize, GLsizei* length, GLchar* label);
void glLabelObjectEXT(GLenum type, GLuint object, GLsizei length, const GLchar* label);
void glFramebufferTexture2DMultisampleEXT(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLsizei samples);
void glEGLImageTargetTexture2DOES(GLenum target, void* image);
void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices);
void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices, GLint basevertex);
void glDrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei primcount, GLuint baseinstance);
void glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei primcount, GLuint baseinstance);
void glDrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei primcount, GLint basevertex, GLuint baseinstance);
void glMultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount);
void glDrawTransformFeedback(GLenum mode, GLuint id);
void glDrawTransformFeedbackInstanced(GLenum mode, GLuint id, GLsizei primcount);
void glDrawTransformFeedbackStream(GLenum mode, GLuint id, GLuint stream);
void glDrawTransformFeedbackStreamInstanced(GLenum mode, GLuint id, GLuint stream, GLsizei primcount);
void glResolveMultisampleFramebufferAPPLE(void);
}
#endif

#endif
