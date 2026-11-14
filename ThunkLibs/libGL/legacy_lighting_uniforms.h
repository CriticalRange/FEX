#pragma once
#include "legacy_lighting_wire.h"
#include <GLES2/gl2.h>
#include <cstdio>

// Call only while `program` is current. No GL state other than uniforms is
// changed. Shader variant assumes untextured geometry; caller must select a
// fully supported path and must NOT expose it as a full GL implementation.
namespace vexa_legacy_lighting {
inline bool upload_wire(GLuint program, const Wire &w) {
  if (!valid_wire(w)) return false;
  GLint current=0; glGetIntegerv(GL_CURRENT_PROGRAM,&current);
  if (static_cast<GLuint>(current)!=program || !program) return false;
  auto loc=[&](const char *name) { return glGetUniformLocation(program,name); };
  auto onei=[&](const char *name,int value) { GLint l=loc(name); if(l>=0)glUniform1i(l,value); };
  auto onef=[&](const char *name,float value) { GLint l=loc(name); if(l>=0)glUniform1f(l,value); };
  auto vec4=[&](const char *name,const float *value) { GLint l=loc(name); if(l>=0)glUniform4fv(l,1,value); };
  auto vec3=[&](const char *name,const float *value) { GLint l=loc(name); if(l>=0)glUniform3fv(l,1,value); };
  GLint l=loc("uModelView"); if(l>=0)glUniformMatrix4fv(l,1,GL_FALSE,w.modelview);
  l=loc("uNormalMatrix"); if(l>=0)glUniformMatrix3fv(l,1,GL_FALSE,w.normal_matrix);
  onei("uTextureEnabled",w.texture_enabled); onei("uTextureEnvMode",w.texture_env_mode);
  onei("uTexture",0); vec4("uTextureEnvColor",w.texture_env_color);
  l=loc("uTextureMatrix"); if(l>=0)glUniformMatrix4fv(l,1,GL_FALSE,w.texture_matrix);
  onei("uLightingEnabled",w.lighting_enabled); onei("uNormalize",w.normalize_enabled);
  onei("uTwoSided",w.two_sided); onei("uLocalViewer",w.local_viewer); vec4("uGlobalAmbient",w.global_ambient);
  char name[64];
  for(int i=0;i<2;++i) {
    const auto &m=w.material[i];
    std::snprintf(name,sizeof(name),"uMaterialAmbient[%d]",i); vec4(name,m.ambient);
    std::snprintf(name,sizeof(name),"uMaterialDiffuse[%d]",i); vec4(name,m.diffuse);
    std::snprintf(name,sizeof(name),"uMaterialSpecular[%d]",i); vec4(name,m.specular);
    std::snprintf(name,sizeof(name),"uMaterialEmission[%d]",i); vec4(name,m.emission);
    std::snprintf(name,sizeof(name),"uMaterialShininess[%d]",i); onef(name,m.shininess);
  }
  for(int i=0;i<8;++i) {
    const auto &x=w.light[i];
    std::snprintf(name,sizeof(name),"uLightEnabled[%d]",i); onei(name,x.enabled);
    std::snprintf(name,sizeof(name),"uLightAmbient[%d]",i); vec4(name,x.ambient);
    std::snprintf(name,sizeof(name),"uLightDiffuse[%d]",i); vec4(name,x.diffuse);
    std::snprintf(name,sizeof(name),"uLightSpecular[%d]",i); vec4(name,x.specular);
    std::snprintf(name,sizeof(name),"uLightPosition[%d]",i); vec4(name,x.position);
    std::snprintf(name,sizeof(name),"uLightDirection[%d]",i); vec3(name,x.direction);
    std::snprintf(name,sizeof(name),"uLightExponent[%d]",i); onef(name,x.exponent);
    std::snprintf(name,sizeof(name),"uLightCutoff[%d]",i); onef(name,x.cutoff);
    std::snprintf(name,sizeof(name),"uLightAttenuation[%d]",i); vec3(name,x.attenuation);
  }
  onei("uFogEnabled",w.fog_enabled); onei("uFogMode",w.fog_mode); vec4("uFogColor",w.fog_color);
  onef("uFogDensity",w.fog_density); onef("uFogStart",w.fog_start); onef("uFogEnd",w.fog_end);
  return true;
}
} // namespace vexa_legacy_lighting
