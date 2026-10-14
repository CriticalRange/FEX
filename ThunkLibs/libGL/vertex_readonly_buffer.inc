// HLSLCC's readonly 64-byte grass transforms can be sampled from the same
// buffer object on GLES drivers which expose zero vertex storage blocks.
// Exact layout/access matching keeps other storage shaders on their native path.
static std::unordered_map<GLuint, bool> VexaGrassShaders;
struct VexaGrassProgram { GLint sampler = -1, base = -1; GLuint texture = 0; };
static std::unordered_map<GLuint, VexaGrassProgram> VexaGrassPrograms;

static bool VexaRewriteGrassBuffer(GLuint shader, GLenum type, std::string& source) {
  VexaGrassShaders.erase(shader);
  if (type != GL_VERTEX_SHADER || source.find("_GrassBuffer_buf") == std::string::npos) return false;
  GLint limit = 0;
  ::glGetIntegerv(0x90D6 /* MAX_VERTEX_SHADER_STORAGE_BLOCKS */, &limit);
  if (limit != 0 || !eglGetProcAddress("glTexBuffer")) return false;
  static const std::regex declaration(R"(struct\s+_GrassBuffer_type\s*\{\s*uint\[16\]\s+value;\s*\};\s*layout\(std430,\s*binding\s*=\s*0\)\s+readonly\s+buffer\s+_GrassBuffer\s*\{\s*_GrassBuffer_type\s+_GrassBuffer_buf\[\];\s*\};)");
  static const std::regex access(R"(_GrassBuffer_buf\[gl_InstanceID\]\.value\[([^\]]+)\])");
  if (!std::regex_search(source, declaration)) return false;
  std::string rewritten = std::regex_replace(source, declaration, "");
  rewritten = std::regex_replace(rewritten, access, "vexaGrassWord(gl_InstanceID, $1)");
  if (rewritten.find("_GrassBuffer_buf") != std::string::npos) return false;
  const size_t main = rewritten.find("void main()");
  if (main == std::string::npos) return false;
  rewritten.insert(main,
    "uniform highp usamplerBuffer vexaGrassData;\n"
    "uniform highp int vexaGrassBase;\n"
    "uint vexaGrassWord(int instance, int word) {\n"
    " return texelFetch(vexaGrassData, vexaGrassBase + instance * 4 + (word >> 2))[word & 3];\n}\n");
  source.swap(rewritten);
  VexaGrassShaders[shader] = true;
  fprintf(stderr, "[VEXA][SSBO] vertex readonly transform buffer -> RGBA32UI texture shader=%u\n", shader);
  return true;
}

static void VexaLinkGrassBuffer(GLuint program) {
  VexaGrassPrograms.erase(program);
  GLuint shaders[8] = {};
  GLsizei count = 0;
  ::glGetAttachedShaders(program, 8, &count, shaders);
  for (GLsizei i = 0; i < count; ++i) {
    if (!VexaGrassShaders.count(shaders[i])) continue;
    VexaGrassProgram data;
    data.sampler = ::glGetUniformLocation(program, "vexaGrassData");
    data.base = ::glGetUniformLocation(program, "vexaGrassBase");
    if (data.sampler < 0 || data.base < 0) continue;
    ::glGenTextures(1, &data.texture);
    VexaGrassPrograms.emplace(program, data);
    fprintf(stderr, "[VEXA][SSBO] texture-backed vertex program=%u sampler=%d\n", program, data.sampler);
  }
}

static void VexaPrepareGrassBuffer(GLuint program) {
  auto it = VexaGrassPrograms.find(program);
  if (it == VexaGrassPrograms.end()) return;
  auto& data = it->second;
  GLint buffer = 0, active = 0, units = 0;
  GLint64 start = 0;
  ::glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 0, &buffer);
  ::glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_START, 0, &start);
  if (buffer == 0) {
    static unsigned missing = 0;
    if (missing++ < 4) fprintf(stderr, "[VEXA][SSBO] missing transform binding program=%u\n", program);
    return;
  }
  ::glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
  ::glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &units);
  ::glActiveTexture(GL_TEXTURE0 + units - 1);
  ::glBindTexture(GL_TEXTURE_BUFFER, data.texture);
  using TexBuffer = void(GL_APIENTRYP)(GLenum, GLenum, GLuint);
  static auto tex_buffer = reinterpret_cast<TexBuffer>(eglGetProcAddress("glTexBuffer"));
  tex_buffer(GL_TEXTURE_BUFFER, GL_RGBA32UI, buffer);
  ::glUniform1i(data.sampler, units - 1);
  ::glUniform1i(data.base, static_cast<GLint>(start / 16));
  ::glActiveTexture(active);
  static unsigned logs = 0;
  if (logs++ < 4) fprintf(stderr, "[VEXA][SSBO] draw program=%u buffer=%d offset=%lld unit=%d\n", program, buffer, (long long)start, units - 1);
}
