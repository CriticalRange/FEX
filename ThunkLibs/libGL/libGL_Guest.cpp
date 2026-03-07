/*
$info$
tags: thunklibs|GL
desc: Handles glXGetProcAddress
$end_info$
*/

#define GL_GLEXT_PROTOTYPES 1
#define GLX_GLXEXT_PROTOTYPES 1

#include <GL/glx.h>
#include <GL/glxext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#undef GL_ARB_viewport_array
#include "glcorearb.h"

#include <dlfcn.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "common/Guest.h"

#include "thunkgen_guest_libGL.inl"

typedef void voidFunc();

#ifdef BUILD_ANDROID
namespace {
constexpr const char* GLProcTracePath = "/sdcard/HyMobile.Android/gl_proc_trace.log";

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

  std::fprintf(stderr,
               "[libGL thunk] %s unresolved GL proc '%s' (%s, host=%p)\n",
               phase ? phase : "log",
               proc_copy,
               reason_copy,
               host_target);

  if (FILE* f = std::fopen(GLProcTracePath, "a")) {
    std::fprintf(f,
                 "[libGL thunk] %s unresolved GL proc '%s' (%s, host=%p)\n",
                 phase ? phase : "log",
                 proc_copy,
                 reason_copy,
                 host_target);
    std::fclose(f);
  }
}

[[noreturn]] void MissingGLProcStub() {
  LogMissingGLProcEvent("invoked", nullptr, nullptr, nullptr);
  std::abort();
}
}
#endif

namespace {
using GLGetFramebufferAttachmentParameterivFn = void (*)(GLenum, GLenum, GLenum, GLint*);

std::atomic<GLGetFramebufferAttachmentParameterivFn> CachedGLGetFramebufferAttachmentParameteriv {nullptr};
std::atomic<bool> CachedGLGetFramebufferAttachmentParameterivResolved {false};
std::atomic<uint32_t> GLProcLinkTraceBudget {512};
std::atomic<bool> GLFramebufferAttachCompatPrelinked {false};

void TraceGLProcLink(std::string_view procname, const void* host_target, uintptr_t guest_target) {
  uint32_t remaining = GLProcLinkTraceBudget.load(std::memory_order_acquire);
  while (remaining > 0 &&
         !GLProcLinkTraceBudget.compare_exchange_weak(
             remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  if (remaining == 0) {
    return;
  }

  std::fprintf(stderr,
               "[libGL thunk] link '%.*s' host=%p guest=%p\n",
               static_cast<int>(procname.size()),
               procname.data(),
               host_target,
               reinterpret_cast<void*>(guest_target));
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

void HyMobileCompat_glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint* params) {
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
  if (GLFramebufferAttachCompatPrelinked.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  const auto guest_target = reinterpret_cast<uintptr_t>(HyMobileCompat_glGetFramebufferAttachmentParameteriv);

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
  // Some GLES drivers expose glClearDepthf through dispatch stubs that can jump
  // to a null slot under FEX. Returning direct guest thunk entry points avoids
  // routing through that host-side dispatch path.
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
  fexfn_pack_FEX_glShaderSource(shader, count, reinterpret_cast<uintptr_t>(string), length);
}

void glShaderSourceARB(GLhandleARB shader, GLsizei count, const GLcharARB** string, const GLint* length) {
  fexfn_pack_FEX_glShaderSource(shader, count, reinterpret_cast<uintptr_t>(string), length);
}

voidFunc* glXGetProcAddress(const GLubyte* procname) {
  std::string_view procname_s {reinterpret_cast<const char*>(procname)};
  if (procname_s == "glShaderSource" || procname_s == "glShaderSourceARB") {
    // Return a guest-callable entrypoint for shader source and avoid exposing
    // raw host pointers that can SIGSEGV when invoked directly by guest code.
    return reinterpret_cast<voidFunc*>(glShaderSource);
  }

#ifdef BUILD_ANDROID
  auto ReturnMissingDiagnostic = [&](const char* reason, const void* host_target = nullptr) -> voidFunc* {
    LogMissingGLProcEvent("returning stub for", reinterpret_cast<const char*>(procname), reason, host_target);
    return reinterpret_cast<voidFunc*>(MissingGLProcStub);
  };

  if (procname_s == "glXGetProcAddress" || procname_s == "glXGetProcAddressARB") {
    return reinterpret_cast<voidFunc*>(glXGetProcAddress);
  }
  if (procname_s == "glGetFramebufferAttachmentParameteriv" || procname_s == "glGetFramebufferAttachmentParameterivEXT") {
    EnsureFramebufferAttachmentCompatPrelinked();
    return reinterpret_cast<voidFunc*>(HyMobileCompat_glGetFramebufferAttachmentParameteriv);
  }
  if (const auto guest_direct = ResolveAndroidDirectGuestProc(procname_s); guest_direct != 0) {
    TraceGLProcLink(procname_s, reinterpret_cast<const void*>(guest_direct), guest_direct);
    return reinterpret_cast<voidFunc*>(guest_direct);
  }

  auto Ret = fexfn_pack_glXGetProcAddress(procname);
  if (!Ret) {
    return ReturnMissingDiagnostic("host lookup returned null");
  }

  auto TargetFuncIt = HostPtrInvokers.find(procname_s);
  if (TargetFuncIt == HostPtrInvokers.end()) {
    const auto alias = ResolveGLInvokerAlias(procname_s);
    if (!alias.empty()) {
      TargetFuncIt = HostPtrInvokers.find(alias);
    }

    if (TargetFuncIt == HostPtrInvokers.end()) {
      return ReturnMissingDiagnostic(alias.empty() ? "guest invoker missing" : "guest invoker missing (alias)", Ret);
    }
  }

  TraceGLProcLink(procname_s, Ret, TargetFuncIt->second);
  LinkAddressToFunction((uintptr_t)Ret, TargetFuncIt->second);
  return Ret;
#else
  if (procname_s == "glGetFramebufferAttachmentParameteriv" || procname_s == "glGetFramebufferAttachmentParameterivEXT") {
    EnsureFramebufferAttachmentCompatPrelinked();
    return reinterpret_cast<voidFunc*>(HyMobileCompat_glGetFramebufferAttachmentParameteriv);
  }
  if (const auto guest_direct = ResolveAndroidDirectGuestProc(procname_s); guest_direct != 0) {
    TraceGLProcLink(procname_s, reinterpret_cast<const void*>(guest_direct), guest_direct);
    return reinterpret_cast<voidFunc*>(guest_direct);
  }

  auto Ret = fexfn_pack_glXGetProcAddress(procname);
  if (!Ret) {
    return nullptr;
  }

  auto TargetFuncIt = HostPtrInvokers.find(procname_s);
  if (TargetFuncIt == HostPtrInvokers.end()) {
    const auto alias = ResolveGLInvokerAlias(procname_s);
    if (!alias.empty()) {
      TargetFuncIt = HostPtrInvokers.find(alias);
    }

    if (TargetFuncIt != HostPtrInvokers.end()) {
      TraceGLProcLink(procname_s, reinterpret_cast<const void*>(Ret), TargetFuncIt->second);
      LinkAddressToFunction((uintptr_t)Ret, TargetFuncIt->second);
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

  TraceGLProcLink(procname_s, reinterpret_cast<const void*>(Ret), TargetFuncIt->second);
  LinkAddressToFunction((uintptr_t)Ret, TargetFuncIt->second);
  return Ret;
#endif
}

voidFunc* glXGetProcAddressARB(const GLubyte* procname) {
  return glXGetProcAddress(procname);
}
}

// Wrapper around malloc() without noexcept specifiers
static void* malloc_wrapper(size_t size) {
  return malloc(size);
}

static void OnInit() {
#ifndef BUILD_ANDROID
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
