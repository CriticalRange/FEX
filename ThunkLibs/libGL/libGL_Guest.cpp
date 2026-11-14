/*
$info$
tags: thunklibs|GL
desc: Handles glXGetProcAddress
$end_info$
*/

#define GL_GLEXT_PROTOTYPES 1
#define GLX_GLXEXT_PROTOTYPES 1

// Ubuntu's x86_64 build sysroot defaults fmod to GLIBC_2.38, while the
// Proton runtime still supplies the original ABI. Both compute the same
// remainder; keep the guest thunk loadable on that older rootfs.
#if defined(__x86_64__) && defined(__linux__)
asm(".symver fmod,fmod@GLIBC_2.2.5");
#endif

// VEXA_FIXES: Android NDK sysroot doesn't provide desktop GLX headers.
#ifdef BUILD_ANDROID
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#include <fcntl.h>
#include <unistd.h>
#if __has_include(<android/log.h>)
#include <android/log.h>
#define VEXA_HAS_ANDROID_LOG 1
#else
#define VEXA_HAS_ANDROID_LOG 0
#endif
// VEXA_FIXES: Legacy ARB shader entrypoints still appear in thunk interfaces.
using GLhandleARB = unsigned int;
using GLcharARB = char;
using __GLXextFuncPtr = void (*)();
#else
#include <GL/glx.h>
#include <GL/glxext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#undef GL_ARB_viewport_array
#include "glcorearb.h"

#include <dlfcn.h>
#include <atomic>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "common/Guest.h"

#ifndef VEXA_GL_GUEST_EXPORT
#define VEXA_GL_GUEST_EXPORT __attribute__((visibility("default")))
#endif

#ifdef BUILD_ANDROID
// VEXA_FIXES: Desktop GL 4.3 makes these core, and this GLES driver does not
// export them. Unity treats the context as desktop GL (GLES: 0) and calls the
// pointer unconditionally. Returning NULL becomes a guest call to address 0:
// an empty L1 slot matches GuestCode 0 and the JIT blr's a null host pointer.
// A no-op keeps init alive. These names are still absent from the extension
// string, so caps that consult the string stay off.
#define VEXA_NOOP_HIDDEN(name)                                                 \
  extern "C" void VexaNoop_##name() {                                          \
    static std::atomic<int> once {0};                                          \
    if (once.fetch_add(1, std::memory_order_relaxed) == 0) {                   \
      std::fprintf(stderr, "[libGL thunk] noop " #name "\n");                  \
    }                                                                          \
  }
VEXA_NOOP_HIDDEN(glTextureView)
VEXA_NOOP_HIDDEN(glTexPageCommitment)
VEXA_NOOP_HIDDEN(glPatchParameteri)
#undef VEXA_NOOP_HIDDEN

static uintptr_t VexaHiddenCoreNoop(std::string_view name) {
  if (name == "glTextureView" || name == "glTextureViewEXT" || name == "glTextureViewOES") {
    return reinterpret_cast<uintptr_t>(VexaNoop_glTextureView);
  }
  if (name == "glTexPageCommitmentARB" || name == "glTexPageCommitmentEXT") {
    return reinterpret_cast<uintptr_t>(VexaNoop_glTexPageCommitment);
  }
  if (name == "glPatchParameteri" || name == "glPatchParameteriEXT" || name == "glPatchParameteriOES") {
    return reinterpret_cast<uintptr_t>(VexaNoop_glPatchParameteri);
  }
  return 0;
}
#endif

#if defined(BUILD_ANDROID)
static int VexaGLGuestFprintf(FILE* stream, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  const int rc = std::vfprintf(stream, fmt, args);
  va_end(args);
  if (stream == stderr) {
#if VEXA_HAS_ANDROID_LOG
    __android_log_vprint(ANDROID_LOG_ERROR, "Vexa-ThunkGLGuest", fmt, copy);
#endif
  }
  va_end(copy);
  return rc;
}
#define fprintf(stream, ...) VexaGLGuestFprintf(stream, __VA_ARGS__)
#endif

#include "thunkgen_guest_libGL.inl"

#ifdef BUILD_ANDROID
#define VEXA_FF_LIGHTING_BRIDGE 1
#include "android_fixed_function.h"
#undef VEXA_GL_GUEST_EXPORT
#define VEXA_GL_GUEST_EXPORT extern "C" __attribute__((visibility("default")))
#include "raster_position_bridge.inc"
#include "legacy_lighting_guest.inc"
#include "pixel_transfer_bridge.inc"
#include "legacy_rgb_guest.inc"
#include "evaluator_draw_guest.inc"
#include "selection_guest.inc"
#include "array_element_guest.inc"
#include "stipple_guest.inc"
#include "unsigned_byte_attrib_guest.inc"
static void VexaSetTexCoord4f(GLuint, GLfloat s, GLfloat t, GLfloat r, GLfloat q) {
  vexa_ff::run_or_record_callback([=] {
    const GLfloat values[]{s,t,r,q};
    std::copy_n(values, 4, vexa_ff::S().texcoord);
    fexfn_pack_glVertexAttrib4f(8,s,t,r,q);
  });
}
#undef VEXA_GL_GUEST_EXPORT
#define VEXA_GL_GUEST_EXPORT __attribute__((visibility("default")))
#endif

typedef void voidFunc();

#ifdef BUILD_ANDROID
namespace {
// VEXA_FIXES: Persist unresolved GL proc diagnostics in app-internal artifacts storage.
constexpr const char* GLProcTracePath = "/data/user/0/com.critical.vexaemulator/files/artifacts/gl_proc_trace.log";

std::mutex MissingGLProcMutex;
char LastMissingGLProc[256] {};
char LastMissingGLProcReason[64] {};

void LogMissingGLProcEvent(const char* phase, const char* procname, const char* reason, const void* host_target = nullptr) {
  char proc_copy[sizeof(LastMissingGLProc)] {};
  char reason_copy[sizeof(LastMissingGLProcReason)] {};

  {
    std::lock_guard lk(MissingGLProcMutex);
    if (procname && *procname) {
      std::snprintf(LastMissingGLProc, sizeof(LastMissingGLProc), "%s", procname);
    } else {
      std::snprintf(LastMissingGLProc, sizeof(LastMissingGLProc), "<null>");
    }

    if (reason && *reason) {
      std::snprintf(LastMissingGLProcReason, sizeof(LastMissingGLProcReason), "%s", reason);
    } else {
      std::snprintf(LastMissingGLProcReason, sizeof(LastMissingGLProcReason), "unknown");
    }

    std::snprintf(proc_copy, sizeof(proc_copy), "%s", LastMissingGLProc);
    std::snprintf(reason_copy, sizeof(reason_copy), "%s", LastMissingGLProcReason);
  }

  fprintf(stderr,
               "[libGL thunk] %s unresolved GL proc '%s' (%s, host=%p)\n",
               phase ? phase : "log",
               proc_copy,
               reason_copy,
               host_target);

  if (FILE* f = std::fopen(GLProcTracePath, "a")) {
    fprintf(f,
                 "[libGL thunk] %s unresolved GL proc '%s' (%s, host=%p)\n",
                 phase ? phase : "log",
                 proc_copy,
                 reason_copy,
                 host_target);
    std::fclose(f);
  }
}

extern "C" uintptr_t MissingGLProcStub() {
  char proc_copy[sizeof(LastMissingGLProc)] {};
  char reason_copy[sizeof(LastMissingGLProcReason)] {};

  {
    std::lock_guard lk(MissingGLProcMutex);
    if (LastMissingGLProc[0] == 0) {
      std::snprintf(LastMissingGLProc, sizeof(LastMissingGLProc), "<unset>");
    }
    if (LastMissingGLProcReason[0] == 0) {
      std::snprintf(LastMissingGLProcReason, sizeof(LastMissingGLProcReason), "unknown");
    }
    std::snprintf(proc_copy, sizeof(proc_copy), "%s", LastMissingGLProc);
    std::snprintf(reason_copy, sizeof(reason_copy), "%s", LastMissingGLProcReason);
  }

  static std::atomic<int> logs {0};
  const int n = logs.fetch_add(1, std::memory_order_relaxed);
  if (n < 24) {
    fprintf(stderr,
                 "[libGL thunk] invoked unresolved GL proc '%s' (%s, host=%p)\n",
                 proc_copy,
                 reason_copy,
                 nullptr);
  } else if (n == 24) {
    fprintf(stderr, "[libGL thunk] further unresolved GL calls suppressed (last='%s')\n", proc_copy);
  }

  return 0;
}
}
#endif

namespace {
using GLGetFramebufferAttachmentParameterivFn = void (*)(GLenum, GLenum, GLenum, GLint*);

std::atomic<uint64_t> GLGuestTraceSeq {0};

// Per-call and per-frame trace lines cost real frame time, so they are opt-in via VEXA_GL_TRACE=1.
bool GLGuestTraceEnabled() {
  static const bool Enabled = [] {
    const char* v = std::getenv("VEXA_GL_TRACE");
    return v && v[0] && v[0] != '0';
  }();
  return Enabled;
}

struct ScopedGLGuestTrace {
  explicit ScopedGLGuestTrace(const char* fn_)
    : fn {fn_ ? fn_ : "<null>"}
    , seq {0} {
    if (!GLGuestTraceEnabled()) return;
    seq = GLGuestTraceSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
    fprintf(stderr,
                 "[libGL-guest] call-enter seq=%llu fn=%s\n",
                 static_cast<unsigned long long>(seq),
                 fn);
  }

  ~ScopedGLGuestTrace() {
    if (!GLGuestTraceEnabled()) return;
    fprintf(stderr,
                 "[libGL-guest] call-exit seq=%llu fn=%s\n",
                 static_cast<unsigned long long>(seq),
                 fn);
  }

  const char* fn;
  uint64_t seq;
};

#define GLGUEST_TRACE_SCOPE() ScopedGLGuestTrace vexa_gl_guest_trace_scope_##__LINE__ {__func__}


std::atomic<GLGetFramebufferAttachmentParameterivFn> CachedGLGetFramebufferAttachmentParameteriv {nullptr};
std::atomic<bool> CachedGLGetFramebufferAttachmentParameterivResolved {false};
std::atomic<uint32_t> GLProcLinkTraceBudget {512};
std::atomic<bool> GLFramebufferAttachCompatPrelinked {false};

#ifdef BUILD_ANDROID
extern "C" {
void* glXChooseVisual(void* display, int screen, int* attributes);
void* glXCreateContext(void* display, void* visual, void* share_list, int direct);
void* glXCreateContextAttribsARB(void* display, void* config, void* share_list, int direct, const int* attribs);
void VexaCompat_glFramebufferTexture3D(unsigned int target, unsigned int attachment, unsigned int textarget,
                                       unsigned int texture, int level, int zoffset);
void VexaCompat_glFramebufferTexture1D(unsigned int target, unsigned int attachment, unsigned int textarget,
                                       unsigned int texture, int level);
void glXDestroyContext(void* display, void* context);
void* glXGetCurrentContext();
unsigned long glXGetCurrentDrawable();
int glXMakeCurrent(void* display, unsigned long drawable, void* context);
void glXSwapBuffers(void* display, unsigned long drawable);
void glXQueryDrawable(void* display, unsigned long drawable, int attribute, unsigned int* value);
void glBegin(GLenum mode);
void glEnd(void);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glNormal3f(GLfloat x, GLfloat y, GLfloat z);
void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glTranslated(GLdouble x, GLdouble y, GLdouble z);
void glPushMatrix(void);
void glPopMatrix(void);
void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble znear, GLdouble zfar);
GLuint glGenLists(GLsizei range);
void glNewList(GLuint list, GLenum mode);
void glEndList(void);
void glCallList(GLuint list);
void glDeleteLists(GLuint list, GLsizei range);
void glShadeModel(GLenum mode);
void glLightfv(GLenum light, GLenum pname, const GLfloat* params);
void glMaterialfv(GLenum face, GLenum pname, const GLfloat* params);
void glTexEnvf(GLenum target, GLenum pname, GLfloat param);
void glTexEnvi(GLenum target, GLenum pname, GLint param);
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat* params);
void glTexEnviv(GLenum target, GLenum pname, const GLint* params);
const char* glXQueryExtensionsString(void* display, int screen);
void* glXGetCurrentDisplay();
void* glXChooseFBConfig(void* display, int screen, const int* attributes, int* num_items);
unsigned long glXCreatePbuffer(void* display, void* config, const int* attrib_list);
void glXDestroyPbuffer(void* display, unsigned long pbuffer);
void* glXGetVisualFromFBConfig(void* display, void* config);
int glXQueryVersion(void* display, int* major, int* minor);
int glXGetConfig(void* display, void* visual, int attribute, int* value);
int glXIsDirect(void* display, void* context);
const char* glXGetClientString(void* display, int name);
const char* glXQueryServerString(void* display, int screen, int name);
void* glXCreateNewContext(void* display, void* config, int render_type, void* share_list, int direct);
int glXGetFBConfigAttrib(void* display, void* config, int attribute, int* value);
void* glXGetFBConfigs(void* display, int screen, int* nelements);
int glXMakeContextCurrent(void* display, unsigned long draw, unsigned long read, void* context);
int glXQueryExtension(void* display, int* error_base, int* event_base);
void glXCopyContext(void* display, void* src, void* dst, unsigned long mask);
unsigned long glXCreateGLXPixmap(void* display, void* visual, unsigned long pixmap);
void glXDestroyGLXPixmap(void* display, unsigned long pixmap);
void glXUseXFont(unsigned long font, int first, int count, int list_base);
void glXWaitGL(void);
void glXWaitX(void);
unsigned long glXCreatePixmap(void* display, void* config, unsigned long pixmap, const int* attribs);
unsigned long glXCreateWindow(void* display, void* config, unsigned long window, const int* attribs);
void glXDestroyPixmap(void* display, unsigned long pixmap);
void glXDestroyWindow(void* display, unsigned long window);
unsigned long glXGetCurrentReadDrawable(void);
int glXQueryContext(void* display, void* context, int attribute, int* value);
void glXSelectEvent(void* display, unsigned long drawable, unsigned long mask);
void glXGetSelectedEvent(void* display, unsigned long drawable, unsigned long* mask);
void glXSwapIntervalEXT(void* display, unsigned long drawable, int interval);
int glXGetSwapIntervalMESA(void);
int glXSwapIntervalMESA(unsigned int interval);
void glLineWidth(GLfloat width);
void glPolygonMode(GLenum face, GLenum mode);
void glTexParameteriv(GLenum target, GLenum pname, const GLint* params);
void glTexImage1D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLint border,
                  GLenum format, GLenum type, const void* pixels);
void glDrawBuffer(GLenum mode);
void glFinish(void);
void glLogicOp(GLenum opcode);
void glPixelStoref(GLenum pname, GLfloat param);
void glGetDoublev(GLenum pname, GLdouble* params);
void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* pixels);
void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat* params);
void glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat* params);
void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* params);
GLboolean glIsEnabled(GLenum cap);
void glDepthRange(GLdouble z_near, GLdouble z_far);
void glGetPointerv(GLenum pname, void** params);
void glCopyTexImage1D(GLenum target, GLint level, GLenum internalformat,
                      GLint x, GLint y, GLsizei width, GLint border);
void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat,
                      GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
void glCopyTexSubImage1D(GLenum target, GLint level, GLint xoffset,
                         GLint x, GLint y, GLsizei width);
void glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width,
                     GLenum format, GLenum type, const void* pixels);
GLboolean glIsTexture(GLuint texture);
}
#endif

void TraceGLProcLink(std::string_view procname, const void* host_target, uintptr_t guest_target) {
  uint32_t remaining = GLProcLinkTraceBudget.load(std::memory_order_acquire);
  while (remaining > 0 &&
         !GLProcLinkTraceBudget.compare_exchange_weak(
             remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  if (remaining == 0) {
    return;
  }

  fprintf(stderr,
               "[libGL thunk] link '%.*s' host=%p guest=%p\n",
               static_cast<int>(procname.size()),
               procname.data(),
               host_target,
               reinterpret_cast<void*>(guest_target));
#ifdef BUILD_ANDROID
  // VEXA_FIXES: Mirror link-map diagnostics to artifacts so Android sessions
  // can be triaged without full-time logcat capture.
  if (FILE* f = std::fopen(GLProcTracePath, "a")) {
    fprintf(f,
                 "[libGL thunk] link '%.*s' host=%p guest=%p\n",
                 static_cast<int>(procname.size()),
                 procname.data(),
                 host_target,
                 reinterpret_cast<void*>(guest_target));
    std::fclose(f);
  }
#endif
}

GLGetFramebufferAttachmentParameterivFn ResolveHostGLGetFramebufferAttachmentParameteriv() {
  if (CachedGLGetFramebufferAttachmentParameterivResolved.load(std::memory_order_acquire)) {
    return CachedGLGetFramebufferAttachmentParameteriv.load(std::memory_order_acquire);
  }

  GLGetFramebufferAttachmentParameterivFn fn = reinterpret_cast<GLGetFramebufferAttachmentParameterivFn>(
      fexfn_pack_glXGetProcAddress(reinterpret_cast<const GLubyte*>("glGetFramebufferAttachmentParameteriv")));
  if (!fn) {
    fn = reinterpret_cast<GLGetFramebufferAttachmentParameterivFn>(
        fexfn_pack_glXGetProcAddress(reinterpret_cast<const GLubyte*>("glGetFramebufferAttachmentParameterivEXT")));
  }

  CachedGLGetFramebufferAttachmentParameteriv.store(fn, std::memory_order_release);
  CachedGLGetFramebufferAttachmentParameterivResolved.store(true, std::memory_order_release);
  return fn;
}

// VEXA_FIXES: Avoid recursive host lookup for framebuffer-attachment query in Android path.
void VexaCompat_glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint* params) {
  if (!params) {
    return;
  }

  (void)target;
  (void)attachment;
  // Do not forward this query to host proc on Android/FEX. The host address for
  // this symbol is also used as a thunk trampoline and can recurse back here.

  // Fallback for GLES drivers where desktop-style query proc is absent.
  // Conservative defaults keep guest initialization running but may differ visually.
  switch (pname) {
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:
      *params = GL_NONE;
      return;
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER:
      *params = 0;
      return;
    case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:
      *params = 8;
      return;
    case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE:
      *params = 24;
      return;
    case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE:
      *params = 8;
      return;
    case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING:
      *params = GL_LINEAR;
      return;
    case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:
      *params = GL_UNSIGNED_NORMALIZED;
      return;
    default:
      break;
  }

  *params = 0;
}

void EnsureFramebufferAttachmentCompatPrelinked() {
#if defined(BUILD_ANDROID)
  // VEXA_FIXES: Android path should not prelink this host proc address to a
  // raw compat function. Return compat directly from glXGetProcAddress branch.
  GLFramebufferAttachCompatPrelinked.store(true, std::memory_order_release);
  fprintf(stderr,
               "[libGL thunk] skipping framebuffer-attach prelink on Android\n");
  return;
#else
  if (GLFramebufferAttachCompatPrelinked.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  const auto guest_target = reinterpret_cast<uintptr_t>(VexaCompat_glGetFramebufferAttachmentParameteriv);

  auto host_target = fexfn_pack_glXGetProcAddress(reinterpret_cast<const GLubyte*>("glGetFramebufferAttachmentParameteriv"));
  if (host_target) {
    LinkAddressToFunction(reinterpret_cast<uintptr_t>(host_target), guest_target);
    TraceGLProcLink("glGetFramebufferAttachmentParameteriv(prelink)",
                    reinterpret_cast<const void*>(host_target),
                    guest_target);
  }

  host_target = fexfn_pack_glXGetProcAddress(reinterpret_cast<const GLubyte*>("glGetFramebufferAttachmentParameterivEXT"));
  if (host_target) {
    LinkAddressToFunction(reinterpret_cast<uintptr_t>(host_target), guest_target);
    TraceGLProcLink("glGetFramebufferAttachmentParameterivEXT(prelink)",
                    reinterpret_cast<const void*>(host_target),
                    guest_target);
  }
#endif
}

std::string_view ResolveGLInvokerAlias(std::string_view procname) {
  if (procname == "glBindBufferARB") return "glBindBuffer";
  if (procname == "glDeleteBuffersARB") return "glDeleteBuffers";
  if (procname == "glGenBuffersARB") return "glGenBuffers";
  if (procname == "glBufferDataARB") return "glBufferData";
  if (procname == "glBufferSubDataARB") return "glBufferSubData";
  if (procname == "glMapBufferARB") return "glMapBufferOES";
  if (procname == "glUnmapBufferARB") return "glUnmapBuffer";
  if (procname == "glQueryCounterEXT") {
    return "glQueryCounter";
  }
  if (procname == "glGetQueryObjectui64vEXT") {
    return "glGetQueryObjectui64v";
  }
  if (procname == "glGenQueriesEXT") {
    return "glGenQueries";
  }
  if (procname == "glDeleteQueriesEXT") {
    return "glDeleteQueries";
  }
  if (procname == "glBeginQueryEXT") {
    return "glBeginQuery";
  }
  if (procname == "glEndQueryEXT") {
    return "glEndQuery";
  }
  if (procname == "glGetQueryObjectuivEXT") {
    return "glGetQueryObjectuiv";
  }
  if (procname == "glGetQueryObjectivEXT") {
    return "glGetQueryObjectiv";
  }
  // GL_EXT_framebuffer_object names. Mali exposes the core GLES entry points
  // only. The signatures match, so the EXT lookup can call the core function.
  if (procname == "glGenerateMipmapEXT") return "glGenerateMipmap";
  if (procname == "glIsRenderbufferEXT") return "glIsRenderbuffer";
  if (procname == "glBindRenderbufferEXT") return "glBindRenderbuffer";
  if (procname == "glDeleteRenderbuffersEXT") return "glDeleteRenderbuffers";
  if (procname == "glGenRenderbuffersEXT") return "glGenRenderbuffers";
  if (procname == "glRenderbufferStorageEXT") return "glRenderbufferStorage";
  if (procname == "glGetRenderbufferParameterivEXT") return "glGetRenderbufferParameteriv";
  if (procname == "glIsFramebufferEXT") return "glIsFramebuffer";
  if (procname == "glBindFramebufferEXT") return "glBindFramebuffer";
  if (procname == "glDeleteFramebuffersEXT") return "glDeleteFramebuffers";
  if (procname == "glGenFramebuffersEXT") return "glGenFramebuffers";
  if (procname == "glCheckFramebufferStatusEXT") return "glCheckFramebufferStatus";
  if (procname == "glFramebufferTexture1DEXT") return "glFramebufferTexture1D";
  if (procname == "glFramebufferTexture2DEXT") return "glFramebufferTexture2D";
  if (procname == "glFramebufferTexture3DEXT") return "glFramebufferTexture3D";
  if (procname == "glFramebufferRenderbufferEXT") return "glFramebufferRenderbuffer";
  return {};
}

uintptr_t ResolveAndroidDirectGuestProc(std::string_view procname) {
  // VEXA_FIXES: Route clear-depth lookups directly to guest thunk entrypoints
  // to avoid problematic host-side dispatch stubs on some GLES drivers.
  if (procname == "glClearStencil") {
    return reinterpret_cast<uintptr_t>(fexfn_pack_glClearStencil);
  }
  if (procname == "glClearDepthf") {
    return reinterpret_cast<uintptr_t>(fexfn_pack_glClearDepthf);
  }
  if (procname == "glClearDepthfOES") {
    return reinterpret_cast<uintptr_t>(fexfn_pack_glClearDepthfOES);
  }
  if (procname == "glClearDepth") {
    return reinterpret_cast<uintptr_t>(fexfn_pack_glClearDepth);
  }
  if (procname == "glViewport") {
    return reinterpret_cast<uintptr_t>(fexfn_pack_glViewport);
  }
  if (procname == "glScissor") {
    return reinterpret_cast<uintptr_t>(fexfn_pack_glScissor);
  }
  if (procname == "glBlitFramebuffer") {
    return reinterpret_cast<uintptr_t>(fexfn_pack_glBlitFramebuffer);
  }
  if (procname == "glBlitFramebufferEXT") {
    return reinterpret_cast<uintptr_t>(fexfn_pack_glBlitFramebufferEXT);
  }
  return 0;
}

// Forward decls for compat exports defined at file end.
extern "C" {
void glClipPlane(GLenum, const GLdouble*);
void glColor4f(GLfloat,GLfloat,GLfloat,GLfloat);void glColor4sv(const GLshort*);void glTexCoord2f(GLfloat,GLfloat);void glPopAttrib(void);
void glOrtho(GLdouble,GLdouble,GLdouble,GLdouble,GLdouble,GLdouble);
void glLoadMatrixf(const GLfloat*);void glLoadMatrixd(const GLdouble*);
void glMultMatrixf(const GLfloat*);void glMultMatrixd(const GLdouble*);
void glScalef(GLfloat,GLfloat,GLfloat);void glScaled(GLdouble,GLdouble,GLdouble);
}
void glDepthBounds(GLclampd, GLclampd);
void glFrameTerminator(void);
void glPushGroupMarker(GLsizei, const GLchar*);
void glPopGroupMarker(void);
void glInsertEventMarker(GLsizei, const GLchar*);
void glLabelObject(GLenum, GLuint, GLsizei, const GLchar*);
void glRenderbufferStorageMultisampleCoverage(GLenum, GLsizei, GLsizei, GLenum, GLsizei, GLsizei);
static void VexaCompat_glTexImage2DMultisample(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean);
static void VexaCompat_glGetTextureParameteriv(GLuint, GLenum, GLint*);
static void VexaCompat_glGetTextureLevelParameteriv(GLuint, GLint, GLenum, GLint*);
// These are odr-used from this anonymous namespace. Definitions that live
// later, in the extern "C" export block, are different functions, so the
// linker leaves these references undefined. ETS2 dlopens libGL with RTLD_NOW
// and treats a failed dlopen as a null glXQueryVersion.
static void VexaWarnBaseInstance(const char* fn, GLuint baseinstance) {
  static int warned = 0;
  if (baseinstance != 0 && warned++ < 8) {
    fprintf(stderr, "[libGL thunk] %s baseinstance=%u emulated via vertex-binding offsets\n", fn, baseinstance);
  }
}
// Base-instance emulation runs natively in the host thunks (one transition).
static void VexaCompat_glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type,
                                                               const void* indices, GLsizei instancecount, GLuint baseinstance) {
  VexaWarnBaseInstance("glDrawElementsInstancedBaseInstance", baseinstance);
  ::glDrawElementsInstancedBaseInstance(mode, count, type, indices, instancecount, baseinstance);
}
static void VexaCompat_glDrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type,
                                                                         const void* indices, GLsizei instancecount,
                                                                         GLint basevertex, GLuint baseinstance) {
  VexaWarnBaseInstance("glDrawElementsInstancedBaseVertexBaseInstance", baseinstance);
  ::glDrawElementsInstancedBaseVertexBaseInstance(mode, count, type, indices, instancecount, basevertex, baseinstance);
}
static void VexaCompat_glDrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count,
                                                             GLsizei instancecount, GLuint baseinstance) {
  VexaWarnBaseInstance("glDrawArraysInstancedBaseInstance", baseinstance);
  ::glDrawArraysInstancedBaseInstance(mode, first, count, instancecount, baseinstance);
}
void glPolygonOffsetClamp(GLfloat factor, GLfloat units, GLfloat clamp) {
  (void)clamp;
  ::glPolygonOffset(factor, units);
}
int glXGetGPUIDsAMD(unsigned int, unsigned int*);
int glXGetGPUInfoAMD(unsigned int, int, GLenum, unsigned int, void*);
unsigned int glXGetContextGPUIDAMD(void*);
int glXSwapIntervalSGI(int);

uintptr_t ResolveAndroidCompatGuestProc(std::string_view procname) {
#ifdef BUILD_ANDROID
  if (auto attrib = VexaUnsignedByteAttribProc(procname)) return attrib;
#endif
  if(procname=="glDrawArrays" || procname=="glDrawArraysEXT")return reinterpret_cast<uintptr_t>(glDrawArrays);
  if(procname=="glDrawElements")return reinterpret_cast<uintptr_t>(glDrawElements);
  if (procname == "glArrayElement") return reinterpret_cast<uintptr_t>(glArrayElement);
  if (procname == "glCopyPixels") return reinterpret_cast<uintptr_t>(glCopyPixels);
  if (procname == "glEdgeFlagPointer") return reinterpret_cast<uintptr_t>(glEdgeFlagPointer);
  if (procname == "glSelectBuffer") return reinterpret_cast<uintptr_t>(glSelectBuffer);
  if (procname == "glFeedbackBuffer") return reinterpret_cast<uintptr_t>(glFeedbackBuffer);
  if (procname == "glGetPolygonStipple") return reinterpret_cast<uintptr_t>(glGetPolygonStipple);
  if (procname == "glIndexPointer") return reinterpret_cast<uintptr_t>(glIndexPointer);
  if (procname == "glInitNames") return reinterpret_cast<uintptr_t>(glInitNames);
  if (procname == "glLineStipple") return reinterpret_cast<uintptr_t>(glLineStipple);
  if (procname == "glLoadName") return reinterpret_cast<uintptr_t>(glLoadName);
  if (procname == "glPassThrough") return reinterpret_cast<uintptr_t>(glPassThrough);
  if (procname == "glPolygonStipple") return reinterpret_cast<uintptr_t>(glPolygonStipple);
  if (procname == "glPopName") return reinterpret_cast<uintptr_t>(glPopName);
  if (procname == "glPushName") return reinterpret_cast<uintptr_t>(glPushName);
  if (procname == "glRenderMode") return reinterpret_cast<uintptr_t>(glRenderMode);

  if (procname == "glEvalPoint1") return reinterpret_cast<uintptr_t>(glEvalPoint1);
  if (procname == "glEvalPoint2") return reinterpret_cast<uintptr_t>(glEvalPoint2);
  if (procname == "glEvalMesh1") return reinterpret_cast<uintptr_t>(glEvalMesh1);
  if (procname == "glEvalMesh2") return reinterpret_cast<uintptr_t>(glEvalMesh2);
  if (procname == "glEvalCoord1d") return reinterpret_cast<uintptr_t>(glEvalCoord1d);
  if (procname == "glEvalCoord1dv") return reinterpret_cast<uintptr_t>(glEvalCoord1dv);
  if (procname == "glEvalCoord1f") return reinterpret_cast<uintptr_t>(glEvalCoord1f);
  if (procname == "glEvalCoord1fv") return reinterpret_cast<uintptr_t>(glEvalCoord1fv);
  if (procname == "glEvalCoord2d") return reinterpret_cast<uintptr_t>(glEvalCoord2d);
  if (procname == "glEvalCoord2dv") return reinterpret_cast<uintptr_t>(glEvalCoord2dv);
  if (procname == "glEvalCoord2f") return reinterpret_cast<uintptr_t>(glEvalCoord2f);
  if (procname == "glEvalCoord2fv") return reinterpret_cast<uintptr_t>(glEvalCoord2fv);

  // Legacy entry points implemented entirely in guest state must bypass the GLES resolver.
  if (procname == "glAreTexturesResident") return reinterpret_cast<uintptr_t>(glAreTexturesResident);
  if (procname == "glBitmap") return reinterpret_cast<uintptr_t>(glBitmap);
  if (procname == "glClearIndex") return reinterpret_cast<uintptr_t>(glClearIndex);
  if (procname == "glColorMaterial") return reinterpret_cast<uintptr_t>(glColorMaterial);
  if (procname == "glDrawPixels") return reinterpret_cast<uintptr_t>(glDrawPixels);
  if (procname == "glEdgeFlag") return reinterpret_cast<uintptr_t>(glEdgeFlag);
  if (procname == "glEdgeFlagv") return reinterpret_cast<uintptr_t>(glEdgeFlagv);
  if (procname == "glFogf") return reinterpret_cast<uintptr_t>(glFogf);
  if (procname == "glFogfv") return reinterpret_cast<uintptr_t>(glFogfv);
  if (procname == "glFogi") return reinterpret_cast<uintptr_t>(glFogi);
  if (procname == "glFogiv") return reinterpret_cast<uintptr_t>(glFogiv);
  if (procname == "glGetLightfv") return reinterpret_cast<uintptr_t>(glGetLightfv);
  if (procname == "glGetLightiv") return reinterpret_cast<uintptr_t>(glGetLightiv);
  if (procname == "glGetMaterialfv") return reinterpret_cast<uintptr_t>(glGetMaterialfv);
  if (procname == "glGetMaterialiv") return reinterpret_cast<uintptr_t>(glGetMaterialiv);
  if (procname == "glGetPixelMapfv") return reinterpret_cast<uintptr_t>(glGetPixelMapfv);
  if (procname == "glGetPixelMapuiv") return reinterpret_cast<uintptr_t>(glGetPixelMapuiv);
  if (procname == "glGetPixelMapusv") return reinterpret_cast<uintptr_t>(glGetPixelMapusv);
  if (procname == "glGetTexEnvfv") return reinterpret_cast<uintptr_t>(glGetTexEnvfv);
  if (procname == "glGetTexEnviv") return reinterpret_cast<uintptr_t>(glGetTexEnviv);
  if (procname == "glGetTexGendv") return reinterpret_cast<uintptr_t>(glGetTexGendv);
  if (procname == "glGetTexGenfv") return reinterpret_cast<uintptr_t>(glGetTexGenfv);
  if (procname == "glGetTexGeniv") return reinterpret_cast<uintptr_t>(glGetTexGeniv);
  if (procname == "glIndexMask") return reinterpret_cast<uintptr_t>(glIndexMask);
  if (procname == "glIndexd") return reinterpret_cast<uintptr_t>(glIndexd);
  if (procname == "glIndexdv") return reinterpret_cast<uintptr_t>(glIndexdv);
  if (procname == "glIndexf") return reinterpret_cast<uintptr_t>(glIndexf);
  if (procname == "glIndexfv") return reinterpret_cast<uintptr_t>(glIndexfv);
  if (procname == "glIndexi") return reinterpret_cast<uintptr_t>(glIndexi);
  if (procname == "glIndexiv") return reinterpret_cast<uintptr_t>(glIndexiv);
  if (procname == "glIndexs") return reinterpret_cast<uintptr_t>(glIndexs);
  if (procname == "glIndexsv") return reinterpret_cast<uintptr_t>(glIndexsv);
  if (procname == "glIndexub") return reinterpret_cast<uintptr_t>(glIndexub);
  if (procname == "glIndexubv") return reinterpret_cast<uintptr_t>(glIndexubv);
  if (procname == "glLightModelf") return reinterpret_cast<uintptr_t>(glLightModelf);
  if (procname == "glLightModelfv") return reinterpret_cast<uintptr_t>(glLightModelfv);
  if (procname == "glLightModeli") return reinterpret_cast<uintptr_t>(glLightModeli);
  if (procname == "glLightModeliv") return reinterpret_cast<uintptr_t>(glLightModeliv);
  if (procname == "glLightf") return reinterpret_cast<uintptr_t>(glLightf);
  if (procname == "glLightfv") return reinterpret_cast<uintptr_t>(glLightfv);
  if (procname == "glLighti") return reinterpret_cast<uintptr_t>(glLighti);
  if (procname == "glLightiv") return reinterpret_cast<uintptr_t>(glLightiv);
  if (procname == "glMaterialf") return reinterpret_cast<uintptr_t>(glMaterialf);
  if (procname == "glMaterialfv") return reinterpret_cast<uintptr_t>(glMaterialfv);
  if (procname == "glMateriali") return reinterpret_cast<uintptr_t>(glMateriali);
  if (procname == "glMaterialiv") return reinterpret_cast<uintptr_t>(glMaterialiv);
  if (procname == "glPixelMapfv") return reinterpret_cast<uintptr_t>(glPixelMapfv);
  if (procname == "glPixelMapuiv") return reinterpret_cast<uintptr_t>(glPixelMapuiv);
  if (procname == "glPixelMapusv") return reinterpret_cast<uintptr_t>(glPixelMapusv);
  if (procname == "glPixelStorei") return reinterpret_cast<uintptr_t>(glPixelStorei);
  if (procname == "glPixelTransferf") return reinterpret_cast<uintptr_t>(glPixelTransferf);
  if (procname == "glPixelTransferi") return reinterpret_cast<uintptr_t>(glPixelTransferi);
  if (procname == "glPixelZoom") return reinterpret_cast<uintptr_t>(glPixelZoom);
  if (procname == "glPrioritizeTextures") return reinterpret_cast<uintptr_t>(glPrioritizeTextures);
  if (procname == "glRasterPos2d") return reinterpret_cast<uintptr_t>(glRasterPos2d);
  if (procname == "glRasterPos2dv") return reinterpret_cast<uintptr_t>(glRasterPos2dv);
  if (procname == "glRasterPos2f") return reinterpret_cast<uintptr_t>(glRasterPos2f);
  if (procname == "glRasterPos2fv") return reinterpret_cast<uintptr_t>(glRasterPos2fv);
  if (procname == "glRasterPos2i") return reinterpret_cast<uintptr_t>(glRasterPos2i);
  if (procname == "glRasterPos2iv") return reinterpret_cast<uintptr_t>(glRasterPos2iv);
  if (procname == "glRasterPos2s") return reinterpret_cast<uintptr_t>(glRasterPos2s);
  if (procname == "glRasterPos2sv") return reinterpret_cast<uintptr_t>(glRasterPos2sv);
  if (procname == "glRasterPos3d") return reinterpret_cast<uintptr_t>(glRasterPos3d);
  if (procname == "glRasterPos3dv") return reinterpret_cast<uintptr_t>(glRasterPos3dv);
  if (procname == "glRasterPos3f") return reinterpret_cast<uintptr_t>(glRasterPos3f);
  if (procname == "glRasterPos3fv") return reinterpret_cast<uintptr_t>(glRasterPos3fv);
  if (procname == "glRasterPos3i") return reinterpret_cast<uintptr_t>(glRasterPos3i);
  if (procname == "glRasterPos3iv") return reinterpret_cast<uintptr_t>(glRasterPos3iv);
  if (procname == "glRasterPos3s") return reinterpret_cast<uintptr_t>(glRasterPos3s);
  if (procname == "glRasterPos3sv") return reinterpret_cast<uintptr_t>(glRasterPos3sv);
  if (procname == "glRasterPos4d") return reinterpret_cast<uintptr_t>(glRasterPos4d);
  if (procname == "glRasterPos4dv") return reinterpret_cast<uintptr_t>(glRasterPos4dv);
  if (procname == "glRasterPos4f") return reinterpret_cast<uintptr_t>(glRasterPos4f);
  if (procname == "glRasterPos4fv") return reinterpret_cast<uintptr_t>(glRasterPos4fv);
  if (procname == "glRasterPos4i") return reinterpret_cast<uintptr_t>(glRasterPos4i);
  if (procname == "glRasterPos4iv") return reinterpret_cast<uintptr_t>(glRasterPos4iv);
  if (procname == "glRasterPos4s") return reinterpret_cast<uintptr_t>(glRasterPos4s);
  if (procname == "glRasterPos4sv") return reinterpret_cast<uintptr_t>(glRasterPos4sv);
  if (procname == "glTexEnvf") return reinterpret_cast<uintptr_t>(glTexEnvf);
  if (procname == "glTexEnvfv") return reinterpret_cast<uintptr_t>(glTexEnvfv);
  if (procname == "glTexEnvi") return reinterpret_cast<uintptr_t>(glTexEnvi);
  if (procname == "glTexEnviv") return reinterpret_cast<uintptr_t>(glTexEnviv);
  if (procname == "glTexGend") return reinterpret_cast<uintptr_t>(glTexGend);
  if (procname == "glTexGendv") return reinterpret_cast<uintptr_t>(glTexGendv);
  if (procname == "glTexGenf") return reinterpret_cast<uintptr_t>(glTexGenf);
  if (procname == "glTexGenfv") return reinterpret_cast<uintptr_t>(glTexGenfv);
  if (procname == "glTexGeni") return reinterpret_cast<uintptr_t>(glTexGeni);
  if (procname == "glTexGeniv") return reinterpret_cast<uintptr_t>(glTexGeniv);

  if(procname=="glColor4f")return reinterpret_cast<uintptr_t>(glColor4f);
  if(procname=="glColor4sv")return reinterpret_cast<uintptr_t>(glColor4sv);
  if(procname=="glTexCoord2f")return reinterpret_cast<uintptr_t>(glTexCoord2f);
  if(procname=="glPopAttrib")return reinterpret_cast<uintptr_t>(glPopAttrib);

  if(procname=="glClipPlane")return reinterpret_cast<uintptr_t>(glClipPlane);
  if(procname=="glOrtho")return reinterpret_cast<uintptr_t>(glOrtho);
  if(procname=="glLoadMatrixf")return reinterpret_cast<uintptr_t>(glLoadMatrixf);
  if(procname=="glLoadMatrixd")return reinterpret_cast<uintptr_t>(glLoadMatrixd);
  if(procname=="glMultMatrixf")return reinterpret_cast<uintptr_t>(glMultMatrixf);
  if(procname=="glMultMatrixd")return reinterpret_cast<uintptr_t>(glMultMatrixd);
  if(procname=="glScalef")return reinterpret_cast<uintptr_t>(glScalef);
  if(procname=="glScaled")return reinterpret_cast<uintptr_t>(glScaled);

#ifdef BUILD_ANDROID
  if (procname == "glGetFloatv") return reinterpret_cast<uintptr_t>(glGetFloatv);
  if (procname == "glGetIntegerv") return reinterpret_cast<uintptr_t>(glGetIntegerv);
  if (procname == "glGetBooleanv") return reinterpret_cast<uintptr_t>(glGetBooleanv);
  if (procname == "glEnable") return reinterpret_cast<uintptr_t>(glEnable);
  if (procname == "glDisable") return reinterpret_cast<uintptr_t>(glDisable);
  if (procname == "glIsEnabled") return reinterpret_cast<uintptr_t>(glIsEnabled);
  if (procname == "glXChooseVisual") return reinterpret_cast<uintptr_t>(glXChooseVisual);
  if (procname == "glXCreateContext") return reinterpret_cast<uintptr_t>(glXCreateContext);
  if (procname == "glXCreateContextAttribsARB") return reinterpret_cast<uintptr_t>(glXCreateContextAttribsARB);
  if (procname == "glFramebufferTexture3D" || procname == "glFramebufferTexture3DEXT") {
    return reinterpret_cast<uintptr_t>(VexaCompat_glFramebufferTexture3D);
  }
  if (procname == "glFramebufferTexture1D" || procname == "glFramebufferTexture1DEXT") {
    return reinterpret_cast<uintptr_t>(VexaCompat_glFramebufferTexture1D);
  }
  if (procname == "glXDestroyContext") return reinterpret_cast<uintptr_t>(glXDestroyContext);
  if (procname == "glXGetCurrentContext") return reinterpret_cast<uintptr_t>(glXGetCurrentContext);
  if (procname == "glXGetCurrentDrawable") return reinterpret_cast<uintptr_t>(glXGetCurrentDrawable);
  if (procname == "glXMakeCurrent") return reinterpret_cast<uintptr_t>(glXMakeCurrent);
  if (procname == "glXSwapBuffers") return reinterpret_cast<uintptr_t>(glXSwapBuffers);
  if (procname == "glXQueryDrawable") return reinterpret_cast<uintptr_t>(glXQueryDrawable);
  if (procname == "glXQueryExtensionsString") return reinterpret_cast<uintptr_t>(glXQueryExtensionsString);
  if (procname == "glXGetCurrentDisplay") return reinterpret_cast<uintptr_t>(glXGetCurrentDisplay);
  if (procname == "glXChooseFBConfig") return reinterpret_cast<uintptr_t>(glXChooseFBConfig);
  if (procname == "glXCreatePbuffer") return reinterpret_cast<uintptr_t>(glXCreatePbuffer);
  if (procname == "glXDestroyPbuffer") return reinterpret_cast<uintptr_t>(glXDestroyPbuffer);
  if (procname == "glXGetVisualFromFBConfig") return reinterpret_cast<uintptr_t>(glXGetVisualFromFBConfig);
  if (procname == "glXQueryVersion") return reinterpret_cast<uintptr_t>(glXQueryVersion);
  if (procname == "glXGetConfig") return reinterpret_cast<uintptr_t>(glXGetConfig);
  if (procname == "glXIsDirect") return reinterpret_cast<uintptr_t>(glXIsDirect);
  if (procname == "glXGetClientString") return reinterpret_cast<uintptr_t>(glXGetClientString);
  if (procname == "glXQueryServerString") return reinterpret_cast<uintptr_t>(glXQueryServerString);
  if (procname == "glXCreateNewContext") return reinterpret_cast<uintptr_t>(glXCreateNewContext);
  if (procname == "glXGetFBConfigAttrib") return reinterpret_cast<uintptr_t>(glXGetFBConfigAttrib);
  if (procname == "glXGetFBConfigs") return reinterpret_cast<uintptr_t>(glXGetFBConfigs);
  if (procname == "glXMakeContextCurrent") return reinterpret_cast<uintptr_t>(glXMakeContextCurrent);
  if (procname == "glXQueryExtension") return reinterpret_cast<uintptr_t>(glXQueryExtension);
  if (procname == "glXCopyContext") return reinterpret_cast<uintptr_t>(glXCopyContext);
  if (procname == "glXCreateGLXPixmap") return reinterpret_cast<uintptr_t>(glXCreateGLXPixmap);
  if (procname == "glXDestroyGLXPixmap") return reinterpret_cast<uintptr_t>(glXDestroyGLXPixmap);
  if (procname == "glXUseXFont") return reinterpret_cast<uintptr_t>(glXUseXFont);
  if (procname == "glXWaitGL") return reinterpret_cast<uintptr_t>(glXWaitGL);
  if (procname == "glXWaitX") return reinterpret_cast<uintptr_t>(glXWaitX);
  if (procname == "glXCreatePixmap") return reinterpret_cast<uintptr_t>(glXCreatePixmap);
  if (procname == "glXCreateWindow") return reinterpret_cast<uintptr_t>(glXCreateWindow);
  if (procname == "glXDestroyPixmap") return reinterpret_cast<uintptr_t>(glXDestroyPixmap);
  if (procname == "glXDestroyWindow") return reinterpret_cast<uintptr_t>(glXDestroyWindow);
  if (procname == "glXGetCurrentReadDrawable") return reinterpret_cast<uintptr_t>(glXGetCurrentReadDrawable);
  if (procname == "glXQueryContext") return reinterpret_cast<uintptr_t>(glXQueryContext);
  if (procname == "glXSelectEvent") return reinterpret_cast<uintptr_t>(glXSelectEvent);
  if (procname == "glXGetSelectedEvent") return reinterpret_cast<uintptr_t>(glXGetSelectedEvent);
  if (procname == "glXSwapIntervalEXT") return reinterpret_cast<uintptr_t>(glXSwapIntervalEXT);
  if (procname == "glXGetSwapIntervalMESA") return reinterpret_cast<uintptr_t>(glXGetSwapIntervalMESA);
  if (procname == "glXSwapIntervalMESA") return reinterpret_cast<uintptr_t>(glXSwapIntervalMESA);
  if (procname == "glLineWidth") return reinterpret_cast<uintptr_t>(glLineWidth);
  if (procname == "glPolygonMode") return reinterpret_cast<uintptr_t>(glPolygonMode);
  if (procname == "glTexParameteriv") return reinterpret_cast<uintptr_t>(glTexParameteriv);
  if (procname == "glTexImage1D") return reinterpret_cast<uintptr_t>(glTexImage1D);
  if (procname == "glDrawBuffer") return reinterpret_cast<uintptr_t>(glDrawBuffer);
  if (procname == "glFinish") return reinterpret_cast<uintptr_t>(glFinish);
  if (procname == "glLogicOp") return reinterpret_cast<uintptr_t>(glLogicOp);
  if (procname == "glPixelStoref") return reinterpret_cast<uintptr_t>(glPixelStoref);
  if (procname == "glGetDoublev") return reinterpret_cast<uintptr_t>(glGetDoublev);
  if (procname == "glGetTexImage") return reinterpret_cast<uintptr_t>(glGetTexImage);
  if (procname == "glGetTexParameterfv") return reinterpret_cast<uintptr_t>(glGetTexParameterfv);
  if (procname == "glGetTexLevelParameterfv") return reinterpret_cast<uintptr_t>(glGetTexLevelParameterfv);
  if (procname == "glGetTexLevelParameteriv") return reinterpret_cast<uintptr_t>(glGetTexLevelParameteriv);
  if (procname == "glIsEnabled") return reinterpret_cast<uintptr_t>(glIsEnabled);
  if (procname == "glDepthRange") return reinterpret_cast<uintptr_t>(glDepthRange);
  if (procname == "glGetPointerv") return reinterpret_cast<uintptr_t>(glGetPointerv);
  if (procname == "glCopyTexImage1D") return reinterpret_cast<uintptr_t>(glCopyTexImage1D);
  if (procname == "glCopyTexImage2D") return reinterpret_cast<uintptr_t>(glCopyTexImage2D);
  if (procname == "glCopyTexSubImage1D") return reinterpret_cast<uintptr_t>(glCopyTexSubImage1D);
  if (procname == "glTexSubImage1D") return reinterpret_cast<uintptr_t>(glTexSubImage1D);
  if (procname == "glCompressedTexImage1D") return reinterpret_cast<uintptr_t>(glCompressedTexImage1D);
  if (procname == "glCompressedTexSubImage1D") return reinterpret_cast<uintptr_t>(glCompressedTexSubImage1D);
  if (procname == "glGetCompressedTexImage") return reinterpret_cast<uintptr_t>(glGetCompressedTexImage);
  if (procname == "glGetBufferSubData") return reinterpret_cast<uintptr_t>(glGetBufferSubData);
  if (procname == "glGetQueryObjectiv") return reinterpret_cast<uintptr_t>(glGetQueryObjectiv);
  if (procname == "glBindFragDataLocation") return reinterpret_cast<uintptr_t>(glBindFragDataLocation);
  if (procname == "glBindFragDataLocationIndexed") return reinterpret_cast<uintptr_t>(glBindFragDataLocationIndexed);
  if (procname == "glGetActiveUniformName") return reinterpret_cast<uintptr_t>(glGetActiveUniformName);
  if (procname == "glMultiDrawElementsBaseVertex") return reinterpret_cast<uintptr_t>(glMultiDrawElementsBaseVertex);
  if (procname == "glMultiDrawArraysIndirect") return reinterpret_cast<uintptr_t>(glMultiDrawArraysIndirect);
  if (procname == "glMultiDrawElementsIndirect") return reinterpret_cast<uintptr_t>(glMultiDrawElementsIndirect);
  if (procname == "glBindBuffersBase") return reinterpret_cast<uintptr_t>(glBindBuffersBase);
  if (procname == "glBindBuffersRange") return reinterpret_cast<uintptr_t>(glBindBuffersRange);
  if (procname == "glBindTextures") return reinterpret_cast<uintptr_t>(glBindTextures);
  if (procname == "glBindSamplers") return reinterpret_cast<uintptr_t>(glBindSamplers);
  if (procname == "glBindImageTextures") return reinterpret_cast<uintptr_t>(glBindImageTextures);
  if (procname == "glBindVertexBuffers") return reinterpret_cast<uintptr_t>(glBindVertexBuffers);
  if (procname == "glInvalidateBufferData") return reinterpret_cast<uintptr_t>(glInvalidateBufferData);
  if (procname == "glInvalidateBufferSubData") return reinterpret_cast<uintptr_t>(glInvalidateBufferSubData);
  // NOTE: Do not advertise glDepthBounds (and the desktop-only names defined near the end of this file:
  // glClipControl, glVertexAttribLFormat, glXGetGPU*AMD, glXSwapIntervalSGI, ...) through this table.
  // Bisected on Megabonk (Unity, Vulkan mode): glDepthBounds resolving here, or glMapBuffer exported
  // from libGL, makes Unity take its desktop-GL path and crash (strlen(NULL) in SDL_GetDisplayName).
  // The functions stay exported for guests that dlsym() libGL directly (ETS2); glMapBuffer lives in
  // libvexa_glmap.so (LD_PRELOAD for ETS2 only).
  if (procname == "glIsTexture") return reinterpret_cast<uintptr_t>(glIsTexture);
  if (procname == "glBegin") return reinterpret_cast<uintptr_t>(glBegin);
  if (procname == "glEnd") return reinterpret_cast<uintptr_t>(glEnd);
  if (procname == "glVertex3f") return reinterpret_cast<uintptr_t>(glVertex3f);
  if (procname == "glNormal3f") return reinterpret_cast<uintptr_t>(glNormal3f);
  // Mali (GLES 3.2) has no base_instance. ETS2 draws most of its geometry through
  // these desktop entry points; without a guest entry they resolve to the no-op
  // MissingGLProcStub and the draw silently disappears.
  if (procname == "glDrawElementsInstancedBaseInstance" || procname == "glDrawElementsInstancedBaseInstanceEXT") {
    return reinterpret_cast<uintptr_t>(VexaCompat_glDrawElementsInstancedBaseInstance);
  }
  if (procname == "glDrawElementsInstancedBaseVertexBaseInstance" || procname == "glDrawElementsInstancedBaseVertexBaseInstanceEXT") {
    return reinterpret_cast<uintptr_t>(VexaCompat_glDrawElementsInstancedBaseVertexBaseInstance);
  }
  if (procname == "glDrawArraysInstancedBaseInstance" || procname == "glDrawArraysInstancedBaseInstanceEXT") {
    return reinterpret_cast<uintptr_t>(VexaCompat_glDrawArraysInstancedBaseInstance);
  }
  if (procname == "glPolygonOffsetClampEXT" || procname == "glPolygonOffsetClamp") {
    return reinterpret_cast<uintptr_t>(glPolygonOffsetClamp);
  }
  if (procname == "glMatrixMode") return reinterpret_cast<uintptr_t>(glMatrixMode);
  if (procname == "glLoadIdentity") return reinterpret_cast<uintptr_t>(glLoadIdentity);
  if (procname == "glTranslatef") return reinterpret_cast<uintptr_t>(glTranslatef);
  if (procname == "glRotatef") return reinterpret_cast<uintptr_t>(glRotatef);
  if (procname == "glTranslated") return reinterpret_cast<uintptr_t>(glTranslated);
  if (procname == "glPushMatrix") return reinterpret_cast<uintptr_t>(glPushMatrix);
  if (procname == "glPopMatrix") return reinterpret_cast<uintptr_t>(glPopMatrix);
  if (procname == "glFrustum") return reinterpret_cast<uintptr_t>(glFrustum);
  if (procname == "glGenLists") return reinterpret_cast<uintptr_t>(glGenLists);
  if (procname == "glNewList") return reinterpret_cast<uintptr_t>(glNewList);
  if (procname == "glEndList") return reinterpret_cast<uintptr_t>(glEndList);
  if (procname == "glCallList") return reinterpret_cast<uintptr_t>(glCallList);
  if (procname == "glDeleteLists") return reinterpret_cast<uintptr_t>(glDeleteLists);
  if (procname == "glShadeModel") return reinterpret_cast<uintptr_t>(glShadeModel);
  if (procname == "glLightfv") return reinterpret_cast<uintptr_t>(glLightfv);
  if (procname == "glMaterialfv") return reinterpret_cast<uintptr_t>(glMaterialfv);
  if (procname == "glTexEnvf") return reinterpret_cast<uintptr_t>(glTexEnvf);
  if (procname == "glTexEnvi") return reinterpret_cast<uintptr_t>(glTexEnvi);
  if (procname == "glTexEnvfv") return reinterpret_cast<uintptr_t>(glTexEnvfv);
  if (procname == "glTexEnviv") return reinterpret_cast<uintptr_t>(glTexEnviv);
#else
  (void)procname;
#endif
  return 0;
}
}

// Maps OpenGL API function names to the address of a guest function which is
// linked to the corresponding host function pointer
const std::unordered_map<std::string_view, uintptr_t /* guest function address */> HostPtrInvokers = std::invoke([]() {
#define PAIR(name, unused) Ret[#name] = reinterpret_cast<uintptr_t>(GetCallerForHostFunction(name));
  std::unordered_map<std::string_view, uintptr_t> Ret;
  FOREACH_internal_SYMBOL(PAIR);
  return Ret;
#undef PAIR
});

extern "C" {
void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) {
  GLGUEST_TRACE_SCOPE();
  fexfn_pack_FEX_glShaderSource(shader, count, reinterpret_cast<uintptr_t>(string), length);
}

void glShaderSourceARB(GLhandleARB shader, GLsizei count, const GLcharARB** string, const GLint* length) {
  GLGUEST_TRACE_SCOPE();
  fexfn_pack_FEX_glShaderSource(shader, count, reinterpret_cast<uintptr_t>(string), length);
}

voidFunc* glXGetProcAddress(const GLubyte* procname) {
  GLGUEST_TRACE_SCOPE();
  if (!procname) {
#ifdef BUILD_ANDROID
    LogMissingGLProcEvent("null procname", nullptr, "glXGetProcAddress argument is null", nullptr);
    return reinterpret_cast<voidFunc*>(MissingGLProcStub);
#else
    fprintf(stderr, "[libGL thunk] glXGetProcAddress called with null procname\n");
    return nullptr;
#endif
  }

  std::string_view procname_s {reinterpret_cast<const char*>(procname)};
  if (procname_s.empty()) {
#ifdef BUILD_ANDROID
    LogMissingGLProcEvent("empty procname", "", "glXGetProcAddress argument is empty", nullptr);
    return reinterpret_cast<voidFunc*>(MissingGLProcStub);
#else
    fprintf(stderr, "[libGL thunk] glXGetProcAddress called with empty procname\n");
    return nullptr;
#endif
  }

  // VEXA_FIXES: Resolve in this order: shader bridge/self handlers, Android direct
  // guest bindings, host lookup, then address-to-invoker link stitching.
#ifdef BUILD_ANDROID
  if (procname_s == "glMapBuffer") {
    // ETS2 asks glXGetProcAddress for the desktop name. The host already
    // implements it as glMapBufferOES. Do not export glMapBuffer from libGL:
    // Unity's dlsym(libGL) would then take the desktop path and crash.
    fprintf(stderr, "[libGL thunk] glXGetProcAddress glMapBuffer -> glMapBufferOES\n");
    return reinterpret_cast<voidFunc*>(glMapBufferOES);
  }
#endif
  if (procname_s == "glShaderSource" || procname_s == "glShaderSourceARB") {
    // VEXA_FIXES: Route shader source through explicit guest bridge helper.
    // Return a guest-callable entrypoint for shader source and avoid exposing
    // raw host pointers that can SIGSEGV when invoked directly by guest code.
    const auto shader_source_addr = reinterpret_cast<uintptr_t>(glShaderSource);
    TraceGLProcLink(procname_s, reinterpret_cast<const void*>(shader_source_addr), shader_source_addr);
    return reinterpret_cast<voidFunc*>(glShaderSource);
  }
#ifdef BUILD_ANDROID
  if (procname_s == "glCreateShaderProgramv") {
    // ETS2 resolves this through glXGetProcAddress. The host would otherwise
    // return Mali's native entry, which receives desktop GLSL untranslated and
    // fails with an empty info log. Our guest implementation builds the program
    // from glCreateShader/glShaderSource, so the ESSL translation applies.
    const auto create_program_addr = reinterpret_cast<uintptr_t>(glCreateShaderProgramv);
    TraceGLProcLink(procname_s, reinterpret_cast<const void*>(create_program_addr), create_program_addr);
    return reinterpret_cast<voidFunc*>(glCreateShaderProgramv);
  }
#endif

#ifdef BUILD_ANDROID
  if (const uintptr_t hidden = VexaHiddenCoreNoop(procname_s)) {
    fprintf(stderr, "[libGL thunk] glXGetProcAddress noop '%.*s'\n",
            static_cast<int>(procname_s.size()), procname_s.data());
    return reinterpret_cast<voidFunc*>(hidden);
  }

  // Godot calls the pointer it stored, including optional fixed-function
  // entries. Return a stub that logs and returns 0 instead of aborting or
  // handing back NULL.
  auto ReturnMissingDiagnostic = [&](const char* reason, const void* host_target = nullptr) -> voidFunc* {
    LogMissingGLProcEvent("no entry for", reinterpret_cast<const char*>(procname), reason, host_target);
    return reinterpret_cast<voidFunc*>(MissingGLProcStub);
  };

  if (procname_s == "glXGetProcAddress" || procname_s == "glXGetProcAddressARB") {
    const auto self_addr = reinterpret_cast<uintptr_t>(glXGetProcAddress);
    TraceGLProcLink(procname_s, reinterpret_cast<const void*>(self_addr), self_addr);
    return reinterpret_cast<voidFunc*>(glXGetProcAddress);
  }
#ifdef IS_32BIT_THUNK
  // A native Android function address cannot fit in an i386 function pointer.
  // Return real guest entrypoints that invoke their generated ABI packers.
  if (auto compat = ResolveAndroidCompatGuestProc(procname_s)) return reinterpret_cast<voidFunc*>(compat);
  static const auto guestEntries = [] {
    std::unordered_map<std::string_view, uintptr_t> entries;
#define GUEST_ENTRY(name, unused) entries[#name] = reinterpret_cast<uintptr_t>(fexfn_pack_##name);
    FOREACH_internal_SYMBOL(GUEST_ENTRY);
#undef GUEST_ENTRY
    return entries;
  }();
  auto entry = guestEntries.find(procname_s);
  if (entry == guestEntries.end()) entry = guestEntries.find(ResolveGLInvokerAlias(procname_s));
  return entry == guestEntries.end() ? nullptr : reinterpret_cast<voidFunc*>(entry->second);
#endif
  if (procname_s == "glGetFramebufferAttachmentParameteriv" || procname_s == "glGetFramebufferAttachmentParameterivEXT") {
    EnsureFramebufferAttachmentCompatPrelinked();
    return reinterpret_cast<voidFunc*>(VexaCompat_glGetFramebufferAttachmentParameteriv);
  }
  if (const auto guest_direct = ResolveAndroidDirectGuestProc(procname_s); guest_direct != 0) {
    TraceGLProcLink(procname_s, reinterpret_cast<const void*>(guest_direct), guest_direct);
    return reinterpret_cast<voidFunc*>(guest_direct);
  }
  if (const auto guest_compat = ResolveAndroidCompatGuestProc(procname_s); guest_compat != 0) {
    fprintf(stderr, "[libGL thunk] glXGetProcAddress compat-hit '%.*s' -> %p\n",
            static_cast<int>(procname_s.size()), procname_s.data(),
            reinterpret_cast<void*>(guest_compat));
    TraceGLProcLink(procname_s, reinterpret_cast<const void*>(guest_compat), guest_compat);
    return reinterpret_cast<voidFunc*>(guest_compat);
  }

  auto Ret = fexfn_pack_glXGetProcAddress(procname);
  if (!Ret) {
    // Host GLES has no EXT suffix. If the core entry exists, hand that host
    // pointer back so the guest calls the real function instead of the abort stub.
    const auto alias = ResolveGLInvokerAlias(procname_s);
    if (!alias.empty()) {
      if (const auto AliasIt = HostPtrInvokers.find(alias); AliasIt != HostPtrInvokers.end()) {
        char alias_name[96];
        if (alias.size() < sizeof(alias_name)) {
          std::memcpy(alias_name, alias.data(), alias.size());
          alias_name[alias.size()] = '\0';
          auto AliasRet = fexfn_pack_glXGetProcAddress(reinterpret_cast<const GLubyte*>(alias_name));
          if (AliasRet) {
            const auto AliasAddr = reinterpret_cast<uintptr_t>(AliasRet);
            TraceGLProcLink(procname_s, reinterpret_cast<const void*>(AliasAddr), AliasIt->second);
            LinkAddressToFunction(AliasAddr, AliasIt->second);
            fprintf(stderr, "[libGL thunk] glXGetProcAddress alias '%.*s' -> '%s'\n",
                    static_cast<int>(procname_s.size()), procname_s.data(), alias_name);
            return AliasRet;
          }
        }
      }
    }
    return ReturnMissingDiagnostic("host lookup returned null");
  }
  // VEXA_FIXES: Normalize function-pointer diagnostics/linking through integer
  // address form; C++ doesn't allow implicit function-pointer to object-pointer conversion.
  const auto RetAddr = reinterpret_cast<uintptr_t>(Ret);
  const auto RetPtr = reinterpret_cast<const void*>(RetAddr);

  auto TargetFuncIt = HostPtrInvokers.find(procname_s);
  if (TargetFuncIt == HostPtrInvokers.end()) {
    const auto alias = ResolveGLInvokerAlias(procname_s);
    if (!alias.empty()) {
      TargetFuncIt = HostPtrInvokers.find(alias);
    }

    if (TargetFuncIt == HostPtrInvokers.end()) {
      return ReturnMissingDiagnostic(alias.empty() ? "guest invoker missing" : "guest invoker missing (alias)", RetPtr);
    }
  }

  TraceGLProcLink(procname_s, RetPtr, TargetFuncIt->second);
  LinkAddressToFunction(RetAddr, TargetFuncIt->second);
  return Ret;
#else
  if (procname_s == "glGetFramebufferAttachmentParameteriv" || procname_s == "glGetFramebufferAttachmentParameterivEXT") {
    EnsureFramebufferAttachmentCompatPrelinked();
    return reinterpret_cast<voidFunc*>(VexaCompat_glGetFramebufferAttachmentParameteriv);
  }
  if (const auto guest_direct = ResolveAndroidDirectGuestProc(procname_s); guest_direct != 0) {
    TraceGLProcLink(procname_s, reinterpret_cast<const void*>(guest_direct), guest_direct);
    return reinterpret_cast<voidFunc*>(guest_direct);
  }

  auto Ret = fexfn_pack_glXGetProcAddress(procname);
  if (!Ret) {
    return nullptr;
  }
  const auto RetAddr = reinterpret_cast<uintptr_t>(Ret);
  const auto RetPtr = reinterpret_cast<const void*>(RetAddr);

  auto TargetFuncIt = HostPtrInvokers.find(procname_s);
  if (TargetFuncIt == HostPtrInvokers.end()) {
    const auto alias = ResolveGLInvokerAlias(procname_s);
    if (!alias.empty()) {
      TargetFuncIt = HostPtrInvokers.find(alias);
    }

    if (TargetFuncIt != HostPtrInvokers.end()) {
      TraceGLProcLink(procname_s, RetPtr, TargetFuncIt->second);
      LinkAddressToFunction(RetAddr, TargetFuncIt->second);
      return Ret;
    }

    // If glXGetProcAddress is querying itself, then we can just return itself.
    // Some games do this for unknown reasons.
    if (procname_s == "glXGetProcAddress" || procname_s == "glXGetProcAddressARB") {
      return reinterpret_cast<voidFunc*>(glXGetProcAddress);
    }

    // Extension found in host but not in our interface definition => Not fatal but warn about it
    // Some games query leaked GLES symbols but don't use them
    // glFrustrumf : ES 1.x function
    //  - Papers, Please
    //  - Dicey Dungeons
    fprintf(stderr, "glXGetProcAddress: not found %s\n", procname);
    return nullptr;
  }

  TraceGLProcLink(procname_s, RetPtr, TargetFuncIt->second);
  LinkAddressToFunction(RetAddr, TargetFuncIt->second);
  return Ret;
#endif
}

voidFunc* glXGetProcAddressARB(const GLubyte* procname) {
  GLGUEST_TRACE_SCOPE();
  return glXGetProcAddress(procname);
}
}

// Wrapper around malloc() without noexcept specifiers
static void* malloc_wrapper(size_t size) {
  return malloc(size);
}

static void free_wrapper(void* ptr) { free(ptr); }

static void OnInit() {
  GLGUEST_TRACE_SCOPE();
#if defined(BUILD_ANDROID) && defined(IS_32BIT_THUNK)
  fexfn_pack_GL_SetGuestMalloc((uintptr_t)malloc_wrapper, (uintptr_t)CallbackUnpack<decltype(malloc_wrapper)>::Unpack);
  fexfn_pack_GL_SetGuestFree((uintptr_t)free_wrapper, (uintptr_t)CallbackUnpack<decltype(free_wrapper)>::Unpack);
#endif
#ifdef BUILD_ANDROID
  // VEXA_FIXES: Android build has no X11 dependency; keep init explicit so
  // missing callback wiring is clearly intentional in logs.
  fprintf(stderr, "[libGL thunk] OnInit Android path: skip X11 guest callback wiring\n");
#else
  // VEXA_FIXES: Android path skips desktop X11 callback wiring (no X11 runtime).
  fexfn_pack_GL_SetGuestMalloc((uintptr_t)malloc_wrapper, (uintptr_t)CallbackUnpack<decltype(malloc_wrapper)>::Unpack);
  fexfn_pack_GL_SetGuestXSync((uintptr_t)XSync, (uintptr_t)CallbackUnpack<decltype(XSync)>::Unpack);
  fexfn_pack_GL_SetGuestXGetVisualInfo((uintptr_t)XGetVisualInfo, (uintptr_t)CallbackUnpack<decltype(XGetVisualInfo)>::Unpack);
  fexfn_pack_GL_SetGuestXDisplayString((uintptr_t)XDisplayString, (uintptr_t)CallbackUnpack<decltype(XDisplayString)>::Unpack);
#endif
}

#ifndef BUILD_ANDROID
// libGL.so must pull in libX11.so as a dependency. Referencing some libX11
// symbol here prevents the linker from optimizing away the unused dependency
auto implicit_libx11_dependency = XSetErrorHandler;
#endif

LOAD_LIB_INIT(libGL, OnInit)

#ifdef BUILD_ANDROID
extern "C" {

// VEXA_FIXES: Unigine probes desktop GLX and legacy GL symbols via dlsym(libGL.so.1, ...).
// Android thunk builds don't naturally export all of them, so provide a compatibility
// symbol surface to let GLWrapper::init() proceed.
constexpr const char* kCompatGLXExts =
  "GLX_ARB_create_context GLX_ARB_create_context_profile GLX_EXT_framebuffer_sRGB GLX_EXT_swap_control GLX_MESA_swap_control";
constexpr uintptr_t kCompatDisplay = 0x47584453ULL;  // "GXDS"
constexpr uintptr_t kCompatVisual = 0x47585649ULL;   // "GXVI"
constexpr uintptr_t kCompatFBConfig = 0x47584642ULL; // "GXFB"
constexpr unsigned long kCompatDrawable = 1UL;
constexpr unsigned long kCompatPbuffer = 1UL;
static thread_local void* g_vexa_current_context = nullptr;
static thread_local unsigned long g_vexa_current_drawable = 0;

#define VEXA_GL_GUEST_EXPORT __attribute__((visibility("default")))

// Xlib Visual / XVisualInfo use the guest pointer and unsigned-long ABI. A zeroed info
// leaves visual NULL, and the next 32-bit load of visual->class faults there.
struct VexaVisual {
  void* ext_data;
  unsigned long visualid;
  int c_class;
  unsigned long red_mask;
  unsigned long green_mask;
  unsigned long blue_mask;
  int bits_per_rgb;
  int map_entries;
};
struct VexaXVisualInfo {
  VexaVisual* visual;
  unsigned long visualid;
  int screen;
  int depth;
  int c_class;
  unsigned long red_mask;
  unsigned long green_mask;
  unsigned long blue_mask;
  int colormap_size;
  int bits_per_rgb;
};
static_assert(offsetof(VexaVisual, c_class) == 2 * sizeof(void*), "Visual.class");
static_assert(sizeof(VexaVisual) == (sizeof(void*) == 8 ? 56 : 32), "Visual size");
static_assert(offsetof(VexaXVisualInfo, c_class) == 2 * sizeof(void*) + 8, "XVisualInfo.class");
static_assert(sizeof(VexaXVisualInfo) == (sizeof(void*) == 8 ? 64 : 40), "XVisualInfo size");

static VexaVisual g_vexa_visual = {nullptr, 1, 4, 0x00ff0000, 0x0000ff00, 0x000000ff, 8, 256};

// Resolve guest Xlib, not the Android host's X11 types or connection.
static void* VexaGuestXlibSymbol(const char* name) {
  static void* xlib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
  return xlib ? dlsym(xlib, name) : nullptr;
}
static bool VexaRealDisplay(void* display) {
  return display && reinterpret_cast<uintptr_t>(display) != kCompatDisplay;
}
static void* VexaGlxVisualInfo(void* display, int screen = -1) {
  if (VexaRealDisplay(display)) {
    auto default_screen = reinterpret_cast<int (*)(void*)>(VexaGuestXlibSymbol("XDefaultScreen"));
    auto default_visual = reinterpret_cast<VexaVisual* (*)(void*, int)>(VexaGuestXlibSymbol("XDefaultVisual"));
    auto get_info = reinterpret_cast<VexaXVisualInfo* (*)(void*, long, VexaXVisualInfo*, int*)>(
      VexaGuestXlibSymbol("XGetVisualInfo"));
    if (!default_screen || !default_visual || !get_info) return nullptr;
    if (screen < 0) screen = default_screen(display);
    auto visual = default_visual(display, screen);
    if (!visual) return nullptr;
    VexaXVisualInfo match{};
    match.visualid = visual->visualid;
    match.screen = screen;
    int count = 0;
    // Xlib allocates the result and the caller releases it with XFree.
    return get_info(display, 3 /* VisualIDMask | VisualScreenMask */, &match, &count);
  }
  auto* info = static_cast<VexaXVisualInfo*>(std::calloc(1, sizeof(VexaXVisualInfo)));
  if (!info) {
    return nullptr;
  }
  info->visual = &g_vexa_visual;
  info->visualid = g_vexa_visual.visualid;
  info->screen = 0;
  info->depth = 24;
  info->c_class = g_vexa_visual.c_class;
  info->red_mask = g_vexa_visual.red_mask;
  info->green_mask = g_vexa_visual.green_mask;
  info->blue_mask = g_vexa_visual.blue_mask;
  info->colormap_size = g_vexa_visual.map_entries;
  info->bits_per_rgb = g_vexa_visual.bits_per_rgb;
  return info;
}

VEXA_GL_GUEST_EXPORT void* glXChooseVisual(void* display, int screen, int* /*attributes*/) {
  fprintf(stderr, "[libGL thunk] compat glXChooseVisual\n");
  return VexaGlxVisualInfo(display, screen);
}

VEXA_GL_GUEST_EXPORT void* glXCreateContext(void* /*display*/, void* /*visual*/, void* share_list, int /*direct*/) {
  fprintf(stderr, "[libGL thunk] compat glXCreateContext\n");
  return reinterpret_cast<void*>(fexfn_pack_VEXA_CreateGlxContext(reinterpret_cast<uintptr_t>(share_list)));
}

VEXA_GL_GUEST_EXPORT void* glXCreateContextAttribsARB(void* /*display*/, void* /*config*/, void* share_list, int /*direct*/,
                                                      const int* /*attribs*/) {
  fprintf(stderr, "[libGL thunk] compat glXCreateContextAttribsARB\n");
  return reinterpret_cast<void*>(fexfn_pack_VEXA_CreateGlxContext(reinterpret_cast<uintptr_t>(share_list)));
}

// Desktop GL attaches one slice of a 3D texture with glFramebufferTexture3D.
// GLES only has glFramebufferTextureLayer, which takes the slice index directly.
VEXA_GL_GUEST_EXPORT void VexaCompat_glFramebufferTexture3D(unsigned int target, unsigned int attachment, unsigned int /*textarget*/,
                                                            unsigned int texture, int level, int zoffset) {
  fprintf(stderr, "[libGL thunk] compat glFramebufferTexture3D layer=%d\n", zoffset);
  fexfn_pack_glFramebufferTextureLayer(target, attachment, texture, level, zoffset);
}

// GLES has no 1D framebuffer attachment. The call shape matches glFramebufferTexture2D.
VEXA_GL_GUEST_EXPORT void VexaCompat_glFramebufferTexture1D(unsigned int target, unsigned int attachment, unsigned int textarget,
                                                            unsigned int texture, int level) {
  constexpr unsigned int kTexture1D = 0x0DE0;
  constexpr unsigned int kTexture2D = 0x0DE1;
  if (textarget == kTexture1D) {
    textarget = kTexture2D;
  }
  fprintf(stderr, "[libGL thunk] compat glFramebufferTexture1D texture=%u level=%d\n", texture, level);
  fexfn_pack_glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

VEXA_GL_GUEST_EXPORT void glXDestroyContext(void* /*display*/, void* context) {
  fprintf(stderr, "[libGL thunk] compat glXDestroyContext context=%p\n", context);
  fexfn_pack_VEXA_DestroyGlxContext(reinterpret_cast<uintptr_t>(context));
  vexa_ff::forget_context(reinterpret_cast<uintptr_t>(context));
}

VEXA_GL_GUEST_EXPORT void* glXGetCurrentContext() {
  return g_vexa_current_context;
}

VEXA_GL_GUEST_EXPORT unsigned long glXGetCurrentDrawable() {
  return g_vexa_current_drawable;
}

VEXA_GL_GUEST_EXPORT int glXMakeCurrent(void* display, unsigned long drawable, void* context) {
  fprintf(stderr, "[libGL thunk] compat glXMakeCurrent drawable=%lu context=%p\n", drawable, context);
  // The host queries the drawable over a separate X11 connection. Wait until
  // the guest connection's buffered CreateWindow requests have been processed;
  // XFlush alone cannot order requests between two server connections.
  if (context && display && reinterpret_cast<uintptr_t>(display) != kCompatDisplay) {
    static auto sync = reinterpret_cast<int (*)(void*, int)>(VexaGuestXlibSymbol("XSync"));
    if (!sync) return 0;
    sync(display, 0);
  }
  const int result = context
    ? fexfn_pack_VEXA_BindGlxSurface(reinterpret_cast<uintptr_t>(context), static_cast<uint32_t>(drawable))
    : fexfn_pack_VEXA_ReleaseGlxSurface();
  // A failed MakeCurrent must preserve the previous current context/drawable.
  if (result) {
    g_vexa_current_context = context;
    g_vexa_current_drawable = context ? drawable : 0;
    vexa_ff::select_context(reinterpret_cast<uintptr_t>(context));
    vexa_ff::init_matrices();
  } else {
    fprintf(stderr, "[libGL thunk] glXMakeCurrent failed\n");
  }
  return result;
}

VEXA_GL_GUEST_EXPORT void glXSwapBuffers(void* /*display*/, unsigned long drawable) {
  if (GLGuestTraceEnabled()) {
    fprintf(stderr, "[libGL thunk] compat glXSwapBuffers drawable=%lu\n", drawable);
  }
  fexfn_pack_VEXA_SwapGlxBuffers(static_cast<uint32_t>(drawable));
}

static void VexaGuestWindowSize(unsigned int* width, unsigned int* height) {
  *width = 1280;
  *height = 720;
#ifdef BUILD_ANDROID
  const int fd = open("/tmp/vexa-window-size", O_RDONLY);
  if (fd < 0) {
    return;
  }
  char buf[64];
  const int n = static_cast<int>(read(fd, buf, sizeof(buf) - 1));
  close(fd);
  if (n <= 0) {
    return;
  }
  buf[n] = '\0';
  // Avoid newer glibc's __isoc23_sscanf: Proton's rootfs can use an older
  // libc than the guest build sysroot. The size file contains two decimals.
  const char* cursor = buf;
  auto number = [&cursor]() -> unsigned int {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n') ++cursor;
    unsigned int value = 0; bool found = false;
    while (*cursor >= '0' && *cursor <= '9') {
      found = true;
      if (value > 100000U) return 0;
      value = value * 10 + static_cast<unsigned int>(*cursor++ - '0');
    }
    return found ? value : 0;
  };
  const unsigned int w = number(), h = number();
  if (w > 1 && h > 1) { *width = w; *height = h; }
#else
  (void)width;
  (void)height;
#endif
}

VEXA_GL_GUEST_EXPORT void glXQueryDrawable(void* /*display*/, unsigned long /*drawable*/, int attribute, unsigned int* value) {
  if (!value) {
    return;
  }
  unsigned int width = 1280;
  unsigned int height = 720;
  VexaGuestWindowSize(&width, &height);
  static unsigned int logged_w = 0;
  static unsigned int logged_h = 0;
  if (width != logged_w || height != logged_h) {
    logged_w = width;
    logged_h = height;
    fprintf(stderr, "[libGL thunk] drawable size %u x %u\n", width, height);
  }
  if (attribute == 0x801D) {
    *value = width;
  } else if (attribute == 0x801E) {
    *value = height;
  } else {
    *value = 0;
  }
}

VEXA_GL_GUEST_EXPORT const char* glXQueryExtensionsString(void* /*display*/, int /*screen*/) {
  return kCompatGLXExts;
}

VEXA_GL_GUEST_EXPORT void* glXGetCurrentDisplay() {
  return reinterpret_cast<void*>(kCompatDisplay);
}

VEXA_GL_GUEST_EXPORT void* glXChooseFBConfig(void* /*display*/, int /*screen*/, const int* /*attributes*/, int* num_items) {
  auto* configs = static_cast<void**>(std::malloc(sizeof(void*)));
  if (configs) {
    configs[0] = reinterpret_cast<void*>(kCompatFBConfig);
  }
  if (num_items) {
    *num_items = configs ? 1 : 0;
  }
  return configs;
}

VEXA_GL_GUEST_EXPORT unsigned long glXCreatePbuffer(void* /*display*/, void* /*config*/, const int* /*attrib_list*/) {
  return kCompatPbuffer;
}

VEXA_GL_GUEST_EXPORT void glXDestroyPbuffer(void* /*display*/, unsigned long pbuffer) {
  fprintf(stderr, "[libGL thunk] compat glXDestroyPbuffer pbuffer=%lu\n", pbuffer);
}

VEXA_GL_GUEST_EXPORT void* glXGetVisualFromFBConfig(void* display, void* /*config*/) {
  fprintf(stderr, "[libGL thunk] compat glXGetVisualFromFBConfig\n");
  return VexaGlxVisualInfo(display);
}

static int vexa_glx_attrib(int attribute, int* value) {
  if (!value) {
    return 1;
  }
  switch (attribute) {
    case 1: *value = 1; break;
    case 2: *value = 32; break;
    case 4: *value = 1; break;
    case 5: *value = 1; break;
    case 8:
    case 9:
    case 10:
    case 11: *value = 8; break;
    case 12: *value = 24; break;
    case 13: *value = 8; break; // GLX_STENCIL_SIZE, matching the D24S8 EGL config
    case 0x20: *value = 0x8000; break;
    case 0x22: *value = 0x8002; break;
    case 0x800B: *value = 1; break;
    case 0x8010: *value = 1; break;
    case 0x8011: *value = 1; break;
    case 0x8012: *value = 1; break;
    case 0x8013: *value = 1; break;
    // Size limits. Unity clamps its backbuffer to GLX_MAX_BUFFER_SIZE, and a
    // zero here makes it fall back to a tiny fixed swapchain instead of the real
    // window size, so report the Android surface scale rather than nothing.
    case 0x8043:  // GLX_MAX_BUFFER_SIZE
    case 0x8073:  // GLX_MAX_TEXTURE_SIZE
    case 0x8016:  // GLX_MAX_TEXTURE_WIDTH
    case 0x8017:  // GLX_MAX_TEXTURE_HEIGHT
    case 0x801E: {  // GLX_MAX_RENDERBUFFER_SIZE
      unsigned int w = 1280, h = 720;
      VexaGuestWindowSize(&w, &h);
      const int limit = (w > h ? w : h) * 4;
      *value = limit < 4096 ? 4096 : limit;
      break;
    }
    case 0x20B2: {  // GLX_FRAMEBUFFER_SRGB_CAPABLE_EXT
      const char* flag = getenv("VEXA_GL_SRGB_SURFACE");
      *value = flag && strcmp(flag, "1") == 0 ? 1 : 0;
      break;
    }
    case 0x80A9: *value = 0; break;  // GLX_SAMPLES
    default: *value = 0; break;
  }
  return 0;
}

VEXA_GL_GUEST_EXPORT int glXQueryVersion(void*, int* major, int* minor) {
  if (major) *major = 1;
  if (minor) *minor = 4;
  return 1;
}

VEXA_GL_GUEST_EXPORT int glXGetConfig(void*, void*, int attribute, int* value) {
  return vexa_glx_attrib(attribute, value);
}

VEXA_GL_GUEST_EXPORT int glXIsDirect(void*, void*) {
  return 1;
}

VEXA_GL_GUEST_EXPORT const char* glXGetClientString(void*, int name) {
  if (name == 1) return "VEXA";
  if (name == 2) return "1.4";
  return kCompatGLXExts;
}

VEXA_GL_GUEST_EXPORT const char* glXQueryServerString(void*, int, int name) {
  return glXGetClientString(nullptr, name);
}

VEXA_GL_GUEST_EXPORT void* glXCreateNewContext(void*, void*, int, void* share_context, int) {
  fprintf(stderr, "[libGL thunk] compat glXCreateNewContext\n");
  return reinterpret_cast<void*>(fexfn_pack_VEXA_CreateGlxContext(reinterpret_cast<uintptr_t>(share_context)));
}

VEXA_GL_GUEST_EXPORT int glXGetFBConfigAttrib(void* display, void*, int attribute, int* value) {
  if (attribute == 0x800B && value && VexaRealDisplay(display)) { // GLX_VISUAL_ID
    auto* info = static_cast<VexaXVisualInfo*>(VexaGlxVisualInfo(display));
    if (!info) return 1;
    *value = static_cast<int>(info->visualid);
    std::free(info);
    return 0;
  }
  return vexa_glx_attrib(attribute, value);
}

VEXA_GL_GUEST_EXPORT void* glXGetFBConfigs(void* display, int screen, int* nelements) {
  return glXChooseFBConfig(display, screen, nullptr, nelements);
}

VEXA_GL_GUEST_EXPORT int glXMakeContextCurrent(void* display, unsigned long draw, unsigned long, void* context) {
  return glXMakeCurrent(display, draw, context);
}

VEXA_GL_GUEST_EXPORT int glXQueryExtension(void*, int* error_base, int* event_base) {
  if (error_base) *error_base = 0;
  if (event_base) *event_base = 0;
  return 1;
}

VEXA_GL_GUEST_EXPORT void glXCopyContext(void*, void*, void*, unsigned long) {}
VEXA_GL_GUEST_EXPORT unsigned long glXCreateGLXPixmap(void*, void*, unsigned long) { return 1; }
VEXA_GL_GUEST_EXPORT void glXDestroyGLXPixmap(void*, unsigned long) {}
VEXA_GL_GUEST_EXPORT void glXUseXFont(unsigned long, int, int, int) {}
VEXA_GL_GUEST_EXPORT void glXWaitGL(void) {}
VEXA_GL_GUEST_EXPORT void glXWaitX(void) {}
VEXA_GL_GUEST_EXPORT unsigned long glXCreatePixmap(void*, void*, unsigned long, const int*) { return 1; }
VEXA_GL_GUEST_EXPORT unsigned long glXCreateWindow(void*, void*, unsigned long window, const int*) {
  const char* real_x11 = getenv("VEXA_REAL_X11");
  return real_x11 && strcmp(real_x11, "1") == 0 ? window : kCompatDrawable;
}
VEXA_GL_GUEST_EXPORT void glXDestroyPixmap(void*, unsigned long) {}
VEXA_GL_GUEST_EXPORT void glXDestroyWindow(void*, unsigned long) {}
VEXA_GL_GUEST_EXPORT unsigned long glXGetCurrentReadDrawable(void) { return g_vexa_current_drawable; }
VEXA_GL_GUEST_EXPORT int glXQueryContext(void*, void*, int attribute, int* value) {
  return vexa_glx_attrib(attribute, value);
}
VEXA_GL_GUEST_EXPORT void glXSelectEvent(void*, unsigned long, unsigned long) {}
VEXA_GL_GUEST_EXPORT void glXGetSelectedEvent(void*, unsigned long, unsigned long* mask) {
  if (mask) *mask = 0;
}
static int g_vexa_swap_interval = 1;
VEXA_GL_GUEST_EXPORT void glXSwapIntervalEXT(void*, unsigned long, int interval) {
  if (fexfn_pack_VEXA_SetSwapInterval(interval)) g_vexa_swap_interval = interval;
}
VEXA_GL_GUEST_EXPORT int glXGetSwapIntervalMESA(void) { return g_vexa_swap_interval; }
VEXA_GL_GUEST_EXPORT int glXSwapIntervalMESA(unsigned int interval) {
  if (!fexfn_pack_VEXA_SetSwapInterval(static_cast<int>(interval))) return 1;
  g_vexa_swap_interval = static_cast<int>(interval);
  return 0;
}

// The Android EGL configuration has no accumulation buffer. Desktop GL
// requires GL_INVALID_OPERATION for a valid accumulation operation without one.
static bool VexaEvaluatorCap(GLenum cap, bool enabled, bool query, GLboolean* result) {
  const int one = vexa_eval::target_slot(cap, 1), two = vexa_eval::target_slot(cap, 2);
  if (one < 0 && two < 0 && cap != 0x0D80) return false;
  if (vexa_ff::in_begin()) { fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); if (result) *result = GL_FALSE; return true; }
  if (query) {
    const auto& state = vexa_ff::S().evaluator;
    const bool value = one >= 0 ? state.enabled1[one] : two >= 0 ? state.enabled2[two] : state.auto_normal;
    if (result) *result = value ? GL_TRUE : GL_FALSE;
  } else {
    vexa_ff::run_or_record_callback([one,two,enabled] {
      auto& state = vexa_ff::S().evaluator;
      if (one >= 0) state.enabled1[one] = enabled;
      else if (two >= 0) state.enabled2[two] = enabled;
      else state.auto_normal = enabled;
    });
  }
  return true;
}
VEXA_GL_GUEST_EXPORT void glEnable(GLenum cap) {
  if (vexa_ff::in_begin()) { fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return; }
  if (!VexaStippleCap(cap,true,false) && !vexa_lighting_guest::set_cap(cap, true) && !VexaEvaluatorCap(cap, true, false, nullptr)) fexfn_pack_glEnable(cap);
}
VEXA_GL_GUEST_EXPORT void glDisable(GLenum cap) {
  if (vexa_ff::in_begin()) { fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return; }
  if (!VexaStippleCap(cap,false,false) && !vexa_lighting_guest::set_cap(cap, false) && !VexaEvaluatorCap(cap, false, false, nullptr)) fexfn_pack_glDisable(cap);
}
VEXA_GL_GUEST_EXPORT GLboolean glIsEnabled(GLenum cap) {
  GLboolean result = GL_FALSE;
  if (vexa_ff::in_begin()) { fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return result; }
  if (VexaStippleCap(cap,false,true,&result)) return result;
  if (vexa_lighting_guest::get_cap(cap, result)) return result;
  return VexaEvaluatorCap(cap, false, true, &result) ? result : fexfn_pack_glIsEnabled(cap);
}

VEXA_GL_GUEST_EXPORT void glClearAccum(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  if (vexa_ff::in_begin()) { fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return; }
  vexa_ff::run_or_record_callback([=] {
    auto& c = vexa_ff::S().accum_clear;
    c[0]=std::clamp(r,-1.0f,1.0f); c[1]=std::clamp(g,-1.0f,1.0f);
    c[2]=std::clamp(b,-1.0f,1.0f); c[3]=std::clamp(a,-1.0f,1.0f);
  });
}
static int VexaCompatQuery(GLenum pname, GLfloat* values) {
  const auto& s=vexa_ff::S();
  switch(pname){
    case 0x0B30: values[0]=s.compiling?(s.compile_execute?0x1301:0x1300):0;return 1;
    case 0x0B31: values[0]=64;return 1;
    case 0x0B32: values[0]=s.list_base;return 1;
    case 0x0B33: values[0]=s.compiling?s.compile_id:0;return 1;
    case 0x0B24: values[0]=s.stipple.line;return 1;
    case 0x0B42: values[0]=s.stipple.polygon;return 1;
    case 0x0B25: values[0]=s.stipple.pattern;return 1;
    case 0x0B26: values[0]=s.stipple.factor;return 1;
    case 0x0D00: values[0]=s.pack_swap_bytes;return 1;
    case 0x0D01: values[0]=s.pack_lsb_first;return 1;
    case 0x0C40: values[0]=s.selection.mode;return 1;
    case 0x0DF1: values[0]=s.selection.feedback_size;return 1;
    case 0x0DF2: values[0]=s.selection.feedback_type;return 1;
    case 0x0DF4: values[0]=s.selection.select_size;return 1;
    case 0x0D37: values[0]=vexa_select::kMaxNameDepth;return 1;
    case 0x0D70: values[0]=s.selection.names.size();return 1;
    default:break;
  }
  switch (pname) {
    case 0x0B01: values[0] = vexa_ff::S().color_index; return 1;
    case 0x0B43: values[0] = vexa_ff::S().edge_flag; return 1;
    case 0x0C20: values[0] = vexa_ff::S().clear_index; return 1;
    case 0x0C21: values[0] = vexa_ff::S().index_mask; return 1;
    case 0x0C30: values[0] = 0; return 1; // GL_INDEX_MODE
    case 0x0C31: values[0] = 1; return 1; // GL_RGBA_MODE
    default: break;
  }
  if (pname == 0x0CF0) { values[0] = vexa_ff::S().unpack_swap_bytes; return 1; }
  if (pname == 0x0CF1) { values[0] = vexa_ff::S().unpack_lsb_first; return 1; }
  double raster[4]{};
  unsigned count = 0;
  if (VexaPixelQuery(pname, raster, count)) { values[0] = static_cast<GLfloat>(raster[0]); return 1; }
  if (VexaRasterQuery(pname, raster, count)) {
    for (unsigned i = 0; i < count; ++i) values[i] = static_cast<GLfloat>(raster[i]);
    return static_cast<int>(count);
  }
  if (pname == 0x0B80) { memcpy(values, vexa_ff::S().accum_clear, 4 * sizeof(GLfloat)); return 4; }
  if (pname >= 0x0D58 && pname <= 0x0D5B) { values[0] = 0; return 1; }
  if (pname == 0x0D30) { values[0] = vexa_eval::kMaxOrder; return 1; }
  return 0;
}
VEXA_GL_GUEST_EXPORT void glGetFloatv(GLenum pname, GLfloat* values) {
  if (!values) return;
  if (vexa_ff::in_begin()) { fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return; }
  if (VexaCompatQuery(pname, values)) return;
  fexfn_pack_glGetFloatv(pname, values);
}
VEXA_GL_GUEST_EXPORT void glGetIntegerv(GLenum pname, GLint* values) {
  if (pname == 0x0C21 && values && !vexa_ff::in_begin()) { *values = static_cast<GLint>(vexa_ff::S().index_mask); return; }
  if (!values) return; GLfloat tmp[4]; int count = VexaCompatQuery(pname, tmp);
  if (!count) { fexfn_pack_glGetIntegerv(pname, values); return; }
  if (vexa_ff::in_begin()) { fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return; }
  for (int i=0;i<count;++i) values[i] = static_cast<GLint>(std::round(tmp[i]));
}
VEXA_GL_GUEST_EXPORT void glGetBooleanv(GLenum pname, GLboolean* values) {
  if (!values) return; GLfloat tmp[4]; int count = VexaCompatQuery(pname, tmp);
  if (!count) { fexfn_pack_glGetBooleanv(pname, values); return; }
  if (vexa_ff::in_begin()) { fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return; }
  for (int i=0;i<count;++i) values[i] = tmp[i] != 0 ? GL_TRUE : GL_FALSE;
}

VEXA_GL_GUEST_EXPORT void glAccum(GLenum op, GLfloat) {
  if (op < 0x0100 || op > 0x0104) fexfn_pack_VEXA_CompatError(0x0500);
  else fexfn_pack_VEXA_CompatError(0x0502);
}

} // extern "C"
#undef VEXA_GL_GUEST_EXPORT
#define VEXA_GL_GUEST_EXPORT extern "C" __attribute__((visibility("default")))
#include "vexa_evaluator_guest.inc"
#undef VEXA_GL_GUEST_EXPORT
#define VEXA_GL_GUEST_EXPORT __attribute__((visibility("default")))
extern "C" {

VEXA_GL_GUEST_EXPORT void glLineWidth(GLfloat width) {
  fprintf(stderr, "[libGL thunk] compat glLineWidth width=%f (no-op)\n", static_cast<double>(width));
}

VEXA_GL_GUEST_EXPORT void glPolygonMode(GLenum face, GLenum mode) {
  static std::atomic<int> once {0};
  if (once.fetch_add(1, std::memory_order_relaxed) == 0) {
    fprintf(stderr, "[libGL thunk] noop glPolygonMode face=0x%x mode=0x%x\n", face, mode);
  }
}

VEXA_GL_GUEST_EXPORT void glTexParameteriv(GLenum target, GLenum pname, const GLint* params) {
  if (params) {
    ::glTexParameteri(target, pname, params[0]);
  }
}

VEXA_GL_GUEST_EXPORT void glTexImage1D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLint border,
                                       GLenum format, GLenum type, const void* pixels) {
  ::glTexImage2D(target, level, internalformat, width, 1, border, format, type, pixels);
}

VEXA_GL_GUEST_EXPORT void glDrawBuffer(GLenum mode) {
  // GLES3 has no glDrawBuffer, only glDrawBuffers. Desktop FBO code paths
  // (e.g. ETS2's depth-only shadow maps) call glDrawBuffer(GL_NONE); dropping
  // it leaves DRAW_BUFFER0 at GL_COLOR_ATTACHMENT0, so a depth-only FBO is
  // incomplete and every draw/clear fails -> shadow map never rendered.
  switch (mode) {
    case GL_NONE:
    case GL_BACK:
    case GL_COLOR_ATTACHMENT0:
      ::glDrawBuffers(1, &mode);
      break;
    default:
      if (mode >= GL_COLOR_ATTACHMENT0 && mode <= GL_COLOR_ATTACHMENT0 + 31) {
        ::glDrawBuffers(1, &mode);
      }
      break;
  }
}

VEXA_GL_GUEST_EXPORT void glFinish(void) {
  ::glFlush();
}

VEXA_GL_GUEST_EXPORT void glLogicOp(GLenum /*opcode*/) {
}

VEXA_GL_GUEST_EXPORT void glPixelStoref(GLenum pname, GLfloat param) {
  ::glPixelStorei(pname, static_cast<GLint>(param));
}

VEXA_GL_GUEST_EXPORT void glGetDoublev(GLenum pname, GLdouble* params) {
  if (!params) {
    return;
  }
  if (vexa_ff::in_begin()) { fexfn_pack_VEXA_CompatError(GL_INVALID_OPERATION); return; }
  GLfloat compat[4];int compat_count=VexaCompatQuery(pname,compat);
  if(compat_count){for(int i=0;i<compat_count;++i)params[i]=compat[i];return;}
  unsigned raster_count = 0;
  if (VexaRasterQuery(pname, params, raster_count)) return;
  if (VexaPixelQuery(pname, params, raster_count)) return;
  int count = 1;
  switch (pname) {
    case 0x0BA6: case 0x0BA7: case 0x0BA8: case 0x84E3: case 0x84E4: case 0x84E5: count = 16; break;
    case 0x0B00: case 0x0B03: case 0x0B04: case 0x0B06: case 0x0B07: case 0x0B53:
    case 0x0B66: case 0x0B80: case 0x0BA2: case 0x0C10: case 0x0C22: case 0x0C23:
    case 0x8005: count = 4; break;
    case 0x0B02: count = 3; break;
    case 0x0B12: case 0x0B22: case 0x0B40: case 0x0B70: case 0x0D3A:
    case 0x846D: case 0x846E: case 0x0DD1: case 0x0DD3: count = 2; break;
    case 0x0DD2: count = 4; break;
    case 0x86A3: glGetIntegerv(0x86A2, &count); break;
    case 0x8DF8: glGetIntegerv(0x8DF9, &count); break;
    case 0x87FF: glGetIntegerv(0x87FE, &count); break;
    default: break;
  }
  if (count <= 0) return;
  std::vector<GLfloat> tmp(static_cast<size_t>(count));
  ::glGetFloatv(pname, tmp.data());
  for (int i = 0; i < count; ++i) params[i] = static_cast<GLdouble>(tmp[i]);
}

VEXA_GL_GUEST_EXPORT void glGetTexImage(GLenum /*target*/, GLint /*level*/, GLenum /*format*/, GLenum /*type*/, void* /*pixels*/) {
}

VEXA_GL_GUEST_EXPORT void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat* params) {
  if (!params) {
    return;
  }
  GLint value = 0;
  ::glGetTexParameteriv(target, pname, &value);
  *params = static_cast<GLfloat>(value);
}

VEXA_GL_GUEST_EXPORT void glGetTexLevelParameterfv(GLenum /*target*/, GLint /*level*/, GLenum /*pname*/, GLfloat* params) {
  if (params) {
    *params = 0.0f;
  }
}

VEXA_GL_GUEST_EXPORT void glGetTexLevelParameteriv(GLenum /*target*/, GLint /*level*/, GLenum /*pname*/, GLint* params) {
  if (params) {
    *params = 0;
  }
}


VEXA_GL_GUEST_EXPORT void glDepthRange(GLdouble z_near, GLdouble z_far) {
  // Desktop GL. ETS2 slices the scene with this (0..0.009, 0.01..0.9, 0..1).
  // A no-op leaves every pass on the default 0..1 window depth, so roads,
  // vehicles and roofs z-fight into black bands. GLES only has the float form.
  fexfn_pack_glDepthRangef(static_cast<GLfloat>(z_near), static_cast<GLfloat>(z_far));
}

VEXA_GL_GUEST_EXPORT void glInterleavedArrays(GLenum format, GLsizei stride, const void* pointer) {
  fexfn_pack_VEXA_InterleavedArrays(format, stride, reinterpret_cast<uintptr_t>(pointer));
}

VEXA_GL_GUEST_EXPORT void glNormalPointer(GLenum type, GLsizei stride, const void* pointer) {
  fexfn_pack_VEXA_NormalPointer(type, stride, reinterpret_cast<uintptr_t>(pointer));
}

VEXA_GL_GUEST_EXPORT void glGetPointerv(GLenum pname, void** params) {
  if(!VexaSelectOutsideBegin())return;
  if(pname==0x0DF0){if(params)*params=vexa_ff::S().selection.feedback_buffer;return;}
  if(pname==0x0DF3){if(params)*params=vexa_ff::S().selection.select_buffer;return;}
  if(pname!=0x808E&&pname!=0x808F&&pname!=0x8090&&pname!=0x8091&&pname!=0x8092&&pname!=0x8093){fexfn_pack_VEXA_CompatError(0x0500);return;}
  if(params)*params=reinterpret_cast<void*>(fexfn_pack_VEXA_GetLegacyPointer(pname));
}

VEXA_GL_GUEST_EXPORT void glCopyTexImage1D(GLenum target, GLint level, GLenum internalformat,
                                           GLint x, GLint y, GLsizei width, GLint border) {
  ::glCopyTexImage2D(target, level, internalformat, x, y, width, 1, border);
}

VEXA_GL_GUEST_EXPORT void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat,
                                           GLint x, GLint y, GLsizei width, GLsizei height, GLint border) {
  fprintf(stderr,
          "[libGL thunk] compat glCopyTexImage2D target=0x%x level=%d size=%dx%d (no-op)\n",
          target, level, width, height);
}

VEXA_GL_GUEST_EXPORT void glCopyTexSubImage1D(GLenum target, GLint level, GLint xoffset,
                                              GLint x, GLint y, GLsizei width) {
  ::glCopyTexSubImage2D(target, level, xoffset, 0, x, y, width, 1);
}

VEXA_GL_GUEST_EXPORT void glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width,
                                          GLenum format, GLenum type, const void* pixels) {
  ::glTexSubImage2D(target, level, xoffset, 0, width, 1, format, type, pixels);
}

VEXA_GL_GUEST_EXPORT GLboolean glIsTexture(GLuint texture) {
  fprintf(stderr, "[libGL thunk] compat glIsTexture texture=%u -> GL_TRUE\n", texture);
  return texture != 0 ? GL_TRUE : GL_FALSE;
}

VEXA_GL_GUEST_EXPORT void glColor4f(GLfloat r,GLfloat g,GLfloat b,GLfloat a){GLfloat v[]{r,g,b,a};vexa_ff::material(0x1201,v);fexfn_pack_glColor4f(r,g,b,a);}
// Desktop GL color variants normalize integer inputs before setting the current color.
VEXA_GL_GUEST_EXPORT void glColor3b(GLbyte r, GLbyte g, GLbyte b) { glColor4f(std::max(static_cast<GLfloat>(static_cast<double>(r) / 127.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(g) / 127.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(b) / 127.0), -1.0f), 1.0f); }
VEXA_GL_GUEST_EXPORT void glColor3bv(const GLbyte* v) { if (!v) return; glColor4f(std::max(static_cast<GLfloat>(static_cast<double>(v[0]) / 127.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[1]) / 127.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[2]) / 127.0), -1.0f), 1.0f); }
VEXA_GL_GUEST_EXPORT void glColor3s(GLshort r, GLshort g, GLshort b) { glColor4f(std::max(static_cast<GLfloat>(static_cast<double>(r) / 32767.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(g) / 32767.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(b) / 32767.0), -1.0f), 1.0f); }
VEXA_GL_GUEST_EXPORT void glColor3sv(const GLshort* v) { if (!v) return; glColor4f(std::max(static_cast<GLfloat>(static_cast<double>(v[0]) / 32767.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[1]) / 32767.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[2]) / 32767.0), -1.0f), 1.0f); }
VEXA_GL_GUEST_EXPORT void glColor3i(GLint r, GLint g, GLint b) { glColor4f(std::max(static_cast<GLfloat>(static_cast<double>(r) / 2147483647.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(g) / 2147483647.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(b) / 2147483647.0), -1.0f), 1.0f); }
VEXA_GL_GUEST_EXPORT void glColor3iv(const GLint* v) { if (!v) return; glColor4f(std::max(static_cast<GLfloat>(static_cast<double>(v[0]) / 2147483647.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[1]) / 2147483647.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[2]) / 2147483647.0), -1.0f), 1.0f); }
VEXA_GL_GUEST_EXPORT void glColor3ub(GLubyte r, GLubyte g, GLubyte b) { glColor4f(static_cast<GLfloat>(static_cast<double>(r) / 255.0), static_cast<GLfloat>(static_cast<double>(g) / 255.0), static_cast<GLfloat>(static_cast<double>(b) / 255.0), 1.0f); }
VEXA_GL_GUEST_EXPORT void glColor3ubv(const GLubyte* v) { if (!v) return; glColor4f(static_cast<GLfloat>(static_cast<double>(v[0]) / 255.0), static_cast<GLfloat>(static_cast<double>(v[1]) / 255.0), static_cast<GLfloat>(static_cast<double>(v[2]) / 255.0), 1.0f); }
VEXA_GL_GUEST_EXPORT void glColor3us(GLushort r, GLushort g, GLushort b) { glColor4f(static_cast<GLfloat>(static_cast<double>(r) / 65535.0), static_cast<GLfloat>(static_cast<double>(g) / 65535.0), static_cast<GLfloat>(static_cast<double>(b) / 65535.0), 1.0f); }
VEXA_GL_GUEST_EXPORT void glColor3usv(const GLushort* v) { if (!v) return; glColor4f(static_cast<GLfloat>(static_cast<double>(v[0]) / 65535.0), static_cast<GLfloat>(static_cast<double>(v[1]) / 65535.0), static_cast<GLfloat>(static_cast<double>(v[2]) / 65535.0), 1.0f); }
VEXA_GL_GUEST_EXPORT void glColor3ui(GLuint r, GLuint g, GLuint b) { glColor4f(static_cast<GLfloat>(static_cast<double>(r) / 4294967295.0), static_cast<GLfloat>(static_cast<double>(g) / 4294967295.0), static_cast<GLfloat>(static_cast<double>(b) / 4294967295.0), 1.0f); }
VEXA_GL_GUEST_EXPORT void glColor3uiv(const GLuint* v) { if (!v) return; glColor4f(static_cast<GLfloat>(static_cast<double>(v[0]) / 4294967295.0), static_cast<GLfloat>(static_cast<double>(v[1]) / 4294967295.0), static_cast<GLfloat>(static_cast<double>(v[2]) / 4294967295.0), 1.0f); }
VEXA_GL_GUEST_EXPORT void glColor3f(GLfloat r, GLfloat g, GLfloat b) { glColor4f(static_cast<GLfloat>(r), static_cast<GLfloat>(g), static_cast<GLfloat>(b), 1.0f); }
VEXA_GL_GUEST_EXPORT void glColor3fv(const GLfloat* v) { if (!v) return; glColor4f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]), 1.0f); }
VEXA_GL_GUEST_EXPORT void glColor3d(GLdouble r, GLdouble g, GLdouble b) { glColor4f(static_cast<GLfloat>(r), static_cast<GLfloat>(g), static_cast<GLfloat>(b), 1.0f); }
VEXA_GL_GUEST_EXPORT void glColor3dv(const GLdouble* v) { if (!v) return; glColor4f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]), 1.0f); }
VEXA_GL_GUEST_EXPORT void glColor4b(GLbyte r, GLbyte g, GLbyte b, GLbyte a) { glColor4f(std::max(static_cast<GLfloat>(static_cast<double>(r) / 127.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(g) / 127.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(b) / 127.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(a) / 127.0), -1.0f)); }
VEXA_GL_GUEST_EXPORT void glColor4bv(const GLbyte* v) { if (!v) return; glColor4f(std::max(static_cast<GLfloat>(static_cast<double>(v[0]) / 127.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[1]) / 127.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[2]) / 127.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[3]) / 127.0), -1.0f)); }
VEXA_GL_GUEST_EXPORT void glColor4s(GLshort r, GLshort g, GLshort b, GLshort a) { glColor4f(std::max(static_cast<GLfloat>(static_cast<double>(r) / 32767.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(g) / 32767.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(b) / 32767.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(a) / 32767.0), -1.0f)); }
VEXA_GL_GUEST_EXPORT void glColor4i(GLint r, GLint g, GLint b, GLint a) { glColor4f(std::max(static_cast<GLfloat>(static_cast<double>(r) / 2147483647.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(g) / 2147483647.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(b) / 2147483647.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(a) / 2147483647.0), -1.0f)); }
VEXA_GL_GUEST_EXPORT void glColor4iv(const GLint* v) { if (!v) return; glColor4f(std::max(static_cast<GLfloat>(static_cast<double>(v[0]) / 2147483647.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[1]) / 2147483647.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[2]) / 2147483647.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[3]) / 2147483647.0), -1.0f)); }
VEXA_GL_GUEST_EXPORT void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { glColor4f(static_cast<GLfloat>(static_cast<double>(r) / 255.0), static_cast<GLfloat>(static_cast<double>(g) / 255.0), static_cast<GLfloat>(static_cast<double>(b) / 255.0), static_cast<GLfloat>(static_cast<double>(a) / 255.0)); }
VEXA_GL_GUEST_EXPORT void glColor4ubv(const GLubyte* v) { if (!v) return; glColor4f(static_cast<GLfloat>(static_cast<double>(v[0]) / 255.0), static_cast<GLfloat>(static_cast<double>(v[1]) / 255.0), static_cast<GLfloat>(static_cast<double>(v[2]) / 255.0), static_cast<GLfloat>(static_cast<double>(v[3]) / 255.0)); }
VEXA_GL_GUEST_EXPORT void glColor4us(GLushort r, GLushort g, GLushort b, GLushort a) { glColor4f(static_cast<GLfloat>(static_cast<double>(r) / 65535.0), static_cast<GLfloat>(static_cast<double>(g) / 65535.0), static_cast<GLfloat>(static_cast<double>(b) / 65535.0), static_cast<GLfloat>(static_cast<double>(a) / 65535.0)); }
VEXA_GL_GUEST_EXPORT void glColor4usv(const GLushort* v) { if (!v) return; glColor4f(static_cast<GLfloat>(static_cast<double>(v[0]) / 65535.0), static_cast<GLfloat>(static_cast<double>(v[1]) / 65535.0), static_cast<GLfloat>(static_cast<double>(v[2]) / 65535.0), static_cast<GLfloat>(static_cast<double>(v[3]) / 65535.0)); }
VEXA_GL_GUEST_EXPORT void glColor4ui(GLuint r, GLuint g, GLuint b, GLuint a) { glColor4f(static_cast<GLfloat>(static_cast<double>(r) / 4294967295.0), static_cast<GLfloat>(static_cast<double>(g) / 4294967295.0), static_cast<GLfloat>(static_cast<double>(b) / 4294967295.0), static_cast<GLfloat>(static_cast<double>(a) / 4294967295.0)); }
VEXA_GL_GUEST_EXPORT void glColor4uiv(const GLuint* v) { if (!v) return; glColor4f(static_cast<GLfloat>(static_cast<double>(v[0]) / 4294967295.0), static_cast<GLfloat>(static_cast<double>(v[1]) / 4294967295.0), static_cast<GLfloat>(static_cast<double>(v[2]) / 4294967295.0), static_cast<GLfloat>(static_cast<double>(v[3]) / 4294967295.0)); }
VEXA_GL_GUEST_EXPORT void glColor4fv(const GLfloat* v) { if (!v) return; glColor4f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]), static_cast<GLfloat>(v[3])); }
VEXA_GL_GUEST_EXPORT void glColor4d(GLdouble r, GLdouble g, GLdouble b, GLdouble a) { glColor4f(static_cast<GLfloat>(r), static_cast<GLfloat>(g), static_cast<GLfloat>(b), static_cast<GLfloat>(a)); }
VEXA_GL_GUEST_EXPORT void glColor4dv(const GLdouble* v) { if (!v) return; glColor4f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]), static_cast<GLfloat>(v[3])); }
VEXA_GL_GUEST_EXPORT void glColor4sv(const GLshort* v){if(v)glColor4f(std::max(v[0]/32767.0f,-1.f),std::max(v[1]/32767.0f,-1.f),std::max(v[2]/32767.0f,-1.f),std::max(v[3]/32767.0f,-1.f));}
VEXA_GL_GUEST_EXPORT void glTexCoord2f(GLfloat s,GLfloat t){VexaSetTexCoord4f(8,s,t,0,1);}
VEXA_GL_GUEST_EXPORT void glPopAttrib(){fexfn_pack_glPopAttrib();if(vexa_ff::clip_enabled()){::glGetVertexAttribfv(3,GL_CURRENT_VERTEX_ATTRIB,vexa_ff::S().color);GLint mode=0;::glGetIntegerv(0x0BA0,&mode);vexa_ff::S().matrix_mode=mode;}}
VEXA_GL_GUEST_EXPORT void glClipPlane(GLenum plane, const GLdouble* equation) {vexa_ff::sync_clip_matrices();fexfn_pack_glClipPlane(plane,equation);}
VEXA_GL_GUEST_EXPORT void glLoadMatrixf(const GLfloat* matrix) {if(matrix){vexa_ff::matrix(matrix,false);}}
VEXA_GL_GUEST_EXPORT void glLoadMatrixd(const GLdouble* matrix) {if(matrix){float m[16];for(int i=0;i<16;++i)m[i]=matrix[i];glLoadMatrixf(m);}}
VEXA_GL_GUEST_EXPORT void glMultMatrixf(const GLfloat* matrix) {if(matrix){vexa_ff::matrix(matrix,true);}}
VEXA_GL_GUEST_EXPORT void glMultMatrixd(const GLdouble* matrix) {if(matrix){float m[16];for(int i=0;i<16;++i)m[i]=matrix[i];glMultMatrixf(m);}}
VEXA_GL_GUEST_EXPORT void glScalef(GLfloat x, GLfloat y, GLfloat z) {float m[16]{};m[0]=x;m[5]=y;m[10]=z;m[15]=1;glMultMatrixf(m);}
VEXA_GL_GUEST_EXPORT void glScaled(GLdouble x, GLdouble y, GLdouble z) {glScalef(x,y,z);}
VEXA_GL_GUEST_EXPORT void glOrtho(GLdouble l,GLdouble r,GLdouble b,GLdouble t,GLdouble n,GLdouble f) {
  if(l==r||b==t||n==f){fexfn_pack_VEXA_CompatError(0x0501);return;}
  GLfloat m[16]{};m[0]=2/(r-l);m[5]=2/(t-b);m[10]=-2/(f-n);m[12]=-(r+l)/(r-l);m[13]=-(t+b)/(t-b);m[14]=-(f+n)/(f-n);m[15]=1;glMultMatrixf(m);
}
VEXA_GL_GUEST_EXPORT void glBegin(GLenum mode) { vexa_ff::begin(mode); }
VEXA_GL_GUEST_EXPORT void glEnd(void) { vexa_ff::end(); }
VEXA_GL_GUEST_EXPORT void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { vexa_ff::vertex(x, y, z); }
VEXA_GL_GUEST_EXPORT void glNormal3f(GLfloat x, GLfloat y, GLfloat z) { vexa_ff::normal(x, y, z); }
// Coordinate type variants share the implemented float paths.
VEXA_GL_GUEST_EXPORT void glVertex2d(GLdouble x, GLdouble y) { glVertex3f(static_cast<GLfloat>(x), static_cast<GLfloat>(y), 0.0f); }
VEXA_GL_GUEST_EXPORT void glVertex2dv(const GLdouble* v) { if (!v) return; glVertex3f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), 0.0f); }
VEXA_GL_GUEST_EXPORT void glVertex2f(GLfloat x, GLfloat y) { glVertex3f(static_cast<GLfloat>(x), static_cast<GLfloat>(y), 0.0f); }
VEXA_GL_GUEST_EXPORT void glVertex2fv(const GLfloat* v) { if (!v) return; glVertex3f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), 0.0f); }
VEXA_GL_GUEST_EXPORT void glVertex2i(GLint x, GLint y) { glVertex3f(static_cast<GLfloat>(x), static_cast<GLfloat>(y), 0.0f); }
VEXA_GL_GUEST_EXPORT void glVertex2iv(const GLint* v) { if (!v) return; glVertex3f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), 0.0f); }
VEXA_GL_GUEST_EXPORT void glVertex2s(GLshort x, GLshort y) { glVertex3f(static_cast<GLfloat>(x), static_cast<GLfloat>(y), 0.0f); }
VEXA_GL_GUEST_EXPORT void glVertex2sv(const GLshort* v) { if (!v) return; glVertex3f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), 0.0f); }
VEXA_GL_GUEST_EXPORT void glVertex3d(GLdouble x, GLdouble y, GLdouble z) { glVertex3f(static_cast<GLfloat>(x), static_cast<GLfloat>(y), static_cast<GLfloat>(z)); }
VEXA_GL_GUEST_EXPORT void glVertex3dv(const GLdouble* v) { if (!v) return; glVertex3f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2])); }
VEXA_GL_GUEST_EXPORT void glVertex3fv(const GLfloat* v) { if (!v) return; glVertex3f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2])); }
VEXA_GL_GUEST_EXPORT void glVertex3i(GLint x, GLint y, GLint z) { glVertex3f(static_cast<GLfloat>(x), static_cast<GLfloat>(y), static_cast<GLfloat>(z)); }
VEXA_GL_GUEST_EXPORT void glVertex3iv(const GLint* v) { if (!v) return; glVertex3f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2])); }
VEXA_GL_GUEST_EXPORT void glVertex3s(GLshort x, GLshort y, GLshort z) { glVertex3f(static_cast<GLfloat>(x), static_cast<GLfloat>(y), static_cast<GLfloat>(z)); }
VEXA_GL_GUEST_EXPORT void glVertex3sv(const GLshort* v) { if (!v) return; glVertex3f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2])); }
VEXA_GL_GUEST_EXPORT void glNormal3b(GLbyte x, GLbyte y, GLbyte z) { glNormal3f(std::max(static_cast<GLfloat>(static_cast<double>(x) / 127.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(y) / 127.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(z) / 127.0), -1.0f)); }
VEXA_GL_GUEST_EXPORT void glNormal3bv(const GLbyte* v) { if (!v) return; glNormal3f(std::max(static_cast<GLfloat>(static_cast<double>(v[0]) / 127.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[1]) / 127.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[2]) / 127.0), -1.0f)); }
VEXA_GL_GUEST_EXPORT void glNormal3s(GLshort x, GLshort y, GLshort z) { glNormal3f(std::max(static_cast<GLfloat>(static_cast<double>(x) / 32767.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(y) / 32767.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(z) / 32767.0), -1.0f)); }
VEXA_GL_GUEST_EXPORT void glNormal3sv(const GLshort* v) { if (!v) return; glNormal3f(std::max(static_cast<GLfloat>(static_cast<double>(v[0]) / 32767.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[1]) / 32767.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[2]) / 32767.0), -1.0f)); }
VEXA_GL_GUEST_EXPORT void glNormal3i(GLint x, GLint y, GLint z) { glNormal3f(std::max(static_cast<GLfloat>(static_cast<double>(x) / 2147483647.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(y) / 2147483647.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(z) / 2147483647.0), -1.0f)); }
VEXA_GL_GUEST_EXPORT void glNormal3iv(const GLint* v) { if (!v) return; glNormal3f(std::max(static_cast<GLfloat>(static_cast<double>(v[0]) / 2147483647.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[1]) / 2147483647.0), -1.0f), std::max(static_cast<GLfloat>(static_cast<double>(v[2]) / 2147483647.0), -1.0f)); }
VEXA_GL_GUEST_EXPORT void glNormal3fv(const GLfloat* v) { if (!v) return; glNormal3f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2])); }
VEXA_GL_GUEST_EXPORT void glNormal3d(GLdouble x, GLdouble y, GLdouble z) { glNormal3f(static_cast<GLfloat>(x), static_cast<GLfloat>(y), static_cast<GLfloat>(z)); }
VEXA_GL_GUEST_EXPORT void glNormal3dv(const GLdouble* v) { if (!v) return; glNormal3f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2])); }
VEXA_GL_GUEST_EXPORT void glTexCoord1d(GLdouble s) { glTexCoord2f(static_cast<GLfloat>(s), 0.0f); }
VEXA_GL_GUEST_EXPORT void glTexCoord1dv(const GLdouble* v) { if (!v) return; glTexCoord2f(static_cast<GLfloat>(v[0]), 0.0f); }
VEXA_GL_GUEST_EXPORT void glTexCoord1f(GLfloat s) { glTexCoord2f(static_cast<GLfloat>(s), 0.0f); }
VEXA_GL_GUEST_EXPORT void glTexCoord1fv(const GLfloat* v) { if (!v) return; glTexCoord2f(static_cast<GLfloat>(v[0]), 0.0f); }
VEXA_GL_GUEST_EXPORT void glTexCoord1i(GLint s) { glTexCoord2f(static_cast<GLfloat>(s), 0.0f); }
VEXA_GL_GUEST_EXPORT void glTexCoord1iv(const GLint* v) { if (!v) return; glTexCoord2f(static_cast<GLfloat>(v[0]), 0.0f); }
VEXA_GL_GUEST_EXPORT void glTexCoord1s(GLshort s) { glTexCoord2f(static_cast<GLfloat>(s), 0.0f); }
VEXA_GL_GUEST_EXPORT void glTexCoord1sv(const GLshort* v) { if (!v) return; glTexCoord2f(static_cast<GLfloat>(v[0]), 0.0f); }
VEXA_GL_GUEST_EXPORT void glTexCoord2d(GLdouble s, GLdouble t) { glTexCoord2f(static_cast<GLfloat>(s), static_cast<GLfloat>(t)); }
VEXA_GL_GUEST_EXPORT void glTexCoord2dv(const GLdouble* v) { if (!v) return; glTexCoord2f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1])); }
VEXA_GL_GUEST_EXPORT void glTexCoord2fv(const GLfloat* v) { if (!v) return; glTexCoord2f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1])); }
VEXA_GL_GUEST_EXPORT void glTexCoord2i(GLint s, GLint t) { glTexCoord2f(static_cast<GLfloat>(s), static_cast<GLfloat>(t)); }
VEXA_GL_GUEST_EXPORT void glTexCoord2iv(const GLint* v) { if (!v) return; glTexCoord2f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1])); }
VEXA_GL_GUEST_EXPORT void glTexCoord2s(GLshort s, GLshort t) { glTexCoord2f(static_cast<GLfloat>(s), static_cast<GLfloat>(t)); }
VEXA_GL_GUEST_EXPORT void glTexCoord2sv(const GLshort* v) { if (!v) return; glTexCoord2f(static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1])); }
// Legacy current texture coordinate uses generic attribute 8 on the host.
VEXA_GL_GUEST_EXPORT void glTexCoord3d(GLdouble s, GLdouble t, GLdouble r) { VexaSetTexCoord4f(8, static_cast<GLfloat>(s), static_cast<GLfloat>(t), static_cast<GLfloat>(r), 1.0f); }
VEXA_GL_GUEST_EXPORT void glTexCoord3dv(const GLdouble* v) { if (!v) return; VexaSetTexCoord4f(8, static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]), 1.0f); }
VEXA_GL_GUEST_EXPORT void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r) { VexaSetTexCoord4f(8, static_cast<GLfloat>(s), static_cast<GLfloat>(t), static_cast<GLfloat>(r), 1.0f); }
VEXA_GL_GUEST_EXPORT void glTexCoord3fv(const GLfloat* v) { if (!v) return; VexaSetTexCoord4f(8, static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]), 1.0f); }
VEXA_GL_GUEST_EXPORT void glTexCoord3i(GLint s, GLint t, GLint r) { VexaSetTexCoord4f(8, static_cast<GLfloat>(s), static_cast<GLfloat>(t), static_cast<GLfloat>(r), 1.0f); }
VEXA_GL_GUEST_EXPORT void glTexCoord3iv(const GLint* v) { if (!v) return; VexaSetTexCoord4f(8, static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]), 1.0f); }
VEXA_GL_GUEST_EXPORT void glTexCoord3s(GLshort s, GLshort t, GLshort r) { VexaSetTexCoord4f(8, static_cast<GLfloat>(s), static_cast<GLfloat>(t), static_cast<GLfloat>(r), 1.0f); }
VEXA_GL_GUEST_EXPORT void glTexCoord3sv(const GLshort* v) { if (!v) return; VexaSetTexCoord4f(8, static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]), 1.0f); }
VEXA_GL_GUEST_EXPORT void glTexCoord4d(GLdouble s, GLdouble t, GLdouble r, GLdouble q) { VexaSetTexCoord4f(8, static_cast<GLfloat>(s), static_cast<GLfloat>(t), static_cast<GLfloat>(r), static_cast<GLfloat>(q)); }
VEXA_GL_GUEST_EXPORT void glTexCoord4dv(const GLdouble* v) { if (!v) return; VexaSetTexCoord4f(8, static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]), static_cast<GLfloat>(v[3])); }
VEXA_GL_GUEST_EXPORT void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q) { VexaSetTexCoord4f(8, static_cast<GLfloat>(s), static_cast<GLfloat>(t), static_cast<GLfloat>(r), static_cast<GLfloat>(q)); }
VEXA_GL_GUEST_EXPORT void glTexCoord4fv(const GLfloat* v) { if (!v) return; VexaSetTexCoord4f(8, static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]), static_cast<GLfloat>(v[3])); }
VEXA_GL_GUEST_EXPORT void glTexCoord4i(GLint s, GLint t, GLint r, GLint q) { VexaSetTexCoord4f(8, static_cast<GLfloat>(s), static_cast<GLfloat>(t), static_cast<GLfloat>(r), static_cast<GLfloat>(q)); }
VEXA_GL_GUEST_EXPORT void glTexCoord4iv(const GLint* v) { if (!v) return; VexaSetTexCoord4f(8, static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]), static_cast<GLfloat>(v[3])); }
VEXA_GL_GUEST_EXPORT void glTexCoord4s(GLshort s, GLshort t, GLshort r, GLshort q) { VexaSetTexCoord4f(8, static_cast<GLfloat>(s), static_cast<GLfloat>(t), static_cast<GLfloat>(r), static_cast<GLfloat>(q)); }
VEXA_GL_GUEST_EXPORT void glTexCoord4sv(const GLshort* v) { if (!v) return; VexaSetTexCoord4f(8, static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]), static_cast<GLfloat>(v[3])); }
// Preserve homogeneous coordinates through the fixed-function vertex path.
VEXA_GL_GUEST_EXPORT void glVertex4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w) { vexa_ff::vertex4(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w)); }
VEXA_GL_GUEST_EXPORT void glVertex4dv(const GLdouble* v) { if (!v) return; vexa_ff::vertex4(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), static_cast<float>(v[3])); }
VEXA_GL_GUEST_EXPORT void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { vexa_ff::vertex4(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w)); }
VEXA_GL_GUEST_EXPORT void glVertex4fv(const GLfloat* v) { if (!v) return; vexa_ff::vertex4(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), static_cast<float>(v[3])); }
VEXA_GL_GUEST_EXPORT void glVertex4i(GLint x, GLint y, GLint z, GLint w) { vexa_ff::vertex4(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w)); }
VEXA_GL_GUEST_EXPORT void glVertex4iv(const GLint* v) { if (!v) return; vexa_ff::vertex4(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), static_cast<float>(v[3])); }
VEXA_GL_GUEST_EXPORT void glVertex4s(GLshort x, GLshort y, GLshort z, GLshort w) { vexa_ff::vertex4(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w)); }
VEXA_GL_GUEST_EXPORT void glVertex4sv(const GLshort* v) { if (!v) return; vexa_ff::vertex4(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), static_cast<float>(v[3])); }
VEXA_GL_GUEST_EXPORT void glMatrixMode(GLenum mode) { vexa_ff::matrix_mode(mode); vexa_ff::sync_clip_matrices(); }
VEXA_GL_GUEST_EXPORT void glLoadIdentity(void) { vexa_ff::load_identity(); vexa_ff::sync_clip_matrices(); }
VEXA_GL_GUEST_EXPORT void glTranslatef(GLfloat x, GLfloat y, GLfloat z) { vexa_ff::translate(x, y, z); vexa_ff::sync_clip_matrices(); }
VEXA_GL_GUEST_EXPORT void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) { vexa_ff::rotate(angle, x, y, z); vexa_ff::sync_clip_matrices(); }
VEXA_GL_GUEST_EXPORT void glTranslated(GLdouble x, GLdouble y, GLdouble z) {
  vexa_ff::translate(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)); vexa_ff::sync_clip_matrices();
}
VEXA_GL_GUEST_EXPORT void glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z) { glRotatef(angle, x, y, z); }
VEXA_GL_GUEST_EXPORT void glRectd(GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2) { glBegin(0x0007); glVertex3f(x1, y1, 0); glVertex3f(x2, y1, 0); glVertex3f(x2, y2, 0); glVertex3f(x1, y2, 0); glEnd(); }
VEXA_GL_GUEST_EXPORT void glRectdv(const GLdouble* a, const GLdouble* b) { if (!a || !b) return; glBegin(0x0007); glVertex3f(a[0], a[1], 0); glVertex3f(b[0], a[1], 0); glVertex3f(b[0], b[1], 0); glVertex3f(a[0], b[1], 0); glEnd(); }
VEXA_GL_GUEST_EXPORT void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2) { glBegin(0x0007); glVertex3f(x1, y1, 0); glVertex3f(x2, y1, 0); glVertex3f(x2, y2, 0); glVertex3f(x1, y2, 0); glEnd(); }
VEXA_GL_GUEST_EXPORT void glRectfv(const GLfloat* a, const GLfloat* b) { if (!a || !b) return; glBegin(0x0007); glVertex3f(a[0], a[1], 0); glVertex3f(b[0], a[1], 0); glVertex3f(b[0], b[1], 0); glVertex3f(a[0], b[1], 0); glEnd(); }
VEXA_GL_GUEST_EXPORT void glRecti(GLint x1, GLint y1, GLint x2, GLint y2) { glBegin(0x0007); glVertex3f(x1, y1, 0); glVertex3f(x2, y1, 0); glVertex3f(x2, y2, 0); glVertex3f(x1, y2, 0); glEnd(); }
VEXA_GL_GUEST_EXPORT void glRectiv(const GLint* a, const GLint* b) { if (!a || !b) return; glBegin(0x0007); glVertex3f(a[0], a[1], 0); glVertex3f(b[0], a[1], 0); glVertex3f(b[0], b[1], 0); glVertex3f(a[0], b[1], 0); glEnd(); }
VEXA_GL_GUEST_EXPORT void glRects(GLshort x1, GLshort y1, GLshort x2, GLshort y2) { glBegin(0x0007); glVertex3f(x1, y1, 0); glVertex3f(x2, y1, 0); glVertex3f(x2, y2, 0); glVertex3f(x1, y2, 0); glEnd(); }
VEXA_GL_GUEST_EXPORT void glRectsv(const GLshort* a, const GLshort* b) { if (!a || !b) return; glBegin(0x0007); glVertex3f(a[0], a[1], 0); glVertex3f(b[0], a[1], 0); glVertex3f(b[0], b[1], 0); glVertex3f(a[0], b[1], 0); glEnd(); }
VEXA_GL_GUEST_EXPORT void glPushMatrix(void) { vexa_ff::push_matrix(); vexa_ff::sync_clip_matrices(); }
VEXA_GL_GUEST_EXPORT void glPopMatrix(void) { vexa_ff::pop_matrix(); vexa_ff::sync_clip_matrices(); }
VEXA_GL_GUEST_EXPORT void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble znear, GLdouble zfar) {
  vexa_ff::frustum(static_cast<float>(left), static_cast<float>(right), static_cast<float>(bottom), static_cast<float>(top),
                   static_cast<float>(znear), static_cast<float>(zfar)); vexa_ff::sync_clip_matrices();
}
VEXA_GL_GUEST_EXPORT GLuint glGenLists(GLsizei range) { return vexa_ff::gen_lists(range); }
VEXA_GL_GUEST_EXPORT void glNewList(GLuint list, GLenum mode) { vexa_ff::new_list(list, mode); }
VEXA_GL_GUEST_EXPORT void glEndList(void) { vexa_ff::end_list(); }
VEXA_GL_GUEST_EXPORT void glCallList(GLuint list) { vexa_ff::call_list(list); }
VEXA_GL_GUEST_EXPORT void glDeleteLists(GLuint list, GLsizei range) { vexa_ff::delete_lists(list, range); }
VEXA_GL_GUEST_EXPORT GLboolean glIsList(GLuint list) { return vexa_ff::is_list(list) ? GL_TRUE : GL_FALSE; }
VEXA_GL_GUEST_EXPORT void glListBase(GLuint base) { vexa_ff::list_base(base); }
VEXA_GL_GUEST_EXPORT void glCallLists(GLsizei n, GLenum type, const void* lists) {
  if (n < 0) { fexfn_pack_VEXA_CompatError(GL_INVALID_VALUE); return; }
  if (type != GL_BYTE && type != GL_UNSIGNED_BYTE && type != GL_SHORT && type != GL_UNSIGNED_SHORT &&
      type != GL_INT && type != GL_UNSIGNED_INT && type != GL_FLOAT && type != 0x1407 && type != 0x1408 && type != 0x1409) {
    fexfn_pack_VEXA_CompatError(GL_INVALID_ENUM); return;
  }
  if (!lists) return;
  const auto* p = static_cast<const unsigned char*>(lists);
  for (GLsizei i = 0; i < n; ++i) {
    uint32_t value = 0;
    switch (type) {
      case GL_BYTE: { int8_t v; memcpy(&v,p,1); value=static_cast<uint32_t>(v); p+=1; break; }
      case GL_UNSIGNED_BYTE: value=*p++; break;
      case GL_SHORT: { int16_t v; memcpy(&v,p,2); value=static_cast<uint32_t>(v); p+=2; break; }
      case GL_UNSIGNED_SHORT: { uint16_t v; memcpy(&v,p,2); value=v; p+=2; break; }
      case GL_INT: { int32_t v; memcpy(&v,p,4); value=static_cast<uint32_t>(v); p+=4; break; }
      case GL_UNSIGNED_INT: memcpy(&value,p,4); p+=4; break;
      case GL_FLOAT: { float v; memcpy(&v,p,4); value=static_cast<uint32_t>(static_cast<int64_t>(v)); p+=4; break; }
      default: { const int bytes = static_cast<int>(type) - 0x1405; for(int j=0;j<bytes;++j) value=(value<<8)|*p++; break; }
    }
    vexa_ff::call_offset(value);
  }
}

VEXA_GL_GUEST_EXPORT void glShadeModel(GLenum /*mode*/) {}

// glMapBuffer is intentionally NOT defined here (see libvexa_glmap.c): exporting it from libGL makes
// Unity (Megabonk, Vulkan mode) take its desktop-GL path and crash.
// ---- Desktop GL entrypoints ETS2 resolves via dlsym(libGL.so.1) ----
// Host GLES lacks most of these; emulate with thunked siblings or no-op.

VEXA_GL_GUEST_EXPORT void glCompressedTexImage1D(GLenum target, GLint level, GLenum internalformat,
                                                  GLsizei width, GLint border, GLsizei imageSize, const void* data) {
  ::glCompressedTexImage2D(GL_TEXTURE_2D, level, internalformat, width, 1, border, imageSize, data);
}

VEXA_GL_GUEST_EXPORT void glCompressedTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width,
                                                     GLenum format, GLsizei imageSize, const void* data) {
  ::glCompressedTexSubImage2D(GL_TEXTURE_2D, level, xoffset, 0, width, 1, format, imageSize, data);
}

VEXA_GL_GUEST_EXPORT void glGetCompressedTexImage(GLenum /*target*/, GLint /*level*/, void* /*img*/) {}

VEXA_GL_GUEST_EXPORT void glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* data) {
  if (!data || size <= 0) return;
  void* p = ::glMapBufferRange(target, offset, size, 0x0001 /*GL_MAP_READ_BIT*/);
  if (!p) return;
  memcpy(data, p, static_cast<size_t>(size));
  ::glUnmapBuffer(target);
}

VEXA_GL_GUEST_EXPORT void glGetQueryObjectiv(GLuint id, GLenum pname, GLint* params) {
  GLuint v = 0;
  ::glGetQueryObjectuiv(id, pname, &v);
  if (params) *params = static_cast<GLint>(v);
}

VEXA_GL_GUEST_EXPORT void glBindFragDataLocation(GLuint /*program*/, GLuint /*color*/, const GLchar* /*name*/) {}
VEXA_GL_GUEST_EXPORT void glBindFragDataLocationIndexed(GLuint /*program*/, GLuint /*color*/, GLuint /*index*/, const GLchar* /*name*/) {}

VEXA_GL_GUEST_EXPORT void glFramebufferTexture1D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
  VexaCompat_glFramebufferTexture1D(target, attachment, textarget, texture, level);
}

VEXA_GL_GUEST_EXPORT void glFramebufferTexture3D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLint zoffset) {
  VexaCompat_glFramebufferTexture3D(target, attachment, textarget, texture, level, zoffset);
}

VEXA_GL_GUEST_EXPORT void glGetActiveUniformName(GLuint program, GLuint uniformIndex, GLsizei bufSize, GLsizei* length, GLchar* uniformName) {
  GLint size = 0;
  GLenum type = 0;
  ::glGetActiveUniform(program, uniformIndex, bufSize, length, &size, &type, uniformName);
}

// Scratch for the (lo, hi) packing below: stack storage for typical batch
// sizes, heap only for unusually large ones.
struct VexaPackBuf {
  unsigned local[64];
  std::vector<unsigned> heap;
  unsigned* get(GLsizei count) {
    const size_t n = static_cast<size_t>(count) * 2;
    if (n <= sizeof(local) / sizeof(local[0])) return local;
    heap.resize(n);
    return heap.data();
  }
};

// Packs 64-bit values as (lo, hi) pairs for the VEXA_* batch thunks.
static const unsigned* VexaPack64(const GLintptr* src, GLsizei count, unsigned* dst) {
  if (!src) return nullptr;
  for (GLsizei i = 0; i < count; ++i) {
    const uint64_t v = static_cast<uint64_t>(static_cast<int64_t>(src[i]));
    dst[2 * i] = static_cast<unsigned>(v);
    dst[2 * i + 1] = static_cast<unsigned>(v >> 32);
  }
  return dst;
}
static const unsigned* VexaPack32(const GLsizei* src, GLsizei count, unsigned* dst) {
  if (!src) return nullptr;
  for (GLsizei i = 0; i < count; ++i) {
    const uint64_t v = static_cast<uint64_t>(static_cast<int64_t>(src[i]));
    dst[2 * i] = static_cast<unsigned>(v);
    dst[2 * i + 1] = static_cast<unsigned>(v >> 32);
  }
  return dst;
}

VEXA_GL_GUEST_EXPORT void glMultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type,
                                                         const void* const* indices, GLsizei drawcount, const GLint* basevertex) {
  if (drawcount <= 0) return;
  VexaPackBuf buf;
  unsigned* offs = buf.get(drawcount);
  for (GLsizei i = 0; i < drawcount; ++i) {
    const uint64_t v = reinterpret_cast<uintptr_t>(indices[i]);
    offs[2 * i] = static_cast<unsigned>(v);
    offs[2 * i + 1] = static_cast<unsigned>(v >> 32);
  }
  fexfn_pack_VEXA_MultiDrawElementsBV(mode, type, count, offs, basevertex, drawcount);
}

VEXA_GL_GUEST_EXPORT void glMultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
  fexfn_pack_VEXA_MultiDrawIndirect(mode, 0, reinterpret_cast<uintptr_t>(indirect), drawcount, stride);
}

VEXA_GL_GUEST_EXPORT void glMultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride) {
  fexfn_pack_VEXA_MultiDrawIndirect(mode, type, reinterpret_cast<uintptr_t>(indirect), drawcount, stride);
}

VEXA_GL_GUEST_EXPORT void glBindBuffersBase(GLenum target, GLuint first, GLsizei count, const GLuint* buffers) {
  fexfn_pack_VEXA_BindMany(1, target, first, count, buffers, nullptr, nullptr);
}

VEXA_GL_GUEST_EXPORT void glBindBuffersRange(GLenum target, GLuint first, GLsizei count, const GLuint* buffers,
                                             const GLintptr* offsets, const GLsizeiptr* sizes) {
  if (count <= 0) return;
  VexaPackBuf o, sz;
  fexfn_pack_VEXA_BindMany(2, target, first, count, buffers, VexaPack64(offsets, count, o.get(count)),
                           VexaPack64(sizes, count, sz.get(count)));
}

VEXA_GL_GUEST_EXPORT void glBindTextures(GLuint first, GLsizei count, const GLuint* textures) {
  fexfn_pack_VEXA_BindMany(3, 0, first, count, textures, nullptr, nullptr);
}

VEXA_GL_GUEST_EXPORT void glBindSamplers(GLuint first, GLsizei count, const GLuint* samplers) {
  fexfn_pack_VEXA_BindMany(4, 0, first, count, samplers, nullptr, nullptr);
}

VEXA_GL_GUEST_EXPORT void glBindImageTextures(GLuint first, GLsizei count, const GLuint* textures) {
  fexfn_pack_VEXA_BindMany(6, 0, first, count, textures, nullptr, nullptr);
}

VEXA_GL_GUEST_EXPORT void glBindVertexBuffers(GLuint first, GLsizei count, const GLuint* buffers,
                                              const GLintptr* offsets, const GLsizei* strides) {
  if (count <= 0) return;
  VexaPackBuf o, st;
  fexfn_pack_VEXA_BindMany(5, 0, first, count, buffers, VexaPack64(offsets, count, o.get(count)),
                           VexaPack32(strides, count, st.get(count)));
}

VEXA_GL_GUEST_EXPORT void glInvalidateBufferData(GLuint /*buffer*/) {}
VEXA_GL_GUEST_EXPORT void glInvalidateBufferSubData(GLuint /*buffer*/, GLintptr /*offset*/, GLsizeiptr /*length*/) {}
VEXA_GL_GUEST_EXPORT void glDepthBounds(GLclampd /*zmin*/, GLclampd /*zmax*/) {}
VEXA_GL_GUEST_EXPORT void glClipControl(GLenum /*origin*/, GLenum /*depth*/) {}
VEXA_GL_GUEST_EXPORT void glVertexAttribLFormat(GLuint /*index*/, GLint /*size*/, GLenum /*type*/, GLuint /*relativeoffset*/) {}


VEXA_GL_GUEST_EXPORT void glFrameTerminator(void) {}
VEXA_GL_GUEST_EXPORT void glPushGroupMarker(GLsizei /*length*/, const GLchar* /*marker*/) {}
VEXA_GL_GUEST_EXPORT void glPopGroupMarker(void) {}
VEXA_GL_GUEST_EXPORT void glInsertEventMarker(GLsizei /*length*/, const GLchar* /*marker*/) {}

VEXA_GL_GUEST_EXPORT GLuint glCreateShaderProgramv(GLenum type, GLsizei count, const GLchar* const* strings) {
  if (count <= 0 || !strings) return 0;
  GLuint shader = ::glCreateShader(type);
  if (!shader) return 0;
  ::glShaderSource(shader, count, strings, nullptr);
  ::glCompileShader(shader);
  // The game only reports "driver provided no explanation"; surface the real
  // Mali compile/link log (and the source head) so failures are diagnosable.
  GLint compiled = GL_FALSE;
  ::glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled != GL_TRUE) {
    char log[2048] = {};
    GLsizei written = 0;
    ::glGetShaderInfoLog(shader, sizeof(log) - 1, &written, log);
    fprintf(stderr, "[libGL thunk] SHADER-COMPILE-FAILED type=0x%x log='%.1500s' src0='%.400s'\n",
            type, log, strings[0] ? strings[0] : "(null)");
  }
  GLuint program = ::glCreateProgram();
  if (!program) { ::glDeleteShader(shader); return 0; }
  // Program pipelines (glUseProgramStages) only accept separable programs.
  ::glProgramParameteri(program, GL_PROGRAM_SEPARABLE, GL_TRUE);
  ::glAttachShader(program, shader);
  ::glLinkProgram(program);
  if (compiled == GL_TRUE) {
    GLint linked = GL_FALSE;
    ::glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
      char log[2048] = {};
      GLsizei written = 0;
      ::glGetProgramInfoLog(program, sizeof(log) - 1, &written, log);
      fprintf(stderr, "[libGL thunk] SHADER-LINK-FAILED type=0x%x log='%.1500s'\n", type, log);
    }
  }
  ::glDetachShader(program, shader);
  ::glDeleteShader(shader);
  return program;
}

VEXA_GL_GUEST_EXPORT void glLabelObject(GLenum identifier, GLuint name, GLsizei length, const GLchar* label) {
  ::glObjectLabel(identifier, name, length, label);
}

VEXA_GL_GUEST_EXPORT int glXGetGPUIDsAMD(unsigned int /*maxCount*/, unsigned int* /*ids*/) { return 0; }
VEXA_GL_GUEST_EXPORT int glXGetGPUInfoAMD(unsigned int /*id*/, int /*property*/, GLenum /*dataType*/, unsigned int /*size*/, void* /*data*/) { return 0; }
VEXA_GL_GUEST_EXPORT unsigned int glXGetContextGPUIDAMD(void* /*ctx*/) { return 0; }

VEXA_GL_GUEST_EXPORT int glXSwapIntervalSGI(int interval) {
  return fexfn_pack_VEXA_SetSwapInterval(interval);
}

VEXA_GL_GUEST_EXPORT void glPolygonOffsetClamp(GLfloat factor, GLfloat units, GLfloat clamp) {
  (void)clamp;
  ::glPolygonOffset(factor, units);
}
VEXA_GL_GUEST_EXPORT void glRenderbufferStorageMultisampleCoverage(GLenum target, GLsizei /*coverageSamples*/,
                                                                    GLsizei colorSamples, GLenum internalformat,
                                                                    GLsizei width, GLsizei height) {
  ::glRenderbufferStorageMultisample(target, colorSamples, internalformat, width, height);
}


static void VexaCompat_glTexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat,
                                                  GLsizei width, GLsizei height, GLboolean fixedsamplelocations) {
  ::glTexStorage2DMultisample(target, samples, internalformat, width, height, fixedsamplelocations);
}

static void VexaCompat_glGetTextureParameteriv(GLuint /*texture*/, GLenum /*pname*/, GLint* params) {
  if (params) *params = 0;
}

static void VexaCompat_glGetTextureLevelParameteriv(GLuint /*texture*/, GLint /*level*/, GLenum /*pname*/, GLint* params) {
  if (params) *params = 0;
}

#undef VEXA_GL_GUEST_EXPORT

}
#endif
