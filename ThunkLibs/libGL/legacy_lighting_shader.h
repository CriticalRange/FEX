#pragma once
// Prototype fixed-function GLES shaders for untextured geometry. Callers must
// populate all uniforms per current context and preserve the guest GL state.
// Texture units/texgen, color-index mode, separate specular color, flat shading,
// display lists and arbitrary-program compatibility are intentionally NOT
// implemented here. This is not a drop-in replacement for the existing shader.
namespace vexa_legacy_lighting {
constexpr const char *untextured_vertex_shader = R"GLSL(
attribute vec4 aPos;
attribute vec3 aNrm;
attribute vec4 aColor;
attribute vec4 aTexCoord;
uniform mat4 uTextureMatrix;
varying vec4 vTexCoord;
uniform int uVertexColors;
uniform mat4 uMvp;
uniform mat4 uModelView;
uniform mat3 uNormalMatrix;
uniform vec4 uColor;
uniform int uLightingEnabled;
uniform int uNormalize;
uniform int uTwoSided;
uniform int uLocalViewer;
uniform vec4 uGlobalAmbient;
uniform vec4 uMaterialAmbient[2];
uniform vec4 uMaterialDiffuse[2];
uniform vec4 uMaterialSpecular[2];
uniform vec4 uMaterialEmission[2];
uniform float uMaterialShininess[2];
uniform int uLightEnabled[8];
uniform vec4 uLightAmbient[8];
uniform vec4 uLightDiffuse[8];
uniform vec4 uLightSpecular[8];
uniform vec4 uLightPosition[8];
uniform vec3 uLightDirection[8];
uniform float uLightExponent[8];
uniform float uLightCutoff[8];
uniform vec3 uLightAttenuation[8];
varying vec4 vFrontColor;
varying vec4 vBackColor;
varying float vFogCoordinate;
vec4 illuminate(vec3 eyePosition, vec3 normal, vec4 ambientMaterial,
                vec4 diffuseMaterial, vec4 specularMaterial, vec4 emissionMaterial,
                float shininess) {
  vec3 viewDirection = uLocalViewer != 0 ? normalize(-eyePosition) : vec3(0.0, 0.0, 1.0);
  vec4 sum = emissionMaterial + uGlobalAmbient * ambientMaterial;
  for (int i=0; i<8; ++i) {
    if (uLightEnabled[i] != 0) {
      vec3 direction;
      float attenuation=1.0;
      if (uLightPosition[i].w == 0.0) direction=normalize(uLightPosition[i].xyz);
      else {
        vec3 delta=uLightPosition[i].xyz / uLightPosition[i].w-eyePosition;
        float distanceToLight=length(delta);
        direction=distanceToLight > 0.0 ? delta/distanceToLight : vec3(0.0);
        vec3 factors=uLightAttenuation[i];
        float denominator=factors.x + factors.y*distanceToLight + factors.z*distanceToLight*distanceToLight;
        attenuation=denominator > 0.0 ? 1.0/denominator : 1.0;
      }
      if (uLightCutoff[i] != 180.0) {
        float spotCos=dot(-direction, normalize(uLightDirection[i]));
        float cutoffCos=cos(radians(uLightCutoff[i]));
        attenuation *= spotCos >= cutoffCos ? pow(max(spotCos, 0.0),uLightExponent[i]) : 0.0;
      }
      float diffuseFactor=max(dot(normal,direction),0.0);
      float specularFactor=0.0;
      if (diffuseFactor > 0.0) {
        vec3 halfway=normalize(direction+viewDirection);
        specularFactor=pow(max(dot(normal,halfway),0.0),shininess);
      }
      sum += attenuation * (uLightAmbient[i]*ambientMaterial +
               diffuseFactor*uLightDiffuse[i]*diffuseMaterial +
               specularFactor*uLightSpecular[i]*specularMaterial);
    }
  }
  sum.a=diffuseMaterial.a;
  return clamp(sum,0.0,1.0);
}
void main() {
  vTexCoord=uTextureMatrix*aTexCoord;
  vec4 eye=uModelView*aPos;
  gl_Position=uMvp*aPos;
  vec3 n=uNormalMatrix*aNrm;
  if (uNormalize != 0) n=normalize(n);
  if (uLightingEnabled != 0) {
    vFrontColor=illuminate(eye.xyz,n,uMaterialAmbient[0],uMaterialDiffuse[0],
                           uMaterialSpecular[0],uMaterialEmission[0],uMaterialShininess[0]);
    if (uTwoSided != 0)
      vBackColor=illuminate(eye.xyz,-n,uMaterialAmbient[1],uMaterialDiffuse[1],
                            uMaterialSpecular[1],uMaterialEmission[1],uMaterialShininess[1]);
    else vBackColor=vFrontColor;
  } else vFrontColor=vBackColor=(uVertexColors != 0 ? aColor : uColor);
  vFogCoordinate=abs(eye.z);
}
)GLSL";
constexpr const char *untextured_fragment_shader = R"GLSL(
precision mediump float;
uniform sampler2D uTexture;
uniform int uTextureEnabled;
uniform int uTextureEnvMode;
uniform vec4 uTextureEnvColor;
varying vec4 vTexCoord;
uniform int uStipple;
uniform vec4 uStippleRows[32];
uniform int uFogEnabled;
uniform int uFogMode;
uniform vec4 uFogColor;
uniform float uFogDensity;
uniform float uFogStart;
uniform float uFogEnd;
varying vec4 vFrontColor;
varying vec4 vBackColor;
varying float vFogCoordinate;
void main() {
  if(uStipple != 0) {
    float y=mod(floor(gl_FragCoord.y),32.0);
    vec4 row=vec4(255.0);
    for(int i=0;i<32;++i) if(float(i)==y) row=uStippleRows[i];
    float x=mod(floor(gl_FragCoord.x),32.0);
    float octet=x<8.0?row.x:(x<16.0?row.y:(x<24.0?row.z:row.w));
    if(mod(floor(octet/exp2(7.0-mod(x,8.0))),2.0)<0.5)discard;
  }
  vec4 c=gl_FrontFacing ? vFrontColor : vBackColor;
  if (uTextureEnabled != 0) {
    vec4 t=texture2DProj(uTexture,vTexCoord);
    if(uTextureEnvMode==0x1e01) c=t;
    else if(uTextureEnvMode==0x2101) c.rgb=mix(c.rgb,t.rgb,t.a);
    else if(uTextureEnvMode==0x0be2) { c.rgb=mix(c.rgb,uTextureEnvColor.rgb,t.rgb); c.a*=t.a; }
    else if(uTextureEnvMode==0x0104) { c.rgb+=t.rgb; c.a*=t.a; }
    else c*=t;
  }
  if (uFogEnabled != 0) {
    float factor;
    if (uFogMode == 0x2601) factor=(uFogEnd-vFogCoordinate)/(uFogEnd-uFogStart);
    else if (uFogMode == 0x0801) { float d=uFogDensity*vFogCoordinate; factor=exp(-d*d); }
    else factor=exp(-uFogDensity*vFogCoordinate);
    c.rgb=mix(uFogColor.rgb,c.rgb,clamp(factor,0.0,1.0));
  }
  gl_FragColor=c;
}
)GLSL";
} // namespace vexa_legacy_lighting
