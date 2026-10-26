#pragma once
struct VexaClipLocations {
  GLint Eye=-1, Projected=-1, Enabled=-1, MVP=-1;
  bool Applied=false;
  GLfloat LastEye[24]{}, LastProjected[24]{}, LastMVP[16]{};
  GLint LastEnabled[6]{};
};
struct VexaClipState {
  GLdouble Equation[6][4] {};
  GLint Enabled[6] {};
  GLfloat Model[16] {}, Projection[16] {};
  GLenum Mode=0x1700;
  // Derived values are context-local. Compare inputs, since attribute-stack
  // restores can update them without going through a matrix/plane setter.
  bool Derived=false;
  GLfloat SavedModel[16]{}, SavedProjection[16]{};
  GLdouble SavedEquation[6][4]{};
  GLfloat Eye[24]{}, Projected[24]{}, MVP[16]{};
  VexaClipState() {
    for(int i:{0,5,10,15})Model[i]=Projection[i]=1;
  }
};
