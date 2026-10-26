static void VexaLegacyPointer(GLuint index, GLint size, GLenum type, GLsizei stride,const void* pointer,bool normalized) {
  if(size<1||size>4){VexaAssemblyError(GL_INVALID_VALUE);return;}
  if(stride<0){VexaAssemblyError(GL_INVALID_VALUE);return;}
  if(!VexaLegacyTypeSize(type)){VexaAssemblyError(GL_INVALID_ENUM);return;}
  auto& client=VexaProgramContext().LegacyClient;client.Used=true;
  auto& state=VexaLegacyArrays()[index];GLint buffer=0;::glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&buffer);
  state.Defined=true;state.Size=size;state.Type=type;state.Stride=stride;state.Pointer=reinterpret_cast<uintptr_t>(pointer);state.Buffer=buffer;state.Normalize=normalized;state.BGRA=false;
  if(buffer&&type!=GL_DOUBLE)::glVertexAttribPointer(index,size,type,normalized,stride,pointer);
}
static void fexfn_impl_libGL_glVertexAttribPointer(GLuint index,GLint size,GLenum type,GLboolean normalized,GLsizei stride,const void* pointer) {
  if(size!=0x80E1) {
    ::glVertexAttribPointer(index,size,type,normalized,stride,pointer);
    // A conventional setter replaces a previous compatibility array.
    if(index<16&&size>=1&&size<=4&&stride>=0&&VexaLegacyTypeSize(type)&&type!=GL_DOUBLE&&
       ((type!=GL_INT_2_10_10_10_REV&&type!=GL_UNSIGNED_INT_2_10_10_10_REV)||size==4))VexaLegacyArrays()[index].Defined=false;
    return;
  }
  if(index>=16||stride<0){VexaAssemblyError(GL_INVALID_VALUE);return;}
  if(!normalized||(type!=GL_UNSIGNED_BYTE&&type!=GL_INT_2_10_10_10_REV&&type!=GL_UNSIGNED_INT_2_10_10_10_REV)){VexaAssemblyError(GL_INVALID_OPERATION);return;}
  GLint enabled=0;::glGetVertexAttribiv(index,GL_VERTEX_ATTRIB_ARRAY_ENABLED,&enabled);
  VexaLegacyPointer(index,4,type,stride,pointer,true);
  VexaProgramContext().Shared->BGRATouched.store(true, std::memory_order_relaxed);
  auto& a=VexaLegacyArrays()[index];a.BGRA=true;a.Enabled=enabled!=0;
}
static void fexfn_impl_libGL_glVertexAttribPointerARB(GLuint index,GLint size,GLenum type,GLboolean normalized,GLsizei stride,const void* pointer){fexfn_impl_libGL_glVertexAttribPointer(index,size,type,normalized,stride,pointer);}
static void fexfn_impl_libGL_glEnableVertexAttribArray(GLuint index){::glEnableVertexAttribArray(index);if(index<16)VexaLegacyArrays()[index].Enabled=true;}
static void fexfn_impl_libGL_glDisableVertexAttribArray(GLuint index){::glDisableVertexAttribArray(index);if(index<16)VexaLegacyArrays()[index].Enabled=false;}
static bool VexaBGRAQuery(GLuint index,GLenum pname){return index<16&&pname==GL_VERTEX_ATTRIB_ARRAY_SIZE&&VexaLegacyArrays()[index].Defined&&VexaLegacyArrays()[index].BGRA;}
static void fexfn_impl_libGL_glGetVertexAttribiv(GLuint index,GLenum pname,GLint* value){if(value&&VexaBGRAQuery(index,pname)){*value=0x80E1;return;}::glGetVertexAttribiv(index,pname,value);}
static void fexfn_impl_libGL_glGetVertexAttribfv(GLuint index,GLenum pname,GLfloat* value){if(value&&VexaBGRAQuery(index,pname)){*value=0x80E1;return;}::glGetVertexAttribfv(index,pname,value);}
static void fexfn_impl_libGL_glDeleteVertexArrays(GLsizei count,const GLuint* arrays){if(count>=0&&arrays)for(int i=0;i<count;++i)VexaProgramContext().LegacyClient.Arrays.erase(arrays[i]);::glDeleteVertexArrays(count,arrays);}
static void fexfn_impl_libGL_glVertexPointer(GLint size,GLenum type,GLsizei stride,const void* pointer) {
  if(size<2||size>4){VexaAssemblyError(GL_INVALID_VALUE);return;}
  if(type!=GL_SHORT&&type!=GL_INT&&type!=GL_FLOAT&&type!=GL_DOUBLE){VexaAssemblyError(GL_INVALID_ENUM);return;}
  VexaLegacyPointer(0,size,type,stride,pointer,false);
}
static void fexfn_impl_libGL_glTexCoordPointer(GLint size,GLenum type,GLsizei stride,const void* pointer) {
  if(type!=GL_SHORT&&type!=GL_INT&&type!=GL_FLOAT&&type!=GL_DOUBLE){VexaAssemblyError(GL_INVALID_ENUM);return;}
  VexaLegacyPointer(8+VexaProgramContext().LegacyClient.Unit,size,type,stride,pointer,false);
}
static void fexfn_impl_libGL_glColorPointer(GLint size,GLenum type,GLsizei stride,const void* pointer) {
  if(size!=3&&size!=4){VexaAssemblyError(GL_INVALID_VALUE);return;}
  VexaLegacyPointer(3,size,type,stride,pointer,type!=GL_FLOAT&&type!=GL_DOUBLE);
}
static void fexfn_impl_libGL_glClientActiveTexture(GLenum texture) {
  if(texture<GL_TEXTURE0||texture>=GL_TEXTURE0+8){VexaAssemblyError(GL_INVALID_ENUM);return;}
  VexaProgramContext().LegacyClient.Unit=texture-GL_TEXTURE0;
}
static void fexfn_impl_libGL_glEnableClientState(GLenum array) {
  const int index=VexaLegacyArrayIndex(array);if(index<0){VexaAssemblyError(GL_INVALID_ENUM);return;}
  VexaProgramContext().LegacyClient.Used=true;VexaLegacyArrays()[index].Enabled=true;::glEnableVertexAttribArray(index);
}
static void fexfn_impl_libGL_glDisableClientState(GLenum array) {
  const int index=VexaLegacyArrayIndex(array);if(index<0){VexaAssemblyError(GL_INVALID_ENUM);return;}
  VexaProgramContext().LegacyClient.Used=true;VexaLegacyArrays()[index].Enabled=false;::glDisableVertexAttribArray(index);
}
static void fexfn_impl_libGL_glColor4f(GLfloat r,GLfloat g,GLfloat b,GLfloat a){::glVertexAttrib4f(3,r,g,b,a);}
static void fexfn_impl_libGL_glColor4sv(const GLshort* v) {
  if(v)fexfn_impl_libGL_glColor4f(std::max(v[0]/32767.0f,-1.f),std::max(v[1]/32767.0f,-1.f),std::max(v[2]/32767.0f,-1.f),std::max(v[3]/32767.0f,-1.f));
}
static void fexfn_impl_libGL_glTexCoord2f(GLfloat s,GLfloat t){::glVertexAttrib4f(8,s,t,0,1);}
static uintptr_t fexfn_impl_libGL_VEXA_GetLegacyPointer(GLenum pname) {
  const int index=pname==0x808E?0:pname==0x808F?2:pname==0x8090?3:pname==0x8092?8+VexaProgramContext().LegacyClient.Unit:-1;
  if(index<0){VexaAssemblyError(GL_INVALID_ENUM);return 0;}return VexaLegacyArrays()[index].Pointer;
}

using VexaLegacyRestore=std::vector<std::function<void()>>;
static void VexaSaveCap(VexaLegacyRestore& out,GLenum cap) {
  const bool enabled=::glIsEnabled(cap);out.push_back([=]{if(enabled)::glEnable(cap);else ::glDisable(cap);});
}
static VexaLegacyRestore VexaCaptureAttributes(GLbitfield mask) {
  VexaLegacyRestore out;auto& context=VexaProgramContext();
  const bool all=mask==0xffffffff;
  auto has=[=](GLbitfield bit){return all||(mask&bit);};
  if(has(0x2000)) {
    for(GLenum cap:{GL_BLEND,GL_CULL_FACE,GL_DEPTH_TEST,GL_STENCIL_TEST,GL_SCISSOR_TEST,GL_DITHER,GL_POLYGON_OFFSET_FILL,GL_SAMPLE_ALPHA_TO_COVERAGE,GL_SAMPLE_COVERAGE,GL_RASTERIZER_DISCARD,GL_PRIMITIVE_RESTART_FIXED_INDEX})VexaSaveCap(out,cap);
    const bool alpha=context.AlphaTest;std::array<GLint,6> clip;std::copy_n(context.Clip.Enabled,6,clip.begin());std::array<bool,2> assembly;std::copy_n(context.Assembly.Enabled,2,assembly.begin());
    out.push_back([=]{auto& state=VexaProgramContext();state.AlphaTest=alpha;std::copy(clip.begin(),clip.end(),state.Clip.Enabled);std::copy(assembly.begin(),assembly.end(),state.Assembly.Enabled);});
  }
  if(has(0x100)) {
    GLint func=0;GLboolean write=0;GLfloat clear=0;::glGetIntegerv(GL_DEPTH_FUNC,&func);::glGetBooleanv(GL_DEPTH_WRITEMASK,&write);::glGetFloatv(GL_DEPTH_CLEAR_VALUE,&clear);VexaSaveCap(out,GL_DEPTH_TEST);
    out.push_back([=]{::glDepthFunc(func);::glDepthMask(write);::glClearDepthf(clear);});
  }
  if(has(0x4000)) {
    GLint srcRGB=0,dstRGB=0,srcA=0,dstA=0,eqRGB=0,eqA=0;GLfloat color[4],clear[4];GLboolean write[4];
    ::glGetIntegerv(GL_BLEND_SRC_RGB,&srcRGB);::glGetIntegerv(GL_BLEND_DST_RGB,&dstRGB);::glGetIntegerv(GL_BLEND_SRC_ALPHA,&srcA);::glGetIntegerv(GL_BLEND_DST_ALPHA,&dstA);::glGetIntegerv(GL_BLEND_EQUATION_RGB,&eqRGB);::glGetIntegerv(GL_BLEND_EQUATION_ALPHA,&eqA);
    ::glGetFloatv(GL_BLEND_COLOR,color);::glGetFloatv(GL_COLOR_CLEAR_VALUE,clear);::glGetBooleanv(GL_COLOR_WRITEMASK,write);VexaSaveCap(out,GL_BLEND);VexaSaveCap(out,GL_DITHER);
    out.push_back([=]{::glBlendFuncSeparate(srcRGB,dstRGB,srcA,dstA);::glBlendEquationSeparate(eqRGB,eqA);::glBlendColor(color[0],color[1],color[2],color[3]);::glClearColor(clear[0],clear[1],clear[2],clear[3]);::glColorMask(write[0],write[1],write[2],write[3]);});
    const bool enabled=context.AlphaTest;const GLenum function=context.AlphaFunction;const GLfloat reference=context.AlphaReference;
    out.push_back([=]{auto& state=VexaProgramContext();state.AlphaTest=enabled;state.AlphaFunction=function;state.AlphaReference=reference;});
  }
  if(has(0x400)) {
    for(bool back:{false,true}) {
      GLint func,ref,value,write,fail,zfail,zpass;
      ::glGetIntegerv(back?GL_STENCIL_BACK_FUNC:GL_STENCIL_FUNC,&func);::glGetIntegerv(back?GL_STENCIL_BACK_REF:GL_STENCIL_REF,&ref);::glGetIntegerv(back?GL_STENCIL_BACK_VALUE_MASK:GL_STENCIL_VALUE_MASK,&value);::glGetIntegerv(back?GL_STENCIL_BACK_WRITEMASK:GL_STENCIL_WRITEMASK,&write);
      ::glGetIntegerv(back?GL_STENCIL_BACK_FAIL:GL_STENCIL_FAIL,&fail);::glGetIntegerv(back?GL_STENCIL_BACK_PASS_DEPTH_FAIL:GL_STENCIL_PASS_DEPTH_FAIL,&zfail);::glGetIntegerv(back?GL_STENCIL_BACK_PASS_DEPTH_PASS:GL_STENCIL_PASS_DEPTH_PASS,&zpass);
      out.push_back([=]{const GLenum face=back?GL_BACK:GL_FRONT;::glStencilFuncSeparate(face,func,ref,value);::glStencilMaskSeparate(face,write);::glStencilOpSeparate(face,fail,zfail,zpass);});
    }
    GLint clear=0;::glGetIntegerv(GL_STENCIL_CLEAR_VALUE,&clear);out.push_back([=]{::glClearStencil(clear);});VexaSaveCap(out,GL_STENCIL_TEST);
  }
  if(has(0x800)) {
    GLint viewport[4];GLfloat depth[2];::glGetIntegerv(GL_VIEWPORT,viewport);::glGetFloatv(GL_DEPTH_RANGE,depth);
    out.push_back([=]{::glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);::glDepthRangef(depth[0],depth[1]);});
  }
  if(has(0x80000)) {GLint box[4];::glGetIntegerv(GL_SCISSOR_BOX,box);out.push_back([=]{::glScissor(box[0],box[1],box[2],box[3]);});VexaSaveCap(out,GL_SCISSOR_TEST);}
  if(has(8)) {
    GLint face,winding;GLfloat factor,units;::glGetIntegerv(GL_CULL_FACE_MODE,&face);::glGetIntegerv(GL_FRONT_FACE,&winding);::glGetFloatv(GL_POLYGON_OFFSET_FACTOR,&factor);::glGetFloatv(GL_POLYGON_OFFSET_UNITS,&units);
    out.push_back([=]{::glCullFace(face);::glFrontFace(winding);::glPolygonOffset(factor,units);});VexaSaveCap(out,GL_CULL_FACE);VexaSaveCap(out,GL_POLYGON_OFFSET_FILL);
  }
  if(has(4)){GLfloat width;::glGetFloatv(GL_LINE_WIDTH,&width);out.push_back([=]{::glLineWidth(width);});}
  if(has(0x1000)) {
    const auto saved=context.Clip;out.push_back([=]{auto& clip=VexaProgramContext().Clip;memcpy(clip.Equation,saved.Equation,sizeof(clip.Equation));memcpy(clip.Enabled,saved.Enabled,sizeof(clip.Enabled));clip.Mode=saved.Mode;});
  }
  if(has(1)) {
    for(GLuint index:{2u,3u,4u,8u,9u,10u,11u,12u,13u,14u,15u}) {GLfloat current[4];::glGetVertexAttribfv(index,GL_CURRENT_VERTEX_ATTRIB,current);out.push_back([=]{::glVertexAttrib4fv(index,current);});}
  }
  if(has(0x20000000)){GLfloat value;GLboolean invert;::glGetFloatv(GL_SAMPLE_COVERAGE_VALUE,&value);::glGetBooleanv(GL_SAMPLE_COVERAGE_INVERT,&invert);out.push_back([=]{::glSampleCoverage(value,invert);});VexaSaveCap(out,GL_SAMPLE_ALPHA_TO_COVERAGE);VexaSaveCap(out,GL_SAMPLE_COVERAGE);}
  if(has(0x40000)) {
    GLint active=0,maximum=0;::glGetIntegerv(GL_ACTIVE_TEXTURE,&active);::glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS,&maximum);
    for(GLint unit=0;unit<maximum;++unit) {
      ::glActiveTexture(GL_TEXTURE0+unit);GLint sampler;::glGetIntegerv(GL_SAMPLER_BINDING,&sampler);
      out.push_back([=]{::glActiveTexture(GL_TEXTURE0+unit);::glBindSampler(unit,sampler);});
      for(const auto& pair:{std::pair{GL_TEXTURE_2D,GL_TEXTURE_BINDING_2D},std::pair{GL_TEXTURE_CUBE_MAP,GL_TEXTURE_BINDING_CUBE_MAP},std::pair{GL_TEXTURE_3D,GL_TEXTURE_BINDING_3D},std::pair{GL_TEXTURE_2D_ARRAY,GL_TEXTURE_BINDING_2D_ARRAY}}) {
        GLint binding;::glGetIntegerv(pair.second,&binding);const GLenum target=pair.first;
        out.push_back([=]{::glActiveTexture(GL_TEXTURE0+unit);::glBindTexture(target,binding);});
      }
    }
    ::glActiveTexture(active);out.push_back([=]{::glActiveTexture(active);});
  }
  return out;
}
static void fexfn_impl_libGL_glPushAttrib(GLbitfield mask) {
  if(mask!=0xffffffff&&(mask&~0x200fffffU)){VexaAssemblyError(GL_INVALID_VALUE);return;}
  auto& stack=VexaProgramContext().LegacyClient.AttributeStack;if(stack.size()>=16){VexaAssemblyError(0x0503);return;}stack.push_back(VexaCaptureAttributes(mask));
}
static void fexfn_impl_libGL_glPopAttrib() {
  auto& stack=VexaProgramContext().LegacyClient.AttributeStack;if(stack.empty()){VexaAssemblyError(0x0504);return;}
  auto saved=std::move(stack.back());stack.pop_back();for(auto& restore:saved)restore();
}
static void fexfn_impl_libGL_glPushClientAttrib(GLbitfield mask) {
  if(mask!=0xffffffff&&(mask&~3U)){VexaAssemblyError(GL_INVALID_VALUE);return;}
  auto& context=VexaProgramContext();auto& stack=context.LegacyClient.ClientStack;
  if(stack.size()>=16){VexaAssemblyError(0x0503);return;}VexaLegacyRestore saved;
  if(mask&1)for(GLenum pname:{GL_PACK_ALIGNMENT,GL_PACK_ROW_LENGTH,GL_PACK_SKIP_ROWS,GL_PACK_SKIP_PIXELS,GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_ROWS,GL_UNPACK_SKIP_PIXELS,GL_UNPACK_IMAGE_HEIGHT,GL_UNPACK_SKIP_IMAGES}) {GLint value;::glGetIntegerv(pname,&value);saved.push_back([=]{::glPixelStorei(pname,value);});}
  if(mask&2) {
    GLint array=0,element=0,vao=0;::glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&array);::glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&element);::glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);
    const auto arrays=context.LegacyClient.Arrays;const GLuint unit=context.LegacyClient.Unit;const bool used=context.LegacyClient.Used;
    saved.push_back([=]{auto& state=VexaProgramContext().LegacyClient;state.Arrays=arrays;state.Unit=unit;state.Used=used;::glBindVertexArray(vao);});
    for(GLuint index=0;index<16;++index) {
      const VexaSavedLegacyAttrib a(index);GLint enabled,divisor;::glGetVertexAttribiv(index,GL_VERTEX_ATTRIB_ARRAY_ENABLED,&enabled);::glGetVertexAttribiv(index,GL_VERTEX_ATTRIB_ARRAY_DIVISOR,&divisor);
      saved.push_back([=]{a.Restore();if(enabled)::glEnableVertexAttribArray(index);else ::glDisableVertexAttribArray(index);::glVertexAttribDivisor(index,divisor);});
    }
    saved.push_back([=]{::glBindBuffer(GL_ARRAY_BUFFER,array);::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,element);});
  }
  stack.push_back(std::move(saved));
}
static void fexfn_impl_libGL_glPopClientAttrib() {
  auto& stack=VexaProgramContext().LegacyClient.ClientStack;if(stack.empty()){VexaAssemblyError(0x0504);return;}
  auto saved=std::move(stack.back());stack.pop_back();for(auto& restore:saved)restore();
}
