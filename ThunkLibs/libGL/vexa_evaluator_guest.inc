// Include after android_fixed_function.h and after the GL guest export macro.
// Integration requirements: State::evaluator and run_or_record_callback must
// exist in vexa_ff; caller must reject map commands inside glBegin/glEnd,
// including while compiling display lists. See vexa_evaluator_state.h.
#include <algorithm>

namespace vexa_eval_guest {
inline void error(unsigned code) { if (code) fexfn_pack_VEXA_CompatError(code); }

template<class T>
void map1(GLenum target, GLdouble u1, GLdouble u2, GLint stride, GLint order, const T* points) {
  if (vexa_ff::in_begin()) {error(0x0502);return;}
  vexa_eval::State temporary;
  const unsigned failure=temporary.set1(target,u1,u2,stride,order,points);
  if (failure) { error(failure); return; }
  const int slot=vexa_eval::target_slot(target,1);
  auto map=std::move(temporary.map1[slot]);
  vexa_ff::run_or_record_callback([slot,map=std::move(map)] {vexa_ff::S().evaluator.map1[slot]=map;});
}
template<class T>
void map2(GLenum target, GLdouble u1, GLdouble u2, GLint ustride, GLint uorder,
          GLdouble v1, GLdouble v2, GLint vstride, GLint vorder, const T* points) {
  if (vexa_ff::in_begin()) {error(0x0502);return;}
  vexa_eval::State temporary;
  const unsigned failure=temporary.set2(target,u1,u2,ustride,uorder,v1,v2,vstride,vorder,points);
  if (failure) { error(failure); return; }
  const int slot=vexa_eval::target_slot(target,2);
  auto map=std::move(temporary.map2[slot]);
  vexa_ff::run_or_record_callback([slot,map=std::move(map)] {vexa_ff::S().evaluator.map2[slot]=map;});
}
template<class T>
void query(GLenum target, GLenum query, T* output) {
  if (vexa_ff::in_begin()) {error(0x0502);return;}
  if (!output) return;
  std::vector<double> values;
  const unsigned failure=vexa_ff::S().evaluator.query(target,query,values);
  if (failure) { error(failure); return; }
  for (size_t i=0;i<values.size();++i) output[i]=static_cast<T>(values[i]);
}
// GL conversion to integer rounds to nearest rather than truncating. Values
// outside GLint range saturate; NaN is left to the implementation as zero.
template<>
inline void query<GLint>(GLenum target, GLenum name, GLint* output) {
  if (vexa_ff::in_begin()) {error(0x0502);return;}
  if (!output) return;
  std::vector<double> values;
  const unsigned failure=vexa_ff::S().evaluator.query(target,name,values);
  if (failure) { error(failure); return; }
  for (size_t i=0;i<values.size();++i) {
    double v=std::isnan(values[i]) ? 0.0 : std::round(values[i]);
    v=std::max<double>(std::numeric_limits<GLint>::min(),std::min<double>(std::numeric_limits<GLint>::max(),v));
    output[i]=static_cast<GLint>(v);
  }
}
inline void grid1(GLint n, double a, double b) {
  if (vexa_ff::in_begin()) {error(0x0502);return;}
  if(n<1) {error(vexa_eval::kInvalidValue);return;}
  vexa_ff::run_or_record_callback([=] {vexa_ff::S().evaluator.set_grid1(n,a,b);});
}
inline void grid2(GLint n, double a, double b, GLint m, double c, double d) {
  if (vexa_ff::in_begin()) {error(0x0502);return;}
  if(n<1 || m<1) {error(vexa_eval::kInvalidValue);return;}
  vexa_ff::run_or_record_callback([=] {vexa_ff::S().evaluator.set_grid2(n,a,b,m,c,d);});
}
}

VEXA_GL_GUEST_EXPORT void glMap1d(GLenum t,GLdouble a,GLdouble b,GLint s,GLint n,const GLdouble* p) {vexa_eval_guest::map1(t,a,b,s,n,p);}
VEXA_GL_GUEST_EXPORT void glMap1f(GLenum t,GLfloat a,GLfloat b,GLint s,GLint n,const GLfloat* p) {vexa_eval_guest::map1(t,a,b,s,n,p);}
VEXA_GL_GUEST_EXPORT void glMap2d(GLenum t,GLdouble a,GLdouble b,GLint s,GLint n,GLdouble c,GLdouble d,GLint q,GLint m,const GLdouble* p) {vexa_eval_guest::map2(t,a,b,s,n,c,d,q,m,p);}
VEXA_GL_GUEST_EXPORT void glMap2f(GLenum t,GLfloat a,GLfloat b,GLint s,GLint n,GLfloat c,GLfloat d,GLint q,GLint m,const GLfloat* p) {vexa_eval_guest::map2(t,a,b,s,n,c,d,q,m,p);}
VEXA_GL_GUEST_EXPORT void glGetMapdv(GLenum t,GLenum q,GLdouble* p) {vexa_eval_guest::query(t,q,p);}
VEXA_GL_GUEST_EXPORT void glGetMapfv(GLenum t,GLenum q,GLfloat* p) {vexa_eval_guest::query(t,q,p);}
VEXA_GL_GUEST_EXPORT void glGetMapiv(GLenum t,GLenum q,GLint* p) {vexa_eval_guest::query(t,q,p);}
VEXA_GL_GUEST_EXPORT void glMapGrid1d(GLint n,GLdouble a,GLdouble b) {vexa_eval_guest::grid1(n,a,b);}
VEXA_GL_GUEST_EXPORT void glMapGrid1f(GLint n,GLfloat a,GLfloat b) {vexa_eval_guest::grid1(n,a,b);}
VEXA_GL_GUEST_EXPORT void glMapGrid2d(GLint n,GLdouble a,GLdouble b,GLint m,GLdouble c,GLdouble d) {vexa_eval_guest::grid2(n,a,b,m,c,d);}
VEXA_GL_GUEST_EXPORT void glMapGrid2f(GLint n,GLfloat a,GLfloat b,GLint m,GLfloat c,GLfloat d) {vexa_eval_guest::grid2(n,a,b,m,c,d);}
