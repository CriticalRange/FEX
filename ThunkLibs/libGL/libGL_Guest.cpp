/*
$info$
tags: thunklibs|GL
desc: Handles glXGetProcAddress
$end_info$
*/

#define GL_GLEXT_PROTOTYPES 1
#define GLX_GLXEXT_PROTOTYPES 1

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
#include "android_fixed_function.h"
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
    , seq {GLGuestTraceSeq.fetch_add(1, std::memory_order_acq_rel) + 1} {
    if (!GLGuestTraceEnabled()) return;
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

uintptr_t ResolveAndroidCompatGuestProc(std::string_view procname) {
#ifdef BUILD_ANDROID
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
  if (procname == "glIsTexture") return reinterpret_cast<uintptr_t>(glIsTexture);
  if (procname == "glBegin") return reinterpret_cast<uintptr_t>(glBegin);
  if (procname == "glEnd") return reinterpret_cast<uintptr_t>(glEnd);
  if (procname == "glVertex3f") return reinterpret_cast<uintptr_t>(glVertex3f);
  if (procname == "glNormal3f") return reinterpret_cast<uintptr_t>(glNormal3f);
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
  if (procname_s == "glShaderSource" || procname_s == "glShaderSourceARB") {
    // VEXA_FIXES: Route shader source through explicit guest bridge helper.
    // Return a guest-callable entrypoint for shader source and avoid exposing
    // raw host pointers that can SIGSEGV when invoked directly by guest code.
    const auto shader_source_addr = reinterpret_cast<uintptr_t>(glShaderSource);
    TraceGLProcLink(procname_s, reinterpret_cast<const void*>(shader_source_addr), shader_source_addr);
    return reinterpret_cast<voidFunc*>(glShaderSource);
  }

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

static void OnInit() {
  GLGUEST_TRACE_SCOPE();
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
  "GLX_ARB_create_context GLX_EXT_framebuffer_sRGB GLX_EXT_swap_control GLX_MESA_swap_control";
constexpr uintptr_t kCompatDisplay = 0x47584453ULL;  // "GXDS"
constexpr uintptr_t kCompatContext = 0x47584354ULL;  // "GXCT"
constexpr uintptr_t kCompatVisual = 0x47585649ULL;   // "GXVI"
constexpr uintptr_t kCompatFBConfig = 0x47584642ULL; // "GXFB"
constexpr unsigned long kCompatDrawable = 1UL;
constexpr unsigned long kCompatPbuffer = 1UL;
static thread_local void* g_vexa_current_context = nullptr;
static thread_local unsigned long g_vexa_current_drawable = 0;

#define VEXA_GL_GUEST_EXPORT __attribute__((visibility("default")))

// Ubuntu Xlib Visual / XVisualInfo, LP64. class sits at 0x10. A zeroed info
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
static_assert(offsetof(VexaVisual, c_class) == 0x10, "Visual.class");
static_assert(sizeof(VexaVisual) == 56, "Visual size");
static_assert(offsetof(VexaXVisualInfo, c_class) == 24, "XVisualInfo.class");
static_assert(sizeof(VexaXVisualInfo) == 64, "XVisualInfo size");

static VexaVisual g_vexa_visual = {nullptr, 1, 4, 0x00ff0000, 0x0000ff00, 0x000000ff, 8, 256};

static void* VexaGlxVisualInfo() {
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

VEXA_GL_GUEST_EXPORT void* glXChooseVisual(void* /*display*/, int /*screen*/, int* /*attributes*/) {
  fprintf(stderr, "[libGL thunk] compat glXChooseVisual\n");
  return VexaGlxVisualInfo();
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
}

VEXA_GL_GUEST_EXPORT void* glXGetCurrentContext() {
  return g_vexa_current_context;
}

VEXA_GL_GUEST_EXPORT unsigned long glXGetCurrentDrawable() {
  return g_vexa_current_drawable;
}

VEXA_GL_GUEST_EXPORT int glXMakeCurrent(void* /*display*/, unsigned long drawable, void* context) {
  fprintf(stderr, "[libGL thunk] compat glXMakeCurrent drawable=%lu context=%p\n", drawable, context);
  g_vexa_current_context = context;
  g_vexa_current_drawable = context ? drawable : 0;
  vexa_ff::init_matrices();
  if (!context) {
    const int released = fexfn_pack_VEXA_ReleaseGlxSurface();
    if (!released) {
      fprintf(stderr, "[libGL thunk] glXMakeCurrent release failed\n");
    }
    return released;
  }
  const int bound = fexfn_pack_VEXA_BindGlxSurface(reinterpret_cast<uintptr_t>(context));
  if (!bound) {
    fprintf(stderr, "[libGL thunk] glXMakeCurrent bind failed\n");
  }
  return bound;
}

VEXA_GL_GUEST_EXPORT void glXSwapBuffers(void* /*display*/, unsigned long drawable) {
  static unsigned diag_swaps;
  if (++diag_swaps % 120 == 0 && access("/tmp/vexa-unity-refresh", F_OK) == 0) {
    Dl_info info {};
    const unsigned char build_id[] = {0xb4,0x4d,0x37,0x66,0x01,0xaf,0x15,0xf4,0x48,0x66,0xad,0x62,0x23,0x20,0xc9,0xa8,0x4d,0xfe,0xa7,0x87};
    if (dladdr(__builtin_return_address(0), &info) && info.dli_fbase && info.dli_fname && strstr(info.dli_fname,"UnityPlayer.so")) {
      auto base = reinterpret_cast<unsigned char*>(info.dli_fbase);
      if (!memcmp(base + 0x2b8, build_id, sizeof(build_id))) {
        auto geti = [&](uintptr_t offset) { return reinterpret_cast<int (*)(void)>(base + offset)(); };
        auto getf = [&](uintptr_t offset) { return reinterpret_cast<float (*)(void)>(base + offset)(); };
        fprintf(stderr, "[VEXA][DIAG] Unity frame=%d time=%f scale=%f screen=%dx%d focused=%d cameras=%d\n",
                geti(0x782f50), getf(0x782ab0), getf(0x782f20), geti(0x765570), geti(0x765580), static_cast<int>(reinterpret_cast<unsigned char (*)(void)>(base + 0x75f240)()), geti(0x762970));
        if (diag_swaps == 120) {
          int w=0,h=0;
          reinterpret_cast<void (*)(int,int*,int*)>(base+0x765460)(0,&w,&h);
          fprintf(stderr,"[VEXA][DIAG] display BEFORE count=%d render=%dx%d\n",geti(0xde0670),w,h);
          reinterpret_cast<unsigned char (*)(void)>(base+0xde04c0)();
          reinterpret_cast<void (*)(int,int*,int*)>(base+0x765460)(0,&w,&h);
          fprintf(stderr,"[VEXA][DIAG] display AFTER count=%d render=%dx%d\n",geti(0xde0670),w,h);
        }
        if (diag_swaps == 120 || diag_swaps == 1200) {
          auto mgr = *reinterpret_cast<unsigned char**>(base + 0x1f2e730);
          for (int list : {0x10, 0x38}) {
            auto ids = *reinterpret_cast<int**>(mgr + list);
            int count = *reinterpret_cast<int*>(mgr + list + 0x10);
            fprintf(stderr, "[VEXA][DIAG] camera list=%x count=%d\n", list, count);
            for (int i=0; ids && i<count && i<16; ++i) {
              auto obj = reinterpret_cast<unsigned char* (*)(int)>(base + 0x780940)(ids[i]);
              auto cam = obj ? *reinterpret_cast<unsigned char**>(obj + 0x10) : nullptr;
              auto global = *reinterpret_cast<unsigned char***>(base+0x1ee55f0);
              auto table = global ? *global : nullptr;
              if (table) {
                auto records = *reinterpret_cast<unsigned char**>(table);
                auto mask = *reinterpret_cast<unsigned int*>(table+8);
                fprintf(stderr, "[VEXA][DIAG] obj=%p cached=%p hashmask=%x\n", obj,cam,mask);
                if (records && mask < 0x1000000) for (size_t o=0; o < (size_t(mask)+8)*3; o+=24) {
                  if (*reinterpret_cast<int*>(records+o+8)==ids[i]) {
                    cam=*reinterpret_cast<unsigned char**>(records+o+16);
                    break;
                  }
                }
              }
              if (!cam) continue;
              auto ci = [&](uintptr_t off) { return reinterpret_cast<int (*)(void*)>(base+off)(cam); };
              auto rt = reinterpret_cast<void* (*)(void*)>(base+0x93e490)(cam);
              float rect[4]{};
              reinterpret_cast<void (*)(void*,float*)>(base+0x761f20)(cam, rect);
              fprintf(stderr, "[VEXA][DIAG] camera id=%d native=%p size=%dx%d rect=%f,%f,%f,%f mask=%x target=%p display=%d clear=%d\n", ids[i], cam, ci(0x761f50), ci(0x761f60), rect[0],rect[1],rect[2],rect[3],ci(0x7618b0),rt,ci(0x762000),ci(0x761b80));
            }
          }
        }

      }
    }
  }
  if (GLGuestTraceEnabled()) {
    fprintf(stderr, "[libGL thunk] compat glXSwapBuffers drawable=%lu\n", drawable);
  }
  fexfn_pack_VEXA_SwapGlxBuffers();
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
  int w = 0;
  int h = 0;
  if (sscanf(buf, "%d %d", &w, &h) == 2 && w > 1 && h > 1) {
    *width = static_cast<unsigned int>(w);
    *height = static_cast<unsigned int>(h);
  }
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

VEXA_GL_GUEST_EXPORT void* glXGetVisualFromFBConfig(void* /*display*/, void* /*config*/) {
  fprintf(stderr, "[libGL thunk] compat glXGetVisualFromFBConfig\n");
  return VexaGlxVisualInfo();
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
    case 13: *value = 0; break;
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

VEXA_GL_GUEST_EXPORT void* glXCreateNewContext(void*, void*, int, void*, int) {
  fprintf(stderr, "[libGL thunk] compat glXCreateNewContext\n");
  return reinterpret_cast<void*>(kCompatContext);
}

VEXA_GL_GUEST_EXPORT int glXGetFBConfigAttrib(void*, void*, int attribute, int* value) {
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
VEXA_GL_GUEST_EXPORT unsigned long glXCreateWindow(void*, void*, unsigned long, const int*) { return kCompatDrawable; }
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

VEXA_GL_GUEST_EXPORT void glDrawBuffer(GLenum /*mode*/) {
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
  GLfloat tmp[16] {};
  ::glGetFloatv(pname, tmp);
  for (int i = 0; i < 16; ++i) {
    params[i] = static_cast<GLdouble>(tmp[i]);
  }
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

VEXA_GL_GUEST_EXPORT GLboolean glIsEnabled(GLenum cap) {
  fprintf(stderr, "[libGL thunk] compat glIsEnabled cap=0x%x -> GL_FALSE\n", cap);
  return GL_FALSE;
}

VEXA_GL_GUEST_EXPORT void glDepthRange(GLdouble z_near, GLdouble z_far) {
  fprintf(stderr, "[libGL thunk] compat glDepthRange near=%f far=%f (no-op)\n",
          static_cast<double>(z_near), static_cast<double>(z_far));
}

VEXA_GL_GUEST_EXPORT void glGetPointerv(GLenum /*pname*/, void** params) {
  if (params) {
    *params = nullptr;
  }
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

VEXA_GL_GUEST_EXPORT void glBegin(GLenum mode) { vexa_ff::begin(mode); }
VEXA_GL_GUEST_EXPORT void glEnd(void) { vexa_ff::end(); }
VEXA_GL_GUEST_EXPORT void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { vexa_ff::vertex(x, y, z); }
VEXA_GL_GUEST_EXPORT void glNormal3f(GLfloat x, GLfloat y, GLfloat z) { vexa_ff::normal(x, y, z); }
VEXA_GL_GUEST_EXPORT void glMatrixMode(GLenum mode) { vexa_ff::matrix_mode(mode); }
VEXA_GL_GUEST_EXPORT void glLoadIdentity(void) { vexa_ff::load_identity(); }
VEXA_GL_GUEST_EXPORT void glTranslatef(GLfloat x, GLfloat y, GLfloat z) { vexa_ff::translate(x, y, z); }
VEXA_GL_GUEST_EXPORT void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) { vexa_ff::rotate(angle, x, y, z); }
VEXA_GL_GUEST_EXPORT void glTranslated(GLdouble x, GLdouble y, GLdouble z) {
  vexa_ff::translate(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
}
VEXA_GL_GUEST_EXPORT void glPushMatrix(void) { vexa_ff::push_matrix(); }
VEXA_GL_GUEST_EXPORT void glPopMatrix(void) { vexa_ff::pop_matrix(); }
VEXA_GL_GUEST_EXPORT void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble znear, GLdouble zfar) {
  vexa_ff::frustum(static_cast<float>(left), static_cast<float>(right), static_cast<float>(bottom), static_cast<float>(top),
                   static_cast<float>(znear), static_cast<float>(zfar));
}
VEXA_GL_GUEST_EXPORT GLuint glGenLists(GLsizei range) { return vexa_ff::gen_lists(range); }
VEXA_GL_GUEST_EXPORT void glNewList(GLuint list, GLenum mode) { vexa_ff::new_list(list, mode); }
VEXA_GL_GUEST_EXPORT void glEndList(void) { vexa_ff::end_list(); }
VEXA_GL_GUEST_EXPORT void glCallList(GLuint list) { vexa_ff::call_list(list); }
VEXA_GL_GUEST_EXPORT void glDeleteLists(GLuint list, GLsizei range) { vexa_ff::delete_lists(list, range); }
VEXA_GL_GUEST_EXPORT void glShadeModel(GLenum /*mode*/) {}
VEXA_GL_GUEST_EXPORT void glLightfv(GLenum /*light*/, GLenum /*pname*/, const GLfloat* /*params*/) {}
VEXA_GL_GUEST_EXPORT void glMaterialfv(GLenum /*face*/, GLenum pname, const GLfloat* params) {
  vexa_ff::material(pname, params);
}

#undef VEXA_GL_GUEST_EXPORT

}
#endif
