// GLSL 1.10/1.20 compatibility varyings are not built-ins in GLSL ES.
// Keep the same names, precision and array extent in both stages so independently
// translated MojoShader shaders link without needing a program-wide rewrite.
static void VexaRewriteLegacyVaryings(std::string* source, GLenum shader_type) {
  if (shader_type != GL_VERTEX_SHADER && shader_type != GL_FRAGMENT_SHADER) return;
  auto identifier = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  };
  std::string declarations;
  auto replace = [&](std::string_view from, std::string_view to, std::string_view declaration) {
    bool used = false;
    for (size_t pos = 0; (pos = source->find(from, pos)) != std::string::npos;) {
      const size_t end = pos + from.size();
      if ((pos == 0 || !identifier((*source)[pos - 1])) &&
          (end == source->size() || !identifier((*source)[end]))) {
        source->replace(pos, from.size(), to);
        pos += to.size();
        used = true;
      } else {
        pos = end;
      }
    }
    if (used) declarations += declaration;
  };
  replace("gl_TexCoord", "vexa_compat_TexCoord", "varying highp vec4 vexa_compat_TexCoord[8];\n");
  replace(shader_type == GL_VERTEX_SHADER ? "gl_FrontColor" : "gl_Color",
          "vexa_compat_Color", "varying highp vec4 vexa_compat_Color;\n");
  replace(shader_type == GL_VERTEX_SHADER ? "gl_FrontSecondaryColor" : "gl_SecondaryColor",
          "vexa_compat_SecondaryColor", "varying highp vec4 vexa_compat_SecondaryColor;\n");
  replace("gl_FogFragCoord", "vexa_compat_FogFragCoord", "varying highp float vexa_compat_FogFragCoord;\n");
  if (!declarations.empty()) source->insert(VexaShaderPrecisionInsertPos(*source), declarations);
}

// Preserve centroid interpolation and 3D samplers in the opt-in desktop
// compatibility path; ESSL 100 cannot express these GLSL 1.20 features.
static void VexaUpgradeLegacyShader(std::string* source, GLenum type) {
  const bool vertex=type==GL_VERTEX_SHADER;
  auto token=[&](const char* from,const char* to){*source=std::regex_replace(*source,std::regex(std::string("\\b")+from+"\\b"),to);};
  token("attribute","in");token("varying",vertex?"out":"in");
  token("texture2D","texture");token("texture3D","texture");token("textureCube","texture");
  token("texture2DProj","textureProj");token("texture2DLod","textureLod");token("textureCubeLod","textureLod");
  std::string declarations;
  if(vertex) {
    if(std::regex_search(*source,std::regex(R"(\bftransform\s*\()"))) {
      *source=std::regex_replace(*source,std::regex(R"(\bftransform\s*\(\s*\))"),"(vexa_compat_MVP * vexa_compat_Vertex)");
      declarations+="uniform highp mat4 vexa_compat_MVP;\nlayout(location=0) in highp vec4 vexa_compat_Vertex;\n";
    }
  } else {
    if(std::regex_search(*source,std::regex(R"(\bgl_FragData\b)"))) {
      unsigned count=1;const std::regex output(R"(gl_FragData\s*\[\s*([0-9]+)\s*\])");
      for(std::sregex_iterator it(source->begin(),source->end(),output),end;it!=end;++it)count=std::max(count,static_cast<unsigned>(std::stoul((*it)[1]))+1);
      token("gl_FragData","vexa_compat_FragData");
      declarations+="layout(location=0) out highp vec4 vexa_compat_FragData["+std::to_string(count)+"];\n";
    }
    if(std::regex_search(*source,std::regex(R"(\bgl_FragColor\b)"))) {
      token("gl_FragColor","vexa_compat_FragColor");declarations+="layout(location=0) out highp vec4 vexa_compat_FragColor;\n";
    }
  }
  if(!declarations.empty())source->insert(VexaShaderPrecisionInsertPos(*source),declarations);
}
