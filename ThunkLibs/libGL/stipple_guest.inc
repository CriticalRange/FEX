static void VexaStippleDraw(const std::vector<float>& data,const float* color,unsigned mode) {
  auto& s=vexa_ff::S(); float mvp[16];vexa_ff::current_mvp(mvp);
  float mask[128];for(int i=0;i<128;++i)mask[i]=s.stipple.mask[i];
  fexfn_pack_VEXA_FFSetStipple(mask,s.stipple.polygon && mode>=4);
  if(s.stipple.line && mode>=1 && mode<=3) {
    GLint viewport[4];fexfn_pack_glGetIntegerv(GL_VIEWPORT,viewport);
    auto lines=vexa_stipple::lines(data,mode,mvp,viewport[2],viewport[3],s.stipple.factor,s.stipple.pattern);
    if(!lines.empty())fexfn_pack_VEXA_FFDrawImmediate(lines.data(),int(lines.size()),mvp,color,GL_LINES);
  } else fexfn_pack_VEXA_FFDrawImmediate(data.data(),int(data.size()),mvp,color,mode<=3?mode:GL_TRIANGLES);
  fexfn_pack_VEXA_FFSetStipple(nullptr,0);
}
static bool VexaStippleCap(GLenum cap,bool enable,bool query,GLboolean* value=nullptr) {
  if(cap!=0x0B24 && cap!=0x0B42)return false;
  if(query){*value=cap==0x0B24?vexa_ff::S().stipple.line:vexa_ff::S().stipple.polygon;return true;}
  vexa_ff::run_or_record_callback([=]{auto& s=vexa_ff::S();(cap==0x0B24?s.stipple.line:s.stipple.polygon)=enable;s.stipple_draw=VexaStippleDraw;});return true;
}
VEXA_GL_GUEST_EXPORT void glLineStipple(GLint factor,GLushort pattern) {
  if(!VexaSelectOutsideBegin())return;
  vexa_ff::run_or_record_callback([=]{auto& s=vexa_ff::S();s.stipple.factor=std::clamp(factor,1,256);s.stipple.pattern=pattern;s.stipple_draw=VexaStippleDraw;});
}
VEXA_GL_GUEST_EXPORT void glPolygonStipple(const GLubyte* pattern) {
  if(!VexaSelectOutsideBegin())return;
  auto unpack=VexaUnpack();size_t bytes=0;
  if(!VexaPixelBytes(unpack,32,32,1,true,bytes)){fexfn_pack_VEXA_CompatError(GL_OUT_OF_MEMORY);return;}
  std::vector<unsigned char> source(bytes);std::vector<float> pixels;
  if(!VexaPixelMapRead(pattern,source.data(),bytes))return;
  if(!vexa_pixel::bitmap_mask(source.data(),source.size(),32,32,unpack,vexa_ff::S().unpack_lsb_first,pixels))return;
  std::array<unsigned char,128> mask{};
  for(int i=0;i<1024;++i)if(pixels[i*4]!=0)mask[i/8]|=1u<<(7-i%8);
  vexa_ff::run_or_record_callback([mask]{vexa_ff::S().stipple.mask=mask;vexa_ff::S().stipple_draw=VexaStippleDraw;});
}
VEXA_GL_GUEST_EXPORT void glGetPolygonStipple(GLubyte* pattern) {
  if(!VexaSelectOutsideBegin())return;
  GLint alignment=4,row=0,skipRows=0,skipPixels=0,buffer=0;
  fexfn_pack_glGetIntegerv(GL_PACK_ALIGNMENT,&alignment);fexfn_pack_glGetIntegerv(GL_PACK_ROW_LENGTH,&row);
  fexfn_pack_glGetIntegerv(GL_PACK_SKIP_ROWS,&skipRows);fexfn_pack_glGetIntegerv(GL_PACK_SKIP_PIXELS,&skipPixels);
  fexfn_pack_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&buffer);
  vexa_pixel::Unpack pack;pack.alignment=alignment;pack.row_length=row;pack.skip_rows=skipRows;pack.skip_pixels=skipPixels;
  size_t bytes=0;if(!VexaPixelBytes(pack,32,32,1,true,bytes)){fexfn_pack_VEXA_CompatError(GL_OUT_OF_MEMORY);return;}
  GLubyte* dst=pattern;
  if(buffer){GLint length=0,mapped=0;fexfn_pack_glGetBufferParameteriv(GL_PIXEL_PACK_BUFFER,GL_BUFFER_SIZE,&length);
    fexfn_pack_glGetBufferParameteriv(GL_PIXEL_PACK_BUFFER,GL_BUFFER_MAPPED,&mapped);auto offset=reinterpret_cast<uintptr_t>(pattern);
    if(mapped||length<0||offset>size_t(length)||bytes>size_t(length)-offset){fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION);return;}
    dst=static_cast<GLubyte*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER,offset,bytes,GL_MAP_READ_BIT|GL_MAP_WRITE_BIT));
  }
  if(!dst)return;
  const size_t stride=(((size_t(row?row:32)+7)/8)+alignment-1)&~(size_t(alignment)-1);
  for(int y=0;y<32;++y)for(int x=0;x<32;++x){size_t bit=size_t(skipPixels)+x;auto& target=dst[(size_t(skipRows)+y)*stride+bit/8];
    unsigned shift=vexa_ff::S().pack_lsb_first?bit%8:7-bit%8;
    bool on=(vexa_ff::S().stipple.mask[y*4+x/8]>>(7-x%8))&1;
    target=(target&~(1u<<shift))|(unsigned(on)<<shift);}
  if(buffer)glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
}
