#pragma once
#include <functional>
struct VexaLegacyArray {
  bool Enabled=false, Defined=false;
  GLint Size=4;
  GLenum Type=GL_FLOAT;
  GLsizei Stride=0;
  uintptr_t Pointer=0;
  GLuint Buffer=0;
  bool Normalize=false, BGRA=false;
};
struct VexaLegacyClient {
  GLuint Unit=0, Staging[16]{}, IndexStaging=0;
  bool Used=false;
  std::unordered_map<GLuint,std::array<VexaLegacyArray,16>> Arrays;
  std::vector<std::vector<std::function<void()>>> AttributeStack, ClientStack;
};

// Linked vertex-input selectors survive program binaries through their names.
struct VexaBGRAProgramState {
  GLint Uniforms[16];
  int32_t LastMask=-1;
  VexaBGRAProgramState() { std::fill(std::begin(Uniforms),std::end(Uniforms),-1); }
};
