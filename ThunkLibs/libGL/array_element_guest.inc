VEXA_GL_GUEST_EXPORT void glIndexPointer(GLenum type,GLsizei stride,const void* pointer) {
  if(VexaSelectOutsideBegin())fexfn_pack_VEXA_AuxArrayPointer(0x8077,type,stride,reinterpret_cast<uintptr_t>(pointer));
}
VEXA_GL_GUEST_EXPORT void glEdgeFlagPointer(GLsizei stride,const void* pointer) {
  if(VexaSelectOutsideBegin())fexfn_pack_VEXA_AuxArrayPointer(0x8079,GL_UNSIGNED_BYTE,stride,reinterpret_cast<uintptr_t>(pointer));
}
VEXA_GL_GUEST_EXPORT void glArrayElement(GLint element) {
  std::array<float,64> values{};const auto mask=fexfn_pack_VEXA_ReadArrayElement(element,values.data());
  // Client arrays are read at list compilation time, as required by GL 1.1.
  vexa_ff::run_or_record_callback([values,mask] {
    auto& state=vexa_ff::S();
    if(mask&(1U<<1))state.color_index=values[4];
    if(mask&(1U<<2)){std::copy_n(values.data()+8,3,state.normal);fexfn_pack_glVertexAttrib4f(2,values[8],values[9],values[10],1);}
    if(mask&(1U<<3)){std::copy_n(values.data()+12,4,state.color);fexfn_pack_glColor4f(values[12],values[13],values[14],values[15]);if(state.lighting.color_material_enabled)state.lighting.update_color_material(state.color);}
    if(mask&(1U<<4))state.edge_flag=values[16]!=0;
    for(unsigned unit=0;unit<8;++unit)if(mask&(1U<<(8+unit))) {
      const float* v=values.data()+(8+unit)*4;
      if(unit==0)std::copy_n(v,4,state.texcoord);
      fexfn_pack_glVertexAttrib4f(8+unit,v[0],v[1],v[2],v[3]);
    }
    if(mask&1)vexa_ff::vertex4(values[0],values[1],values[2],values[3]);
  });
}
static bool VexaArraySoftwareDraw() {
  const auto& s=vexa_ff::S();
  if(s.compiling||s.selection.mode!=vexa_select::kRender)return true;
  if(!s.stipple.line&&!s.stipple.polygon)return false;
  GLint program=0;fexfn_pack_glGetIntegerv(GL_CURRENT_PROGRAM,&program);
  return program==0;
}
VEXA_GL_GUEST_EXPORT void glDrawArrays(GLenum mode,GLint first,GLsizei count) {
  if(!VexaSelectOutsideBegin())return;
  if(!VexaArraySoftwareDraw()){fexfn_pack_glDrawArrays(mode,first,count);return;}
  if(mode>9){fexfn_pack_VEXA_CompatError(GL_INVALID_ENUM);return;}
  if(first<0||count<0||count>INT_MAX-first){fexfn_pack_VEXA_CompatError(GL_INVALID_VALUE);return;}
  vexa_ff::begin(mode);for(int i=0;i<count;++i)glArrayElement(first+i);vexa_ff::end();
}
VEXA_GL_GUEST_EXPORT void glDrawElements(GLenum mode,GLsizei count,GLenum type,const void* indices) {
  if(!VexaSelectOutsideBegin())return;
  if(!VexaArraySoftwareDraw()){fexfn_pack_glDrawElements(mode,count,type,indices);return;}
  if(mode>9||(type!=GL_UNSIGNED_BYTE&&type!=GL_UNSIGNED_SHORT&&type!=GL_UNSIGNED_INT)){fexfn_pack_VEXA_CompatError(GL_INVALID_ENUM);return;}
  if(count<0){fexfn_pack_VEXA_CompatError(GL_INVALID_VALUE);return;}
  if(!count)return;
  if(count>16*1024*1024){fexfn_pack_VEXA_CompatError(GL_OUT_OF_MEMORY);return;}
  std::vector<unsigned> elements(count);
  if(!fexfn_pack_VEXA_ReadElementIndices(type,count,reinterpret_cast<uintptr_t>(indices),elements.data()))return;
  for(auto index:elements)if(index>INT_MAX){fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION);return;}
  vexa_ff::begin(mode);for(auto index:elements)glArrayElement(index);vexa_ff::end();
}
