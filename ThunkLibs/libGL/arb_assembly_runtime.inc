#include "arb_assembly_translate.h"
static void VexaAssemblyError(GLenum error) {
  auto& pending=VexaProgramContext().CompatError;
  if(!pending)pending=error;
}
static int VexaAssemblyTarget(GLenum target) {
  return target==0x8620?0:target==0x8804?1:-1;
}
static bool VexaAssemblyQuery(GLenum pname, GLfloat* value) {
  auto& state=VexaProgramContext().Assembly;
  const int target=VexaAssemblyTarget(pname);
  if(target>=0) {*value=state.Enabled[target];return true;}
  if(pname==0x864B) {*value=state.ErrorPosition;return true;}
  return false;
}
static VexaAssemblyObject& VexaAssemblyBound(int stage) {
  auto& ctx=VexaProgramContext();
  if(auto name=ctx.Assembly.Binding[stage])return ctx.Shared->AssemblyNames.at(name);
  auto& result=ctx.Assembly.Default[stage];result.Target=stage?0x8804:0x8620;return result;
}
static GLuint VexaAssemblyCompile(int stage, const std::string& source, std::string& log) {
  std::string text=stage && VexaAlphaTestAvailable()?VexaAlphaInstrument(source):source;
  text=VexaClipInstrument(std::move(text),stage?GL_FRAGMENT_SHADER:GL_VERTEX_SHADER);
  const GLuint shader=::glCreateShader(stage?GL_FRAGMENT_SHADER:GL_VERTEX_SHADER);
  const char* ptr=text.c_str();::glShaderSource(shader,1,&ptr,nullptr);::glCompileShader(shader);
  GLint good=0;::glGetShaderiv(shader,GL_COMPILE_STATUS,&good);
  if(!good) {
    GLint size=0;::glGetShaderiv(shader,GL_INFO_LOG_LENGTH,&size);log.resize(std::max(size,1));
    ::glGetShaderInfoLog(shader,log.size(),nullptr,log.data());::glDeleteShader(shader);return 0;
  }
  return shader;
}

// Temporary native program binding lasts exactly one guest draw. In particular,
// GetHandleARB/CURRENT_PROGRAM and later GLSL uniforms must still see program 0.
class VexaAssemblyDrawScope {
  std::unique_lock<std::recursive_mutex> Lock;
  bool Bound = false;
public:
  bool Ready = true;
  VexaAssemblyDrawScope() {
    auto& context=VexaProgramContext();auto& state=context.Assembly;
    if(!state.Enabled[0]&&!state.Enabled[1])return;
    GLint current=0;::glGetIntegerv(GL_CURRENT_PROGRAM,&current);
    if(current || context.IntendedPipeline)return; // GLSL takes precedence.
    Lock=std::unique_lock(context.Shared->Mutex);
    if(!state.Enabled[0]||!state.Enabled[1]) {VexaAssemblyError(GL_INVALID_OPERATION);Ready=false;return;}
    auto& vertex=VexaAssemblyBound(0);auto& fragment=VexaAssemblyBound(1);
    if(!vertex.Shader||!fragment.Shader) {VexaAssemblyError(GL_INVALID_OPERATION);Ready=false;return;}
    const std::string key=std::to_string(vertex.Revision)+"/"+std::to_string(fragment.Revision);
    auto found=state.Linked.find(key);
    if(found==state.Linked.end()) {
      // Limit retained native pairs; cached guest objects and source remain.
      if(state.Linked.size()>=32) {
        for(auto& [unused,old]:state.Linked) {context.Shared->AlphaPrograms.erase(old.Program);context.Shared->ClipPrograms.erase(old.Program);::glDeleteProgram(old.Program);}
        state.Linked.clear();
      }
      VexaAssemblyLinked linked;
      linked.Program=::glCreateProgram();context.Shared->AlphaPrograms.erase(linked.Program);context.Shared->ClipPrograms.erase(linked.Program);
      ::glAttachShader(linked.Program,vertex.Shader);::glAttachShader(linked.Program,fragment.Shader);::glLinkProgram(linked.Program);
      GLint good=0;::glGetProgramiv(linked.Program,GL_LINK_STATUS,&good);
      if(!good) {
        char log[2048]{};::glGetProgramInfoLog(linked.Program,sizeof(log),nullptr,log);
        fprintf(stderr,"[VEXA][ARBASM] pair link failed: %s\n",log);::glDeleteProgram(linked.Program);
        VexaAssemblyError(GL_INVALID_OPERATION);Ready=false;return;
      }
      GLint count=0;::glGetProgramiv(linked.Program,GL_ACTIVE_UNIFORMS,&count);
      for(int i=0;i<count;++i) {
        char name[128]{};GLsizei length=0;GLint size=0;GLenum type=0;
        ::glGetActiveUniform(linked.Program,i,sizeof(name),&length,&size,&type,name);
        const GLint location=::glGetUniformLocation(linked.Program,name);
        if(!strncmp(name,"uVLocal[",8))linked.Uniforms.push_back({location,size,0,0});
        else if(!strncmp(name,"uFLocal[",8))linked.Uniforms.push_back({location,size,1,0});
        else if(!strncmp(name,"uVEnv[",6))linked.Uniforms.push_back({location,size,0,1});
        else if(!strncmp(name,"uFEnv[",6))linked.Uniforms.push_back({location,size,1,1});
        else if(!strncmp(name,"vexa_asm_tex",12))linked.Uniforms.push_back({location,1,atoi(name+12),2});
      }
      found=state.Linked.emplace(key,std::move(linked)).first;
    }
    auto& linked=found->second;::glUseProgram(linked.Program);Bound=true;
    for(auto& uniform:linked.Uniforms) {
      if(uniform.Kind==2) {
        if(!uniform.Applied)::glUniform1i(uniform.Location,uniform.Stage);
      }
      else {
        const auto& data=uniform.Kind==1?state.Env[uniform.Stage]:(uniform.Stage?fragment.Local:vertex.Local);
        const auto count=std::min(uniform.Count,128);
        const size_t bytes=static_cast<size_t>(count)*4*sizeof(GLfloat);
        if(!uniform.Applied || uniform.Values.size()!=static_cast<size_t>(count)*4 || memcmp(uniform.Values.data(),data[0].data(),bytes)) {
          ::glUniform4fv(uniform.Location,count,data[0].data());
          uniform.Values.resize(static_cast<size_t>(count)*4);
          memcpy(uniform.Values.data(),data.data(),bytes);
        }
      }
      uniform.Applied=true;
    }
  }
  ~VexaAssemblyDrawScope() {if(Bound)::glUseProgram(0);}
};
