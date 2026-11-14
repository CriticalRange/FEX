#include "selection_draw_core.h"
static bool VexaSelectOutsideBegin() {
  if(!vexa_ff::in_begin())return true;
  fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION);return false;
}
static void VexaSelectError(unsigned error) { if(error)fexfn_pack_VEXA_CompatError(error); }
static void VexaSelectionDraw(const std::vector<float>& data,unsigned mode) {
  GLint cullFace=0x0405,frontFace=0x0901;
  fexfn_pack_glGetIntegerv(GL_CULL_FACE_MODE,&cullFace);fexfn_pack_glGetIntegerv(GL_FRONT_FACE,&frontFace);
  vexa_select::draw(vexa_ff::S().selection,data,mode,VexaRasterInput(),fexfn_pack_glIsEnabled(GL_CULL_FACE),cullFace,frontFace);
}
VEXA_GL_GUEST_EXPORT void glSelectBuffer(GLsizei size,GLuint* buffer) {
  if(VexaSelectOutsideBegin())VexaSelectError(vexa_ff::S().selection.selectBuffer(size,buffer));
}
VEXA_GL_GUEST_EXPORT void glFeedbackBuffer(GLsizei size,GLenum type,GLfloat* buffer) {
  if(VexaSelectOutsideBegin())VexaSelectError(vexa_ff::S().selection.feedbackBuffer(size,type,buffer));
}
VEXA_GL_GUEST_EXPORT GLint glRenderMode(GLenum mode) {
  if(!VexaSelectOutsideBegin())return 0;
  int result=0;const auto error=vexa_ff::S().selection.renderMode(mode,result);
  VexaSelectError(error);vexa_ff::S().selection_draw=VexaSelectionDraw;return result;
}
VEXA_GL_GUEST_EXPORT void glInitNames() {
  if(VexaSelectOutsideBegin())vexa_ff::run_or_record_callback([]{VexaSelectError(vexa_ff::S().selection.initNames());});
}
VEXA_GL_GUEST_EXPORT void glPushName(GLuint name) {
  if(VexaSelectOutsideBegin())vexa_ff::run_or_record_callback([name]{VexaSelectError(vexa_ff::S().selection.pushName(name));});
}
VEXA_GL_GUEST_EXPORT void glPopName() {
  if(VexaSelectOutsideBegin())vexa_ff::run_or_record_callback([]{VexaSelectError(vexa_ff::S().selection.popName());});
}
VEXA_GL_GUEST_EXPORT void glLoadName(GLuint name) {
  if(VexaSelectOutsideBegin())vexa_ff::run_or_record_callback([name]{VexaSelectError(vexa_ff::S().selection.loadName(name));});
}
VEXA_GL_GUEST_EXPORT void glPassThrough(GLfloat token) {
  if(VexaSelectOutsideBegin())vexa_ff::run_or_record_callback([token]{vexa_ff::S().selection.passThrough(token);});
}
