
static std::atomic<bool> VexaBGRAGPUEnabled{true};
// Keep the attribute's public name/location. Only shader reads use a private
// value so glBindAttribLocation, reflection and program binaries still work.
static std::string VexaBGRAInstrument(std::string source) {
 const std::string code=VexaAlphaCode(source);
 if(code.find("vexa_bgra_original_main")!=std::string::npos)return source;
 static const std::regex main_re(R"(\bvoid\s+main\s*\(\s*(?:void\s*)?\)\s*\{)");
 std::smatch main;if(!std::regex_search(code,main,main_re))return source;
 static const std::regex input_re(R"((?:^|[;\n}])\s*(?:layout\s*\([^)]*\)\s*)?(?:in|attribute)\s+(?:(?:highp|mediump|lowp)\s+)?vec4\s+([A-Za-z_][A-Za-z_0-9]*)\s*(?=;))");
 std::unordered_map<std::string,size_t> inputs;
 for(std::sregex_iterator it(code.begin(),code.end(),input_re),end;it!=end;++it)inputs.emplace((*it)[1].str(),(*it).position(1));
 if(inputs.empty())return source;
 // Replace tokens in reverse offset order, excluding the public declarations.
 static const std::regex token_re(R"(\b[A-Za-z_][A-Za-z_0-9]*\b)");
 std::vector<std::pair<size_t,std::string>> edits;
 for(std::sregex_iterator it(code.begin(),code.end(),token_re),end;it!=end;++it){auto input=inputs.find(it->str());if(input!=inputs.end()&&size_t(it->position())!=input->second)edits.emplace_back(it->position(),it->str());}
 for(auto it=edits.rbegin();it!=edits.rend();++it)source.replace(it->first,it->second.size(),"vexa_bgra_value_"+it->second);
 std::string declarations,init;
 for(const auto& [name,pos]:inputs){declarations+="uniform bool vexa_bgra_enable_"+name+";\nhighp vec4 vexa_bgra_value_"+name+";\n";init+="vexa_bgra_value_"+name+"=vexa_bgra_enable_"+name+"?"+name+".bgra:"+name+";\n";}
 const auto rewrittenCode=VexaAlphaCode(source);
 if(!std::regex_search(rewrittenCode,main,main_re))return source;
 source.replace(main.position(),main.length(),"void vexa_bgra_original_main(){");
 source+="\nvoid main(){\n"+init+"vexa_bgra_original_main();}\n";
 source.insert(VexaShaderPrecisionInsertPos(source),"\n"+declarations);
 return source;
}
static uint32_t VexaPrepareBGRAVertexInputs(const std::array<VexaLegacyArray,16>& arrays) {
 auto& ctx=VexaProgramContext();std::lock_guard lock(ctx.Shared->Mutex);
 VexaRefreshProgramGeneration();
 if(ctx.Assembly.Enabled[0]||ctx.Assembly.Enabled[1])return 0;
 GLint program=0;::glGetIntegerv(GL_CURRENT_PROGRAM,&program);
 if(!program&&ctx.IntendedPipeline)::glGetProgramPipelineiv(ctx.IntendedPipeline,GL_VERTEX_SHADER,&program);
 if(!program)return 0;
 auto [it,added]=ctx.Shared->BGRAPrograms.try_emplace(program);auto& loc=it->second;
 if(added){
  GLint count=0,maxLength=0;::glGetProgramiv(program,GL_ACTIVE_ATTRIBUTES,&count);::glGetProgramiv(program,GL_ACTIVE_ATTRIBUTE_MAX_LENGTH,&maxLength);
  std::string name(std::max(maxLength,1),'\0');
  for(GLint a=0;a<count;++a){GLsizei length=0;GLint size=0;GLenum type=0;::glGetActiveAttrib(program,a,maxLength,&length,&size,&type,name.data());
   if(type!=GL_FLOAT_VEC4||size!=1)continue;std::string attr(name.data(),length);const GLint index=::glGetAttribLocation(program,attr.c_str());
   if(index>=0&&index<16)loc.Uniforms[index]=::glGetUniformLocation(program,("vexa_bgra_enable_"+attr).c_str());
  }
 }
 uint32_t mask=0;
 for(unsigned i=0;i<16;++i)if(loc.Uniforms[i]>=0){const auto& a=arrays[i];const bool enabled=VexaBGRAGPUEnabled.load()&&a.Enabled&&a.Defined&&a.BGRA&&a.Buffer;
  if(enabled)mask|=1u<<i;
 }
 if(loc.LastMask!=static_cast<int32_t>(mask)) {
  for(unsigned i=0;i<16;++i)if(loc.Uniforms[i]>=0)::glProgramUniform1i(program,loc.Uniforms[i],(mask>>i)&1u);
  loc.LastMask=mask;
 }
 return mask;
}
static GLuint VexaLegacyVAO(){GLint vao=0;::glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);return vao;}
static std::array<VexaLegacyArray,16>& VexaLegacyArrays(){return VexaProgramContext().LegacyClient.Arrays[VexaLegacyVAO()];}
static int VexaLegacyArrayIndex(GLenum array) {
  if(array==0x8074)return 0; // VERTEX_ARRAY
  if(array==0x8075)return 2; // NORMAL_ARRAY
  if(array==0x8076)return 3; // COLOR_ARRAY
  if(array==0x8078)return 8+VexaProgramContext().LegacyClient.Unit;
  return -1;
}
static unsigned VexaLegacyTypeSize(GLenum type) {
  switch(type){case GL_BYTE:case GL_UNSIGNED_BYTE:return 1;case GL_SHORT:case GL_UNSIGNED_SHORT:case GL_HALF_FLOAT:return 2;case GL_INT_2_10_10_10_REV:case GL_UNSIGNED_INT_2_10_10_10_REV:case GL_FLOAT:case GL_INT:case GL_UNSIGNED_INT:case GL_FIXED:return 4;case GL_DOUBLE:return 8;default:return 0;}
}
static int VexaLegacyQuery(GLenum pname,GLfloat* value) {
  auto& client=VexaProgramContext().LegacyClient;
  if(pname==0x84E1){*value=GL_TEXTURE0+client.Unit;return 1;}
  if(pname==0x8871||pname==0x84E2){*value=8;return 1;}
  const int enabled=VexaLegacyArrayIndex(pname);
  if(enabled>=0){*value=VexaLegacyArrays()[enabled].Enabled;return 1;}
  int index=-1,field=-1;
  if(pname>=0x807A&&pname<=0x807C){index=0;field=pname-0x807A;}
  if(pname>=0x8081&&pname<=0x8083){index=3;field=pname-0x8081;}
  if(pname>=0x8088&&pname<=0x808A){index=8+client.Unit;field=pname-0x8088;}
  if(pname==0x8896){index=0;field=3;}if(pname==0x8898){index=3;field=3;}if(pname==0x889A){index=8+client.Unit;field=3;}
  if(index<0)return 0;
  const auto& a=VexaLegacyArrays()[index];*value=field==0?a.Size:field==1?a.Type:field==2?a.Stride:a.Buffer;return 1;
}
struct VexaSavedLegacyAttrib {
  GLuint Index=0;GLint Buffer=0,Size=4,Type=GL_FLOAT,Normalized=0,Stride=0;void* Pointer=nullptr;
  explicit VexaSavedLegacyAttrib(GLuint index):Index(index) {
    ::glGetVertexAttribiv(index,GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,&Buffer);
    ::glGetVertexAttribiv(index,GL_VERTEX_ATTRIB_ARRAY_SIZE,&Size);::glGetVertexAttribiv(index,GL_VERTEX_ATTRIB_ARRAY_TYPE,&Type);
    ::glGetVertexAttribiv(index,GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,&Normalized);::glGetVertexAttribiv(index,GL_VERTEX_ATTRIB_ARRAY_STRIDE,&Stride);
    ::glGetVertexAttribPointerv(index,GL_VERTEX_ATTRIB_ARRAY_POINTER,&Pointer);
  }
  void Restore() const {::glBindBuffer(GL_ARRAY_BUFFER,Buffer);::glVertexAttribPointer(Index,Size,Type,Normalized,Stride,Pointer);}
};
class VexaLegacyArrayScope {
  GLint OldArray=0,OldElement=0;
  bool ChangedElement=false;
  std::vector<VexaSavedLegacyAttrib> Saved;
  static constexpr uint64_t MaxBytes=256ULL*1024*1024;
  static bool ReadBuffer(GLenum target,GLuint buffer,uint64_t offset,uint64_t size,std::vector<uint8_t>& data) {
    if(size>MaxBytes||offset>INT64_MAX||size>INT64_MAX-offset)return false;
    GLint old=0;::glGetIntegerv(target==GL_COPY_READ_BUFFER?GL_COPY_READ_BUFFER_BINDING:GL_ELEMENT_ARRAY_BUFFER_BINDING,&old);
    ::glBindBuffer(target,buffer);GLint64 total=0;::glGetBufferParameteri64v(target,GL_BUFFER_SIZE,&total);
    if(total<0||offset+size>static_cast<uint64_t>(total)){::glBindBuffer(target,old);return false;}
    void* map=::glMapBufferRange(target,offset,size,GL_MAP_READ_BIT);
    if(!map){::glBindBuffer(target,old);return false;}
    data.resize(size);memcpy(data.data(),map,size);::glUnmapBuffer(target);::glBindBuffer(target,old);return true;
  }
public:
  bool Ready=true;
  const void* Indices=nullptr;
  VexaLegacyArrayScope(GLint first=-1,GLsizei count=-1,GLenum indexType=0,const void* indices=nullptr,GLint baseVertex=0):Indices(indices) {
    auto& client=VexaProgramContext().LegacyClient;
    // GLSL uniforms start at zero. A share group which never used a BGRA
    // compatibility array has no selector to reset and no arrays to stage.
    if (!client.Used && !VexaProgramContext().Shared->BGRATouched.load(std::memory_order_relaxed)) return;
    auto it=client.Arrays.find(VexaLegacyVAO());
    static const std::array<VexaLegacyArray,16> empty{};
    const uint32_t gpuBGRA=VexaPrepareBGRAVertexInputs(it==client.Arrays.end()?empty:it->second);
    if(!client.Used||it==client.Arrays.end())return;
    bool memory=false;
    for(unsigned i=0;i<16;++i){const auto& a=it->second[i];if(a.Enabled&&a.Defined&&(!a.Buffer||a.Type==GL_DOUBLE||(a.BGRA&&!(gpuBGRA&(1u<<i)))))memory=true;}
    if(!memory)return;
    if(first<0||count<0){VexaAssemblyError(GL_INVALID_OPERATION);Ready=false;return;}
    if(!count)return;
    ::glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&OldArray);::glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&OldElement);
    uint64_t last=static_cast<uint64_t>(first)+count-1;
    if(indexType) {
      unsigned step=indexType==GL_UNSIGNED_BYTE?1:indexType==GL_UNSIGNED_SHORT?2:indexType==GL_UNSIGNED_INT?4:0;
      if(!step){VexaAssemblyError(GL_INVALID_ENUM);Ready=false;return;}
      const uint64_t bytes=static_cast<uint64_t>(count)*step;
      if(bytes>MaxBytes){VexaAssemblyError(GL_INVALID_OPERATION);Ready=false;return;}
      std::vector<uint8_t> storage;const uint8_t* data=static_cast<const uint8_t*>(indices);
      if(OldElement) {if(!ReadBuffer(GL_ELEMENT_ARRAY_BUFFER,OldElement,reinterpret_cast<uintptr_t>(indices),bytes,storage)){VexaAssemblyError(GL_INVALID_OPERATION);Ready=false;return;}data=storage.data();}
      else if(!data){VexaAssemblyError(GL_INVALID_OPERATION);Ready=false;return;}
      uint32_t maximum=0;
      for(int i=0;i<count;++i){uint32_t value=0;memcpy(&value,data+static_cast<size_t>(i)*step,step);maximum=std::max(maximum,value);}
      const int64_t end=static_cast<int64_t>(maximum)+baseVertex;if(end<0){VexaAssemblyError(GL_INVALID_OPERATION);Ready=false;return;}last=end;
      if(!OldElement) {
        if(!client.IndexStaging)::glGenBuffers(1,&client.IndexStaging);
        ::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,client.IndexStaging);::glBufferData(GL_ELEMENT_ARRAY_BUFFER,bytes,data,GL_STREAM_DRAW);Indices=nullptr;ChangedElement=true;
      }
    }
    for(unsigned index=0;index<16;++index) {
      const auto& a=it->second[index];if(!a.Enabled||!a.Defined||(a.Buffer&&a.Type!=GL_DOUBLE&&(!a.BGRA||(gpuBGRA&(1u<<index)))))continue;
      const bool packedBGRA=a.BGRA&&a.Type!=GL_UNSIGNED_BYTE;
      const uint64_t component=VexaLegacyTypeSize(a.Type),packed=packedBGRA?4:component*a.Size,stride=a.Stride?a.Stride:packed;
      if(!component||last>MaxBytes/stride||last*stride+packed>MaxBytes||(!a.Buffer&&!a.Pointer)){VexaAssemblyError(GL_INVALID_OPERATION);Ready=false;return;}
      const uint64_t bytes=last*stride+packed;std::vector<uint8_t> source;
      const uint8_t* data=reinterpret_cast<const uint8_t*>(a.Pointer);
      if(a.Buffer){if(!ReadBuffer(GL_COPY_READ_BUFFER,a.Buffer,a.Pointer,bytes,source)){VexaAssemblyError(GL_INVALID_OPERATION);Ready=false;return;}data=source.data();}
      Saved.emplace_back(index);if(!client.Staging[index])::glGenBuffers(1,&client.Staging[index]);::glBindBuffer(GL_ARRAY_BUFFER,client.Staging[index]);
      if(a.BGRA) {
        if(last>=MaxBytes/(4*sizeof(GLfloat))){VexaAssemblyError(GL_INVALID_OPERATION);Ready=false;return;}
        std::vector<GLfloat> floats((last+1)*4);
        for(uint64_t v=0;v<=last;++v){GLfloat rgba[4]{};
          if(!packedBGRA){for(int c=0;c<4;++c)rgba[c]=data[v*stride+c]/255.0f;}
          else {uint32_t bits=0;memcpy(&bits,data+v*stride,4);for(int c=0;c<4;++c){const unsigned width=c==3?2:10;const uint32_t raw=(bits>>(c*10))&((1u<<width)-1);if(a.Type==GL_INT_2_10_10_10_REV){const int32_t signedValue=(raw&(1u<<(width-1)))?int32_t(raw)-int32_t(1u<<width):int32_t(raw);rgba[c]=std::max(-1.f,signedValue/float((1u<<(width-1))-1));}else rgba[c]=raw/float((1u<<width)-1);}}
          floats[v*4]=rgba[2];floats[v*4+1]=rgba[1];floats[v*4+2]=rgba[0];floats[v*4+3]=rgba[3];}
        ::glBufferData(GL_ARRAY_BUFFER,floats.size()*sizeof(GLfloat),floats.data(),GL_STREAM_DRAW);::glVertexAttribPointer(index,4,GL_FLOAT,GL_FALSE,0,nullptr);
      } else if(a.Type==GL_DOUBLE) {
        std::vector<GLfloat> floats((last+1)*a.Size);
        for(uint64_t v=0;v<=last;++v)for(int c=0;c<a.Size;++c){GLdouble d;memcpy(&d,data+v*stride+c*sizeof(d),sizeof(d));floats[v*a.Size+c]=d;}
        ::glBufferData(GL_ARRAY_BUFFER,floats.size()*sizeof(GLfloat),floats.data(),GL_STREAM_DRAW);::glVertexAttribPointer(index,a.Size,GL_FLOAT,GL_FALSE,0,nullptr);
      }else {::glBufferData(GL_ARRAY_BUFFER,bytes,data,GL_STREAM_DRAW);::glVertexAttribPointer(index,a.Size,a.Type,a.Normalize,stride,nullptr);}
    }
    ::glBindBuffer(GL_ARRAY_BUFFER,OldArray);
  }
  ~VexaLegacyArrayScope(){for(const auto& saved:Saved)saved.Restore();if(!Saved.empty())::glBindBuffer(GL_ARRAY_BUFFER,OldArray);if(ChangedElement)::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,OldElement);}
};
