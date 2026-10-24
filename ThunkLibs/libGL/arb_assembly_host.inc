static int VexaAssemblyCheckedTarget(GLenum target) {
  int stage=VexaAssemblyTarget(target);
  if(stage<0)VexaAssemblyError(GL_INVALID_ENUM);
  return stage;
}
static void fexfn_impl_libGL_glGenProgramsARB(GLsizei count, GLuint* programs) {
  if(count<0){VexaAssemblyError(GL_INVALID_VALUE);return;}
  if(!programs)return;
  VEXA_PROGRAM_LOCK();auto& shared=*VexaProgramContext().Shared;
  for(int i=0;i<count;++i) {
    while(!shared.NextAssemblyName||shared.AssemblyNames.contains(shared.NextAssemblyName))++shared.NextAssemblyName;
    programs[i]=shared.NextAssemblyName++;
    shared.AssemblyNames.emplace(programs[i],VexaAssemblyObject{});
  }
}
static void fexfn_impl_libGL_glBindProgramARB(GLenum target, GLuint program) {
  const int stage=VexaAssemblyCheckedTarget(target);if(stage<0)return;
  VEXA_PROGRAM_LOCK();auto& context=VexaProgramContext();
  if(program) {
    auto& object=context.Shared->AssemblyNames[program];
    if(object.Target && object.Target!=target){VexaAssemblyError(GL_INVALID_OPERATION);return;}
    object.Target=target;
  }
  context.Assembly.Binding[stage]=program;
  static std::atomic<unsigned> traces{};
  if(traces.fetch_add(1)<16)fprintf(stderr,"[VEXA][ARBASM] bind target=0x%x name=%u\n",target,program);
}
static GLboolean fexfn_impl_libGL_glIsProgramARB(GLuint program) {
  VEXA_PROGRAM_LOCK();auto& names=VexaProgramContext().Shared->AssemblyNames;
  auto it=names.find(program);return program&&it!=names.end()&&it->second.Target?GL_TRUE:GL_FALSE;
}
static void fexfn_impl_libGL_glDeleteProgramsARB(GLsizei count, const GLuint* programs) {
  if(count<0){VexaAssemblyError(GL_INVALID_VALUE);return;}if(!programs)return;
  VEXA_PROGRAM_LOCK();auto& context=VexaProgramContext();
  std::lock_guard contexts(g_vexa_program_context_mutex);
  for(int i=0;i<count;++i) {
    auto it=context.Shared->AssemblyNames.find(programs[i]);
    if(!programs[i]||it==context.Shared->AssemblyNames.end())continue;
    if(it->second.Shader)::glDeleteShader(it->second.Shader);
    for(auto& [egl,other]:g_vexa_program_contexts)if(other->Shared==context.Shared)
      for(auto& binding:other->Assembly.Binding)if(binding==programs[i])binding=0;
    for(auto& binding:context.Assembly.Binding)if(binding==programs[i])binding=0;
    context.Shared->AssemblyNames.erase(it);
  }
}
static void fexfn_impl_libGL_glProgramStringARB(GLenum target, GLenum format, GLsizei length, const void* text) {
  const int stage=VexaAssemblyCheckedTarget(target);if(stage<0)return;
  if(format!=0x8875){VexaAssemblyError(GL_INVALID_ENUM);return;}
  if(length<0){VexaAssemblyError(GL_INVALID_VALUE);return;}
  if(!text&&length){VexaAssemblyError(GL_INVALID_VALUE);return;}
  VEXA_PROGRAM_LOCK();auto& context=VexaProgramContext();auto& object=VexaAssemblyBound(stage);
  const std::string source(length?static_cast<const char*>(text):"",length);
  auto result=VexaAssemblyTranslator(stage==0).Translate(source);
  std::string log=result.Error;
  GLuint shader=result.GLSL.empty()?0:VexaAssemblyCompile(stage,result.GLSL,log);
  if(object.Shader)::glDeleteShader(object.Shader);
  object.Shader=shader;object.Source=source;object.Revision=++context.Shared->AssemblyRevision;
  object.Instructions=result.Instructions;object.Temporaries=result.Temporaries;object.TextureInstructions=result.TextureInstructions;
  context.Assembly.ErrorPosition=shader?-1:result.Position>=0?result.Position:length;
  context.Assembly.ErrorString=std::move(log);
  static std::atomic<unsigned> traces{};
  if(!shader||traces.fetch_add(1)<32)fprintf(stderr,"[VEXA][ARBASM] upload target=0x%x name=%u bytes=%d compiled=%u error=%s\n",target,context.Assembly.Binding[stage],length,shader,context.Assembly.ErrorString.c_str());
  if(!shader)VexaAssemblyError(GL_INVALID_OPERATION);
}
static void fexfn_impl_libGL_glGetProgramivARB(GLenum target, GLenum pname, GLint* value) {
  const int stage=VexaAssemblyCheckedTarget(target);if(stage<0||!value)return;
  VEXA_PROGRAM_LOCK();auto& object=VexaAssemblyBound(stage);GLint result=0;
  switch(pname) {
    case 0x8677:result=VexaProgramContext().Assembly.Binding[stage];break; // BINDING
    case 0x8627:result=object.Source.size();break; // LENGTH
    case 0x8876:result=0x8875;break; // FORMAT ASCII
    case 0x88B6:result=object.Shader?1:0;break; // UNDER_NATIVE_LIMITS
    case 0x88A0:case 0x88A2:result=object.Instructions;break;
    case 0x88A4:case 0x88A6:result=object.Temporaries;break;
    case 0x8805:case 0x8808:result=object.Instructions-object.TextureInstructions;break;
    case 0x8806:case 0x8809:case 0x8807:case 0x880A:result=object.TextureInstructions;break;
    case 0x88A1:case 0x88A3:case 0x880B:case 0x880C:case 0x880D:case 0x880E:case 0x880F:case 0x8810:result=1024;break;
    case 0x88A5:case 0x88A7:result=32;break;
    case 0x88B4:case 0x88B5:case 0x88A9:case 0x88AB:result=128;break;
    case 0x88AD:case 0x88AF:result=stage==0?16:11;break;
    case 0x88B1:case 0x88B3:result=stage==0?1:0;break;
    default:VexaAssemblyError(GL_INVALID_ENUM);return;
  }
  *value=result;
}
static void fexfn_impl_libGL_glGetProgramStringARB(GLenum target, GLenum pname, void* text) {
  const int stage=VexaAssemblyCheckedTarget(target);if(stage<0)return;
  if(pname!=0x8628){VexaAssemblyError(GL_INVALID_ENUM);return;}
  VEXA_PROGRAM_LOCK();const auto& source=VexaAssemblyBound(stage).Source;
  if(text&&!source.empty())memcpy(text,source.data(),source.size());
}

template<class T>static void VexaAssemblySetParameter(GLenum target, GLuint index, const T* value, bool env) {
  const int stage=VexaAssemblyCheckedTarget(target);if(stage<0)return;
  if(index>=128){VexaAssemblyError(GL_INVALID_VALUE);return;}if(!value)return;
  VEXA_PROGRAM_LOCK();auto& destination=env?VexaProgramContext().Assembly.Env[stage][index]:VexaAssemblyBound(stage).Local[index];
  for(int i=0;i<4;++i)destination[i]=static_cast<GLfloat>(value[i]);
}
template<class T>static void VexaAssemblyGetParameter(GLenum target, GLuint index, T* value, bool env) {
  const int stage=VexaAssemblyCheckedTarget(target);if(stage<0)return;
  if(index>=128){VexaAssemblyError(GL_INVALID_VALUE);return;}if(!value)return;
  VEXA_PROGRAM_LOCK();const auto& source=env?VexaProgramContext().Assembly.Env[stage][index]:VexaAssemblyBound(stage).Local[index];
  for(int i=0;i<4;++i)value[i]=static_cast<T>(source[i]);
}
#define VEXA_ASM_PARAMS(scope,kind,type,env) \
  static void fexfn_impl_libGL_glProgram##scope##Parameter4##kind##ARB(GLenum target, GLuint index, type x,type y,type z,type w) {const type v[]{x,y,z,w};VexaAssemblySetParameter(target,index,v,env);} \
  static void fexfn_impl_libGL_glProgram##scope##Parameter4##kind##vARB(GLenum target,GLuint index,const type* value) {VexaAssemblySetParameter(target,index,value,env);} \
  static void fexfn_impl_libGL_glGetProgram##scope##Parameter##kind##vARB(GLenum target,GLuint index,type* value) {VexaAssemblyGetParameter(target,index,value,env);}
VEXA_ASM_PARAMS(Local,f,GLfloat,false)
VEXA_ASM_PARAMS(Local,d,GLdouble,false)
VEXA_ASM_PARAMS(Env,f,GLfloat,true)
VEXA_ASM_PARAMS(Env,d,GLdouble,true)
#undef VEXA_ASM_PARAMS

static void fexfn_impl_libGL_glProgramEnvParameters4fvEXT(GLenum target, GLuint index, GLsizei count, const GLfloat* values) {
  const int stage=VexaAssemblyCheckedTarget(target);if(stage<0)return;
  if(count<0 || index>128 || static_cast<GLuint>(count)>128-index){VexaAssemblyError(GL_INVALID_VALUE);return;}
  if(!values)return;VEXA_PROGRAM_LOCK();auto& params=VexaProgramContext().Assembly.Env[stage];
  for(int i=0;i<count;++i)for(int j=0;j<4;++j)params[index+i][j]=values[i*4+j];
}
