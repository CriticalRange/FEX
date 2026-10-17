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

// Forward decls for compat exports defined at file end.
void glDepthBounds(GLclampd, GLclampd);
void glSelectBuffer(GLsizei, GLuint*);
void glColorPointer(GLint, GLenum, GLsizei, const void*);
void glEnableClientState(GLenum);
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
    fprintf(stderr, "[libGL thunk] %s baseinstance=%u ignored (no GL_EXT_base_instance on this driver)\n", fn, baseinstance);
  }
}
static void VexaCompat_glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type,
                                                               const void* indices, GLsizei instancecount, GLuint baseinstance) {
  VexaWarnBaseInstance("glDrawElementsInstancedBaseInstance", baseinstance);
  ::glDrawElementsInstanced(mode, count, type, indices, instancecount);
}
static void VexaCompat_glDrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type,
                                                                         const void* indices, GLsizei instancecount,
                                                                         GLint basevertex, GLuint baseinstance) {
  VexaWarnBaseInstance("glDrawElementsInstancedBaseVertexBaseInstance", baseinstance);
  ::glDrawElementsInstancedBaseVertex(mode, count, type, indices, instancecount, basevertex);
}
static void VexaCompat_glDrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count,
                                                             GLsizei instancecount, GLuint baseinstance) {
  VexaWarnBaseInstance("glDrawArraysInstancedBaseInstance", baseinstance);
  ::glDrawArraysInstanced(mode, first, count, instancecount);
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
  "GLX_ARB_create_context GLX_EXT_framebuffer_sRGB GLX_EXT_swap_control GLX_MESA_swap_control";
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
  const int result = context
    ? fexfn_pack_VEXA_BindGlxSurface(reinterpret_cast<uintptr_t>(context))
    : fexfn_pack_VEXA_ReleaseGlxSurface();
  // A failed MakeCurrent must preserve the previous current context/drawable.
  if (result) {
    g_vexa_current_context = context;
    g_vexa_current_drawable = context ? drawable : 0;
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

VEXA_GL_GUEST_EXPORT void glMultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type,
                                                         const void* const* indices, GLsizei drawcount, const GLint* basevertex) {
  for (GLsizei i = 0; i < drawcount; ++i) {
    ::glDrawElementsBaseVertex(mode, count[i], type, indices[i], basevertex[i]);
  }
}

VEXA_GL_GUEST_EXPORT void glMultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
  if (drawcount <= 0) return;
  if (stride == 0) stride = 16;
  const auto off = reinterpret_cast<uintptr_t>(indirect);
  const GLsizeiptr map_size = off + static_cast<GLsizeiptr>(stride) * drawcount;
  const uint8_t* p = static_cast<const uint8_t*>(
      ::glMapBufferRange(0x8F3F /*GL_DRAW_INDIRECT_BUFFER*/, 0, map_size, 0x0001));
  if (!p) return;
  for (GLsizei i = 0; i < drawcount; ++i) {
    const uint32_t* e = reinterpret_cast<const uint32_t*>(p + off + static_cast<size_t>(stride) * i);
    ::glDrawArraysInstanced(mode, static_cast<GLint>(e[2]), static_cast<GLsizei>(e[0]), static_cast<GLsizei>(e[1]));
  }
  ::glUnmapBuffer(0x8F3F);
}

VEXA_GL_GUEST_EXPORT void glMultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride) {
  if (drawcount <= 0) return;
  if (stride == 0) stride = 20;
  const auto off = reinterpret_cast<uintptr_t>(indirect);
  const GLsizeiptr map_size = off + static_cast<GLsizeiptr>(stride) * drawcount;
  const uint8_t* p = static_cast<const uint8_t*>(
      ::glMapBufferRange(0x8F3F /*GL_DRAW_INDIRECT_BUFFER*/, 0, map_size, 0x0001));
  if (!p) return;
  const GLsizeiptr elem_size = (type == 0x1403 /*UNSIGNED_SHORT*/) ? 2 : 4;
  for (GLsizei i = 0; i < drawcount; ++i) {
    const uint32_t* e = reinterpret_cast<const uint32_t*>(p + off + static_cast<size_t>(stride) * i);
    ::glDrawElementsInstancedBaseVertex(mode, static_cast<GLsizei>(e[0]), type,
                                        reinterpret_cast<const void*>(static_cast<uintptr_t>(e[2] * elem_size)),
                                        static_cast<GLsizei>(e[1]), static_cast<GLint>(e[3]));
  }
  ::glUnmapBuffer(0x8F3F);
}

VEXA_GL_GUEST_EXPORT void glBindBuffersBase(GLenum target, GLuint first, GLsizei count, const GLuint* buffers) {
  for (GLsizei i = 0; i < count; ++i) ::glBindBufferBase(target, first + i, buffers[i]);
}

VEXA_GL_GUEST_EXPORT void glBindBuffersRange(GLenum target, GLuint first, GLsizei count, const GLuint* buffers,
                                             const GLintptr* offsets, const GLsizeiptr* sizes) {
  for (GLsizei i = 0; i < count; ++i) ::glBindBufferRange(target, first + i, buffers[i], offsets[i], sizes[i]);
}

VEXA_GL_GUEST_EXPORT void glBindTextures(GLuint first, GLsizei count, const GLuint* textures) {
  for (GLsizei i = 0; i < count; ++i) {
    if (!textures[i]) continue;
    ::glActiveTexture(0x84C0 /*GL_TEXTURE0*/ + first + i);
    ::glBindTexture(GL_TEXTURE_2D, textures[i]);
  }
}

VEXA_GL_GUEST_EXPORT void glBindSamplers(GLuint first, GLsizei count, const GLuint* samplers) {
  for (GLsizei i = 0; i < count; ++i) ::glBindSampler(first + i, samplers[i]);
}

VEXA_GL_GUEST_EXPORT void glBindImageTextures(GLuint first, GLsizei count, const GLuint* textures) {
  for (GLsizei i = 0; i < count; ++i) {
    ::glBindImageTexture(first + i, textures[i], 0, GL_TRUE, 0, 0x88B9 /*GL_WRITE_ONLY*/, 0x8058 /*GL_RGBA8*/);
  }
}

VEXA_GL_GUEST_EXPORT void glBindVertexBuffers(GLuint first, GLsizei count, const GLuint* buffers,
                                              const GLintptr* offsets, const GLsizei* strides) {
  for (GLsizei i = 0; i < count; ++i) {
    ::glBindVertexBuffer(first + i, buffers[i], offsets ? offsets[i] : 0, strides ? strides[i] : 0);
  }
}

VEXA_GL_GUEST_EXPORT void glInvalidateBufferData(GLuint /*buffer*/) {}
VEXA_GL_GUEST_EXPORT void glInvalidateBufferSubData(GLuint /*buffer*/, GLintptr /*offset*/, GLsizeiptr /*length*/) {}
VEXA_GL_GUEST_EXPORT void glDepthBounds(GLclampd /*zmin*/, GLclampd /*zmax*/) {}
VEXA_GL_GUEST_EXPORT void glClipControl(GLenum /*origin*/, GLenum /*depth*/) {}
VEXA_GL_GUEST_EXPORT void glVertexAttribLFormat(GLuint /*index*/, GLint /*size*/, GLenum /*type*/, GLuint /*relativeoffset*/) {}
VEXA_GL_GUEST_EXPORT void glSelectBuffer(GLsizei /*size*/, GLuint* /*buffer*/) {}
VEXA_GL_GUEST_EXPORT void glColorPointer(GLint /*size*/, GLenum /*type*/, GLsizei /*stride*/, const void* /*pointer*/) {}
VEXA_GL_GUEST_EXPORT void glEnableClientState(GLenum /*array*/) {}
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
