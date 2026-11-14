#pragma once
namespace VexaInterleaved {
  struct Layout { unsigned stride, tex, color, normal, vertex; int texSize, colorSize, vertexSize; bool byteColor; };
  static constexpr unsigned absent = ~0U;
  static constexpr Layout layouts[] = {
    {8, absent,absent,absent,0, 0,0,2,false}, {12,absent,absent,absent,0, 0,0,3,false},
    {12,absent,0,absent,4, 0,4,2,true}, {16,absent,0,absent,4, 0,4,3,true},
    {24,absent,0,absent,12, 0,3,3,false}, {24,absent,absent,0,12, 0,0,3,false},
    {40,absent,0,16,28, 0,4,3,false}, {20,0,absent,absent,8, 2,0,3,false},
    {32,0,absent,absent,16, 4,0,4,false}, {24,0,8,absent,12, 2,4,3,true},
    {32,0,8,absent,20, 2,3,3,false}, {32,0,absent,8,20, 2,0,3,false},
    {48,0,8,24,36, 2,4,3,false}, {60,0,16,32,44, 4,4,4,false}
  };
}
