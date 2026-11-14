#include "vexa_evaluator_emit.h"

static vexa_eval::Vertex VexaEvalCurrent() {
  vexa_eval::Vertex current;
  std::copy_n(vexa_ff::S().normal, 3, current.normal.begin());
  std::copy_n(vexa_ff::S().color, 4, current.color.begin());
  std::copy_n(vexa_ff::S().texcoord, 4, current.texcoord.begin());
  current.index = vexa_ff::S().color_index;
  return current;
}
static void VexaEvalVertex(const vexa_eval::Vertex& vertex) {
  auto& state = vexa_ff::S();
  const auto saved = VexaEvalCurrent();
  std::copy(vertex.normal.begin(), vertex.normal.end(), state.normal);
  std::copy(vertex.color.begin(), vertex.color.end(), state.color);
  std::copy(vertex.texcoord.begin(), vertex.texcoord.end(), state.texcoord);
  state.color_index = vertex.index;
  vexa_ff::vertex4(vertex.position[0], vertex.position[1], vertex.position[2], vertex.position[3]);
  std::copy(saved.normal.begin(), saved.normal.end(), state.normal);
  std::copy(saved.color.begin(), saved.color.end(), state.color);
  std::copy(saved.texcoord.begin(), saved.texcoord.end(), state.texcoord);
  state.color_index = saved.index;
}
static void VexaEvalDraw(const vexa_eval::Capture& capture) {
  for (const auto& vertex : capture.standalone()) VexaEvalVertex(vertex);
  for (const auto& primitive : capture.primitives()) {
    vexa_ff::begin(primitive.mode);
    for (const auto& vertex : primitive.vertices) VexaEvalVertex(vertex);
    vexa_ff::end();
  }
}
static void VexaEvalCoord(double u, double v, bool two) {
  vexa_ff::run_or_record_callback([u,v,two] {
    vexa_eval::Capture capture(VexaEvalCurrent());
    if (two) vexa_eval::emit2(vexa_ff::S().evaluator,u,v,capture);
    else vexa_eval::emit1(vexa_ff::S().evaluator,u,capture);
    VexaEvalDraw(capture);
  });
}
#define VEXA_EVAL_COORD(suffix, type) \
  VEXA_GL_GUEST_EXPORT void glEvalCoord1##suffix(type u) { VexaEvalCoord(u,0,false); } \
  VEXA_GL_GUEST_EXPORT void glEvalCoord1##suffix##v(const type* u) { if(u)VexaEvalCoord(u[0],0,false); } \
  VEXA_GL_GUEST_EXPORT void glEvalCoord2##suffix(type u, type v) { VexaEvalCoord(u,v,true); } \
  VEXA_GL_GUEST_EXPORT void glEvalCoord2##suffix##v(const type* uv) { if(uv)VexaEvalCoord(uv[0],uv[1],true); }
VEXA_EVAL_COORD(d, GLdouble)
VEXA_EVAL_COORD(f, GLfloat)
#undef VEXA_EVAL_COORD
VEXA_GL_GUEST_EXPORT void glEvalPoint1(GLint i) {
  vexa_ff::run_or_record_callback([i] {
    const auto& grid = vexa_ff::S().evaluator.grid1;
    VexaEvalCoord(grid.u1 + (grid.u2-grid.u1)*i/grid.un,0,false);
  });
}
VEXA_GL_GUEST_EXPORT void glEvalPoint2(GLint i, GLint j) {
  vexa_ff::run_or_record_callback([i,j] {
    const auto& grid = vexa_ff::S().evaluator.grid2;
    VexaEvalCoord(grid.u1 + (grid.u2-grid.u1)*i/grid.un,grid.v1 + (grid.v2-grid.v1)*j/grid.vn,true);
  });
}
VEXA_GL_GUEST_EXPORT void glEvalMesh1(GLenum mode, GLint i1, GLint i2) {
  if (vexa_ff::in_begin()) { fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return; }
  if (mode != 0x1B00 && mode != 0x1B01) { fexfn_pack_VEXA_CompatError(GL_INVALID_ENUM); return; }
  vexa_ff::run_or_record_callback([=] {
    vexa_eval::Capture capture(VexaEvalCurrent());
    const auto error = vexa_eval::mesh1(vexa_ff::S().evaluator,mode,i1,i2,capture);
    if (error) fexfn_pack_VEXA_CompatError(error); else VexaEvalDraw(capture);
  });
}
VEXA_GL_GUEST_EXPORT void glEvalMesh2(GLenum mode, GLint i1, GLint i2, GLint j1, GLint j2) {
  if (vexa_ff::in_begin()) { fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return; }
  if (mode != 0x1B00 && mode != 0x1B01 && mode != 0x1B02) { fexfn_pack_VEXA_CompatError(GL_INVALID_ENUM); return; }
  vexa_ff::run_or_record_callback([=] {
    vexa_eval::Capture capture(VexaEvalCurrent());
    const auto error = vexa_eval::mesh2(vexa_ff::S().evaluator,mode,i1,i2,j1,j2,capture);
    if (error) fexfn_pack_VEXA_CompatError(error); else VexaEvalDraw(capture);
  });
}
