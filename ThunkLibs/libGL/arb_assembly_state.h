#pragma once
#include <array>
struct VexaAssemblyObject {
  GLenum Target = 0;
  GLuint Shader = 0;
  uint64_t Revision = 0;
  std::string Source;
  std::array<std::array<GLfloat,4>,128> Local {};
  GLint Instructions = 0, Temporaries = 0, TextureInstructions = 0;
};
struct VexaAssemblyUniform {
  GLint Location;
  GLsizei Count;
  int Stage, Kind; // 0 local, 1 environment, 2 sampler
  bool Applied = false;
  std::vector<GLfloat> Values;
};
struct VexaAssemblyLinked {
  GLuint Program = 0;
  std::vector<VexaAssemblyUniform> Uniforms;
};
struct VexaAssemblyContext {
  GLuint Binding[2] {};
  bool Enabled[2] {};
  VexaAssemblyObject Default[2];
  std::array<std::array<std::array<GLfloat,4>,128>,2> Env {};
  std::unordered_map<std::string,VexaAssemblyLinked> Linked;
  GLint ErrorPosition = -1;
  std::string ErrorString;
};
