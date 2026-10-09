#include <common/GeneratorInterface.h>

#include <GLES2/gl2.h>

template<auto>
struct fex_gen_config {
  unsigned version = 2;
};

template<auto, int, typename = void>
struct fex_gen_param {};

template<>
struct fex_gen_param<glShaderSource, 2, const GLchar* const*> : fexgen::assume_compatible_data_layout {};

#define FEX_GLES(name) \
  template<>           \
  struct fex_gen_config<name> {};

FEX_GLES(glActiveTexture)
FEX_GLES(glAttachShader)
FEX_GLES(glBindAttribLocation)
FEX_GLES(glBindBuffer)
FEX_GLES(glBindFramebuffer)
FEX_GLES(glBindRenderbuffer)
FEX_GLES(glBindTexture)
FEX_GLES(glBlendFunc)
FEX_GLES(glBufferData)
FEX_GLES(glBufferSubData)
FEX_GLES(glCheckFramebufferStatus)
FEX_GLES(glClear)
FEX_GLES(glClearColor)
FEX_GLES(glCompileShader)
FEX_GLES(glCreateProgram)
FEX_GLES(glCreateShader)
FEX_GLES(glCullFace)
FEX_GLES(glDeleteBuffers)
FEX_GLES(glDeleteFramebuffers)
FEX_GLES(glDeleteProgram)
FEX_GLES(glDeleteRenderbuffers)
FEX_GLES(glDeleteShader)
FEX_GLES(glDeleteTextures)
FEX_GLES(glDepthFunc)
FEX_GLES(glDepthMask)
FEX_GLES(glDisable)
FEX_GLES(glDisableVertexAttribArray)
FEX_GLES(glDrawArrays)
FEX_GLES(glDrawElements)
FEX_GLES(glEnable)
FEX_GLES(glEnableVertexAttribArray)
FEX_GLES(glFinish)
FEX_GLES(glFlush)
FEX_GLES(glFramebufferRenderbuffer)
FEX_GLES(glFramebufferTexture2D)
FEX_GLES(glFrontFace)
FEX_GLES(glGenBuffers)
FEX_GLES(glGenFramebuffers)
FEX_GLES(glGenRenderbuffers)
FEX_GLES(glGenTextures)
FEX_GLES(glGenerateMipmap)
FEX_GLES(glGetAttribLocation)
FEX_GLES(glGetError)
FEX_GLES(glGetIntegerv)
FEX_GLES(glGetProgramInfoLog)
FEX_GLES(glGetProgramiv)
FEX_GLES(glGetShaderInfoLog)
FEX_GLES(glGetShaderiv)
FEX_GLES(glGetString)
FEX_GLES(glGetUniformLocation)
FEX_GLES(glHint)
FEX_GLES(glLineWidth)
FEX_GLES(glLinkProgram)
FEX_GLES(glPixelStorei)
FEX_GLES(glReadPixels)
FEX_GLES(glRenderbufferStorage)
FEX_GLES(glScissor)
FEX_GLES(glShaderSource)
FEX_GLES(glTexImage2D)
FEX_GLES(glTexParameteri)
FEX_GLES(glTexParameterf)
FEX_GLES(glUniform1f)
FEX_GLES(glUniform1i)
FEX_GLES(glUniform2f)
FEX_GLES(glUniform3f)
FEX_GLES(glUniform4f)
FEX_GLES(glUniform4fv)
FEX_GLES(glUniformMatrix4fv)
FEX_GLES(glUseProgram)
FEX_GLES(glVertexAttribPointer)

// Place the guest viewport in the part of the Android surface the log panel does not cover.
template<>
struct fex_gen_config<glViewport> : fexgen::custom_host_impl {};

#undef FEX_GLES
