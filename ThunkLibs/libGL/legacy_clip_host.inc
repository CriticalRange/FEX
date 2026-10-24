static void fexfn_impl_libGL_VEXA_CompatError(GLenum error){VexaAssemblyError(error);}
static void fexfn_impl_libGL_VEXA_SetClipMatrices(const GLfloat* model, const GLfloat* projection, GLenum mode) {
  if(!VexaClipAvailable()||!model||!projection)return;
  auto& state=VexaProgramContext().Clip;
  memcpy(state.Model,model,sizeof(state.Model));memcpy(state.Projection,projection,sizeof(state.Projection));state.Mode=mode;
}
static void fexfn_impl_libGL_glClipPlane(GLenum plane,const GLdouble* equation) {
  if(plane<0x3000||plane>=0x3006){VexaAssemblyError(GL_INVALID_ENUM);return;}if(!equation)return;
  auto& state=VexaProgramContext().Clip;GLdouble inverse[16]{};
  if(!VexaInverse4(state.Model,inverse)){VexaAssemblyError(GL_INVALID_OPERATION);return;}
  for(int i=0;i<4;++i){state.Equation[plane-0x3000][i]=0;for(int j=0;j<4;++j)state.Equation[plane-0x3000][i]+=inverse[i*4+j]*equation[j];}
}
static void fexfn_impl_libGL_glGetClipPlane(GLenum plane,GLdouble* equation) {
  if(plane<0x3000||plane>=0x3006){VexaAssemblyError(GL_INVALID_ENUM);return;}
  if(equation)memcpy(equation,VexaProgramContext().Clip.Equation[plane-0x3000],4*sizeof(GLdouble));
}
