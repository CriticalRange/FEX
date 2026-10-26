static bool VexaClipAvailable() {
  static const bool enabled=[] {const auto p=getenv("VEXA_GL_CLIP_PLANES");return p&&!strcmp(p,"1");}();return enabled;
}
static bool VexaInverse4(const GLfloat* input, GLdouble* inverse) {
  double a[4][8]{};
  for(int r=0;r<4;++r)for(int c=0;c<4;++c){a[r][c]=input[c*4+r];a[r][c+4]=r==c;}
  for(int c=0;c<4;++c) {
    int pivot=c;for(int r=c+1;r<4;++r)if(std::abs(a[r][c])>std::abs(a[pivot][c]))pivot=r;
    if(a[pivot][c]==0)return false;
    for(int j=0;j<8;++j)std::swap(a[c][j],a[pivot][j]);
    const double divisor=a[c][c];for(double& value:a[c])value/=divisor;
    for(int r=0;r<4;++r)if(r!=c){const double f=a[r][c];for(int j=0;j<8;++j)a[r][j]-=f*a[c][j];}
  }
  for(int r=0;r<4;++r)for(int c=0;c<4;++c)inverse[c*4+r]=a[r][c+4];
  return true;
}
static int VexaClipQuery(GLenum pname, GLfloat* value) {
  if(!VexaClipAvailable())return 0;
  const auto& state=VexaProgramContext().Clip;
  if(pname>=0x3000&&pname<0x3006){*value=state.Enabled[pname-0x3000];return 1;}
  if(pname==0x0D32){*value=6;return 1;}
  if(pname==0x0BA0){*value=state.Mode;return 1;}
  if(pname==0x0BA6||pname==0x0BA7){memcpy(value,pname==0x0BA6?state.Model:state.Projection,16*sizeof(GLfloat));return 16;}
  return 0;
}
static std::string VexaClipInstrument(std::string source, GLenum type) {
  if(!VexaClipAvailable()||(type!=GL_VERTEX_SHADER&&type!=GL_FRAGMENT_SHADER))return source;
  const auto code=VexaAlphaCode(source);
  if(code.find("vexa_clip_original_main")!=std::string::npos)return source;
  if(!std::regex_search(code,std::regex(R"(\bvoid\s+main\s*\(\s*(?:void\s*)?\)\s*\{)")))return source;
  std::vector<size_t> names;
  static const std::regex main_name(R"(\bmain\b)");
  for(std::sregex_iterator it(code.begin(),code.end(),main_name),end;it!=end;++it)names.push_back(it->position());
  for(auto it=names.rbegin();it!=names.rend();++it)source.replace(*it,4,"vexa_clip_original_main");
  const bool vertex=type==GL_VERTEX_SHADER;
  const bool legacy=source.find("#version 100")!=std::string::npos||source.find("#version")==std::string::npos;
  std::string declarations="\n"+std::string(legacy?"varying ":vertex?"out ":"in ")+"highp float vexa_clip_distance[6];\n";
  if(vertex) {
    const bool eye=std::regex_search(code,std::regex(R"(\bgl_ClipVertex\b)"));
    if(eye) {
      source=std::regex_replace(source,std::regex(R"(\bgl_ClipVertex\b)"),"vexa_clip_vertex");
      declarations+="highp vec4 vexa_clip_vertex;\nuniform highp vec4 vexa_clip_eye[6];\n";
    } else declarations+="uniform highp vec4 vexa_clip_projected[6];\n";
    source+="\nvoid main(){vexa_clip_original_main();\n";
    for(int i=0;i<6;++i)source+="vexa_clip_distance["+std::to_string(i)+"]=dot("+(eye?std::string("vexa_clip_eye["):std::string("vexa_clip_projected["))+std::to_string(i)+"],"+(eye?"vexa_clip_vertex":"gl_Position")+");\n";
    source+="}\n";
  } else {
    declarations+="uniform bool vexa_clip_enabled[6];\n";
    // Raster clipping precedes fragment shader side effects and depth/stencil.
    source=std::regex_replace(source,std::regex(R"(layout\s*\(\s*early_fragment_tests\s*\)\s*in\s*;)"),"");
    source+="\nvoid main(){\n";
    for(int i=0;i<6;++i)source+="if(vexa_clip_enabled["+std::to_string(i)+"]&&vexa_clip_distance["+std::to_string(i)+"]<0.0)discard;\n";
    source+="vexa_clip_original_main();}\n";
  }
  source.insert(VexaShaderPrecisionInsertPos(source),declarations);
  return source;
}
static void VexaApplyClipPlanes() {
  if(!VexaClipAvailable())return;
  auto& ctx=VexaProgramContext();std::lock_guard lock(ctx.Shared->Mutex);
  GLint programs[2]{};::glGetIntegerv(GL_CURRENT_PROGRAM,programs);
  if(!programs[0]&&ctx.IntendedPipeline) {
    ::glGetProgramPipelineiv(ctx.IntendedPipeline,GL_VERTEX_SHADER,programs);
    ::glGetProgramPipelineiv(ctx.IntendedPipeline,GL_FRAGMENT_SHADER,programs+1);
  }
  auto& state=ctx.Clip;
  const bool projectionChanged=!state.Derived || memcmp(state.SavedProjection,state.Projection,sizeof(state.Projection));
  if(projectionChanged || memcmp(state.SavedEquation,state.Equation,sizeof(state.Equation))) {
    GLdouble inverse[16]{};const bool valid=VexaInverse4(state.Projection,inverse);
    std::fill_n(state.Projected,24,0.f);
    for(int p=0;p<6;++p)for(int i=0;i<4;++i) {
      state.Eye[p*4+i]=state.Equation[p][i];
      if(valid)for(int j=0;j<4;++j)state.Projected[p*4+i]+=inverse[i*4+j]*state.Equation[p][j];
    }
    memcpy(state.SavedEquation,state.Equation,sizeof(state.Equation));
  }
  if(projectionChanged || memcmp(state.SavedModel,state.Model,sizeof(state.Model))) {
    std::fill_n(state.MVP,16,0.f);
    for(int c=0;c<4;++c)for(int r=0;r<4;++r)for(int k=0;k<4;++k)state.MVP[c*4+r]+=state.Projection[k*4+r]*state.Model[c*4+k];
    memcpy(state.SavedModel,state.Model,sizeof(state.Model));
  }
  memcpy(state.SavedProjection,state.Projection,sizeof(state.Projection));
  state.Derived=true;
  for(int n=0;n<2;++n) {
    const GLuint program=programs[n];if(!program||(n&&program==static_cast<GLuint>(programs[0])))continue;
    auto [it,added]=ctx.Shared->ClipPrograms.try_emplace(program);
    auto& loc=it->second;
    if(added) {
      loc.Eye=::glGetUniformLocation(program,"vexa_clip_eye[0]");
      loc.Projected=::glGetUniformLocation(program,"vexa_clip_projected[0]");
      loc.Enabled=::glGetUniformLocation(program,"vexa_clip_enabled[0]");
      loc.MVP=::glGetUniformLocation(program,"vexa_compat_MVP");
    }
    if(loc.Eye>=0 && (!loc.Applied || memcmp(loc.LastEye,state.Eye,sizeof(state.Eye))))
      ::glProgramUniform4fv(program,loc.Eye,6,state.Eye);
    if(loc.Projected>=0 && (!loc.Applied || memcmp(loc.LastProjected,state.Projected,sizeof(state.Projected))))
      ::glProgramUniform4fv(program,loc.Projected,6,state.Projected);
    if(loc.Enabled>=0 && (!loc.Applied || memcmp(loc.LastEnabled,state.Enabled,sizeof(state.Enabled))))
      ::glProgramUniform1iv(program,loc.Enabled,6,state.Enabled);
    if(loc.MVP>=0 && (!loc.Applied || memcmp(loc.LastMVP,state.MVP,sizeof(state.MVP))))
      ::glProgramUniformMatrix4fv(program,loc.MVP,1,GL_FALSE,state.MVP);
    memcpy(loc.LastEye,state.Eye,sizeof(state.Eye));
    memcpy(loc.LastProjected,state.Projected,sizeof(state.Projected));
    memcpy(loc.LastEnabled,state.Enabled,sizeof(state.Enabled));
    memcpy(loc.LastMVP,state.MVP,sizeof(state.MVP));
    loc.Applied=true;
  }
}
