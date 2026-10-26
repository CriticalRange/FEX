// Desktop alpha testing for the opt-in compatibility path. The uniforms are
// internal implementation state; update them before every guest draw because
// a program can be shared by contexts with different alpha-test settings.
static bool VexaAlphaTestAvailable() {
  static const bool enabled = [] { const char* value = getenv("VEXA_GL_ALPHA_TEST"); return value && !strcmp(value, "1"); }();
  return enabled;
}

static void VexaAlphaFunc(GLenum func, GLfloat ref) {
  auto& state = VexaProgramContext();
  if (func < GL_NEVER || func > GL_ALWAYS) {
    if (!state.CompatError) state.CompatError = GL_INVALID_ENUM;
    return;
  }
  state.AlphaFunction = func;
  state.AlphaReference = std::clamp(ref, 0.0f, 1.0f);
}

static bool VexaAlphaQuery(GLenum pname, GLfloat* value) {
  if (!VexaAlphaTestAvailable()) return false;
  const auto& state = VexaProgramContext();
  switch (pname) {
    case 0x0BC0: *value = state.AlphaTest ? 1.0f : 0.0f; return true;
    case 0x0BC1: *value = static_cast<GLfloat>(state.AlphaFunction); return true;
    case 0x0BC2: *value = state.AlphaReference; return true;
    default: return false;
  }
}

static GLboolean fexfn_impl_libGL_glIsEnabled(GLenum cap) {
  GLfloat legacy;if(VexaLegacyQuery(cap,&legacy))return legacy!=0;
  if(VexaClipAvailable()&&cap>=0x3000&&cap<0x3006)return VexaProgramContext().Clip.Enabled[cap-0x3000]?GL_TRUE:GL_FALSE;
  if (const int stage=VexaAssemblyTarget(cap);stage>=0) return VexaProgramContext().Assembly.Enabled[stage]?GL_TRUE:GL_FALSE;
  if (VexaAlphaTestAvailable() && cap == 0x0BC0) return VexaProgramContext().AlphaTest ? GL_TRUE : GL_FALSE;
  return ::glIsEnabled(cap);
}
static void fexfn_impl_libGL_glGetFloatv(GLenum pname, GLfloat* value) {
  if(value&&(VexaClipQuery(pname,value)||VexaLegacyQuery(pname,value)))return;
  if (value && (VexaAssemblyQuery(pname, value) || VexaAlphaQuery(pname, value))) return;
  ::glGetFloatv(pname, value);
}
static void fexfn_impl_libGL_glGetBooleanv(GLenum pname, GLboolean* value) {
  GLfloat legacy;if(value&&VexaLegacyQuery(pname,&legacy)){*value=legacy!=0;return;}
  GLfloat clip[16];
  if(value){const int count=VexaClipQuery(pname,clip);if(count){for(int i=0;i<count;++i)value[i]=clip[i]!=0;return;}}
  GLfloat result;
  if (value && (VexaAssemblyQuery(pname, &result) || VexaAlphaQuery(pname, &result))) { *value = result != 0 ? GL_TRUE : GL_FALSE; return; }
  ::glGetBooleanv(pname, value);
}
static GLenum fexfn_impl_libGL_glGetError() {
  auto& error = VexaProgramContext().CompatError;
  if (error) { const auto result = error; error = GL_NO_ERROR; return result; }
  return ::glGetError();
}

// Mask comments, preserving offsets for lexical rewrites and declarations.
static std::string VexaAlphaCode(std::string_view source) {
  std::string code(source);
  for (size_t i = 0; i + 1 < code.size();) {
    if (code[i] == '/' && (code[i + 1] == '/' || code[i + 1] == '*')) {
      const bool line = code[i + 1] == '/';
      size_t end = line ? code.find('\n', i + 2) : code.find("*/", i + 2);
      end = end == std::string::npos ? code.size() : end + (line ? 0 : 2);
      for (; i < end; ++i) if (code[i] != '\n') code[i] = ' ';
    } else ++i;
  }
  return code;
}

static std::string VexaAlphaOutputUniform(std::string_view output) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string name = "vexa_alpha_output_";
  for (unsigned char c : output) { name += hex[c >> 4]; name += hex[c & 15]; }
  return name;
}

static std::string VexaAlphaInstrument(std::string source) {
  const std::string code = VexaAlphaCode(source);
  if (code.find("vexa_alpha_function") != std::string::npos) return source;
  static const std::regex main_re(R"(\bvoid\s+main\s*\(\s*(?:void\s*)?\)\s*\{)");
  std::smatch main_match;
  if (!std::regex_search(code, main_match, main_re)) return source;
  std::string declarations = "\nuniform highp int vexa_alpha_function;\nuniform highp float vexa_alpha_reference;\n";
  std::string alpha;
  if (std::regex_search(code, std::regex(R"(\bgl_FragColor\b)"))) alpha = "vexa_alpha_value = gl_FragColor.a;\n";
  else if (std::regex_search(code, std::regex(R"(\bgl_FragData\b)"))) alpha = "vexa_alpha_value = gl_FragData[0].a;\n";
  else {
    // Output locations may be assigned by glBindFragDataLocation as well as
    // layout qualifiers. Each output gets a selector uniform whose encoded
    // name survives program binaries; link reflection selects location zero.
    static const std::regex out_re(R"((?:^|[;\n}])\s*(?:layout\s*\([^)]*\)\s*)?(?:(?:invariant|precise|flat|smooth|centroid|sample)\s+)*out\s+(?:(?:highp|mediump|lowp)\s+)?vec4\s+([A-Za-z_][A-Za-z_0-9]*)\s*(?:\[\s*([0-9]+)\s*\])?\s*(?=;))");
    std::unordered_set<std::string> seen;
    for (std::sregex_iterator it(code.begin(), code.end(), out_re), end; it != end; ++it) {
      const auto& match = *it;
      const bool array = match[2].matched;
      const int count = array ? std::stoi(match[2].str()) : 1;
      for (int i = 0; i < count; ++i) {
        std::string output = match[1].str() + (array ? "[" + std::to_string(i) + "]" : "");
        if (!seen.insert(output).second) continue;
        const std::string uniform = VexaAlphaOutputUniform(output);
        declarations += "uniform bool " + uniform + ";\n";
        alpha += "if (" + uniform + ") vexa_alpha_value = " + output + ".a;\n";
      }
    }
  }
  // A shader with no color output has no defined alpha to compare.
  if (alpha.empty()) return source;
  // Rename declarations as well as the definition. Leaving a main(void)
  // prototype behind makes some GLES drivers reject the wrapper as an overload.
  static const std::regex main_name_re(R"(\bmain\b)");
  std::vector<size_t> main_names;
  for (std::sregex_iterator it(code.begin(), code.end(), main_name_re), end; it != end; ++it)
    main_names.push_back(static_cast<size_t>(it->position()));
  for (auto it = main_names.rbegin(); it != main_names.rend(); ++it)
    source.replace(*it, 4, "vexa_alpha_original_main");
  // Explicit early tests would update depth/stencil before discard. Desktop
  // alpha testing happens before those updates, so remove this forced mode.
  source = std::regex_replace(source, std::regex(R"(layout\s*\(\s*early_fragment_tests\s*\)\s*in\s*;)"), "");
  source.insert(VexaShaderPrecisionInsertPos(source), declarations);
  source += "\nvoid main() {\nvexa_alpha_original_main();\nif (vexa_alpha_function != 0) {\nhighp float vexa_alpha_value = 1.0;\n" + alpha +
    "bool pass = vexa_alpha_function == 8 ||\n"
    "(vexa_alpha_function == 2 && vexa_alpha_value < vexa_alpha_reference) ||\n"
    "(vexa_alpha_function == 3 && vexa_alpha_value == vexa_alpha_reference) ||\n"
    "(vexa_alpha_function == 4 && vexa_alpha_value <= vexa_alpha_reference) ||\n"
    "(vexa_alpha_function == 5 && vexa_alpha_value > vexa_alpha_reference) ||\n"
    "(vexa_alpha_function == 6 && vexa_alpha_value != vexa_alpha_reference) ||\n"
    "(vexa_alpha_function == 7 && vexa_alpha_value >= vexa_alpha_reference);\n"
    "if (!pass) discard;\n}\n}\n";
  return source;
}

static VexaAlphaProgramState VexaAlphaProgram(GLuint program) {
  VexaAlphaProgramState state;
  state.Function = ::glGetUniformLocation(program, "vexa_alpha_function");
  state.Reference = ::glGetUniformLocation(program, "vexa_alpha_reference");
  if (state.Function < 0) return state;
  GLint count = 0, maximum = 0;
  ::glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
  ::glGetProgramiv(program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maximum);
  std::string name(static_cast<size_t>(std::max(maximum, 1)), '\0');
  constexpr std::string_view prefix = "vexa_alpha_output_";
  for (GLint i = 0; i < count; ++i) {
    GLsizei length = 0; GLint size = 0; GLenum type = 0;
    ::glGetActiveUniform(program, i, maximum, &length, &size, &type, name.data());
    std::string_view uniform(name.data(), static_cast<size_t>(length));
    if (!uniform.starts_with(prefix)) continue;
    const auto encoded = uniform.substr(prefix.size());
    std::string output;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t n = 0; n + 1 < encoded.size(); n += 2) {
      const int hi = digit(encoded[n]), lo = digit(encoded[n + 1]);
      if (hi < 0 || lo < 0) { output.clear(); break; }
      output += static_cast<char>((hi << 4) | lo);
    }
    if (!output.empty()) {
      const GLint location = ::glGetUniformLocation(program, std::string(uniform).c_str());
      ::glProgramUniform1i(program, location, ::glGetFragDataLocation(program, output.c_str()) == 0);
    }
  }
  return state;
}

static void VexaApplyAlphaTest() {
  if (!VexaAlphaTestAvailable()) return;
  auto& context = VexaProgramContext();
  std::lock_guard lock(context.Shared->Mutex);
  GLint program = 0;
  ::glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  if (!program && context.IntendedPipeline) ::glGetProgramPipelineiv(context.IntendedPipeline, GL_FRAGMENT_SHADER, &program);
  if (!program) return;
  auto& programs = context.Shared->AlphaPrograms;
  auto it = programs.find(program);
  if (it == programs.end()) it = programs.emplace(program, VexaAlphaProgram(program)).first;
  auto& uniforms = it->second;
  if (uniforms.Function < 0) return;
  const GLint function = context.AlphaTest ? context.AlphaFunction - GL_NEVER + 1 : 0;
  if (!uniforms.Applied || uniforms.LastFunction != function)
    ::glProgramUniform1i(program, uniforms.Function, function);
  if (!uniforms.Applied || memcmp(&uniforms.LastReference, &context.AlphaReference, sizeof(GLfloat)))
    ::glProgramUniform1f(program, uniforms.Reference, context.AlphaReference);
  uniforms.LastFunction = function;
  uniforms.LastReference = context.AlphaReference;
  uniforms.Applied = true;
}
