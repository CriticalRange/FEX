// Program objects are shared by EGL share groups; pipelines and current
// bindings belong to a context. Select this state only on context switches,
// never with an eglGetCurrentContext query for every uniform/draw.
struct VexaProgramShareState {
  std::recursive_mutex Mutex;
  std::unordered_map<GLuint, std::vector<std::pair<GLenum, std::string>>> Sources;
  std::unordered_map<GLuint, std::pair<GLenum, std::string>> DirectSources;
  std::unordered_map<GLuint, std::vector<GLuint>> Clones;
  std::unordered_map<GLuint, std::unordered_map<GLint, GLint>> Locations;
  std::unordered_set<GLuint> Linked;
  std::unordered_set<GLuint> Deleted;
  std::unordered_map<GLuint, std::string> ShaderSources;
  std::unordered_map<GLuint, std::pair<GLenum, std::string>> CompiledSources;
  std::unordered_set<GLuint> DeletedShaders;
  std::vector<GLuint> Retired;
  uint64_t Generation = 1;
};

struct VexaPipelineClone {
  GLuint Original = 0, Clone = 0;
};

struct VexaProgramContextState {
  ~VexaProgramContextState();
  std::shared_ptr<VexaProgramShareState> Shared = std::make_shared<VexaProgramShareState>();
  std::unordered_map<GLuint, int> Pipelines;
  std::unordered_map<GLuint, VexaPipelineClone> OwnedClones;
  GLuint IntendedPipeline = 0, StageProgram = 0, CurrentProgram = 0;
  uint64_t SeenGeneration = 0;
};

static std::mutex g_vexa_program_context_mutex;
static std::unordered_map<EGLContext, std::shared_ptr<VexaProgramContextState>> g_vexa_program_contexts;
static thread_local std::shared_ptr<VexaProgramContextState> g_vexa_program_context;

static VexaProgramContextState& VexaProgramContext() {
  // Also allows standalone EGL probes/legacy bootstrap contexts to use the
  // same implementation without a guest GLX context registration.
  if (!g_vexa_program_context) g_vexa_program_context = std::make_shared<VexaProgramContextState>();
  return *g_vexa_program_context;
}

static void VexaRegisterProgramContext(EGLContext context, EGLContext shared) {
  std::lock_guard lock(g_vexa_program_context_mutex);
  auto state = std::make_shared<VexaProgramContextState>();
  if (auto it = g_vexa_program_contexts.find(shared); it != g_vexa_program_contexts.end()) state->Shared = it->second->Shared;
  g_vexa_program_contexts[context] = std::move(state);
}

static void VexaSelectProgramContext(EGLContext context) {
  if (context == EGL_NO_CONTEXT) {
    g_vexa_program_context.reset();
    return;
  }
  std::shared_ptr<VexaProgramContextState> selected;
  {
    std::lock_guard lock(g_vexa_program_context_mutex);
    auto& state = g_vexa_program_contexts[context];
    if (!state) state = std::make_shared<VexaProgramContextState>();
    selected = state;
  }
  // Dropping the previous TLS state can acquire its share-group mutex.
  // Never run that destructor while holding the registry mutex.
  g_vexa_program_context = std::move(selected);
}

static void VexaForgetClone(VexaProgramShareState& state, GLuint original, GLuint clone) {
  auto it = state.Clones.find(original);
  if (it != state.Clones.end()) {
    auto& list = it->second;
    list.erase(std::remove(list.begin(), list.end(), clone), list.end());
    if (list.empty()) state.Clones.erase(it);
  }
  state.Locations.erase(clone);
}

VexaProgramContextState::~VexaProgramContextState() {
  std::lock_guard lock(Shared->Mutex);
  for (const auto& [pipe, clone] : OwnedClones) {
    VexaForgetClone(*Shared, clone.Original, clone.Clone);
    Shared->Retired.push_back(clone.Clone);
  }
  ++Shared->Generation;
}

static void VexaUnregisterProgramContext(EGLContext context) {
  std::shared_ptr<VexaProgramContextState> retired;
  {
    std::lock_guard lock(g_vexa_program_context_mutex);
    auto it = g_vexa_program_contexts.find(context);
    if (it == g_vexa_program_contexts.end()) return;
    retired = std::move(it->second);
    g_vexa_program_contexts.erase(it);
  }
  // Another thread may still have a destroy-pending EGL context current.
  // Its TLS reference keeps the metadata alive until it releases/binds a
  // different context; only then can its clones be retired.
  if (g_vexa_program_context == retired) g_vexa_program_context.reset();
}

static void VexaCollectProgramState() {
  auto& state = *VexaProgramContext().Shared;
  for (GLuint clone : state.Retired) ::glDeleteProgram(clone);
  state.Retired.clear();
  for (auto it = state.DeletedShaders.begin(); it != state.DeletedShaders.end();) {
    if (::glIsShader(*it)) { ++it; continue; }
    state.CompiledSources.erase(*it);
    state.ShaderSources.erase(*it);
    it = state.DeletedShaders.erase(it);
  }
  for (auto it = state.Deleted.begin(); it != state.Deleted.end();) {
    // A clone stands in for the guest's pipeline reference. Some GLES
    // drivers detach deleted programs from unbound pipelines; defer the
    // native delete until the last replacement reference has gone away.
    auto clones = state.Clones.find(*it);
    if (clones != state.Clones.end() && !clones->second.empty()) { ++it; continue; }
    if (::glIsProgram(*it)) ::glDeleteProgram(*it);
    if (::glIsProgram(*it)) { ++it; continue; }
    state.Sources.erase(*it);
    state.DirectSources.erase(*it);
    state.Linked.erase(*it);
    it = state.Deleted.erase(it);
  }
}

// Caller holds Shared->Mutex. Replacing a vertex stage with a clone must
// not change the guest program's deferred-deletion lifetime.
static void VexaReleasePipelineClone(GLuint pipeline, bool restore) {
  auto& context = VexaProgramContext();
  auto it = context.OwnedClones.find(pipeline);
  if (it == context.OwnedClones.end()) return;
  const auto clone = it->second;
  if (restore) ::glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, clone.Original);
  VexaForgetClone(*context.Shared, clone.Original, clone.Clone);
  ::glDeleteProgram(clone.Clone);
  context.OwnedClones.erase(it);
}

static void VexaRefreshProgramGeneration() {
  auto& context = VexaProgramContext();
  if (context.SeenGeneration == context.Shared->Generation) return;
  while (!context.OwnedClones.empty()) VexaReleasePipelineClone(context.OwnedClones.begin()->first, true);
  context.Pipelines.clear();
  context.SeenGeneration = context.Shared->Generation;
  VexaCollectProgramState();
}

static void VexaSnapshotProgramSources(GLuint program) {
  auto& state = *VexaProgramContext().Shared;
  const bool replaces_executable = state.Sources.find(program) != state.Sources.end() ||
                                   state.DirectSources.find(program) != state.DirectSources.end();
  // Query the attached shader objects, including delete-pending shaders.
  // Never consume source data: one shader can be linked into many programs.
  GLint count = 0;
  ::glGetProgramiv(program, GL_ATTACHED_SHADERS, &count);
  if (count < 0 || count > 1024) return;
  std::vector<GLuint> attached(static_cast<size_t>(count));
  GLsizei written = 0;
  if (count) ::glGetAttachedShaders(program, count, &written, attached.data());
  std::vector<std::pair<GLenum, std::string>> sources;
  for (GLsizei i = 0; i < written; ++i) {
    if (auto compiled = state.CompiledSources.find(attached[i]); compiled != state.CompiledSources.end()) {
      sources.push_back(compiled->second);
      continue;
    }
    GLint type = 0, length = 0;
    ::glGetShaderiv(attached[i], GL_SHADER_TYPE, &type);
    ::glGetShaderiv(attached[i], GL_SHADER_SOURCE_LENGTH, &length);
    if (length <= 1) continue;
    std::string source(static_cast<size_t>(length), '\0');
    GLsizei size = 0;
    ::glGetShaderSource(attached[i], length, &size, source.data());
    source.resize(static_cast<size_t>(size));
    sources.emplace_back(static_cast<GLenum>(type), std::move(source));
  }
  state.Sources[program] = std::move(sources);
  state.DirectSources.erase(program);
  state.Linked.erase(program);
  // Streaming a completely new shader must not invalidate unrelated live
  // pipeline clones. Only replacing an existing executable needs a refresh.
  if (replaces_executable) ++state.Generation;
}

static void VexaSnapshotCompiledShader(GLuint shader) {
  auto& state = *VexaProgramContext().Shared;
  GLint type = 0, length = 0;
  ::glGetShaderiv(shader, GL_SHADER_TYPE, &type);
  ::glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &length);
  if (length <= 1) return;
  std::string source(static_cast<size_t>(length), '\0');
  GLsizei written = 0;
  ::glGetShaderSource(shader, length, &written, source.data());
  source.resize(static_cast<size_t>(written));
  state.CompiledSources[shader] = {static_cast<GLenum>(type), std::move(source)};
}

static void VexaCopyUniform(GLuint original, GLuint clone, GLint from, GLint to, GLenum type) {
  if (from < 0 || to < 0) return;
  GLfloat f[16] = {};
  GLint i[4] = {};
  GLuint u[4] = {};
  switch (type) {
#define VEXA_COPY_FLOAT(Type, Count) case Type: ::glGetUniformfv(original, from, f); ::glProgramUniform##Count##fv(clone, to, 1, f); break
    VEXA_COPY_FLOAT(GL_FLOAT, 1);
    VEXA_COPY_FLOAT(GL_FLOAT_VEC2, 2);
    VEXA_COPY_FLOAT(GL_FLOAT_VEC3, 3);
    VEXA_COPY_FLOAT(GL_FLOAT_VEC4, 4);
#undef VEXA_COPY_FLOAT
#define VEXA_COPY_MATRIX(Type, Shape) case Type: ::glGetUniformfv(original, from, f); ::glProgramUniformMatrix##Shape##fv(clone, to, 1, GL_FALSE, f); break
    VEXA_COPY_MATRIX(GL_FLOAT_MAT2, 2);
    VEXA_COPY_MATRIX(GL_FLOAT_MAT3, 3);
    VEXA_COPY_MATRIX(GL_FLOAT_MAT4, 4);
    VEXA_COPY_MATRIX(GL_FLOAT_MAT2x3, 2x3);
    VEXA_COPY_MATRIX(GL_FLOAT_MAT2x4, 2x4);
    VEXA_COPY_MATRIX(GL_FLOAT_MAT3x2, 3x2);
    VEXA_COPY_MATRIX(GL_FLOAT_MAT3x4, 3x4);
    VEXA_COPY_MATRIX(GL_FLOAT_MAT4x2, 4x2);
    VEXA_COPY_MATRIX(GL_FLOAT_MAT4x3, 4x3);
#undef VEXA_COPY_MATRIX
#define VEXA_COPY_INT(Type, Count) case Type: ::glGetUniformiv(original, from, i); ::glProgramUniform##Count##iv(clone, to, 1, i); break
    VEXA_COPY_INT(GL_INT_VEC2, 2);
    VEXA_COPY_INT(GL_INT_VEC3, 3);
    VEXA_COPY_INT(GL_INT_VEC4, 4);
    VEXA_COPY_INT(GL_BOOL_VEC2, 2);
    VEXA_COPY_INT(GL_BOOL_VEC3, 3);
    VEXA_COPY_INT(GL_BOOL_VEC4, 4);
#undef VEXA_COPY_INT
#define VEXA_COPY_UINT(Type, Count) case Type: ::glGetUniformuiv(original, from, u); ::glProgramUniform##Count##uiv(clone, to, 1, u); break
    VEXA_COPY_UINT(GL_UNSIGNED_INT, 1);
    VEXA_COPY_UINT(GL_UNSIGNED_INT_VEC2, 2);
    VEXA_COPY_UINT(GL_UNSIGNED_INT_VEC3, 3);
    VEXA_COPY_UINT(GL_UNSIGNED_INT_VEC4, 4);
#undef VEXA_COPY_UINT
    // Scalar ints, bools, sampler and image uniforms use integer values.
    default: ::glGetUniformiv(original, from, i); ::glProgramUniform1iv(clone, to, 1, i); break;
  }
}

#define VEXA_PROGRAM_LOCK() std::lock_guard vexa_program_lock(VexaProgramContext().Shared->Mutex)
#define g_vexa_prog_sources (VexaProgramContext().Shared->Sources)
#define g_vexa_prog_direct_source (VexaProgramContext().Shared->DirectSources)
#define g_vexa_clones_of_prog (VexaProgramContext().Shared->Clones)
#define g_vexa_clone_locs (VexaProgramContext().Shared->Locations)
#define g_vexa_linked_programs (VexaProgramContext().Shared->Linked)
#define g_vexa_pipe_state (VexaProgramContext().Pipelines)
#define g_vexa_intended_pipe (VexaProgramContext().IntendedPipeline)
#define g_vexa_stage_program (VexaProgramContext().StageProgram)
#define g_vexa_diag_curprog (VexaProgramContext().CurrentProgram)
#define VexaShaderSourceMutex (VexaProgramContext().Shared->Mutex)
#define VexaShaderSourceById (VexaProgramContext().Shared->ShaderSources)
