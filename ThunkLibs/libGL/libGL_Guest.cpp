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
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "common/Guest.h"

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

[[noreturn]] void MissingGLProcStub() {
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

  fprintf(stderr,
               "[libGL thunk] invoked unresolved GL proc '%s' (%s, host=%p)\n",
               proc_copy,
               reason_copy,
               nullptr);

  if (FILE* f = std::fopen(GLProcTracePath, "a")) {
    fprintf(f,
                 "[libGL thunk] invoked unresolved GL proc '%s' (%s, host=%p)\n",
                 proc_copy,
                 reason_copy,
                 nullptr);
    std::fclose(f);
  }

  std::abort();
}
}
#endif

namespace {
using GLGetFramebufferAttachmentParameterivFn = void (*)(GLenum, GLenum, GLenum, GLint*);

std::atomic<uint64_t> GLGuestTraceSeq {0};

struct ScopedGLGuestTrace {
  explicit ScopedGLGuestTrace(const char* fn_)
    : fn {fn_ ? fn_ : "<null>"}
    , seq {GLGuestTraceSeq.fetch_add(1, std::memory_order_acq_rel) + 1} {
    fprintf(stderr,
                 "[libGL-guest] call-enter seq=%llu fn=%s\n",
                 static_cast<unsigned long long>(seq),
                 fn);
  }

  ~ScopedGLGuestTrace() {
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
void glXDestroyContext(void* display, void* context);
void* glXGetCurrentContext();
unsigned long glXGetCurrentDrawable();
int glXMakeCurrent(void* display, unsigned long drawable, void* context);
void glXSwapBuffers(void* display, unsigned long drawable);
const char* glXQueryExtensionsString(void* display, int screen);
void* glXGetCurrentDisplay();
void* glXChooseFBConfig(void* display, int screen, const int* attributes, int* num_items);
unsigned long glXCreatePbuffer(void* display, void* config, const int* attrib_list);
void glXDestroyPbuffer(void* display, unsigned long pbuffer);
void* glXGetVisualFromFBConfig(void* display, void* config);
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
  return 0;
}

uintptr_t ResolveAndroidCompatGuestProc(std::string_view procname) {
#ifdef BUILD_ANDROID
  if (procname == "glXChooseVisual") return reinterpret_cast<uintptr_t>(glXChooseVisual);
  if (procname == "glXCreateContext") return reinterpret_cast<uintptr_t>(glXCreateContext);
  if (procname == "glXDestroyContext") return reinterpret_cast<uintptr_t>(glXDestroyContext);
  if (procname == "glXGetCurrentContext") return reinterpret_cast<uintptr_t>(glXGetCurrentContext);
  if (procname == "glXGetCurrentDrawable") return reinterpret_cast<uintptr_t>(glXGetCurrentDrawable);
  if (procname == "glXMakeCurrent") return reinterpret_cast<uintptr_t>(glXMakeCurrent);
  if (procname == "glXSwapBuffers") return reinterpret_cast<uintptr_t>(glXSwapBuffers);
  if (procname == "glXQueryExtensionsString") return reinterpret_cast<uintptr_t>(glXQueryExtensionsString);
  if (procname == "glXGetCurrentDisplay") return reinterpret_cast<uintptr_t>(glXGetCurrentDisplay);
  if (procname == "glXChooseFBConfig") return reinterpret_cast<uintptr_t>(glXChooseFBConfig);
  if (procname == "glXCreatePbuffer") return reinterpret_cast<uintptr_t>(glXCreatePbuffer);
  if (procname == "glXDestroyPbuffer") return reinterpret_cast<uintptr_t>(glXDestroyPbuffer);
  if (procname == "glXGetVisualFromFBConfig") return reinterpret_cast<uintptr_t>(glXGetVisualFromFBConfig);
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
  // VEXA_FIXES: Android branch keeps startup resilient by emitting diagnostics and
  // returning a deterministic abort stub when host/guest proc mapping is incomplete.
  auto ReturnMissingDiagnostic = [&](const char* reason, const void* host_target = nullptr) -> voidFunc* {
    LogMissingGLProcEvent("returning stub for", reinterpret_cast<const char*>(procname), reason, host_target);
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
constexpr const char* kCompatGLXExts = "GLX_ARB_create_context GLX_EXT_framebuffer_sRGB";
constexpr uintptr_t kCompatDisplay = 0x47584453ULL;  // "GXDS"
constexpr uintptr_t kCompatContext = 0x47584354ULL;  // "GXCT"
constexpr uintptr_t kCompatVisual = 0x47585649ULL;   // "GXVI"
constexpr uintptr_t kCompatFBConfig = 0x47584642ULL; // "GXFB"
constexpr unsigned long kCompatDrawable = 1UL;
constexpr unsigned long kCompatPbuffer = 1UL;

#define VEXA_GL_GUEST_EXPORT __attribute__((visibility("default")))

VEXA_GL_GUEST_EXPORT void* glXChooseVisual(void* /*display*/, int /*screen*/, int* /*attributes*/) {
  fprintf(stderr, "[libGL thunk] compat glXChooseVisual\n");
  return reinterpret_cast<void*>(kCompatVisual);
}

VEXA_GL_GUEST_EXPORT void* glXCreateContext(void* /*display*/, void* /*visual*/, void* /*share_list*/, int /*direct*/) {
  fprintf(stderr, "[libGL thunk] compat glXCreateContext\n");
  return reinterpret_cast<void*>(kCompatContext);
}

VEXA_GL_GUEST_EXPORT void glXDestroyContext(void* /*display*/, void* context) {
  fprintf(stderr, "[libGL thunk] compat glXDestroyContext context=%p\n", context);
}

VEXA_GL_GUEST_EXPORT void* glXGetCurrentContext() {
  return reinterpret_cast<void*>(kCompatContext);
}

VEXA_GL_GUEST_EXPORT unsigned long glXGetCurrentDrawable() {
  return kCompatDrawable;
}

VEXA_GL_GUEST_EXPORT int glXMakeCurrent(void* /*display*/, unsigned long drawable, void* context) {
  fprintf(stderr, "[libGL thunk] compat glXMakeCurrent drawable=%lu context=%p\n", drawable, context);
  return 1;
}

VEXA_GL_GUEST_EXPORT void glXSwapBuffers(void* /*display*/, unsigned long drawable) {
  fprintf(stderr, "[libGL thunk] compat glXSwapBuffers drawable=%lu\n", drawable);
  glFlush();
}

VEXA_GL_GUEST_EXPORT const char* glXQueryExtensionsString(void* /*display*/, int /*screen*/) {
  return kCompatGLXExts;
}

VEXA_GL_GUEST_EXPORT void* glXGetCurrentDisplay() {
  return reinterpret_cast<void*>(kCompatDisplay);
}

VEXA_GL_GUEST_EXPORT void* glXChooseFBConfig(void* /*display*/, int /*screen*/, const int* /*attributes*/, int* num_items) {
  static void* configs[1] {reinterpret_cast<void*>(kCompatFBConfig)};
  if (num_items) {
    *num_items = 1;
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
  return reinterpret_cast<void*>(kCompatVisual);
}

VEXA_GL_GUEST_EXPORT void glLineWidth(GLfloat width) {
  fprintf(stderr, "[libGL thunk] compat glLineWidth width=%f (no-op)\n", static_cast<double>(width));
}

VEXA_GL_GUEST_EXPORT void glPolygonMode(GLenum /*face*/, GLenum /*mode*/) {
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

#undef VEXA_GL_GUEST_EXPORT

}
#endif
