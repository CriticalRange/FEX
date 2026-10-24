// Included after the core wrappers: ARB names must keep their translation and
// program-lifetime behavior rather than bypassing them through the GLES loader.
#define VEXA_ARB_FORWARD(name, core, result, params, args) \
  static result fexfn_impl_libGL_##name params { \
    static const auto vexa_core_entry = reinterpret_cast<decltype(&::core)>( \
      fexfn_impl_libGL_glXGetProcAddress(reinterpret_cast<const GLubyte*>(#core))); \
    return vexa_core_entry args; \
  }
VEXA_ARB_SHADER_CORE_ALIASES(VEXA_ARB_FORWARD)
#undef VEXA_ARB_FORWARD

static void VexaARBError(GLenum error) {
  auto& pending = VexaProgramContext().CompatError;
  if (!pending) pending = error;
}

static GLenum VexaARBObjectType(GLuint object) {
  if (::glIsShader(object)) return 0x8B48; // GL_SHADER_OBJECT_ARB
  if (::glIsProgram(object)) return 0x8B40; // GL_PROGRAM_OBJECT_ARB
  VexaARBError(GL_INVALID_VALUE);
  return 0;
}

static bool VexaARBObjectParameter(GLuint object, GLenum pname, GLint* value) {
  const GLenum type = VexaARBObjectType(object);
  if (!type) return false;
  const bool shader = type == 0x8B48;
  switch (pname) {
    case 0x8B4E: *value = type; return true; // OBJECT_TYPE_ARB
    case GL_SHADER_TYPE:
    case GL_COMPILE_STATUS:
    case GL_SHADER_SOURCE_LENGTH:
      if (!shader) { VexaARBError(GL_INVALID_OPERATION); return false; }
      ::glGetShaderiv(object, pname, value);
      return true;
    case GL_LINK_STATUS:
    case GL_VALIDATE_STATUS:
    case GL_ATTACHED_SHADERS:
    case GL_ACTIVE_UNIFORMS:
    case GL_ACTIVE_UNIFORM_MAX_LENGTH:
      if (shader) { VexaARBError(GL_INVALID_OPERATION); return false; }
      fexfn_impl_libGL_glGetProgramiv(object, pname, value);
      return true;
    case GL_DELETE_STATUS:
    case GL_INFO_LOG_LENGTH:
      if (shader) ::glGetShaderiv(object, pname, value);
      else fexfn_impl_libGL_glGetProgramiv(object, pname, value);
      return true;
    default:
      VexaARBError(GL_INVALID_ENUM);
      return false;
  }
}

static void fexfn_impl_libGL_glDeleteObjectARB(GLuint object) {
  if (!object) return;
  VEXA_PROGRAM_LOCK();
  const GLenum type = VexaARBObjectType(object);
  if (type == 0x8B48) fexfn_impl_libGL_glDeleteShader(object);
  else if (type == 0x8B40) fexfn_impl_libGL_glDeleteProgram(object);
}

static GLuint fexfn_impl_libGL_glGetHandleARB(GLenum pname) {
  if (pname != 0x8B40) { VexaARBError(GL_INVALID_ENUM); return 0; }
  GLint program = 0;
  ::glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  return static_cast<GLuint>(program);
}

static void fexfn_impl_libGL_glGetObjectParameterivARB(GLuint object, GLenum pname, GLint* value) {
  if (!value) return;
  VEXA_PROGRAM_LOCK();
  VexaARBObjectParameter(object, pname, value);
}

static void fexfn_impl_libGL_glGetObjectParameterfvARB(GLuint object, GLenum pname, GLfloat* value) {
  if (!value) return;
  VEXA_PROGRAM_LOCK();
  GLint result = 0;
  if (VexaARBObjectParameter(object, pname, &result)) *value = static_cast<GLfloat>(result);
}

static void fexfn_impl_libGL_glGetInfoLogARB(GLuint object, GLsizei maximum, GLsizei* length, GLchar* log) {
  if (maximum < 0) { VexaARBError(GL_INVALID_VALUE); return; }
  VEXA_PROGRAM_LOCK();
  const GLenum type = VexaARBObjectType(object);
  if (type == 0x8B48) ::glGetShaderInfoLog(object, maximum, length, log);
  else if (type == 0x8B40) ::glGetProgramInfoLog(object, maximum, length, log);
}
