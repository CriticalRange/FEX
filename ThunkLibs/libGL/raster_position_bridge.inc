// Guest integration: execute display-list raster commands with replay-time state.
static vexa_raster::Position& VexaRasterPosition() { return vexa_ff::S().raster; }
static vexa_raster::Input VexaRasterInput() {
  vexa_raster::Input input;
  std::copy_n(vexa_ff::S().model, 16, input.modelview.begin());
  std::copy_n(vexa_ff::S().projection, 16, input.projection.begin());
  std::copy_n(vexa_ff::S().color, 4, input.color.begin());
  fexfn_pack_glGetIntegerv(GL_VIEWPORT, input.viewport.data());
  GLfloat range[2]{};
  fexfn_pack_glGetFloatv(GL_DEPTH_RANGE, range);
  std::copy_n(range, 2, input.depth_range.begin());
  GLfloat texcoord[4]{};
  fexfn_pack_glGetVertexAttribfv(8, GL_CURRENT_VERTEX_ATTRIB, texcoord);
  std::copy_n(texcoord, 4, input.texcoord.begin());
  for (unsigned i = 0; i < 6; ++i) {
    input.clip_enabled[i] = fexfn_pack_glIsEnabled(0x3000 + i);
    if (input.clip_enabled[i]) fexfn_pack_glGetClipPlane(0x3000 + i, input.clip_planes[i].data());
  }
  return input;
}
static bool VexaRasterRecord(const vexa_raster::Vec4& object) {
  if (vexa_ff::in_begin()) {
    fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION);
    return true;
  }
  if (!vexa_ff::S().compiling) return false;
  vexa_ff::run_or_record_callback([object] {
    vexa_raster::raster_pos(VexaRasterPosition(), VexaRasterInput(), object);
  });
  return true;
}
#include "raster_position_guest.inc"
