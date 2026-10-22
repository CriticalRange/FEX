/*
$info$
tags: thunklibs|GL
desc: Uses glXGetProcAddress instead of dlsym
$end_info$
*/

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <cstdarg>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <memory>
#include <algorithm>
#include <sstream>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "clone_binary_cache.h"
#include <unistd.h>
#include <sys/syscall.h>

#define GL_GLEXT_PROTOTYPES 1
#define GLX_GLXEXT_PROTOTYPES 1

#include "glcorearb.h"

// VEXA_FIXES: Android host path uses EGL/GLES headers and a GLX-compat shim layer.
#ifdef BUILD_ANDROID
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl31.h>
#include <GLES3/gl3ext.h>
#include "common/AndroidWindow.h"
#if __has_include(<android/log.h>)
#include <android/log.h>
#define VEXA_HAS_ANDROID_LOG 1
#else
#define VEXA_HAS_ANDROID_LOG 0
#endif
#ifdef VEXA_ENABLE_RUNTIME_SHADER_TRANSLATOR
#include <shaderc/shaderc.hpp>
#include <spirv_glsl.hpp>
#include <sys/stat.h>
#endif
#else
#include <GL/glx.h>
#include <GL/glxext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <xcb/xcb.h>
#endif

#include "common/Host.h"
#ifndef BUILD_ANDROID
#include "common/X11Manager.h"
#endif

#if defined(BUILD_ANDROID)
#include "program_cache_state.inc"
static int VexaGLHostFprintf(FILE* stream, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  const int rc = std::vfprintf(stream, fmt, args);
  va_end(args);
  if (stream == stderr) {
#if VEXA_HAS_ANDROID_LOG
    __android_log_vprint(ANDROID_LOG_ERROR, "Vexa-ThunkGLHost", fmt, copy);
#endif
  }
  va_end(copy);
  return rc;
}
#define fprintf(stream, ...) VexaGLHostFprintf(stream, __VA_ARGS__)
#endif

#ifdef BUILD_ANDROID
// VEXA_FIXES: Provide minimal GLX/X11 type shims so upstream libGL thunk interfaces
// compile on Android while routing behavior through EGL/GLES.
struct _XDisplay {};
using Display = _XDisplay;
#ifdef IS_32BIT_THUNK
using guest_size_t = uint32_t;
#else
using guest_size_t = size_t;
#endif
struct Visual;
using VisualID = unsigned long;

struct __GLXFBConfigRec {};
struct __GLXcontextRec {};
struct GLXHyperpipeNetworkSGIX {
  char Placeholder {};
};
struct GLXHyperpipeConfigSGIX {
  char Placeholder {};
};

struct XVisualInfo {
  Visual* visual;
  VisualID visualid;
  int screen;
  int depth;
  int c_class;
  unsigned long red_mask;
  unsigned long green_mask;
  unsigned long blue_mask;
  int colormap_size;
  int bits_per_rgb;
};

using Bool = int;
using XID = unsigned long;
using Window = XID;
using Font = XID;
using Pixmap = XID;
using Colormap = XID;
using GLXDrawable = XID;
using GLXPixmap = XID;
using GLXPbuffer = XID;
using GLXPbufferSGIX = XID;
using GLXWindow = XID;
using GLXVideoCaptureDeviceNV = XID;
using GLXVideoDeviceNV = XID;
using GLXContextID = XID;
using GLXContext = __GLXcontextRec*;
using GLXFBConfig = __GLXFBConfigRec*;
using GLXFBConfigSGIX = __GLXFBConfigRec*;
using __GLXextFuncPtr = void (*)();
using GLhandleARB = unsigned int;
using GLcharARB = char;
using GLvdpauSurfaceNV = GLintptr;
#endif

#ifndef BUILD_ANDROID
template<>
struct host_layout<_XDisplay*> {
  _XDisplay* data;
  _XDisplay* guest_display;

  host_layout(guest_layout<_XDisplay*>&);

  ~host_layout();
};

static X11Manager x11_manager;
#endif

static void* (*GuestMalloc)(guest_size_t) = nullptr;
#ifdef BUILD_ANDROID
static void (*GuestFree)(void*) = nullptr;
static void fexfn_impl_libGL_GL_SetGuestFree(uintptr_t target, uintptr_t unpacker) {
  MakeHostTrampolineForGuestFunctionAt(target, unpacker, &GuestFree);
}
#endif
#ifdef BUILD_ANDROID
#include "android_gl_guest32.h"
#endif
namespace {
constexpr uint64_t kThunkGLXGetProcAddress = 0x4C4942474C0001ULL;
constexpr uint64_t kThunkGLXCreateContext = 0x4C4942474C0002ULL;
constexpr uint64_t kThunkGLGuestMalloc = 0x4C4942474C0003ULL;

constexpr uint64_t kStallInfoThresholdNs = 16ULL * 1000ULL * 1000ULL;
constexpr uint64_t kStallWarnThresholdNs = 100ULL * 1000ULL * 1000ULL;
std::atomic<uint64_t> GLHostLastProgressNs {0};
std::atomic<uint64_t> GLHostLastProgressSerial {0};
std::atomic<const char*> GLHostLastCall {"<none>"};
std::mutex GLHostLastProcMutex;
char GLHostLastRequestedProc[128] {};

uint64_t GLHostNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
    .count();
}

void GLHostMarkProgress(const char* call) {
  GLHostLastCall.store(call ? call : "<null>", std::memory_order_release);
  GLHostLastProgressNs.store(GLHostNowNs(), std::memory_order_release);
  GLHostLastProgressSerial.fetch_add(1, std::memory_order_acq_rel);
}

void GLHostSetLastRequestedProc(const char* procname) {
  std::scoped_lock lk(GLHostLastProcMutex);
  if (!procname) {
    std::snprintf(GLHostLastRequestedProc, sizeof(GLHostLastRequestedProc), "%s", "<null>");
    return;
  }
  std::snprintf(GLHostLastRequestedProc, sizeof(GLHostLastRequestedProc), "%s", procname);
}

// Verbose host logging is opt-in: create files/vexa_glverbose.flag to enable.
// The steady-state path stays quiet so per-frame fprintf I/O can't add latency.
static bool VexaGLVerbose() {
  static const int enabled =
    access("/data/data/com.critical.vexaemulator/files/vexa_glverbose.flag", F_OK) == 0 ? 1 : 0;
  return enabled != 0;
}
#define VEXA_LOGV(...) do { if (VexaGLVerbose()) fprintf(stderr, __VA_ARGS__); } while (0)

// Heavy GL diagnostics (synchronous KHR_debug callbacks, present-path pixel
// readbacks and framebuffer dumps) are opt-in: create files/vexa_gldbg.flag.
static bool VexaGLDebugEnabled() {
  static const int enabled =
    access("/data/data/com.critical.vexaemulator/files/vexa_gldbg.flag", F_OK) == 0 ? 1 : 0;
  return enabled != 0;
}

#ifdef BUILD_ANDROID
void GLHostLogCurrentEGLState(const char* site) {
  const EGLDisplay display = eglGetCurrentDisplay();
  const EGLSurface draw_surface = eglGetCurrentSurface(EGL_DRAW);
  const EGLSurface read_surface = eglGetCurrentSurface(EGL_READ);
  const EGLContext context = eglGetCurrentContext();
  VEXA_LOGV(
               "[libGL-host] %s egl_state display=%p draw=%p read=%p context=%p\n",
               site ? site : "<unknown>",
               reinterpret_cast<void*>(display),
               reinterpret_cast<void*>(draw_surface),
               reinterpret_cast<void*>(read_surface),
               reinterpret_cast<void*>(context));
}

const char* GLHostGLStringName(GLenum name) {
  switch (name) {
    case GL_VENDOR: return "GL_VENDOR";
    case GL_RENDERER: return "GL_RENDERER";
    case GL_VERSION: return "GL_VERSION";
    case GL_SHADING_LANGUAGE_VERSION: return "GL_SHADING_LANGUAGE_VERSION";
    case GL_EXTENSIONS: return "GL_EXTENSIONS";
    default: return "UNKNOWN";
  }
}

// ES fragment shaders have no default precision for float and no default at
// all for most opaque types, while desktop GLSL never declares precision. A
// raw substring check for "precision " both misses partially-qualified sources
// and false-triggers on unrelated tokens (e.g. GL_ARB_shader_precision), so
// always emit a complete default-precision set. Precision statements later in
// the source still override these defaults.
constexpr std::string_view kVexaShaderPrecisionEs300 {
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp sampler2D;\n"
    "precision highp sampler3D;\n"
    "precision highp samplerCube;\n"
    "precision highp sampler2DShadow;\n"
    "precision highp samplerCubeShadow;\n"
    "precision highp sampler2DArray;\n"
    "precision highp sampler2DArrayShadow;\n"
    "precision highp isampler2D;\n"
    "precision highp isampler3D;\n"
    "precision highp isamplerCube;\n"
    "precision highp isampler2DArray;\n"
    "precision highp usampler2D;\n"
    "precision highp usampler3D;\n"
    "precision highp usamplerCube;\n"
    "precision highp usampler2DArray;\n"};

// ES 3.1/3.2-only sampler types (multisample, cube-array, texel-buffer).
constexpr std::string_view kVexaShaderPrecisionEs320Extra {
    "precision highp sampler2DMS;\n"
    "precision highp isampler2DMS;\n"
    "precision highp usampler2DMS;\n"
    "precision highp sampler2DMSArray;\n"
    "precision highp isampler2DMSArray;\n"
    "precision highp usampler2DMSArray;\n"
    "precision highp samplerCubeArray;\n"
    "precision highp samplerCubeArrayShadow;\n"
    "precision highp isamplerCubeArray;\n"
    "precision highp usamplerCubeArray;\n"
    "precision highp samplerBuffer;\n"
    "precision highp isamplerBuffer;\n"
    "precision highp usamplerBuffer;\n"};

// Insert after the #version line and the contiguous leading #extension block,
// so extension directives remain in the prologue where drivers expect them.
[[maybe_unused]] static size_t VexaShaderPrecisionInsertPos(std::string_view source) {
  size_t insert_pos = 0;
  const auto version_pos = source.find("#version");
  if (version_pos != std::string_view::npos) {
    const auto version_end = source.find('\n', version_pos);
    insert_pos = (version_end == std::string_view::npos) ? source.size() : (version_end + 1);
  }

  size_t cursor = insert_pos;
  while (cursor < source.size()) {
    const auto line_end = source.find('\n', cursor);
    const std::string_view line = source.substr(cursor, line_end == std::string_view::npos ? source.size() - cursor : line_end - cursor);

    size_t begin = 0;
    while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t')) {
      ++begin;
    }
    const auto trimmed = line.substr(begin);

    if (trimmed.empty()) {
      if (line_end == std::string_view::npos) {
        break;
      }
      cursor = line_end + 1;
      continue;
    }

    if (trimmed.starts_with("#extension")) {
      insert_pos = (line_end == std::string_view::npos) ? source.size() : (line_end + 1);
      if (line_end == std::string_view::npos) {
        break;
      }
      cursor = line_end + 1;
      continue;
    }

    break;
  }

  return insert_pos;
}
#endif

void GLHostLogDuration(const char* call, uint64_t elapsed_ns) {
  if (elapsed_ns >= kStallWarnThresholdNs) {
    VEXA_LOGV(
                 "[libGL-host] stall-warn call=%s duration_ms=%llu\n",
                 call ? call : "<null>",
                 static_cast<unsigned long long>(elapsed_ns / 1000000ULL));
  } else if (elapsed_ns >= kStallInfoThresholdNs) {
    VEXA_LOGV(
                 "[libGL-host] stall-info call=%s duration_ms=%llu\n",
                 call ? call : "<null>",
                 static_cast<unsigned long long>(elapsed_ns / 1000000ULL));
  }
}

struct ScopedGLCallTimer {
  explicit ScopedGLCallTimer(const char* name_)
    : name {name_}
    , start_ns {VexaGLVerbose() ? GLHostNowNs() : 0} {
    GLHostMarkProgress(name);
  }
  ~ScopedGLCallTimer() {
    if (!start_ns) return;
    const uint64_t elapsed_ns = GLHostNowNs() - start_ns;
    GLHostLogDuration(name, elapsed_ns);
    GLHostMarkProgress(name);
  }
  const char* name;
  uint64_t start_ns;
};

std::atomic<uint64_t> GLHostTraceSeq {0};
// Total time inside host-side GL thunk calls, accumulated per frame window and
// reported in the [VEXA][FRAME] summary. Splits host GL/driver time from
// guest/JIT time when diagnosing low FPS.
std::atomic<uint64_t> g_vexa_glcall_ns {0};

// Per-call trace lines cost real frame time, so they are opt-in via VEXA_GL_TRACE=1.
bool GLHostTraceEnabled() {
  static const bool Enabled = [] {
    const char* v = getenv("VEXA_GL_TRACE");
    return v && v[0] && v[0] != '0';
  }();
  return Enabled;
}

struct ScopedGLThunkTrace {
  explicit ScopedGLThunkTrace(const char* fn_)
    : fn {fn_ ? fn_ : "<null>"}
    , seq {0}
    , start_ns {0} {
    GLHostLastCall.store(fn, std::memory_order_relaxed);
    if (GLHostTraceEnabled()) {
      seq = GLHostTraceSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
      start_ns = GLHostNowNs();
      VEXA_LOGV(
                   "[libGL-host] call-enter seq=%llu fn=%s\n",
                   static_cast<unsigned long long>(seq),
                   fn);
    }
  }

  ~ScopedGLThunkTrace() {
    if (!GLHostTraceEnabled()) return;
    const uint64_t elapsed_ns = GLHostNowNs() - start_ns;
    g_vexa_glcall_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
    {
      VEXA_LOGV(
                   "[libGL-host] call-exit seq=%llu fn=%s duration_us=%llu\n",
                   static_cast<unsigned long long>(seq),
                   fn,
                   static_cast<unsigned long long>(elapsed_ns / 1000ULL));
    }
    GLHostLogDuration(fn, elapsed_ns);
    GLHostMarkProgress(fn);
  }

  const char* fn;
  uint64_t seq;
  uint64_t start_ns;
};

#define GLHOST_TRACE_SCOPE() ScopedGLThunkTrace vexa_gl_trace_scope_##__LINE__ {__func__}

extern "C" __attribute__((visibility("default")))
uint64_t Vexa_GLHost_LastProgressNs() {
  return GLHostLastProgressNs.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default")))
uint64_t Vexa_GLHost_LastProgressSerial() {
  return GLHostLastProgressSerial.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default")))
const char* Vexa_GLHost_LastCall() {
  const char* call = GLHostLastCall.load(std::memory_order_acquire);
  return call ? call : "<none>";
}

extern "C" __attribute__((visibility("default")))
const char* Vexa_GLHost_LastRequestedProc() {
  std::scoped_lock lk(GLHostLastProcMutex);
  return GLHostLastRequestedProc[0] ? GLHostLastRequestedProc : "<none>";
}

// VEXA_FIXES: Optional runtime GLSL->SPIR-V->ESSL translation path for desktop-shader guests.
#if defined(BUILD_ANDROID) && defined(VEXA_ENABLE_RUNTIME_SHADER_TRANSLATOR)
std::mutex ShaderSourceCacheMutex;
std::unordered_map<size_t, std::string> ShaderSourceCache;
using RuntimeLaunchSerialGetter = uint64_t (*)();
std::atomic<uint64_t> LastSeenRuntimeLaunchSerial {0};

constexpr int32_t kDefaultShaderTranspileLogBudget = -1;
constexpr int32_t kDefaultShaderDriverErrorLogBudget = 256;
std::atomic<int32_t> ShaderDriverErrorLogBudget {kDefaultShaderDriverErrorLogBudget};

std::atomic<int32_t> ShaderTranspileLogBudget {kDefaultShaderTranspileLogBudget}; // -1 = unlimited
std::once_flag ShaderDumpDirInitOnce;
bool ShaderDumpDirsReady {false};

// VEXA_FIXES: Keep shader debug dumps in app-internal artifacts storage.
constexpr const char* ShaderDumpRootDir = "/data/user/0/com.critical.vexaemulator/files/artifacts/shaders";
constexpr const char* ShaderDumpOriginalDir = "/data/user/0/com.critical.vexaemulator/files/artifacts/shaders/original";
constexpr const char* ShaderDumpTranslatedDir = "/data/user/0/com.critical.vexaemulator/files/artifacts/shaders/translated";
constexpr const char* ShaderDriverErrorLogPath = "/data/user/0/com.critical.vexaemulator/files/artifacts/shaders/driver_errors.log";

uint64_t QueryRuntimeLaunchSerial() {
  static RuntimeLaunchSerialGetter getter = nullptr;
  static bool resolved = false;
  if (!resolved) {
    resolved = true;
    getter = reinterpret_cast<RuntimeLaunchSerialGetter>(dlsym(RTLD_DEFAULT, "Vexa_GetRuntimeLaunchSerial"));

    if (!getter) {
      if (void* h = dlopen("libvexa.so", RTLD_NOW | RTLD_NOLOAD)) {
        getter = reinterpret_cast<RuntimeLaunchSerialGetter>(dlsym(h, "Vexa_GetRuntimeLaunchSerial"));
      }
    }

    if (!getter) {
      if (void* h = dlopen("libFEXCore.so", RTLD_NOW | RTLD_NOLOAD)) {
        getter = reinterpret_cast<RuntimeLaunchSerialGetter>(dlsym(h, "Vexa_GetRuntimeLaunchSerial"));
      }
    }
  }

  return getter ? getter() : 0;
}

void MaybeResetShaderStateForNewLaunch() {
  const uint64_t serial = QueryRuntimeLaunchSerial();
  if (serial == 0) {
    return;
  }

  uint64_t last = LastSeenRuntimeLaunchSerial.load(std::memory_order_acquire);
  if (serial == last) {
    return;
  }
  if (!LastSeenRuntimeLaunchSerial.compare_exchange_strong(
          last, serial, std::memory_order_acq_rel, std::memory_order_acquire)) {
    return;
  }

  ShaderDriverErrorLogBudget.store(kDefaultShaderDriverErrorLogBudget, std::memory_order_release);
  ShaderTranspileLogBudget.store(kDefaultShaderTranspileLogBudget, std::memory_order_release);
  {
    std::lock_guard lk(ShaderSourceCacheMutex);
    ShaderSourceCache.clear();
  }

  VEXA_LOGV(
               "[libGL-host] launch-serial=%llu reset shader cache and budgets\n",
               static_cast<unsigned long long>(serial));
}

bool ConsumeShaderTranspileLogBudget() {
  int32_t remaining = ShaderTranspileLogBudget.load(std::memory_order_acquire);

  // Unlimited mode for deep debugging sessions.
  if (remaining < 0) {
    return true;
  }

  while (remaining > 0 &&
         !ShaderTranspileLogBudget.compare_exchange_weak(
             remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
  }

  return remaining > 0;
}

void LogShaderTranspileEvent(const char* event, GLenum shader_type, const char* detail = nullptr) {
  MaybeResetShaderStateForNewLaunch();
  if (!ConsumeShaderTranspileLogBudget()) {
    return;
  }

  VEXA_LOGV("[libGL-host] shader-transpile %s type=0x%x%s%s\n",
               event ? event : "unknown",
               static_cast<unsigned>(shader_type),
               detail ? " detail=" : "",
               detail ? detail : "");
}

shaderc_shader_kind ShaderKindFromGLType(GLenum shader_type) {
  switch (shader_type) {
    case GL_VERTEX_SHADER:
      return shaderc_vertex_shader;
    case GL_FRAGMENT_SHADER:
      return shaderc_fragment_shader;
    case GL_GEOMETRY_SHADER:
      return shaderc_geometry_shader;
    case GL_TESS_CONTROL_SHADER:
      return shaderc_tess_control_shader;
    case GL_TESS_EVALUATION_SHADER:
      return shaderc_tess_evaluation_shader;
    case GL_COMPUTE_SHADER:
      return shaderc_compute_shader;
    default:
      return shaderc_glsl_infer_from_source;
  }
}

const char* ShaderTypeTag(GLenum shader_type) {
  switch (shader_type) {
    case GL_VERTEX_SHADER:
      return "vert";
    case GL_FRAGMENT_SHADER:
      return "frag";
    case GL_GEOMETRY_SHADER:
      return "geom";
    case GL_TESS_CONTROL_SHADER:
      return "tesc";
    case GL_TESS_EVALUATION_SHADER:
      return "tese";
    case GL_COMPUTE_SHADER:
      return "comp";
    default:
      return "unknown";
  }
}

bool EnsureDirectoryExists(const char* path) {
  if (!path || !*path) {
    return false;
  }

  if (::mkdir(path, 0775) == 0) {
    return true;
  }

  return errno == EEXIST;
}

bool EnsureShaderDumpDirectories() {
  std::call_once(ShaderDumpDirInitOnce, []() {
    ShaderDumpDirsReady =
        EnsureDirectoryExists(ShaderDumpRootDir) &&
        EnsureDirectoryExists(ShaderDumpOriginalDir) &&
        EnsureDirectoryExists(ShaderDumpTranslatedDir);
  });

  return ShaderDumpDirsReady;
}

void DumpShaderDebugText(bool original_dir, GLenum shader_type, size_t cache_key, const char* suffix, std::string_view text) {
  if (!suffix || !*suffix || text.empty()) {
    return;
  }

  if (!EnsureShaderDumpDirectories()) {
    return;
  }

  const char* dir = original_dir ? ShaderDumpOriginalDir : ShaderDumpTranslatedDir;
  const char* tag = ShaderTypeTag(shader_type);

  char path[512] {};
  std::snprintf(path, sizeof(path), "%s/%016zx_%s_%s", dir, cache_key, tag, suffix);

  if (FILE* f = std::fopen(path, "wb")) {
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
  }
}

std::string FlattenShaderSource(GLsizei count, const GLchar* const* strings, const GLint* lengths) {
  if (!strings || count <= 0) {
    return {};
  }

  size_t total_size = 0;
  for (GLsizei i = 0; i < count; ++i) {
    const char* chunk = reinterpret_cast<const char*>(strings[i]);
    if (!chunk) {
      continue;
    }

    if (lengths && lengths[i] >= 0) {
      total_size += static_cast<size_t>(lengths[i]);
    } else {
      total_size += std::strlen(chunk);
    }
  }

  std::string result;
  result.reserve(total_size);
  for (GLsizei i = 0; i < count; ++i) {
    const char* chunk = reinterpret_cast<const char*>(strings[i]);
    if (!chunk) {
      continue;
    }

    if (lengths && lengths[i] >= 0) {
      result.append(chunk, static_cast<size_t>(lengths[i]));
    } else {
      result.append(chunk);
    }
  }
  return result;
}

bool IsAlreadyESSL(std::string_view source) {
  const auto version_pos = source.find("#version");
  if (version_pos == std::string_view::npos) {
    return false;
  }

  const auto line_end = source.find('\n', version_pos);
  const auto version_line = source.substr(version_pos, line_end == std::string_view::npos ? source.size() - version_pos
                                                                                           : line_end - version_pos);
  return version_line.find(" es") != std::string_view::npos;
}

bool ParseGLSLVersionNumber(std::string_view version_line, int* out_version) {
  if (!out_version) {
    return false;
  }

  const auto version_pos = version_line.find("#version");
  if (version_pos == std::string_view::npos) {
    return false;
  }

  size_t cursor = version_pos + std::string_view {"#version"}.size();
  while (cursor < version_line.size() && std::isspace(static_cast<unsigned char>(version_line[cursor]))) {
    ++cursor;
  }

  size_t digits_begin = cursor;
  while (cursor < version_line.size() && std::isdigit(static_cast<unsigned char>(version_line[cursor]))) {
    ++cursor;
  }

  if (digits_begin == cursor) {
    return false;
  }

  int version = 0;
  for (size_t i = digits_begin; i < cursor; ++i) {
    version = (version * 10) + (version_line[i] - '0');
  }

  *out_version = version;
  return true;
}

std::string RewriteVersionDirective(std::string_view source, std::string_view new_version_line) {
  if (source.empty()) {
    return std::string {new_version_line};
  }

  const auto version_pos = source.find("#version");
  if (version_pos == std::string_view::npos) {
    std::string rewritten;
    rewritten.reserve(new_version_line.size() + 1 + source.size());
    rewritten.append(new_version_line);
    rewritten.push_back('\n');
    rewritten.append(source);
    return rewritten;
  }

  const auto line_end = source.find('\n', version_pos);
  std::string rewritten;
  rewritten.reserve(source.size() + 16);
  rewritten.append(source.substr(0, version_pos));
  rewritten.append(new_version_line);
  if (line_end != std::string_view::npos) {
    rewritten.append(source.substr(line_end));
  } else {
    rewritten.push_back('\n');
  }
  return rewritten;
}

std::string PromoteDesktopVersionForSPIRV(std::string_view source) {
  constexpr std::string_view desktop_target {"#version 330 core"};

  const auto version_pos = source.find("#version");
  if (version_pos == std::string_view::npos) {
    std::string rewritten;
    rewritten.reserve(desktop_target.size() + 1 + source.size());
    rewritten.append(desktop_target);
    rewritten.push_back('\n');
    rewritten.append(source);
    return rewritten;
  }

  const auto line_end = source.find('\n', version_pos);
  const auto current_version_line =
      source.substr(version_pos, line_end == std::string_view::npos ? source.size() - version_pos : line_end - version_pos);

  if (current_version_line.find(" es") != std::string_view::npos) {
    return std::string {source};
  }

  int version_number = 0;
  if (!ParseGLSLVersionNumber(current_version_line, &version_number)) {
    return std::string {source};
  }

  if (version_number >= 330) {
    return std::string {source};
  }

  return RewriteVersionDirective(source, desktop_target);
}

std::string NormalizeVersionToES(std::string_view source) {
  constexpr std::string_view target_version {"#version 320 es"};

  if (source.empty()) {
    return std::string {target_version};
  }

  const auto version_pos = source.find("#version");
  if (version_pos == std::string_view::npos) {
    std::string normalized;
    normalized.reserve(target_version.size() + 1 + source.size());
    normalized.append(target_version);
    normalized.push_back('\n');
    normalized.append(source);
    return normalized;
  }

  const auto line_end = source.find('\n', version_pos);
  const auto current_version_line =
      source.substr(version_pos, line_end == std::string_view::npos ? source.size() - version_pos : line_end - version_pos);

  if (current_version_line.find(" es") != std::string_view::npos) {
    int version_number = 0;
    if (ParseGLSLVersionNumber(current_version_line, &version_number) && version_number == 320) {
      return std::string {source};
    }
  }

  return RewriteVersionDirective(source, target_version);
}

std::string CollapseBackslashNewlines(std::string_view source) {
  std::string out;
  out.reserve(source.size());

  for (size_t i = 0; i < source.size();) {
    if (source[i] == '\\') {
      if (i + 2 < source.size() && source[i + 1] == '\r' && source[i + 2] == '\n') {
        out.push_back(' ');
        i += 3;
        continue;
      }
      if (i + 1 < source.size() && source[i + 1] == '\n') {
        out.push_back(' ');
        i += 2;
        continue;
      }
    }

    out.push_back(source[i]);
    ++i;
  }

  return out;
}

std::string PrepareSourceForTranspile(std::string_view source) {
  std::string prepared = PromoteDesktopVersionForSPIRV(source);
  prepared = CollapseBackslashNewlines(prepared);
  return prepared;
}

size_t FindPrecisionInsertPos(std::string_view source) {
  size_t insert_pos = 0;
  const auto version_pos = source.find("#version");
  if (version_pos != std::string_view::npos) {
    const auto version_end = source.find('\n', version_pos);
    insert_pos = (version_end == std::string_view::npos) ? source.size() : (version_end + 1);
  }

  size_t cursor = insert_pos;
  while (cursor < source.size()) {
    const auto line_end = source.find('\n', cursor);
    const auto line = source.substr(cursor, line_end == std::string_view::npos ? source.size() - cursor : line_end - cursor);

    size_t trimmed_begin = 0;
    while (trimmed_begin < line.size() && std::isspace(static_cast<unsigned char>(line[trimmed_begin]))) {
      ++trimmed_begin;
    }
    const auto trimmed = line.substr(trimmed_begin);

    if (trimmed.empty()) {
      if (line_end == std::string_view::npos) {
        break;
      }
      cursor = line_end + 1;
      continue;
    }

    if (trimmed.rfind("#extension", 0) == 0) {
      insert_pos = (line_end == std::string_view::npos) ? source.size() : (line_end + 1);
      if (line_end == std::string_view::npos) {
        break;
      }
      cursor = line_end + 1;
      continue;
    }

    break;
  }

  return insert_pos;
}

void EnsureFragmentPrecision(std::string* source, GLenum shader_type) {
  if (!source || shader_type != GL_FRAGMENT_SHADER) {
    return;
  }

  const size_t insert_at = FindPrecisionInsertPos(*source);
  source->insert(insert_at, kVexaShaderPrecisionEs300);
  source->insert(insert_at + kVexaShaderPrecisionEs300.size(), kVexaShaderPrecisionEs320Extra);
}

std::string TrimASCII(std::string_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string {text.substr(begin, end - begin)};
}

std::string LTrimASCII(std::string_view text) {
  size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  return std::string {text.substr(begin)};
}

bool StartsWithASCII(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool IsWordBoundary(char c) {
  const unsigned char uc = static_cast<unsigned char>(c);
  return !(std::isalnum(uc) || c == '_');
}

bool ContainsWordASCII(std::string_view text, std::string_view word) {
  if (word.empty() || text.size() < word.size()) {
    return false;
  }

  size_t pos = text.find(word);
  while (pos != std::string_view::npos) {
    const bool left_ok = pos == 0 || IsWordBoundary(text[pos - 1]);
    const size_t right_index = pos + word.size();
    const bool right_ok = right_index >= text.size() || IsWordBoundary(text[right_index]);
    if (left_ok && right_ok) {
      return true;
    }
    pos = text.find(word, pos + 1);
  }

  return false;
}

void EnsureRequiredExtensions(std::string* source, GLenum shader_type) {
  if (!source || source->empty()) {
    return;
  }

  const bool uses_texture_buffer =
      ContainsWordASCII(*source, "samplerBuffer") ||
      ContainsWordASCII(*source, "isamplerBuffer") ||
      ContainsWordASCII(*source, "usamplerBuffer");

  if (uses_texture_buffer &&
      source->find("GL_EXT_texture_buffer") == std::string::npos &&
      source->find("GL_OES_texture_buffer") == std::string::npos) {
    constexpr std::string_view kTextureBufferExt =
        "#if defined(GL_EXT_texture_buffer)\n"
        "#extension GL_EXT_texture_buffer : require\n"
        "#elif defined(GL_OES_texture_buffer)\n"
        "#extension GL_OES_texture_buffer : require\n"
        "#endif\n";

    source->insert(FindPrecisionInsertPos(*source), kTextureBufferExt);
    LogShaderTranspileEvent("inject-ext-texture-buffer", shader_type);
  }
}

std::string StripLocationQualifierFromLayoutLine(std::string_view line) {
  const auto layout_pos = line.find("layout(");
  if (layout_pos == std::string_view::npos) {
    return std::string {line};
  }

  const auto layout_close = line.find(')', layout_pos + 7);
  if (layout_close == std::string_view::npos) {
    return std::string {line};
  }

  const auto layout_inner = line.substr(layout_pos + 7, layout_close - (layout_pos + 7));
  std::vector<std::string> kept_qualifiers;
  size_t cursor = 0;
  while (cursor < layout_inner.size()) {
    const auto comma = layout_inner.find(',', cursor);
    const auto token = layout_inner.substr(cursor, comma == std::string_view::npos ? layout_inner.size() - cursor : comma - cursor);
    const std::string trimmed = TrimASCII(token);
    if (!trimmed.empty() && !StartsWithASCII(trimmed, "location")) {
      kept_qualifiers.emplace_back(trimmed);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    cursor = comma + 1;
  }

  std::string rewritten;
  rewritten.reserve(line.size());
  rewritten.append(line.substr(0, layout_pos));
  if (!kept_qualifiers.empty()) {
    rewritten.append("layout(");
    for (size_t i = 0; i < kept_qualifiers.size(); ++i) {
      if (i != 0) {
        rewritten.append(", ");
      }
      rewritten.append(kept_qualifiers[i]);
    }
    rewritten.push_back(')');
    rewritten.append(line.substr(layout_close + 1));
  } else {
    rewritten.append(LTrimASCII(line.substr(layout_close + 1)));
  }

  return rewritten;
}

std::string StripUniformLocationQualifier(std::string_view line) {
  const auto uniform_pos = line.find("uniform");
  if (uniform_pos == std::string_view::npos) {
    return std::string {line};
  }

  const auto semicolon_pos = line.find(';', uniform_pos);
  if (semicolon_pos == std::string_view::npos) {
    return std::string {line};
  }

  const auto layout_pos = line.find("layout(");
  if (layout_pos == std::string_view::npos || layout_pos > uniform_pos) {
    return std::string {line};
  }

  const auto layout_close = line.find(')', layout_pos + 7);
  if (layout_close == std::string_view::npos || layout_close > uniform_pos) {
    return std::string {line};
  }

  return StripLocationQualifierFromLayoutLine(line);
}

std::string StripUniformBindingQualifier(std::string_view line) {
  const auto uniform_pos = line.find("uniform");
  if (uniform_pos == std::string_view::npos) {
    return std::string {line};
  }

  const auto semicolon_pos = line.find(';', uniform_pos);
  if (semicolon_pos == std::string_view::npos) {
    return std::string {line};
  }

  const auto layout_pos = line.find("layout(");
  if (layout_pos == std::string_view::npos || layout_pos > uniform_pos) {
    return std::string {line};
  }

  const auto layout_close = line.find(')', layout_pos + 7);
  if (layout_close == std::string_view::npos || layout_close > uniform_pos) {
    return std::string {line};
  }

  const auto layout_inner = line.substr(layout_pos + 7, layout_close - (layout_pos + 7));
  if (layout_inner.find("binding") == std::string_view::npos) {
    return std::string {line};
  }

  std::string rewritten;
  rewritten.reserve(line.size());
  rewritten.append(line.substr(0, layout_pos));
  rewritten.append(LTrimASCII(line.substr(layout_close + 1)));
  return rewritten;
}

std::string StripUniformInitializer(std::string_view line) {
  const auto uniform_pos = line.find("uniform");
  if (uniform_pos == std::string_view::npos) {
    return std::string {line};
  }

  const auto semicolon_pos = line.find(';', uniform_pos);
  if (semicolon_pos == std::string_view::npos) {
    return std::string {line};
  }

  const auto equals_pos = line.find('=', uniform_pos);
  if (equals_pos == std::string_view::npos || equals_pos > semicolon_pos) {
    return std::string {line};
  }

  std::string rewritten;
  rewritten.reserve(line.size());
  rewritten.append(line.substr(0, equals_pos));
  rewritten.push_back(';');
  if (semicolon_pos + 1 < line.size()) {
    rewritten.append(line.substr(semicolon_pos + 1));
  }
  return rewritten;
}

std::string StripInterfaceLocationQualifier(std::string_view line, GLenum shader_type) {
  if (line.find("layout(") == std::string_view::npos || line.find(';') == std::string_view::npos) {
    return std::string {line};
  }

  if (line.find("uniform") != std::string_view::npos) {
    return std::string {line};
  }

  const bool has_in = ContainsWordASCII(line, "in");
  const bool has_out = ContainsWordASCII(line, "out");
  if (!has_in && !has_out) {
    return std::string {line};
  }

  // Keep fragment color attachment mappings. Strip only stage interface varying locations.
  if (shader_type == GL_FRAGMENT_SHADER && has_out && !has_in) {
    return std::string {line};
  }

  return StripLocationQualifierFromLayoutLine(line);
}

std::string SanitizeTranslatedESSL(std::string_view source, GLenum shader_type) {
  std::string output;
  output.reserve(source.size());

  size_t cursor = 0;
  while (cursor < source.size()) {
    const auto line_end = source.find('\n', cursor);
    const bool has_newline = line_end != std::string_view::npos;
    const auto line = source.substr(cursor, has_newline ? line_end - cursor : source.size() - cursor);

    std::string rewritten = StripUniformLocationQualifier(line);
    rewritten = StripUniformBindingQualifier(rewritten);
    rewritten = StripUniformInitializer(rewritten);
    rewritten = StripInterfaceLocationQualifier(rewritten, shader_type);
    output.append(rewritten);

    if (has_newline) {
      output.push_back('\n');
      cursor = line_end + 1;
    } else {
      cursor = source.size();
    }
  }

  return output;
}

size_t MakeShaderCacheKey(GLenum shader_type, std::string_view source) {
  const size_t source_hash = std::hash<std::string_view> {}(source);
  return source_hash ^ (static_cast<size_t>(shader_type) * 0x9e3779b97f4a7c15ULL);
}

bool TranspileDesktopGLSLToESSL(GLenum shader_type, std::string_view source, std::string* out_source, std::string* out_error = nullptr) {
  if (!out_source) {
    return false;
  }

  shaderc::Compiler compiler;
  shaderc::CompileOptions options;
  options.SetSourceLanguage(shaderc_source_language_glsl);
  options.SetTargetEnvironment(shaderc_target_env_opengl, shaderc_env_version_opengl_4_5);
  options.SetTargetSpirv(shaderc_spirv_version_1_0);
  // Desktop GLSL usually omits explicit interface/resource layouts; SPIR-V generation
  // requires them, so map locations/bindings deterministically during compilation.
  options.SetAutoMapLocations(true);
  options.SetAutoBindUniforms(true);

  const auto kind = ShaderKindFromGLType(shader_type);
  const std::string source_for_spirv = PrepareSourceForTranspile(source);
  const auto compilation =
      compiler.CompileGlslToSpv(source_for_spirv, kind, "hymobile_guest_shader.glsl", options);
  if (compilation.GetCompilationStatus() != shaderc_compilation_status_success) {
    const auto error_msg = compilation.GetErrorMessage();
    if (out_error) {
      *out_error = error_msg;
    }
    LogShaderTranspileEvent("compile-failed", shader_type, error_msg.empty() ? "unknown shaderc failure" : error_msg.c_str());
    return false;
  }

  try {
    std::vector<uint32_t> spirv(compilation.cbegin(), compilation.cend());
    spirv_cross::CompilerGLSL cross_compiler(std::move(spirv));

    auto glsl_options = cross_compiler.get_common_options();
    glsl_options.es = true;
    glsl_options.version = 320;
    glsl_options.vulkan_semantics = false;
    glsl_options.separate_shader_objects = false;
    cross_compiler.set_common_options(glsl_options);

    *out_source = SanitizeTranslatedESSL(cross_compiler.compile(), shader_type);
  } catch (const std::exception& ex) {
    if (out_error) {
      *out_error = ex.what();
    }
    LogShaderTranspileEvent("cross-failed", shader_type, ex.what());
    return false;
  } catch (...) {
    if (out_error) {
      *out_error = "unknown spirv-cross exception";
    }
    LogShaderTranspileEvent("cross-failed", shader_type, "unknown spirv-cross exception");
    return false;
  }

  EnsureFragmentPrecision(out_source, shader_type);

  return true;
}
#endif
}

#ifndef BUILD_ANDROID
host_layout<_XDisplay*>::host_layout(guest_layout<_XDisplay*>& guest)
  : guest_display(guest.force_get_host_pointer()) {
  data = x11_manager.GuestToHostDisplay(guest_display);
}

host_layout<_XDisplay*>::~host_layout() {
  // Flush host-side event queue to make effects of the guest-side connection visible
  x11_manager.HostXFlush(data);
}

// Functions returning _XDisplay* should be handled explicitly via ptr_passthrough
guest_layout<_XDisplay*> to_guest(host_layout<_XDisplay*>) = delete;
#endif

static void* AndroidGuestMallocFallback(guest_size_t size) {
  return std::malloc(size);
}

static void fexfn_impl_libGL_GL_SetGuestMalloc(uintptr_t GuestTarget, uintptr_t GuestUnpacker) {
  GLHOST_TRACE_SCOPE();
#if defined(BUILD_ANDROID) && !defined(IS_32BIT_THUNK)
  // The 64-bit guest shares the host pointer width. i386 needs guest allocation.
  VEXA_LOGV("[libGL-host] GL_SetGuestMalloc target=0x%llx unpacker=0x%llx\n",
               (unsigned long long)GuestTarget, (unsigned long long)GuestUnpacker);
  (void)GuestTarget;
  (void)GuestUnpacker;
  GuestMalloc = &AndroidGuestMallocFallback;
#else
  MakeHostTrampolineForGuestFunctionAt(GuestTarget, GuestUnpacker, &GuestMalloc);
#endif
}

#ifdef BUILD_ANDROID
// VEXA_FIXES: X11 callback bridges are desktop-only; Android path intentionally no-ops.
static void fexfn_impl_libGL_GL_SetGuestXGetVisualInfo(uintptr_t, uintptr_t) {
  GLHOST_TRACE_SCOPE();
}

static void fexfn_impl_libGL_GL_SetGuestXSync(uintptr_t, uintptr_t) {
  GLHOST_TRACE_SCOPE();
}

static void fexfn_impl_libGL_GL_SetGuestXDisplayString(uintptr_t, uintptr_t) {
  GLHOST_TRACE_SCOPE();
}
#else
static void fexfn_impl_libGL_GL_SetGuestXGetVisualInfo(uintptr_t GuestTarget, uintptr_t GuestUnpacker) {
  GLHOST_TRACE_SCOPE();
  MakeHostTrampolineForGuestFunctionAt(GuestTarget, GuestUnpacker, &x11_manager.GuestXGetVisualInfo);
}

static void fexfn_impl_libGL_GL_SetGuestXSync(uintptr_t GuestTarget, uintptr_t GuestUnpacker) {
  GLHOST_TRACE_SCOPE();
  MakeHostTrampolineForGuestFunctionAt(GuestTarget, GuestUnpacker, &x11_manager.GuestXSync);
}

static void fexfn_impl_libGL_GL_SetGuestXDisplayString(uintptr_t GuestTarget, uintptr_t GuestUnpacker) {
  GLHOST_TRACE_SCOPE();
  MakeHostTrampolineForGuestFunctionAt(GuestTarget, GuestUnpacker, &x11_manager.GuestXDisplayString);
}
#endif

#if defined(BUILD_ANDROID)
struct VexaGlxState {
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLSurface surface = EGL_NO_SURFACE;
  EGLContext context = EGL_NO_CONTEXT;
  EGLConfig config = nullptr;
  int buffer_format = 0;
  ANativeWindow* window = nullptr;
  uint64_t window_serial = 0;
  bool ready = false;
  bool shadow = false;
};

// Headless runs must not touch a SurfaceTexture-backed ANativeWindow at all:
// eglSwapBuffers would queue a buffer, the consumer's frame-available callback
// jumps into Java on this (guest render) thread, and a managed fault there is
// eaten by the FEX host-signal handler instead of becoming an NPE. A pbuffer
// keeps the whole present path inside EGL.
static bool VexaHeadlessMode() {
  static const bool headless = [] {
    const char* e = getenv("VEXA_HEADLESS");
    return e && (e[0] == '1' || !strcmp(e, "true"));
  }();
  return headless;
}

static int VexaHeadlessEnvInt(const char* name, int fallback) {
  const char* v = getenv(name);
  if (!v || !v[0]) return fallback;
  const long n = strtol(v, nullptr, 10);
  return n > 0 ? static_cast<int>(n) : fallback;
}

// The fake X11/RandR layer reads the guest-visible size from a file the app
// publishes. With no real window nobody writes it; mirror the headless
// SurfaceTexture dimensions so the guest does not believe the screen is 0x0.
static void VexaPublishGuestSize(int w, int h) {
  using Pub = void (*)(int, int);
  static const Pub fn = [] {
    if (void* vexa = dlopen("libvexa.so", RTLD_NOW | RTLD_NOLOAD)) {
      return reinterpret_cast<Pub>(dlsym(vexa, "Vexa_PublishGuestWindowSize"));
    }
    return Pub{};
  }();
  if (fn) fn(w, h);
}

// The SurfaceView can hand us a new ANativeWindow at any time (surface
// recreated). eglSwapBuffers against the old window still "works" — frames are
// queued into a surface nobody composites — which is exactly a black screen.
// Rebuild the EGL surface whenever the bridge serial moves.
static uint64_t VexaRuntimeSurfaceSerial() {
  using Getter = uint64_t (*)();
  static Getter fn = [] {
    if (void* vexa = dlopen("libvexa.so", RTLD_NOW | RTLD_NOLOAD)) {
      return reinterpret_cast<Getter>(dlsym(vexa, "Vexa_GetRuntimeSurfaceSerial"));
    }
    return Getter {};
  }();
  return fn ? fn() : 0;
}

static void fexfn_impl_libGL_glViewport(GLint x, GLint y, GLsizei width, GLsizei height);

extern uint64_t g_vexa_diag_draws;
// VEXA_FIXES: Swap counter, so a long frame can be lined up against what else
// happened at that moment (shader compiles, buffer uploads).
static uint64_t g_vexa_swap_no = 0;
// VEXA diag: when non-zero, the diag wrappers log every call (one frame only).
static int g_vexa_trace_frame = 0;
// VEXA: set when the guest draws/clears/blits into FB0 during the current frame.
static bool g_vexa_fb0_drawn = false;

// Per-frame record of which FBOs received draws. Unity GLCore finishes the
// frame in an offscreen FBO but does not leave it bound at swap time, so the
// present fallback must find it by draw activity, not by binding.
struct VexaFrameFbo {
  GLuint fbo = 0;
  uint32_t draws = 0;
};
static VexaFrameFbo g_vexa_frame_fbos[32];
static size_t g_vexa_frame_fbo_count = 0;
// Frames that touched more FBOs than the tracking array holds; the overflowed
// entries are invisible to the present fallback, so it can pick a wrong target.
static uint32_t g_vexa_frame_fbo_overflow = 0;

static uint32_t g_vexa_fb0_draw_count = 0;
static bool g_vexa_fb0_blit = false;
// Diagnostic record of what actually went to FB0 this frame (up to 8 draws).
struct VexaFb0Draw {
  GLenum mode = 0;
  GLsizei count = 0;
  GLint vp_w = 0, vp_h = 0;
  GLuint prog = 0;
  GLint tex = 0;
  GLint blend = 0;
  GLint blend_src = 0, blend_dst = 0;
  GLboolean mask[4] = {};
};
static VexaFb0Draw g_vexa_fb0_draws[8];
static int g_vexa_fb0_draw_n = 0;
// Ordered per-frame event log: seq of each draw (and the fbo it targeted) plus
// bind transitions. Distinguishes "guest really drew the tilemap to the RT"
// from "our tracking lied".
static uint32_t g_vexa_frame_seq = 0;
struct VexaFrameEvent {
  uint32_t seq = 0;
  GLuint fbo = 0;
  GLsizei count = 0;
  GLuint prog = 0;
};
static VexaFrameEvent g_vexa_frame_events[48];
static int g_vexa_frame_event_n = 0;
static char g_vexa_frame_binds[160];
static int g_vexa_frame_binds_len = 0;
static VexaGlxState g_vexa_glx;
static void VexaTrackFrameFbo(GLuint fbo) {
  if (fbo == 0) {
    // Guest contexts that fail to bind the window surface fall back to a
    // per-context pbuffer (VEXA_BindGlxSurface). Draws to "fb0" there are
    // invisible — they must not suppress the present fallback, or the real
    // frame left in an offscreen FBO is dropped and a black frame presents
    // (VS Bat Country inverse flicker).
    if (eglGetCurrentSurface(EGL_DRAW) == g_vexa_glx.surface) {
      g_vexa_fb0_drawn = true;
      ++g_vexa_fb0_draw_count;
    }
    return;
  }
  for (size_t i = 0; i < g_vexa_frame_fbo_count; ++i) {
    if (g_vexa_frame_fbos[i].fbo == fbo) {
      ++g_vexa_frame_fbos[i].draws;
      return;
    }
  }
  if (g_vexa_frame_fbo_count < sizeof(g_vexa_frame_fbos) / sizeof(g_vexa_frame_fbos[0])) {
    auto& e = g_vexa_frame_fbos[g_vexa_frame_fbo_count++];
    e.fbo = fbo;
    e.draws = 1;
  } else {
    ++g_vexa_frame_fbo_overflow;
  }
}
extern uint64_t g_vexa_diag_clears;
extern uint64_t g_vexa_diag_useprog;
extern uint64_t g_vexa_diag_bindfbo;
extern uint64_t g_vexa_diag_dispatches;

// Surface serial for which the VEXA_GLX_SWAP_INTERVAL env override was last
// applied. ~0 forces an apply on the first swap of any surface.
static uint64_t g_vexa_env_interval_serial = ~0ull;

// Called with g_vexa_context_mutex held. Keep the old surface on creation
// failure so a later bind/swap can retry. Do not bind the bootstrap context:
// Unity's render and resource threads own separate guest contexts.
static bool VexaSrgbSurface() {
  const char* flag = getenv("VEXA_GL_SRGB_SURFACE");
  return flag && strcmp(flag, "1") == 0;
}
// When the launch env selects the Vulkan renderer, guest GL probing (Unity
// loads libGL before picking Vulkan) must not connect EGL to the runtime
// window — vkCreateAndroidSurfaceKHR needs the only api connect. GL runs on a
// shadow pbuffer so the probe still sees a working GLES context.
static bool VexaGlShadowForVulkan() {
  static const bool shadow = [] {
    const char* backend = getenv("VEXA_GRAPHICS_BACKEND");
    return backend && strcmp(backend, "vulkan") == 0;
  }();
  return shadow;
}
// eglDestroySurface defers destruction while another thread holds the
// surface current; call Surface::disconnect(api) directly as a fallback so
// the producer frees now. NATIVE_WINDOW_API_EGL == 1, DisconnectModeNormal == 0.
static void VexaDisconnectWindowApi(ANativeWindow* window) {
  using SurfaceDisconnect = int (*)(void*, int, int);
  static auto* surface_disconnect = reinterpret_cast<SurfaceDisconnect>(
      dlsym(RTLD_DEFAULT, "_ZN7android7Surface10disconnectEiNS_22IGraphicBufferProducer14DisconnectModeE"));
  if (surface_disconnect) {
    surface_disconnect(window, 1, 0);
  }
}
static bool VexaRefreshGlxSurface() {
  if (VexaHeadlessMode() || g_vexa_glx.shadow) return g_vexa_glx.surface != EGL_NO_SURFACE;
  const uint64_t serial = VexaRuntimeSurfaceSerial();
  if (!serial || serial == g_vexa_glx.window_serial) return true;
  ANativeWindow* window = VexaAcquireRuntimeWindow(2000);
  if (!window) return false;
  if (window == g_vexa_glx.window) {
    g_vexa_glx.window_serial = serial;
    ANativeWindow_release(window);
    return true;
  }
  if (g_vexa_glx.buffer_format)
    ANativeWindow_setBuffersGeometry(window, 0, 0, g_vexa_glx.buffer_format);
  const EGLint colors[] = {EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_SRGB_KHR, EGL_NONE};
  EGLSurface surface = eglCreateWindowSurface(g_vexa_glx.display, g_vexa_glx.config, window, VexaSrgbSurface() ? colors : nullptr);
  if (surface == EGL_NO_SURFACE && eglGetError() == EGL_BAD_ALLOC) {
    // The producer can still carry our previous surface's api connect when
    // the runtime hands us a replacement window (idle doze, rotation): the
    // old EGLSurface stays alive as long as another thread holds it current,
    // so its deferred destroy never releases the slot. Force the stale
    // connect off this producer and retry once.
    VexaDisconnectWindowApi(window);
    surface = eglCreateWindowSurface(g_vexa_glx.display, g_vexa_glx.config, window, VexaSrgbSurface() ? colors : nullptr);
  }
  if (surface == EGL_NO_SURFACE) {
    static unsigned rebuild_failures = 0;
    if ((rebuild_failures++ & 255) == 0) {
      fprintf(stderr, "[VEXA][GLX] surface rebuild failed 0x%x (x%u)\n", eglGetError(), rebuild_failures);
    }
    ANativeWindow_release(window);
    return false;
  }
  // EGL defers destruction while the old surface is current on any thread.
  // Bind/swap below move the caller's existing context to the replacement.
  eglDestroySurface(g_vexa_glx.display, g_vexa_glx.surface);
  ANativeWindow_release(g_vexa_glx.window);
  g_vexa_glx.surface = surface;
  g_vexa_glx.window = window;
  g_vexa_glx.window_serial = serial;
  fprintf(stderr, "[VEXA][GLX] surface rebuilt serial=%llu\n", (unsigned long long)serial);
  return true;
}

static bool VexaEnsureGlx() {
  if (g_vexa_glx.ready && g_vexa_glx.display != EGL_NO_DISPLAY && g_vexa_glx.context != EGL_NO_CONTEXT &&
      g_vexa_glx.surface != EGL_NO_SURFACE) {
    if (!VexaRefreshGlxSurface()) return false;
    if (eglMakeCurrent(g_vexa_glx.display, g_vexa_glx.surface, g_vexa_glx.surface, g_vexa_glx.context) == EGL_TRUE) {
      return true;
    }
    fprintf(stderr, "[VEXA][GLX] eglMakeCurrent failed 0x%x\n", eglGetError());
    return false;
  }
  const bool headless = VexaHeadlessMode();
  const bool shadow = !headless && VexaGlShadowForVulkan();
  ANativeWindow* window = (headless || shadow) ? nullptr : VexaAcquireRuntimeWindow(2000);
  if (!window && !headless && !shadow) {
    return false;
  }
  EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display == EGL_NO_DISPLAY || eglInitialize(display, nullptr, nullptr) != EGL_TRUE) {
    if (window) ANativeWindow_release(window);
    fprintf(stderr, "[VEXA][GLX] eglInitialize failed 0x%x\n", eglGetError());
    return false;
  }
#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x00000040
#endif
  const EGLint config_attribs[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT | EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 16,
    EGL_NONE
  };
  EGLConfig config = nullptr;
  EGLint count = 0;
  if (eglChooseConfig(display, config_attribs, &config, 1, &count) != EGL_TRUE || count < 1) {
    if (window) ANativeWindow_release(window);
    fprintf(stderr, "[VEXA][GLX] eglChooseConfig failed 0x%x\n", eglGetError());
    return false;
  }
  EGLint format = 0;
  if (window &&
      eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &format) == EGL_TRUE && format != 0) {
    ANativeWindow_setBuffersGeometry(window, 0, 0, format);
  }
  EGLSurface surface = EGL_NO_SURFACE;
  int headless_w = 0, headless_h = 0;
  if (headless || shadow) {
    headless_w = VexaHeadlessEnvInt("VEXA_HEADLESS_W", 1280);
    headless_h = VexaHeadlessEnvInt("VEXA_HEADLESS_H", 720);
    const EGLint pbattrs[] = {EGL_WIDTH, headless_w, EGL_HEIGHT, headless_h,
      EGL_GL_COLORSPACE_KHR, VexaSrgbSurface() ? EGL_GL_COLORSPACE_SRGB_KHR : EGL_GL_COLORSPACE_LINEAR_KHR, EGL_NONE};
    surface = eglCreatePbufferSurface(display, config, pbattrs);
  } else {
    const EGLint colors[] = {EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_SRGB_KHR, EGL_NONE};
    surface = eglCreateWindowSurface(display, config, window, VexaSrgbSurface() ? colors : nullptr);
  }
  if (surface == EGL_NO_SURFACE) {
    fprintf(stderr, "[VEXA][GLX] eglCreate%sSurface failed 0x%x\n",
            (headless || shadow) ? "Pbuffer" : "Window", eglGetError());
    if (window) ANativeWindow_release(window);
    return false;
  }
  // Indirect draws and compute need GLES 3.1. Fall back if the driver rejects a version.
#ifndef EGL_CONTEXT_MAJOR_VERSION
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#endif
#ifndef EGL_CONTEXT_MINOR_VERSION
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#endif
  const EGLint ctx_32[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
  const EGLint ctx_31[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
  const EGLint ctx_30[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  const EGLint ctx_20[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  const EGLint* const ctx_tries[] = {ctx_32, ctx_31, ctx_30, ctx_20};
  const char* const ctx_names[] = {"3.2", "3.1", "3.0", "2.0"};
  EGLContext context = EGL_NO_CONTEXT;
  const char* ctx_name = "none";
  for (int i = 0; i < 4; ++i) {
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_tries[i]);
    if (context != EGL_NO_CONTEXT &&
        eglMakeCurrent(display, surface, surface, context) == EGL_TRUE) {
      ctx_name = ctx_names[i];
      break;
    }
    fprintf(stderr, "[VEXA][GLX] GLES %s context failed 0x%x\n", ctx_names[i], eglGetError());
    if (context != EGL_NO_CONTEXT) {
      eglDestroyContext(display, context);
      context = EGL_NO_CONTEXT;
    }
  }
  if (context == EGL_NO_CONTEXT) {
    eglDestroySurface(display, surface);
    if (window) ANativeWindow_release(window);
    return false;
  }
  // EGL defaults to swap interval 1; the env override is applied lazily in the
  // swap path (VEXA_GLX_SWAP_INTERVAL) where the surface is current.
  g_vexa_glx.display = display;
  g_vexa_glx.surface = surface;
  g_vexa_glx.context = context;
  g_vexa_glx.config = config;
  g_vexa_glx.buffer_format = format;
  g_vexa_glx.window = window;
  g_vexa_glx.window_serial = shadow ? 0 : VexaRuntimeSurfaceSerial();
  g_vexa_glx.ready = true;
  g_vexa_glx.shadow = shadow;
  fprintf(stderr, "[VEXA][GLX] surface ready %p gles %s serial %llu headless=%d shadow=%d\n", surface, ctx_name,
          (unsigned long long)g_vexa_glx.window_serial, headless ? 1 : 0, shadow ? 1 : 0);
  if (headless) {
    VexaPublishGuestSize(headless_w, headless_h);
  }
  fexfn_impl_libGL_glViewport(0, 0, 300, 300);
  return true;
}

struct VexaGuestGlxContext {
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface pbuffer = EGL_NO_SURFACE;
  bool window = false;
};
static std::mutex g_vexa_context_mutex;
static std::unordered_map<uintptr_t, VexaGuestGlxContext> g_vexa_contexts;

static auto fexfn_impl_libGL_VEXA_CreateGlxContext(uintptr_t share) -> uintptr_t {
  std::lock_guard lock(g_vexa_context_mutex);
  // Initialize the shared display/config/surface once. The bootstrap context
  // is only used to select a supported GLES version, never returned to guests.
  if (!g_vexa_glx.ready) {
    if (!VexaEnsureGlx()) return 0;
    eglMakeCurrent(g_vexa_glx.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  EGLContext shared = EGL_NO_CONTEXT;
  if (share) {
    auto it = g_vexa_contexts.find(share);
    if (it == g_vexa_contexts.end()) return 0;
    shared = it->second.context;
  }
  EGLint major = 3, minor = 0;
  eglQueryContext(g_vexa_glx.display, g_vexa_glx.context, EGL_CONTEXT_CLIENT_VERSION, &major);
  eglQueryContext(g_vexa_glx.display, g_vexa_glx.context, EGL_CONTEXT_MINOR_VERSION, &minor);
  const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, major, EGL_CONTEXT_MINOR_VERSION, minor, EGL_NONE};
  EGLContext context = eglCreateContext(g_vexa_glx.display, g_vexa_glx.config, shared, attrs);
  if (context == EGL_NO_CONTEXT) {
    fprintf(stderr, "[VEXA][GLX] create context failed 0x%x\n", eglGetError());
    return 0;
  }
  bool window = true;
  for (const auto& [id, c] : g_vexa_contexts) if (c.window) window = false;
  EGLSurface pbuffer = EGL_NO_SURFACE;
  {
    const EGLint pbattrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    pbuffer = eglCreatePbufferSurface(g_vexa_glx.display, g_vexa_glx.config, pbattrs);
    if (pbuffer == EGL_NO_SURFACE) {
      eglDestroyContext(g_vexa_glx.display, context);
      return 0;
    }
  }
  // Opaque IDs must fit both guest ABIs; EGL handles are host pointers.
  static uint32_t next_id = 1;
  uint32_t id;
  do { id = next_id++; } while (!id || g_vexa_contexts.contains(id));
  VexaRegisterProgramContext(context, shared);
  g_vexa_contexts.emplace(id, VexaGuestGlxContext {context, pbuffer, window});
  fprintf(stderr, "[VEXA][GLX] create context=%p share=%p window=%d\n", context,
          reinterpret_cast<void*>(share), window);
  return id;
}

static auto fexfn_impl_libGL_VEXA_DestroyGlxContext(uintptr_t id) -> void {
  std::lock_guard lock(g_vexa_context_mutex);
  auto it = g_vexa_contexts.find(id);
  if (it == g_vexa_contexts.end()) return;
  if (eglGetCurrentContext() == it->second.context)
    eglMakeCurrent(g_vexa_glx.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  VexaUnregisterProgramContext(it->second.context);
  eglDestroyContext(g_vexa_glx.display, it->second.context);
  if (it->second.pbuffer != EGL_NO_SURFACE) eglDestroySurface(g_vexa_glx.display, it->second.pbuffer);
  g_vexa_contexts.erase(it);
}

static auto fexfn_impl_libGL_VEXA_BindGlxSurface(uintptr_t id) -> int {
  std::lock_guard lock(g_vexa_context_mutex);
  auto it = g_vexa_contexts.find(id);
  if (it == g_vexa_contexts.end()) return 0;
  if (!VexaRefreshGlxSurface()) return 0;
  const auto& c = it->second;
  if (eglMakeCurrent(g_vexa_glx.display, g_vexa_glx.surface, g_vexa_glx.surface, c.context) == EGL_TRUE) {
    VexaSelectProgramContext(c.context);
    return 1;
  }
  const EGLint error = eglGetError();
  // A resource-upload context can be current concurrently with the render
  // context. EGL surfaces cannot be current on two threads at once.
  if (error == EGL_BAD_ACCESS &&
      eglMakeCurrent(g_vexa_glx.display, c.pbuffer, c.pbuffer, c.context) == EGL_TRUE) {
    VexaSelectProgramContext(c.context);
    return 1;
  }
  fprintf(stderr, "[VEXA][GLX] bind context=%p failed 0x%x\n", c.context, error);
  return 0;
}

static auto fexfn_impl_libGL_VEXA_ReleaseGlxSurface() -> int {
  if (!g_vexa_glx.ready || g_vexa_glx.display == EGL_NO_DISPLAY) {
    return 1;
  }
  // glXMakeCurrent(NULL) must drop the context on this thread. Leaving it
  // current makes the next thread's eglMakeCurrent fail with EGL_BAD_ACCESS,
  // and Unity then creates shaders against no context.
  const EGLBoolean ok = eglMakeCurrent(g_vexa_glx.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (ok != EGL_TRUE) {
    fprintf(stderr, "[VEXA][GLX] release failed 0x%x\n", eglGetError());
    return 0;
  }
  VexaSelectProgramContext(EGL_NO_CONTEXT);
  return 1;
}

static void VexaCoverPresent();

// KHR_debug values (not exposed by the GLES headers this TU includes).
constexpr GLenum kVexaGLDebugTypeError = 0x824C;
constexpr GLenum kVexaGLDebugOutput = 0x92E0;
constexpr GLenum kVexaGLDebugOutputSync = 0x8242;
using VexaGLDebugProc = void(GL_APIENTRYP)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar*, const void*);

// Synchronous KHR_debug reports the driver validation branch, which need not
// identify the actual input enum. Keep the arguments on the calling thread.
static thread_local GLenum VexaUploadFormat = 0, VexaUploadType = 0;
static thread_local GLsizei VexaUploadWidth = 0, VexaUploadHeight = 0;
struct VexaUploadTrace {
  VexaUploadTrace(GLenum format, GLenum type, GLsizei width, GLsizei height) {
    VexaUploadFormat = format; VexaUploadType = type;
    VexaUploadWidth = width; VexaUploadHeight = height;
  }
  ~VexaUploadTrace() { VexaUploadFormat = VexaUploadType = 0; VexaUploadWidth = VexaUploadHeight = 0; }
};

static void GL_APIENTRY VexaGLDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                            const GLchar* message, const void*) {
  static int logged = 0;
  static std::string last_msg;
  if (type != kVexaGLDebugTypeError || logged >= 80) {
    return;
  }
  const std::string msg = message ? message : "";
  if (msg == last_msg) {
    return;
  }
  last_msg = msg;
  ++logged;
  fprintf(stderr, "[VEXA][GLDBG-ARGS] upload fmt=%x type=%x size=%dx%d\n",
          VexaUploadFormat, VexaUploadType, VexaUploadWidth, VexaUploadHeight);
  fprintf(stderr, "[VEXA][GLDBG] src=%x type=%x id=%u sev=%x tid=%ld: %.*s\n", source, type, id, severity,
          (long)syscall(SYS_gettid), length > 0 ? (int)length : 512, msg.c_str());
}

static void VexaInstallGLDebugOnce() {
  // GL_DEBUG_OUTPUT_SYNCHRONOUS makes the driver run its validation branch on
  // every GL call; only install the callback when diagnostics are requested.
  if (!VexaGLDebugEnabled()) return;
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  using CbFn = void(GL_APIENTRYP)(VexaGLDebugProc, const void*);
  auto fn = reinterpret_cast<CbFn>(eglGetProcAddress("glDebugMessageCallbackKHR"));
  if (!fn) {
    fn = reinterpret_cast<CbFn>(eglGetProcAddress("glDebugMessageCallback"));
  }
  if (!fn) {
    fprintf(stderr, "[VEXA][GLDBG] no debug callback entry\n");
    return;
  }
  ::glEnable(kVexaGLDebugOutput);
  ::glEnable(kVexaGLDebugOutputSync);
  fn(VexaGLDebugCallback, nullptr);
  fprintf(stderr, "[VEXA][GLDBG] installed err=0x%x\n", glGetError());
}

// VEXA: present fallback. Some guests finish the frame in an offscreen FBO and
// swap without copying it to FB0 (observed with Unity GLCore on this stack:
// last pass goes to FBO 2, no blit to 0). Copy that FBO to the surface.
static void VexaPresentFallback() {
  const bool drew_fb0 = g_vexa_fb0_drawn;
  g_vexa_fb0_drawn = false;
  const size_t nfbo = g_vexa_frame_fbo_count;
  g_vexa_frame_fbo_count = 0;
  const uint32_t overflow = g_vexa_frame_fbo_overflow;
  g_vexa_frame_fbo_overflow = 0;
  const uint32_t fb0_draws = g_vexa_fb0_draw_count;
  g_vexa_fb0_draw_count = 0;
  const bool fb0_blit = g_vexa_fb0_blit;
  g_vexa_fb0_blit = false;
  const int fb0_n = g_vexa_fb0_draw_n;
  g_vexa_fb0_draw_n = 0;
  const int frame_event_n = g_vexa_frame_event_n;
  g_vexa_frame_event_n = 0;
  const int frame_binds_len = g_vexa_frame_binds_len;
  g_vexa_frame_binds_len = 0;
  char frame_binds[sizeof(g_vexa_frame_binds)];
  memcpy(frame_binds, g_vexa_frame_binds, sizeof(frame_binds));
  g_vexa_frame_binds[0] = 0;
  VexaFrameEvent frame_events[48];
  memcpy(frame_events, g_vexa_frame_events, sizeof(frame_events));
  if (VexaGLDebugEnabled() && overflow > 0) {
    fprintf(stderr, "[VEXA][PRESENT] fbo tracking overflow +%u (nfbo=%zu fb0=%d)\n", overflow, nfbo, drew_fb0);
  }
  if (drew_fb0 && nfbo > 0) {
    // fb0 was touched but offscreen FBOs also got draws: dump the candidates
    // we are dropping so we can see if the real frame stayed in an FBO.
    static int fb0_probe_logs = 0;
    if (VexaGLDebugEnabled() && fb0_probe_logs < 256) {
      ++fb0_probe_logs;
      GLint prev_read = 0;
      ::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
      std::string cand;
      char tmp[96];
      for (size_t i = 0; i < nfbo; ++i) {
        const GLuint f = g_vexa_frame_fbos[i].fbo;
        ::glBindFramebuffer(GL_READ_FRAMEBUFFER, f);
        GLint ctype = 0, cname = 0, cw = 0, ch = 0;
        ::glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &ctype);
        ::glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &cname);
        if (ctype == GL_TEXTURE) {
          GLint level = 0, prev_tex = 0;
          ::glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                  GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &level);
          ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
          ::glBindTexture(GL_TEXTURE_2D, cname);
          ::glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &cw);
          ::glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &ch);
          ::glBindTexture(GL_TEXTURE_2D, prev_tex);
        } else if (ctype == GL_RENDERBUFFER) {
          GLint prev_rb = 0;
          ::glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);
          ::glBindRenderbuffer(GL_RENDERBUFFER, cname);
          ::glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &cw);
          ::glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &ch);
          ::glBindRenderbuffer(GL_RENDERBUFFER, prev_rb);
        }
        snprintf(tmp, sizeof(tmp), " f%u:%ux%u(t%d x%u)", f, cw, ch,
                 ctype == GL_TEXTURE ? cname : -1, g_vexa_frame_fbos[i].draws);
        cand += tmp;
      }
      ::glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
      char draws[384];
      int off = 0;
      for (int i = 0; i < fb0_n; ++i) {
        const auto& d = g_vexa_fb0_draws[i];
        off += snprintf(draws + off, sizeof(draws) - off,
                        " [m%x n%d vp%dx%d p%u t%d b%d]", d.mode, d.count, d.vp_w, d.vp_h, d.prog,
                        d.tex, d.blend);
      }
      fprintf(stderr, "[VEXA][PRESENT] fb0 drawn (%u draws blit=%d)%s | skipped:%s\n", fb0_draws,
              fb0_blit, draws, cand.c_str());
      // Ordered event log: binds and draw targets in issue order.
      char events[512];
      int eoff = snprintf(events, sizeof(events), "binds %.*s draws", frame_binds_len, frame_binds);
      for (int i = 0; i < frame_event_n && eoff < (int)sizeof(events) - 16; ++i) {
        const auto& ev = frame_events[i];
        eoff += snprintf(events + eoff, sizeof(events) - eoff, " %c%u(p%u)", ev.fbo ? 'F' : 'B',
                         ev.fbo ? ev.fbo : 0, ev.prog);
      }
      fprintf(stderr, "[VEXA][PRESENT] %s\n", events);
      // Dump what is actually about to be presented (fb0 pixels before swap).
      static int fb0_dumps = 0;
      if (fb0_dumps < 12) {
        ++fb0_dumps;
        GLint vp[4] = {};
        ::glGetIntegerv(GL_VIEWPORT, vp);
        const GLint w = vp[2] > 0 ? vp[2] : 2712;
        const GLint h = vp[3] > 0 ? vp[3] : 1220;
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        ::glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        ::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        const GLenum rd_err = ::glGetError();
        char path[192];
        snprintf(path, sizeof(path),
                 "/data/data/com.critical.vexaemulator/files/artifacts/ets2/fb0_d%u_%dx%d.ppm",
                 fb0_draws, w, h);
        if (FILE* f = fopen(path, "wb")) {
          fprintf(f, "P6\n%d %d\n255\n", w, h);
          for (GLint yy = h - 1; yy >= 0; --yy)
            for (GLint xx = 0; xx < w; ++xx) {
              const uint8_t* p = px.data() + (static_cast<size_t>(yy) * w + xx) * 4;
              fputc(p[0], f);
              fputc(p[1], f);
              fputc(p[2], f);
            }
          fclose(f);
        }
        fprintf(stderr, "[VEXA][PRESENT] fb0 dump err=0x%x -> %s\n", rd_err, path);
      }
    }
  }
  if (drew_fb0 || nfbo == 0) {
    return;
  }
  GLint prev_draw_fbo = 0, prev_read_fbo = 0;
  ::glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
  ::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
  // Pick the FBO that actually holds the finished frame: among the FBOs that
  // received draws this frame, prefer the largest color attachment. A texture
  // attachment can be presented by the quad path; renderbuffers need the blit.
  GLint dfbo = 0, type = 0, name = 0, w = 0, h = 0;
  GLint best_area = 0, best_draws = 0;
  for (size_t i = 0; i < nfbo; ++i) {
    const GLuint cand = g_vexa_frame_fbos[i].fbo;
    ::glBindFramebuffer(GL_READ_FRAMEBUFFER, cand);
    GLint ctype = 0, cname = 0;
    ::glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &ctype);
    ::glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &cname);
    GLint cw = 0, ch = 0;
    if (ctype == GL_TEXTURE) {
      GLint level = 0, prev_tex = 0;
      ::glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                              GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &level);
      ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
      ::glBindTexture(GL_TEXTURE_2D, cname);
      ::glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &cw);
      ::glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &ch);
      ::glBindTexture(GL_TEXTURE_2D, prev_tex);
    } else if (ctype == GL_RENDERBUFFER) {
      GLint prev_rb = 0;
      ::glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);
      ::glBindRenderbuffer(GL_RENDERBUFFER, cname);
      ::glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &cw);
      ::glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &ch);
      ::glBindRenderbuffer(GL_RENDERBUFFER, prev_rb);
    }
    const GLint area = cw * ch;
    // Log what the guest actually rendered into, with the real attachment size.
    // A guest that renders offscreen (Unity 2022+ URP) never touches FB0, so the
    // present fallback can only pick the right target if these sizes are known.
    {
      static int logged_targets = 0;
      if (logged_targets < 12) {
        ++logged_targets;
        fprintf(stderr, "[VEXA][PRESENT] target fbo=%u draws=%u att=0x%x dim=%dx%d\n", cand,
                g_vexa_frame_fbos[i].draws, ctype, cw, ch);
      }
    }
    const bool better =
        area > best_area || (area == best_area && area > 0 &&
                             ((ctype == GL_TEXTURE && type != GL_TEXTURE) ||
                              g_vexa_frame_fbos[i].draws > best_draws));
    if (better) {
      best_area = area;
      best_draws = g_vexa_frame_fbos[i].draws;
      dfbo = cand;
      type = ctype;
      name = cname;
      w = cw;
      h = ch;
    }
  }
  EGLint sw = 0, sh = 0;
  eglQuerySurface(g_vexa_glx.display, g_vexa_glx.surface, EGL_WIDTH, &sw);
  eglQuerySurface(g_vexa_glx.display, g_vexa_glx.surface, EGL_HEIGHT, &sh);
  {
    // A presented target that flips between FBO ids/sizes on consecutive
    // frames is the visible-flicker signature (VS Bat Country inverse).
    static GLuint last_dfbo = 0;
    static int last_w = 0, last_h = 0;
    static int flips = 0;
    if (dfbo != last_dfbo || w != last_w || h != last_h) {
      ++flips;
      if (flips < 32) {
        std::string cand;
        char tmp[64];
        for (size_t i = 0; i < nfbo; ++i) {
          snprintf(tmp, sizeof(tmp), "%s%u(x%u)", i ? " " : "", g_vexa_frame_fbos[i].fbo,
                   g_vexa_frame_fbos[i].draws);
          cand += tmp;
        }
        fprintf(stderr, "[VEXA][PRESENT] winner change fbo=%d %dx%d draws=%d (prev=%d %dx%d) cands: %s\n", dfbo,
                w, h, best_draws, last_dfbo, last_w, last_h, cand.c_str());
      }
      last_dfbo = dfbo;
      last_w = w;
      last_h = h;
    }
  }
  static int logged = 0;
  if (logged < 3) {
    ++logged;
    fprintf(stderr, "[VEXA][PRESENT] fallback fbo=%d att_type=0x%x name=%d %dx%d draws=%d -> surface %dx%d\n", dfbo,
            type, name, w, h, best_draws, sw, sh);
  }
  if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0) {
    ::glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read_fbo);
    return;
  }
  // Sample the winner's pixels at a few points in time: tells us whether the
  // frame FBO actually contains rendered content or the game outputs black.
  {
    static int calls = 0, sampled = 0;
    ++calls;
    if (VexaGLDebugEnabled() &&
        (calls == 5 || calls == 500 || calls == 1500 || calls == 3000 || calls == 8000)) {
      ++sampled;
      ::glBindFramebuffer(GL_READ_FRAMEBUFFER, dfbo);
      while (::glGetError() != GL_NO_ERROR) {
      }
      ::glReadBuffer(GL_COLOR_ATTACHMENT0);
      GLubyte px[9 * 4];
      int nonzero = 0;
      for (int i = 0; i < 9; ++i) {
        ::glReadPixels(w * (i % 3 + 1) / 4, h * (i / 3 + 1) / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px + i * 4);
      }
      const GLenum serr = ::glGetError();
      for (int i = 0; i < 9; ++i) {
        if (px[i * 4] || px[i * 4 + 1] || px[i * 4 + 2]) {
          ++nonzero;
        }
      }
      fprintf(stderr,
              "[VEXA][PRESENT] fbo%d src nz=%d err=0x%x: %02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x "
              "%02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x\n",
              dfbo, nonzero, serr, px[0], px[1], px[2], px[4], px[5], px[6], px[8], px[9], px[10], px[12], px[13],
              px[14], px[16], px[17], px[18], px[20], px[21], px[22], px[24], px[25], px[26], px[28], px[29], px[30],
              px[32], px[33], px[34]);
      if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
        std::vector<GLubyte> img(static_cast<size_t>(w) * h * 4);
        ::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, img.data());
        char path[128];
        snprintf(path, sizeof(path), "/data/data/com.critical.vexaemulator/files/artifacts/megabonk/fbo%d_%d.ppm",
                 dfbo, sampled);
        if (FILE* f = fopen(path, "wb")) {
          fprintf(f, "P6\n%d %d\n255\n", w, h);
          for (int y = h - 1; y >= 0; --y) {
            for (int x = 0; x < w; ++x) {
              const GLubyte* p = img.data() + (static_cast<size_t>(y) * w + x) * 4;
              fwrite(p, 1, 3, f);
            }
          }
          fclose(f);
          fprintf(stderr, "[VEXA][PRESENT] dumped %dx%d fbo%d to %s\n", w, h, dfbo, path);
        }
      }
      ::glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read_fbo);
    }
  }
  // The blit path fails on ES when the guest FBO's color format differs from
  // the surface (e.g. RGBA16F -> RGBA8), which is exactly what Unity GLCore
  // does. A textured quad draw converts formats in the shader instead.
  bool presented = false;
  if (type == GL_TEXTURE && w > 0 && h > 0) {
    static GLuint quad_prog = 0;
    static GLuint quad_vao = 0;
    if (quad_prog == 0) {
      const char* vs_src =
          "#version 320 es\n"
          "layout(location=0) out vec2 uv;\n"
          "void main() { vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
          "  uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }";
      const char* fs_src =
          "#version 320 es\n"
          "precision mediump float;\n"
          "layout(location=0) in vec2 uv;\n"
          "layout(location=0) out vec4 frag;\n"
          "uniform sampler2D tex;\n"
          "void main() { frag = texture(tex, uv); }";
      GLuint vs = ::glCreateShader(GL_VERTEX_SHADER);
      GLuint fs = ::glCreateShader(GL_FRAGMENT_SHADER);
      ::glShaderSource(vs, 1, &vs_src, nullptr);
      ::glShaderSource(fs, 1, &fs_src, nullptr);
      ::glCompileShader(vs);
      ::glCompileShader(fs);
      quad_prog = ::glCreateProgram();
      ::glAttachShader(quad_prog, vs);
      ::glAttachShader(quad_prog, fs);
      ::glLinkProgram(quad_prog);
      GLint ok = 0;
      ::glGetProgramiv(quad_prog, GL_LINK_STATUS, &ok);
      if (!ok) {
        ::glDeleteProgram(quad_prog);
        quad_prog = 0;
      }
      ::glDeleteShader(vs);
      ::glDeleteShader(fs);
      ::glGenVertexArrays(1, &quad_vao);
    }
    if (quad_prog != 0) {
      while (::glGetError() != GL_NO_ERROR) {
      }
      GLint prev_prog = 0, prev_vao = 0, prev_tex = 0, prev_active = 0;
      GLint prev_vp[4] = {};
      ::glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
      ::glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
      ::glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
      ::glActiveTexture(GL_TEXTURE0);
      ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
      ::glGetIntegerv(GL_VIEWPORT, prev_vp);
      const GLboolean was_scissor = ::glIsEnabled(GL_SCISSOR_TEST);
      const GLboolean was_depth = ::glIsEnabled(GL_DEPTH_TEST);
      const GLboolean was_stencil = ::glIsEnabled(GL_STENCIL_TEST);
      const GLboolean was_blend = ::glIsEnabled(GL_BLEND);
      const GLboolean was_cull = ::glIsEnabled(GL_CULL_FACE);
      const GLboolean was_discard = ::glIsEnabled(GL_RASTERIZER_DISCARD);
      ::glDisable(GL_SCISSOR_TEST);
      ::glDisable(GL_DEPTH_TEST);
      ::glDisable(GL_STENCIL_TEST);
      ::glDisable(GL_BLEND);
      ::glDisable(GL_CULL_FACE);
      ::glDisable(GL_RASTERIZER_DISCARD);
      ::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      ::glViewport(0, 0, sw, sh);
      ::glUseProgram(quad_prog);
      ::glBindVertexArray(quad_vao);
      ::glBindTexture(GL_TEXTURE_2D, name);
      ::glDrawArrays(GL_TRIANGLES, 0, 3);
      const GLenum err = ::glGetError();
      if (err != GL_NO_ERROR && logged < 6) {
        ++logged;
        fprintf(stderr, "[VEXA][PRESENT] fallback quad err=0x%x\n", err);
      } else if (logged < 6) {
        ++logged;
        fprintf(stderr, "[VEXA][PRESENT] fallback quad drew tex=%d\n", name);
      }
      presented = true;
      ::glBindTexture(GL_TEXTURE_2D, prev_tex);
      ::glActiveTexture(prev_active);
      ::glBindVertexArray(prev_vao);
      ::glUseProgram(prev_prog);
      ::glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
      if (was_scissor) ::glEnable(GL_SCISSOR_TEST);
      if (was_depth) ::glEnable(GL_DEPTH_TEST);
      if (was_stencil) ::glEnable(GL_STENCIL_TEST);
      if (was_blend) ::glEnable(GL_BLEND);
      if (was_cull) ::glEnable(GL_CULL_FACE);
      if (was_discard) ::glEnable(GL_RASTERIZER_DISCARD);
      ::glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read_fbo);
      ::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw_fbo);
    }
  }
  if (!presented) {
    GLint read_buf = GL_COLOR_ATTACHMENT0;
    const GLboolean scissor = ::glIsEnabled(GL_SCISSOR_TEST);
    ::glDisable(GL_SCISSOR_TEST);
    ::glBindFramebuffer(GL_READ_FRAMEBUFFER, dfbo);
    ::glGetIntegerv(GL_READ_BUFFER, &read_buf);
    ::glReadBuffer(GL_COLOR_ATTACHMENT0);
    ::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    ::glBlitFramebuffer(0, 0, w, h, 0, 0, sw, sh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    const GLenum err = ::glGetError();
    if (err != GL_NO_ERROR && logged < 6) {
      ++logged;
      fprintf(stderr, "[VEXA][PRESENT] fallback blit err=0x%x\n", err);
    }
    ::glReadBuffer(read_buf);
    ::glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read_fbo);
    ::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw_fbo);
    if (scissor) {
      ::glEnable(GL_SCISSOR_TEST);
    }
  }
}

static auto fexfn_impl_libGL_VEXA_SwapGlxBuffers() -> void {
  std::lock_guard lock(g_vexa_context_mutex);
  if (!g_vexa_glx.ready && !VexaEnsureGlx()) {
    return;
  }
  if (!VexaRefreshGlxSurface()) return;
  if (eglGetCurrentSurface(EGL_DRAW) != g_vexa_glx.surface) {
    const EGLContext context = eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT ||
        eglMakeCurrent(g_vexa_glx.display, g_vexa_glx.surface, g_vexa_glx.surface, context) != EGL_TRUE) {
      fprintf(stderr, "[VEXA][GLX] present context bind failed 0x%x\n", eglGetError());
      return;
    }
  }
  VexaInstallGLDebugOnce();
  VexaCoverPresent();
  VexaPresentFallback();
  const char* capture_flag = "/data/data/com.critical.vexaemulator/files/headless_frame.flag";
  if (VexaHeadlessMode() && access(capture_flag, F_OK) == 0) {
    unlink(capture_flag);
    EGLint width = 0, height = 0;
    eglQuerySurface(g_vexa_glx.display, g_vexa_glx.surface, EGL_WIDTH, &width);
    eglQuerySurface(g_vexa_glx.display, g_vexa_glx.surface, EGL_HEIGHT, &height);
    if (width > 0 && height > 0 && width <= 8192 && height <= 8192) {
      GLint read_fbo = 0, pack_row = 0;
      ::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
      ::glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row);
      ::glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      ::glPixelStorei(GL_PACK_ROW_LENGTH, 0);
      std::vector<GLubyte> pixels(static_cast<size_t>(width) * height * 4);
      ::glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
      ::glPixelStorei(GL_PACK_ROW_LENGTH, pack_row);
      ::glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
      const char* path = "/data/data/com.critical.vexaemulator/files/artifacts/headless-frame.ppm";
      if (FILE* file = fopen(path, "wb")) {
        fprintf(file, "P6\n%d %d\n255\n", width, height);
        for (int y = height - 1; y >= 0; --y)
          for (int x = 0; x < width; ++x)
            fwrite(pixels.data() + (static_cast<size_t>(y) * width + x) * 4, 1, 3, file);
        fclose(file);
      }
    }
  }
  {
    static int diag_swaps = 0;
    ++diag_swaps;
    if (g_vexa_trace_frame) {
      GLint dfbo = -1, rfbo = -1;
      glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfbo);
      glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfbo);
      fprintf(stderr, "[VEXA][TRACE] swap dfbo=%d rfbo=%d err=0x%x -- end trace\n", dfbo, rfbo, glGetError());
    }
    const char* trace_flag = "/data/data/com.critical.vexaemulator/files/unity_trace_frame.flag";
    const bool trace_requested = access(trace_flag, F_OK) == 0;
    if (trace_requested) unlink(trace_flag);
    g_vexa_trace_frame = trace_requested ? 1 : 0;
    if (g_vexa_trace_frame) {
      fprintf(stderr, "[VEXA][TRACE] begin frame after swap %d tid=%ld\n", diag_swaps, (long)syscall(SYS_gettid));
    }
    if ((diag_swaps & 127) == 0) {
      static uint64_t previous_draws = 0, previous_dispatches = 0;
      fprintf(stderr,
              "[VEXA][GLX] diag swaps=%d draws=%llu draws/frame=%.2f dispatches=%llu dispatches/frame=%.2f window=128 clears=%llu useprog=%llu bindfbo=%llu traced_calls_total=%llu\n",
              diag_swaps, (unsigned long long)g_vexa_diag_draws,
              (double)(g_vexa_diag_draws - previous_draws) / 128,
              (unsigned long long)g_vexa_diag_dispatches,
              (double)(g_vexa_diag_dispatches - previous_dispatches) / 128,
              (unsigned long long)g_vexa_diag_clears,
              (unsigned long long)g_vexa_diag_useprog, (unsigned long long)g_vexa_diag_bindfbo,
              (unsigned long long)GLHostTraceSeq.load());
      previous_draws = g_vexa_diag_draws;
      previous_dispatches = g_vexa_diag_dispatches;
    }
  }
  // Frame-time distribution. An average hides stutter completely: a run can
  // average 20 ms while every twentieth frame takes 200 ms, and that is what
  // the player feels. Report the tail, not just the mean.
  {
    static int frame_no = 0;
    static uint64_t last_ns = 0;
    static uint64_t previous_draws = 0, previous_useprog = 0, previous_bindfbo = 0;
    static uint64_t ring[256];
    static unsigned ring_pos = 0;
    static unsigned ring_n = 0;
    const uint64_t now_ns = GLHostNowNs();
    if (last_ns != 0 && now_ns > last_ns) {
      const uint64_t dt_ns = now_ns - last_ns;
      ring[ring_pos] = dt_ns;
      ring_pos = (ring_pos + 1) % 256;
      if (ring_n < 256) {
        ++ring_n;
      }
      if (dt_ns > 33ull * 1000000ull) {
        VEXA_LOGV(
                "[VEXA][SPIKE] swap=%llu frame=%.2fms draws=%llu useprog=%llu bindfbo=%llu\n",
                static_cast<unsigned long long>(g_vexa_swap_no),
                static_cast<double>(dt_ns) / 1e6,
                static_cast<unsigned long long>(g_vexa_diag_draws - previous_draws),
                static_cast<unsigned long long>(g_vexa_diag_useprog - previous_useprog),
                static_cast<unsigned long long>(g_vexa_diag_bindfbo - previous_bindfbo));
      }
      if ((frame_no & 127) == 0 && ring_n > 32) {
        uint64_t sorted[256];
        std::memcpy(sorted, ring, ring_n * sizeof(uint64_t));
        std::sort(sorted, sorted + ring_n);
        uint64_t sum = 0;
        for (unsigned i = 0; i < ring_n; ++i) {
          sum += sorted[i];
        }
        unsigned over_33 = 0;
        for (unsigned i = 0; i < ring_n; ++i) {
          if (sorted[i] > 33ull * 1000000ull) {
            ++over_33;
          }
        }
        const auto ms = [](uint64_t ns) { return static_cast<double>(ns) / 1e6; };
        const uint64_t gl_ns = g_vexa_glcall_ns.exchange(0, std::memory_order_relaxed);
        const unsigned elapsed_frames = static_cast<unsigned>(frame_no & 127) == 0 ? 128 : 0;
        fprintf(stderr,
                "[VEXA][FRAME] n=%u avg=%.2fms p50=%.2f p90=%.2f p99=%.2f max=%.2f over33=%u (%.1f%%) glms/f=%.2f\n",
                ring_n, ms(sum / ring_n), ms(sorted[ring_n / 2]), ms(sorted[(ring_n * 9) / 10]),
                ms(sorted[(ring_n * 99) / 100]), ms(sorted[ring_n - 1]), over_33,
                100.0 * over_33 / ring_n,
                elapsed_frames ? ms(gl_ns) / elapsed_frames : 0.0);
      }
    }
    last_ns = now_ns;
    previous_draws = g_vexa_diag_draws;
    previous_useprog = g_vexa_diag_useprog;
    previous_bindfbo = g_vexa_diag_bindfbo;
    ++frame_no;
    ++g_vexa_swap_no;
  }
  // Swap interval is per-surface in EGL; re-apply the env override whenever the
  // window surface was rebuilt (serial change) so the override survives surface
  // recreation.
  {
    static const int env_interval = [] {
      const char* iv = getenv("VEXA_GLX_SWAP_INTERVAL");
      return iv ? atoi(iv) : -1;
    }();
    if (env_interval >= 0 && g_vexa_glx.window_serial != g_vexa_env_interval_serial) {
      const int interval = env_interval;
      const EGLBoolean ok = eglSwapInterval(g_vexa_glx.display, interval);
      fprintf(stderr, "[VEXA][GLX] env swap interval %d -> %s (serial=%llu)\n", interval,
              ok == EGL_TRUE ? "ok" : "failed", (unsigned long long)g_vexa_glx.window_serial);
      g_vexa_env_interval_serial = g_vexa_glx.window_serial;
    }
  }
  if (eglSwapBuffers(g_vexa_glx.display, g_vexa_glx.surface) != EGL_TRUE) {
    fprintf(stderr, "[VEXA][GLX] eglSwapBuffers failed 0x%x\n", eglGetError());
  }
}

static auto fexfn_impl_libGL_VEXA_SetSwapInterval(int interval) -> int {
  std::lock_guard lock(g_vexa_context_mutex);
  EGLDisplay display = eglGetCurrentDisplay();
  if (display == EGL_NO_DISPLAY) {
    if (!g_vexa_glx.ready && !VexaEnsureGlx()) {
      return 0;
    }
    display = g_vexa_glx.display;
  }
  const EGLBoolean ok = eglSwapInterval(display, interval);
  fprintf(stderr, "[VEXA][GLX] swap interval %d -> %s\n", interval, ok == EGL_TRUE ? "ok" : "failed");
  return ok == EGL_TRUE ? 1 : 0;
}

struct VexaVisibleRect {
  bool ok = false;
  GLint x = 0;
  GLint y = 0;
  GLsizei width = 0;
  GLsizei height = 0;
  int surface_w = 0;
  int surface_h = 0;
};

// Cover the whole surface. A guest window that already matches the surface stays
// at 0,0. A smaller one is scaled up and centered; a negative origin lets GL clip
// the overflow so the picture still fills the screen.
static VexaVisibleRect VexaVisibleRectFor(GLsizei width, GLsizei height) {
  static int surface_w = 0;
  static int surface_h = 0;
  if (surface_w <= 0 || surface_h <= 0 || surface_h > surface_w) {
    if (VexaHeadlessMode()) {
      surface_w = VexaHeadlessEnvInt("VEXA_HEADLESS_W", 1280);
      surface_h = VexaHeadlessEnvInt("VEXA_HEADLESS_H", 720);
    } else {
      ANativeWindow* window = VexaAcquireRuntimeWindow(0);
      if (window) {
        const int w = ANativeWindow_getWidth(window);
        const int h = ANativeWindow_getHeight(window);
        ANativeWindow_release(window);
        if (w > 0 && h > 0) {
          surface_w = w;
          surface_h = h;
        }
      }
    }
  }
  VexaVisibleRect rect {};
  rect.surface_w = surface_w;
  rect.surface_h = surface_h;
  if (surface_w <= 0 || surface_h <= 0 || width <= 0 || height <= 0) {
    return rect;
  }
  GLsizei view_w = 0;
  GLsizei view_h = 0;
  if (static_cast<int64_t>(surface_w) * height >= static_cast<int64_t>(surface_h) * width) {
    view_w = static_cast<GLsizei>(surface_w);
    view_h = static_cast<GLsizei>((static_cast<int64_t>(surface_w) * height + width / 2) / width);
  } else {
    view_h = static_cast<GLsizei>(surface_h);
    view_w = static_cast<GLsizei>((static_cast<int64_t>(surface_h) * width + height / 2) / height);
  }
  if (view_w < surface_w) {
    view_w = static_cast<GLsizei>(surface_w);
  }
  if (view_h < surface_h) {
    view_h = static_cast<GLsizei>(surface_h);
  }
  rect.ok = true;
  rect.x = (surface_w - static_cast<int>(view_w)) / 2;
  rect.y = (surface_h - static_cast<int>(view_h)) / 2;
  rect.width = view_w;
  rect.height = view_h;
  return rect;
}

static GLint g_vexa_guest_x = 0;
static GLint g_vexa_guest_y = 0;
static GLsizei g_vexa_guest_w = 0;
static GLsizei g_vexa_guest_h = 0;
// Largest near-origin fb0 viewport: the guest's drawable size. Direct-draw
// games never blit, so VexaCoverPresent derives their pointer map from this.
static GLsizei g_vexa_win_w = 0;
static GLsizei g_vexa_win_h = 0;

static void VexaNoteGuestViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
  // The 300x300 EnsureGlx viewport is below half of the 1280x720 guest window.
  if (width < 640 || height < 360) {
    return;
  }
  GLint draw_fbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &draw_fbo);
  if (draw_fbo != 0) {
    return;
  }
  if (x <= 1 && y <= 1 &&
      static_cast<int64_t>(width) * height > static_cast<int64_t>(g_vexa_win_w) * g_vexa_win_h) {
    g_vexa_win_w = width;
    g_vexa_win_h = height;
  }
  if (g_vexa_guest_x == x && g_vexa_guest_y == y && g_vexa_guest_w == width && g_vexa_guest_h == height) {
    return;
  }
  g_vexa_guest_x = x;
  g_vexa_guest_y = y;
  g_vexa_guest_w = width;
  g_vexa_guest_h = height;
  fprintf(stderr, "[VEXA][GL] guest viewport %d,%d %dx%d\n", x, y, width, height);
}

static GLint VexaBoundDrawFramebuffer() {
  GLint draw_fbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &draw_fbo);
  return draw_fbo;
}

// True when the guest rectangle already lies on the surface. The window size
// is the surface size, so this holds for any panel: the guest placed the rect.
static bool VexaRectInsideSurface(int x, int y, int width, int height, int surface_w, int surface_h) {
  return surface_w > 0 && surface_h > 0 && width > 0 && height > 0 && x >= -1 && y >= -1 &&
         x + width <= surface_w + 1 && y + height <= surface_h + 1;
}

// A main view parked at the GL origin, smaller than the surface, draws in the
// bottom-left corner. A centered or edge-fitted frame has a non-zero origin or
// already spans an axis, and must stay where the guest placed it.
static bool VexaCornerShrink(int x, int y, int width, int height, int surface_w, int surface_h) {
  return x <= 1 && y <= 1 && surface_w > 0 && surface_h > 0 &&
         (width + 8 < surface_w || height + 8 < surface_h);
}

struct VexaFitRect {
  int x, y, w, h;
};

static void VexaDisplayPrefs(int* aspect, int* rate) {
  using Fn = void (*)(int*, int*);
  static Fn fn = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "Vexa_GetDisplayPrefs"));
    if (!fn) {
      if (void* handle = dlopen("libvexa.so", RTLD_NOW | RTLD_NOLOAD)) {
        fn = reinterpret_cast<Fn>(dlsym(handle, "Vexa_GetDisplayPrefs"));
      }
    }
  }
  *aspect = 0;
  *rate = 0;
  if (fn) {
    fn(aspect, rate);
  }
}

// Destination of a window smaller than the surface: the whole surface when
// stretching, otherwise the largest centered rect with the window's aspect.
static VexaFitRect VexaFitFor(int win_w, int win_h, int sw, int sh) {
  int aspect = 0, rate = 0;
  VexaDisplayPrefs(&aspect, &rate);
  if (!aspect || win_w <= 0 || win_h <= 0) {
    return {0, 0, sw, sh};
  }
  int fw = sw;
  int fh = static_cast<int>((static_cast<int64_t>(sw) * win_h + win_w / 2) / win_w);
  if (fh > sh) {
    fh = sh;
    fw = static_cast<int>((static_cast<int64_t>(sh) * win_w + win_h / 2) / win_h);
  }
  return {(sw - fw) / 2, (sh - fh) / 2, fw, fh};
}

static void VexaPublishPresentMap(int x0, int y0, int cw, int ch, int sx, int sy, int src_w, int src_h, int sw,
                                  int sh);

static void fexfn_impl_libGL_glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
  static bool logged = false;
  static bool logged_fbo = false;
  VexaNoteGuestViewport(x, y, width, height);
  if (VexaBoundDrawFramebuffer() != 0) {
    if (!logged_fbo) {
      logged_fbo = true;
      fprintf(stderr, "[VEXA][GL] glViewport fbo passthrough %d,%d %dx%d\n", x, y, width, height);
    }
    glViewport(x, y, width, height);
    return;
  }
  // The 300x300 context bootstrap is not the guest window.
  if (width < 640 || height < 360) {
    glViewport(x, y, width, height);
    return;
  }
  VexaVisibleRect rect = VexaVisibleRectFor(width, height);
  if (rect.ok && VexaCornerShrink(x, y, width, height, rect.surface_w, rect.surface_h)) {
    const VexaFitRect fit = VexaFitFor(width, height, rect.surface_w, rect.surface_h);
    glViewport(fit.x, fit.y, fit.w, fit.h);
    // The window-sized viewport is where the guest's content lands, fitted
    // into the surface. Publish it so the SDL2/X11 input paths undo the same
    // letterbox transform even when the game never blits (VexaCoverPresent
    // only runs on the swap path).
    if (x <= 1 && y <= 1 &&
        static_cast<int64_t>(width) * height >=
            static_cast<int64_t>(g_vexa_win_w) * g_vexa_win_h) {
      const int sx = static_cast<int>(-static_cast<int64_t>(fit.x) * width / fit.w);
      const int sy = static_cast<int>(-static_cast<int64_t>(fit.y) * height / fit.h);
      const int src_w = static_cast<int>(static_cast<int64_t>(width) * rect.surface_w / fit.w);
      const int src_h = static_cast<int>(static_cast<int64_t>(height) * rect.surface_h / fit.h);
      VexaPublishPresentMap(0, rect.surface_h - height, width, height, sx, sy, src_w, src_h, rect.surface_w,
                            rect.surface_h);
    }
    if (!logged) {
      logged = true;
      fprintf(stderr, "[VEXA][GL] glViewport corner %d,%d %dx%d -> 0,0 %dx%d\n", x, y, width, height, rect.surface_w,
              rect.surface_h);
    }
    return;
  }
  if (rect.ok && VexaRectInsideSurface(x, y, width, height, rect.surface_w, rect.surface_h)) {
    glViewport(x, y, width, height);
    return;
  }
  if (rect.ok) {
    glViewport(rect.x, rect.y, rect.width, rect.height);
    if (!logged) {
      logged = true;
      fprintf(stderr, "[VEXA][GL] glViewport %d,%d %dx%d -> %d,%d %dx%d on %dx%d\n",
              x, y, width, height, rect.x, rect.y, rect.width, rect.height, rect.surface_w, rect.surface_h);
    }
    return;
  }
  glViewport(x, y, width, height);
}

static void fexfn_impl_libGL_glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
  static bool logged = false;
  static bool logged_fbo = false;
  if (VexaBoundDrawFramebuffer() != 0) {
    if (!logged_fbo) {
      logged_fbo = true;
      fprintf(stderr, "[VEXA][GL] glScissor fbo passthrough %d,%d %dx%d\n", x, y, width, height);
    }
    glScissor(x, y, width, height);
    return;
  }
  if (width < 640 || height < 360) {
    glScissor(x, y, width, height);
    return;
  }
  VexaVisibleRect rect = VexaVisibleRectFor(width, height);
  if (rect.ok && VexaCornerShrink(x, y, width, height, rect.surface_w, rect.surface_h)) {
    const VexaFitRect fit = VexaFitFor(width, height, rect.surface_w, rect.surface_h);
    glScissor(fit.x, fit.y, fit.w, fit.h);
    if (!logged) {
      logged = true;
      fprintf(stderr, "[VEXA][GL] glScissor corner %d,%d %dx%d -> 0,0 %dx%d\n", x, y, width, height, rect.surface_w,
              rect.surface_h);
    }
    return;
  }
  if (rect.ok && VexaRectInsideSurface(x, y, width, height, rect.surface_w, rect.surface_h)) {
    glScissor(x, y, width, height);
    return;
  }
  if (rect.ok) {
    glScissor(rect.x, rect.y, rect.width, rect.height);
    if (!logged) {
      logged = true;
      fprintf(stderr, "[VEXA][GL] glScissor %d,%d %dx%d -> %d,%d %dx%d on %dx%d\n",
              x, y, width, height, rect.x, rect.y, rect.width, rect.height, rect.surface_w, rect.surface_h);
    }
    return;
  }
  glScissor(x, y, width, height);
}

static GLint VexaMapGuestToVisible(GLint value, GLint guest_origin, GLsizei guest_span, GLint visible_origin, GLsizei visible_span) {
  if (guest_span <= 0) {
    return value;
  }
  return visible_origin + static_cast<GLint>((static_cast<int64_t>(value - guest_origin) * visible_span) / guest_span);
}

// The guest window is the surface. A blit that already lands on it is the
// guest's own layout for this size. VexaCoverPresent must not crop it.
static int g_present_x0 = 0;
static int g_present_y0 = 0;
static int g_present_x1 = 0;
static int g_present_y1 = 0;
static bool g_present_ready = false;

// Screen pixels match the guest window when the frame is not rescaled.
// Identity (origin 0, crop == surface) keeps a touch on that window pixel.
static void VexaPublishPresentMap(int x0, int y0, int cw, int ch, int sx, int sy, int src_w, int src_h, int sw,
                                  int sh) {
  using Fn = void (*)(int, int, int, int, int, int, int, int, int, int);
  static Fn fn = nullptr;
  static int logged = 0;
  static int last[10];
  static int have_last = 0;
  const int next[10] = {x0, y0, cw, ch, sx, sy, src_w, src_h, sw, sh};
  if (have_last && std::memcmp(last, next, sizeof(last)) == 0) {
    return;
  }
  if (!fn) {
    fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "Vexa_PublishPresentMap"));
    if (!fn) {
      if (void* handle = dlopen("libvexa.so", RTLD_NOW | RTLD_NOLOAD)) {
        fn = reinterpret_cast<Fn>(dlsym(handle, "Vexa_PublishPresentMap"));
      }
    }
  }
  if (!fn) {
    if (logged < 1) {
      ++logged;
      fprintf(stderr, "[VEXA][GL] present map unpublished\n");
    }
    return;
  }
  fn(x0, y0, cw, ch, sx, sy, src_w, src_h, sw, sh);
  std::memcpy(last, next, sizeof(last));
  have_last = 1;
  if (logged < 2) {
    ++logged;
    fprintf(stderr, "[VEXA][GL] present map %d %d %d %d %d %d %d %d %d %d\n", x0, y0, cw, ch, sx, sy, src_w, src_h, sw,
            sh);
  }
}

static void VexaNotePresentDest(GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1) {
  const int x0 = dstX0 < dstX1 ? dstX0 : dstX1;
  const int y0 = dstY0 < dstY1 ? dstY0 : dstY1;
  const int x1 = dstX0 < dstX1 ? dstX1 : dstX0;
  const int y1 = dstY0 < dstY1 ? dstY1 : dstY0;
  if (x1 - x0 < 64 || y1 - y0 < 64) {
    return;
  }
  g_present_x0 = x0;
  g_present_y0 = y0;
  g_present_x1 = x1;
  g_present_y1 = y1;
  g_present_ready = true;
}

static void VexaCoverPresent() {
  int x0, y0, cw, ch;
  if (g_present_ready) {
    x0 = g_present_x0;
    y0 = g_present_y0;
    cw = g_present_x1 - x0;
    ch = g_present_y1 - y0;
    g_present_ready = false;
  } else if (g_vexa_win_w >= 64 && g_vexa_win_h >= 64) {
    // The guest draws straight to fb0 instead of blitting: its window-sized
    // viewport is the content rect the pointer map must undo.
    x0 = 0;
    y0 = 0;
    cw = g_vexa_win_w;
    ch = g_vexa_win_h;
  } else {
    return;
  }
  const VexaVisibleRect surface = VexaVisibleRectFor(cw, ch);
  const int sw = surface.surface_w;
  const int sh = surface.surface_h;
  if (sw <= 0 || sh <= 0 || cw < 64 || ch < 64) {
    return;
  }
  // Inside the surface, including a centered fit on a wider or taller panel:
  // leave the guest's rectangle. Covering it would crop the long axis, and the
  // number of pixels depends on the two aspect ratios, not on one device.
  {
    static int last_rate = -1;
    int aspect = 0, rate = 0;
    VexaDisplayPrefs(&aspect, &rate);
    if (rate != last_rate) {
      last_rate = rate;
      if (rate > 0) {
        if (ANativeWindow* window = VexaAcquireRuntimeWindow(0)) {
          using SetRateFn = int (*)(ANativeWindow*, float, int8_t);
          if (auto set_rate = reinterpret_cast<SetRateFn>(dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"))) {
            set_rate(window, static_cast<float>(rate), 0);
          }
          ANativeWindow_release(window);
          fprintf(stderr, "[VEXA][GL] surface frame rate %d Hz\n", rate);
        }
      }
    }
  }
  if (VexaCornerShrink(x0, y0, cw, ch, sw, sh)) {
    // Window smaller than the surface: the pointer map must undo the scale,
    // and the letterbox offset when keeping the aspect ratio.
    const VexaFitRect fit = VexaFitFor(cw, ch, sw, sh);
    const int sx = static_cast<int>(-static_cast<int64_t>(fit.x) * cw / fit.w);
    const int sy = static_cast<int>(-static_cast<int64_t>(fit.y) * ch / fit.h);
    const int src_w = static_cast<int>(static_cast<int64_t>(cw) * sw / fit.w);
    const int src_h = static_cast<int>(static_cast<int64_t>(ch) * sh / fit.h);
    VexaPublishPresentMap(0, sh - ch, cw, ch, sx, sy, src_w, src_h, sw, sh);
    return;
  }
  if (VexaRectInsideSurface(x0, y0, cw, ch, sw, sh)) {
    VexaPublishPresentMap(0, 0, sw, sh, 0, 0, sw, sh, sw, sh);
    static int kept = 0;
    if (kept < 2) {
      ++kept;
      fprintf(stderr, "[VEXA][GL] present keep %d,%d %dx%d on %dx%d\n", x0, y0, cw, ch, sw, sh);
    }
    return;
  }
  // The part that fell outside the surface was never stored. Resampling the
  // clipped remainder would still drop the guest's edges.
  VexaPublishPresentMap(0, 0, sw, sh, 0, 0, sw, sh, sw, sh);
}

// A blit whose destination already lies on the surface is in window pixels.
// Remapping it through the cover rect crops or shifts that layout.
static void VexaBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                GLbitfield mask, GLenum filter) {
  if (g_vexa_trace_frame) {
    GLint read = 0, draw = 0, src_encoding = 0, dst_encoding = 0;
    ::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    ::glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    ::glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, read ? GL_COLOR_ATTACHMENT0 : GL_BACK,
        GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &src_encoding);
    ::glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, draw ? GL_COLOR_ATTACHMENT0 : GL_BACK,
        GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &dst_encoding);
    fprintf(stderr, "[VEXA][COLOR] blit read=%d encoding=%x draw=%d encoding=%x srgb_write=%d\n",
        read, src_encoding, draw, dst_encoding, ::glIsEnabled(0x8DB9));
  }
  GLint dx0 = dstX0;
  GLint dy0 = dstY0;
  GLint dx1 = dstX1;
  GLint dy1 = dstY1;
  GLint draw_fbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &draw_fbo);
  if (draw_fbo == 0) {
    if (eglGetCurrentSurface(EGL_DRAW) == g_vexa_glx.surface) {
      g_vexa_fb0_drawn = true;
      g_vexa_fb0_blit = true;
    }
    VexaNotePresentDest(dstX0, dstY0, dstX1, dstY1);
    const int dw = dstX1 >= dstX0 ? dstX1 - dstX0 : dstX0 - dstX1;
    const int dh = dstY1 >= dstY0 ? dstY1 - dstY0 : dstY0 - dstY1;
    const bool have_guest = g_vexa_guest_w > 0 && g_vexa_guest_h > 0;
    const GLsizei guest_w = have_guest ? g_vexa_guest_w : static_cast<GLsizei>(dw);
    const GLsizei guest_h = have_guest ? g_vexa_guest_h : static_cast<GLsizei>(dh);
    const GLint guest_x = have_guest ? g_vexa_guest_x : (dstX0 < dstX1 ? dstX0 : dstX1);
    const GLint guest_y = have_guest ? g_vexa_guest_y : (dstY0 < dstY1 ? dstY0 : dstY1);
    const int dst_x = dstX0 < dstX1 ? dstX0 : dstX1;
    const int dst_y = dstY0 < dstY1 ? dstY0 : dstY1;
    VexaVisibleRect rect = VexaVisibleRectFor(guest_w, guest_h);
    if (rect.surface_w > 0 && rect.surface_h > 0 &&
        VexaCornerShrink(dst_x, dst_y, dw, dh, rect.surface_w, rect.surface_h)) {
      const VexaFitRect fit = VexaFitFor(dw, dh, rect.surface_w, rect.surface_h);
      dx0 = dstX0 < dstX1 ? fit.x : fit.x + fit.w;
      dx1 = dstX0 < dstX1 ? fit.x + fit.w : fit.x;
      dy0 = dstY0 < dstY1 ? fit.y : fit.y + fit.h;
      dy1 = dstY0 < dstY1 ? fit.y + fit.h : fit.y;
      static int corner_blit = 0;
      if (corner_blit < 4) {
        ++corner_blit;
        fprintf(stderr, "[VEXA][GL] blit corner %d,%d,%d,%d -> %d,%d,%d,%d\n", dstX0, dstY0, dstX1, dstY1, dx0, dy0,
                dx1, dy1);
      }
    }
    const bool placed = VexaRectInsideSurface(dst_x, dst_y, dw, dh, rect.surface_w, rect.surface_h);
    // Already the full surface. A guest window that is merely larger than half
    // the surface still needs to be scaled into the cover rect.
    const bool host_sized = rect.surface_w > 0 && rect.surface_h > 0 && dw + 8 >= rect.surface_w && dh + 8 >= rect.surface_h;
    const bool tiny = guest_w > 0 && guest_h > 0 && (dw * 2 < static_cast<int>(guest_w) || dh * 2 < static_cast<int>(guest_h));
    if (!placed && rect.ok && !host_sized && !tiny && guest_w > 0 && guest_h > 0) {
      dx0 = VexaMapGuestToVisible(dstX0, guest_x, guest_w, rect.x, rect.width);
      dy0 = VexaMapGuestToVisible(dstY0, guest_y, guest_h, rect.y, rect.height);
      dx1 = VexaMapGuestToVisible(dstX1, guest_x, guest_w, rect.x, rect.width);
      dy1 = VexaMapGuestToVisible(dstY1, guest_y, guest_h, rect.y, rect.height);
      const int min_x = dx0 < dx1 ? dx0 : dx1;
      const int min_y = dy0 < dy1 ? dy0 : dy1;
      const int max_x = dx0 < dx1 ? dx1 : dx0;
      const int max_y = dy0 < dy1 ? dy1 : dy0;
      // Mali rejects a blit that starts outside the surface, which left the 16:9 bars in place.
      if (min_x < 0 || min_y < 0 || max_x > rect.surface_w || max_y > rect.surface_h) {
        dx0 = dstX0;
        dy0 = dstY0;
        dx1 = dstX1;
        dy1 = dstY1;
      }
      static int logged = 0;
      if (logged < 8) {
        ++logged;
        fprintf(stderr,
                "[VEXA][GL] blit %d,%d,%d,%d -> %d,%d,%d,%d guest %d,%d %dx%d visible %d,%d %dx%d\n",
                dstX0, dstY0, dstX1, dstY1, dx0, dy0, dx1, dy1, guest_x, guest_y, guest_w, guest_h, rect.x, rect.y,
                rect.width, rect.height);
      }
    } else {
      static int skipped = 0;
      if (skipped < 4) {
        ++skipped;
        fprintf(stderr, "[VEXA][GL] blit passthrough %d,%d,%d,%d host_sized=%d tiny=%d guest %dx%d\n", dstX0, dstY0,
                dstX1, dstY1, host_sized ? 1 : 0, tiny ? 1 : 0, guest_w, guest_h);
      }
    }
  }
  // Debug: sample the blit SOURCE and destination to see whether the game
  // rendered content (source) and whether the blit deposits it (dest).
  if (draw_fbo == 0) {
    static int sampled = 0;
    if (sampled < 6) {
      ++sampled;
      GLint read_fbo = -1;
      glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
      while (glGetError() != GL_NO_ERROR) {
      }
      GLubyte src_px[9 * 4];
      int nonzero = 0;
      for (int gy = 0; gy < 3; ++gy) {
        for (int gx = 0; gx < 3; ++gx) {
          const GLint sx = srcX0 + (srcX1 - srcX0) * (2 * gx + 1) / 6;
          const GLint sy = srcY0 + (srcY1 - srcY0) * (2 * gy + 1) / 6;
          GLubyte* dst = src_px + (gy * 3 + gx) * 4;
          glReadPixels(sx, sy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, dst);
          if (dst[0] || dst[1] || dst[2]) {
            ++nonzero;
          }
        }
      }
      const GLenum src_err = glGetError();
      fprintf(stderr,
              "[VEXA][GL] blit src rfbo=%d nz=%d: %02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x "
              "%02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x err=0x%x tid=%ld\n",
              read_fbo, nonzero, src_px[0], src_px[1], src_px[2], src_px[4], src_px[5], src_px[6], src_px[8],
              src_px[9], src_px[10], src_px[12], src_px[13], src_px[14], src_px[16], src_px[17], src_px[18],
              src_px[20], src_px[21], src_px[22], src_px[24], src_px[25], src_px[26], src_px[28], src_px[29],
              src_px[30], src_px[32], src_px[33], src_px[34], src_err, (long)syscall(SYS_gettid));
      if (sampled == 3) {
        const GLint rw = srcX1 - srcX0;
        const GLint rh = srcY1 - srcY0;
        if (rw > 0 && rh > 0 && rw <= 8192 && rh <= 8192) {
          std::vector<GLubyte> img(static_cast<size_t>(rw) * rh * 4);
          glReadPixels(srcX0, srcY0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, img.data());
          const char* path = "/data/data/com.critical.vexaemulator/files/artifacts/megabonk/fb_dump.ppm";
          if (FILE* f = fopen(path, "wb")) {
            fprintf(f, "P6\n%d %d\n255\n", rw, rh);
            for (int y = rh - 1; y >= 0; --y) {
              for (int x = 0; x < rw; ++x) {
                const GLubyte* p = img.data() + (static_cast<size_t>(y) * rw + x) * 4;
                fwrite(p, 1, 3, f);
              }
            }
            fclose(f);
            fprintf(stderr, "[VEXA][GL] dumped %dx%d to %s\n", rw, rh, path);
          }
        }
      }
    }
  }
  ::glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dx0, dy0, dx1, dy1, mask, filter);
}

static void fexfn_impl_libGL_glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                                               GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
  if (g_vexa_trace_frame) {
    GLint dfbo = -1, rfbo = -1;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfbo);
    fprintf(stderr, "[VEXA][TRACE] blit %d,%d,%d,%d -> %d,%d,%d,%d mask=%x rfbo=%d dfbo=%d pending_err=0x%x\n", srcX0,
            srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, rfbo, dfbo, glGetError());
  }
  VexaBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

static void fexfn_impl_libGL_glBlitFramebufferEXT(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0,
                                                  GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
  VexaBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

// VEXA diag: count guest rendering activity reaching the host. The FBO we
// sample at blit time is all-zero; these counters show whether the guest is
// drawing at all, what its clear color is, and which FBO receives draws.
uint64_t g_vexa_diag_draws = 0;
uint64_t g_vexa_diag_dispatches = 0;
uint64_t g_vexa_diag_clears = 0;
uint64_t g_vexa_diag_useprog = 0;
uint64_t g_vexa_diag_bindfbo = 0;
// Current program is tracked per EGL context (program_cache_state.inc).
static GLuint g_vexa_diag_curfbo = 0;
static GLuint g_vexa_diag_drawfbo = 0;

#include "vertex_readonly_buffer.inc"

static void fexfn_impl_libGL_glGetProgramInterfaceiv(GLuint program, GLenum interface, GLenum pname, GLint* params) {
  if (interface == GL_SHADER_STORAGE_BLOCK && VexaGrassPrograms.count(program) && params) {
    if (pname == GL_ACTIVE_RESOURCES) { *params = 1; fprintf(stderr, "[VEXA][SSBO] active resource program=%u\n", program); return; }
    if (pname == GL_MAX_NAME_LENGTH) { *params = 13; return; }
  }
  ::glGetProgramInterfaceiv(program, interface, pname, params);
}
static void fexfn_impl_libGL_glGetProgramResourceName(GLuint program, GLenum interface, GLuint index, GLsizei size, GLsizei* length, GLchar* name) {
  if (interface == GL_SHADER_STORAGE_BLOCK && index == 0 && VexaGrassPrograms.count(program)) {
    const GLsizei n = size > 0 ? std::min<GLsizei>(12, size - 1) : 0;
    if (name && size > 0) { memcpy(name, "_GrassBuffer", n); name[n] = '\0'; }
    if (length) *length = n;
    fprintf(stderr, "[VEXA][SSBO] block name program=%u size=%d\n", program, size);
    return;
  }
  ::glGetProgramResourceName(program, interface, index, size, length, name);
}
static void fexfn_impl_libGL_glGetProgramResourceiv(GLuint program, GLenum interface, GLuint index, GLsizei count,
                                                    const GLenum* props, GLsizei size, GLsizei* length, GLint* params) {
  if (interface == GL_SHADER_STORAGE_BLOCK && index == 0 && VexaGrassPrograms.count(program)) {
    const GLsizei n = std::min(count, size);
    for (GLsizei i = 0; i < n; ++i) {
      switch (props[i]) {
        case GL_NAME_LENGTH: params[i] = 13; break;
        case GL_BUFFER_BINDING: params[i] = 0; break;
        case GL_BUFFER_DATA_SIZE: params[i] = 64; break;
        case GL_REFERENCED_BY_VERTEX_SHADER: params[i] = 1; break;
        default: params[i] = 0; break;
      }
      fprintf(stderr, "[VEXA][SSBO] resource program=%u prop=%x value=%d\n", program, props[i], params[i]);
    }
    if (length) *length = n;
    return;
  }
  ::glGetProgramResourceiv(program, interface, index, count, props, size, length, params);
}
static void fexfn_impl_libGL_glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
  if (target == GL_SHADER_STORAGE_BUFFER) {
    static unsigned logs = 0;
    if (logs++ < 16) fprintf(stderr, "[VEXA][SSBO] bindbase index=%u buffer=%u\n", index, buffer);
  }
  ::glBindBufferBase(target, index, buffer);
}
static void fexfn_impl_libGL_glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
  if (target == GL_SHADER_STORAGE_BUFFER) {
    static unsigned logs = 0;
    if (logs++ < 16) fprintf(stderr, "[VEXA][SSBO] bindrange index=%u buffer=%u offset=%lld size=%lld\n", index, buffer, (long long)offset, (long long)size);
  }
  static bool ubo_diag = access("/data/data/com.critical.vexaemulator/files/vexa_tbo_dump.flag", F_OK) == 0;
  if (ubo_diag && target == GL_UNIFORM_BUFFER) {
    static int align_v = -2;
    if (align_v == -2) { align_v = -1; ::glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align_v); }
    fprintf(stderr, "[libGL-host] uborange idx=%u buf=%u off=%lld size=%lld align=%d err_pre=0x%x\n",
            index, buffer, (long long)offset, (long long)size, align_v, ::glGetError());
  }
  ::glBindBufferRange(target, index, buffer, offset, size);
  if (ubo_diag && target == GL_UNIFORM_BUFFER) {
    const GLenum err = ::glGetError();
    if (err != GL_NO_ERROR)
      fprintf(stderr, "[libGL-host] uborange FAIL idx=%u off=%lld size=%lld err=0x%x\n", index, (long long)offset, (long long)size, err);
  }
}

static void VexaDiagDraw(const char* name, GLenum mode, GLsizei count) {
  VexaPrepareGrassBuffer(g_vexa_diag_curprog);
  if (g_vexa_trace_frame && access("/data/data/com.critical.vexaemulator/files/unity_shader_dump.flag", F_OK) == 0) {
    GLint program = 0;
    ::glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    if (program != 0)
    for (const char* uniform : {"_Color", "_AlphaParam", "_MainTex", "_MainTex_ST", "hlslcc_mtx4x4unity_MatrixVP[0]", "hlslcc_mtx4x4unity_ObjectToWorld[0]"}) {
      const GLint location = ::glGetUniformLocation(program, uniform);
      if (location < 0) continue;
      GLfloat values[16] = {};
      ::glGetUniformfv(program, location, values);
      fprintf(stderr, "[VEXA][UNIFORM] prog=%d %s loc=%d value=%g,%g,%g,%g\n", program, uniform, location, values[0], values[1], values[2], values[3]);
    }
  }
  ++g_vexa_diag_draws;
  VexaTrackFrameFbo(g_vexa_diag_drawfbo);
  // These snapshots are consumed only by the debug present probe. Keep the
  // FBO tracking above active for presentation, without querying driver state
  // for diagnostic records during normal rendering.
  const bool capture_present_debug = VexaGLDebugEnabled();
  if (capture_present_debug && g_vexa_frame_event_n < 48) {
    auto& ev = g_vexa_frame_events[g_vexa_frame_event_n++];
    ev.seq = ++g_vexa_frame_seq;
    ev.fbo = g_vexa_diag_drawfbo;
    ev.count = count;
    GLint prog = 0;
    ::glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    ev.prog = static_cast<GLuint>(prog);
  }
  if (capture_present_debug && g_vexa_diag_drawfbo == 0 && g_vexa_fb0_draw_n < 8) {
    GLint vp[4] = {};
    ::glGetIntegerv(GL_VIEWPORT, vp);
    GLint prog = 0;
    ::glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    auto& d = g_vexa_fb0_draws[g_vexa_fb0_draw_n++];
    d.mode = mode;
    d.count = count;
    d.vp_w = vp[2];
    d.vp_h = vp[3];
    d.prog = static_cast<GLuint>(prog);
    ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &d.tex);
    ::glGetIntegerv(GL_BLEND, &d.blend);
    ::glGetIntegerv(GL_BLEND_SRC_RGB, &d.blend_src);
    ::glGetIntegerv(GL_BLEND_DST_RGB, &d.blend_dst);
    ::glGetBooleanv(GL_COLOR_WRITEMASK, d.mask);
  }
  if (g_vexa_trace_frame) {
    GLint dfbo = -1;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfbo);
    fprintf(stderr, "[VEXA][TRACE] draw %s mode=%x n=%d dfbo=%d prog=%u pending_err=0x%x\n", name, mode, count, dfbo,
            g_vexa_diag_curprog, glGetError());
    GLint pipe = 0;
    ::glGetIntegerv(GL_PROGRAM_PIPELINE_BINDING, &pipe);
    if (pipe != 0) {
      GLint vs = 0, fs = 0, tc = 0, te = 0, gs = 0;
      ::glGetProgramPipelineiv(static_cast<GLuint>(pipe), GL_VERTEX_SHADER, &vs);
      ::glGetProgramPipelineiv(static_cast<GLuint>(pipe), GL_FRAGMENT_SHADER, &fs);
      ::glGetProgramPipelineiv(static_cast<GLuint>(pipe), GL_TESS_CONTROL_SHADER, &tc);
      ::glGetProgramPipelineiv(static_cast<GLuint>(pipe), GL_TESS_EVALUATION_SHADER, &te);
      ::glGetProgramPipelineiv(static_cast<GLuint>(pipe), GL_GEOMETRY_SHADER, &gs);
      fprintf(stderr, "[VEXA][TRACE]   pipe=%d vs=%d fs=%d tc=%d te=%d gs=%d\n", pipe, vs, fs, tc, te, gs);
    }
  }
  // glGetError can force the mobile driver to synchronize pending work. Keep
  // this periodic error probe in explicit tracing sessions only; normal
  // rendering already reports failures through the opt-in debug callback.
  if (GLHostTraceEnabled() && (g_vexa_diag_draws & 1023) == 0) {
    GLenum err = glGetError();
    fprintf(stderr,
            "[VEXA][GL] draws=%llu last=%s mode=%x n=%d fbo=%u prog=%u clears=%llu err=%x\n",
            (unsigned long long)g_vexa_diag_draws, name, mode, count, g_vexa_diag_curfbo,
            g_vexa_diag_curprog, (unsigned long long)g_vexa_diag_clears, err);
    static unsigned incomplete_logged = 0;
    if (err == GL_INVALID_FRAMEBUFFER_OPERATION && incomplete_logged++ < 4) {
      GLint fbo = 0, texture = 0;
      ::glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
      ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
      fprintf(stderr, "[VEXA][FBO] id=%d status=%x\n", fbo, ::glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER));
      for (GLenum attachment : {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2,
                                GL_COLOR_ATTACHMENT3, GL_COLOR_ATTACHMENT4, GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT}) {
        GLint kind = 0, object = 0, format = 0, width = 0, height = 0;
        ::glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &kind);
        if (kind == GL_NONE) continue;
        ::glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object);
        if (kind == GL_TEXTURE) {
          ::glBindTexture(GL_TEXTURE_2D, object);
          ::glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
          ::glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
          ::glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
        }
        fprintf(stderr, "[VEXA][FBO] attachment=%x kind=%x object=%d format=%x size=%dx%d\n", attachment, kind, object, format, width, height);
      }
      ::glBindTexture(GL_TEXTURE_2D, texture);
      while (::glGetError() != GL_NO_ERROR) {}
    }
  }
}

// Diagnose a failed draw: report the framebuffer completeness, its color
// attachment, and the program link state while state is still current.
static void VexaPostDrawDiag() {
  // glGetError() on a tiled GPU drains the command pipeline, so calling it
  // after every draw costs real frame time. Diagnostic only.
  if (!GLHostTraceEnabled()) {
    return;
  }
  const GLenum err = ::glGetError();
  if (err == GL_NO_ERROR) {
    return;
  }
  static int logged = 0;
  if (logged >= 20) {
    return;
  }
  ++logged;
  GLint dfbo = -1, prog = -1, link = -1, st = -1, atype = 0, aname = 0;
  ::glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfbo);
  ::glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
  if (prog > 0) {
    ::glGetProgramiv(prog, GL_LINK_STATUS, &link);
  }
  if (dfbo > 0) {
    st = ::glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    ::glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &atype);
    ::glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &aname);
  }
  GLint vao = -1, ab = -1, eab = -1;
  ::glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
  ::glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
  ::glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &eab);
  // Feedback-loop check: is the draw FBO's texture bound for sampling?
  int feedback_unit = -1;
  if (atype == GL_TEXTURE && aname > 0) {
    GLint prev_active = GL_TEXTURE0;
    ::glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    for (int u = 0; u < 16; ++u) {
      ::glActiveTexture(GL_TEXTURE0 + u);
      GLint bound = 0;
      ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
      if (bound == aname) {
        feedback_unit = u;
        break;
      }
    }
    ::glActiveTexture(prev_active);
  }
  int attribs_on = 0;
  for (int a = 0; a < 16; ++a) {
    GLint en = 0;
    ::glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en);
    if (en) {
      attribs_on |= 1 << a;
    }
  }
  fprintf(stderr,
          "[VEXA][GL] drawfail err=0x%x fbo=%d status=0x%x att=0x%x/%d prog=%d link=%d vao=%d ab=%d eab=%d attribs=0x%x fbunit=%d\n",
          err, dfbo, st, atype, aname, prog, link, vao, ab, eab, attribs_on, feedback_unit);

  // Pipeline draws (prog=0): Mali rejects the draw when stage interfaces
  // mismatch ("number of varyings differ"). Enumerate each stage's
  // PROGRAM_INPUT/OUTPUT resources so the offending pair can be compared.
  GLint pipe = 0;
  ::glGetIntegerv(0x825A /* GL_PROGRAM_PIPELINE_BINDING */, &pipe);
  while (::glGetError() != GL_NO_ERROR) {
  }
  if (pipe > 0) {
    GLint valid = 0;
    ::glGetProgramPipelineiv(pipe, GL_VALIDATE_STATUS, &valid);
    fprintf(stderr, "[VEXA][PIPE] pipeline=%d validate=%d\n", (int)pipe, (int)valid);
    auto dump_iface = [](GLenum stage, GLuint program, GLenum iface) {
      GLint count = 0;
      ::glGetProgramInterfaceiv(program, iface, GL_ACTIVE_RESOURCES, &count);
      fprintf(stderr, "[VEXA][PIPE] stage=0x%x prog=%u iface=0x%x active=%d\n", (unsigned)stage, program,
              (unsigned)iface, (int)count);
      for (GLint i = 0; i < count && i < 24; ++i) {
        const GLenum props[] = {0x92E3 /* GL_NAME_LENGTH */, 0x92E0 /* GL_LOCATION */};
        GLint vals[2] = {};
        ::glGetProgramResourceiv(program, iface, (GLuint)i, 2, props, 2, nullptr, vals);
        char rname[128] = {};
        ::glGetProgramResourceName(program, iface, (GLuint)i, (GLsizei)sizeof(rname), nullptr, rname);
        fprintf(stderr, "[VEXA][PIPE]   res[%d] loc=%d name=%s\n", (int)i, (int)vals[1], rname);
      }
    };
    for (GLenum stage : {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER}) {
      GLint stage_prog = 0;
      ::glGetProgramPipelineiv(pipe, stage, &stage_prog);
      if (stage_prog <= 0) continue;
      GLint stage_link = -1;
      ::glGetProgramiv((GLuint)stage_prog, GL_LINK_STATUS, &stage_link);
      fprintf(stderr, "[VEXA][PIPE] stage=0x%x prog=%d link=%d\n", (unsigned)stage, (int)stage_prog,
              (int)stage_link);
      dump_iface(stage, (GLuint)stage_prog,
                 stage == GL_VERTEX_SHADER ? 0x92E4 /* GL_PROGRAM_OUTPUT */ : 0x92E3 /* GL_PROGRAM_INPUT */);
    }
    while (::glGetError() != GL_NO_ERROR) {
    }
  }
}

// Active stage program is tracked per EGL context.

static GLuint VexaStageProgram() {
  if (g_vexa_stage_program) return g_vexa_stage_program;
  GLint pipe = 0;
  ::glGetIntegerv(0x825A /* GL_PROGRAM_PIPELINE_BINDING */, &pipe);
  while (::glGetError() != GL_NO_ERROR) {
  }
  if (!pipe) return 0;
  const GLenum stages[] = {GL_FRAGMENT_SHADER, GL_VERTEX_SHADER, 0x8DD9, 0x91B9};
  for (GLenum stage : stages) {
    GLint program = 0;
    ::glGetProgramPipelineiv(pipe, stage, &program);
    while (::glGetError() != GL_NO_ERROR) {
    }
    if (program) {
      g_vexa_stage_program = static_cast<GLuint>(program);
      return g_vexa_stage_program;
    }
  }
  return 0;
}

// ---- separable-pipeline vertex clone ----------------------------------------
// ETS2 pairs separable programs whose interfaces are legal supersets on
// desktop (the vertex stage writes varyings the fragment stage never reads).
// Mali's program-pipeline validator requires matching interfaces and rejects
// every draw on such pipelines ("the number of varyings differ"), so whole
// passes are silently skipped (black sky, missing decals/particles). When a
// pipeline fails validation we compile a sibling separable vertex program
// whose source has the unused outputs demoted to plain globals (the `out`
// qualifier is dropped; assignments keep compiling and are dead-code
// eliminated), then swap it into the pipeline's vertex stage. The pipeline
// stays a real pipeline; uniform and block-binding calls aimed at the
// original program are mirrored to the clone.
// Pipeline validation state is local to each EGL context.
// Sources and clone metadata live in the EGL share group.

static bool VexaIsNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// (name, location) for every active resource of the given interface.
static std::vector<std::pair<std::string, GLint>> VexaInterfaceVars(GLuint prog, GLenum iface) {
  std::vector<std::pair<std::string, GLint>> out;
  GLint count = 0;
  ::glGetProgramInterfaceiv(prog, iface, GL_ACTIVE_RESOURCES, &count);
  for (GLint i = 0; i < count && i < 64; ++i) {
    const GLenum props[] = {0x930E /* GL_LOCATION */};
    GLint loc = -1;
    ::glGetProgramResourceiv(prog, iface, (GLuint)i, 1, props, 1, nullptr, &loc);
    char name[128] = {};
    ::glGetProgramResourceName(prog, iface, (GLuint)i, (GLsizei)sizeof(name), nullptr, name);
    if (name[0]) out.emplace_back(name, loc);
  }
  return out;
}

// Demotes `out` declarations of the given variables to plain globals. Works
// line by line: a line that contains an `out` qualifier and declares the name
// loses that qualifier; assignments to the variable keep compiling.
static bool VexaDemoteOutputs(std::string& src, const std::vector<std::string>& names) {
  bool all = true;
  for (const auto& name : names) {
    bool done = false;
    size_t pos = 0;
    while (pos < src.size()) {
      const size_t nl = src.find('\n', pos);
      const size_t lend = (nl == std::string::npos) ? src.size() : nl;
      const std::string_view line(src.data() + pos, lend - pos);
      // must contain the name as a whole word
      size_t np = line.find(name);
      bool word = false;
      while (np != std::string_view::npos) {
        const bool l = np == 0 || !VexaIsNameChar(line[np - 1]);
        const bool r = np + name.size() >= line.size() || !VexaIsNameChar(line[np + name.size()]);
        if (l && r) { word = true; break; }
        np = line.find(name, np + 1);
      }
      if (word) {
        // find an `out` qualifier token before the name on this line, with no
        // '(' between it and the name (rules out `foo(out x)` call syntax)
        for (size_t op = line.find("out"); op != std::string_view::npos && op < np;
             op = line.find("out", op + 1)) {
          const bool l = op == 0 || !VexaIsNameChar(line[op - 1]);
          const bool r = op + 3 >= line.size() || !VexaIsNameChar(line[op + 3]);
          if (!l || !r) continue;
          if (line.substr(op + 3, np - (op + 3)).find('(') != std::string_view::npos) continue;
          // only strip when this statement is a declaration ending in ';'
          if (line.find(';', np) == std::string_view::npos) break;
          // A preceding `layout(...)` or interpolation qualifier (flat,
          // smooth, ...) applies to the interface declaration; it is invalid
          // on a plain variable, so erase it as well.
          size_t from = op;
          for (;;) {
            size_t ws = from;
            while (ws > 0 && (line[ws - 1] == ' ' || line[ws - 1] == '\t')) --ws;
            size_t wend = ws;
            while (wend > 0 && VexaIsNameChar(line[wend - 1])) --wend;
            const std::string_view w = line.substr(wend, wend != ws ? ws - wend : 0);
            if (w == "flat" || w == "smooth" || w == "noperspective" || w == "centroid" ||
                w == "invariant") {
              from = wend;
              continue;
            }
            if (w.empty() && ws > 0 && line[ws - 1] == ')') {
              // `layout(...)` closes with ')' — check the token before '('
              size_t lq = line.rfind("layout", ws - 1);
              if (lq != std::string_view::npos) {
                size_t lp = lq + 6;
                while (lp < line.size() && (line[lp] == ' ' || line[lp] == '\t')) ++lp;
                if (lp < line.size() && line[lp] == '(') {
                  from = lq;
                  break;
                }
              }
            }
            break;
          }
          src.erase(pos + from, op + 3 - from);
          done = true;
          break;
        }
      }
      pos = lend + (nl == std::string::npos ? 0 : 1);
      if (done) break;
    }
    if (!done) {
      fprintf(stderr, "[VEXA][PIPE] clone: could not demote out %s\n", name.c_str());
      all = false;
    }
  }
  return all;
}

// Builds a separable clone of the pipeline's vertex program with the varyings
// the fragment stage doesn't read removed, and installs it into the pipeline.
#include "clone_binary_cache.inc"

static GLuint VexaBuildStrippedVertexProgram(GLuint pipe) {
  VEXA_PROGRAM_LOCK();
  const auto cache_start = std::chrono::steady_clock::now();
  GLint vs_prog = 0, fs_prog = 0;
  ::glGetProgramPipelineiv(pipe, GL_VERTEX_SHADER, &vs_prog);
  ::glGetProgramPipelineiv(pipe, GL_FRAGMENT_SHADER, &fs_prog);
  {
    GLint tcs = 0, tes = 0, gs = 0, cs = 0;
    ::glGetProgramPipelineiv(pipe, GL_TESS_CONTROL_SHADER, &tcs);
    ::glGetProgramPipelineiv(pipe, GL_TESS_EVALUATION_SHADER, &tes);
    ::glGetProgramPipelineiv(pipe, GL_GEOMETRY_SHADER, &gs);
    ::glGetProgramPipelineiv(pipe, GL_COMPUTE_SHADER, &cs);
    if (tcs || tes || gs || cs) {
      fprintf(stderr, "[VEXA][PIPE] pipeline=%d extra stages tcs=%d tes=%d gs=%d cs=%d\n", (int)pipe,
              (int)tcs, (int)tes, (int)gs, (int)cs);
    }
  }
  while (::glGetError() != GL_NO_ERROR) {
  }
  if (!vs_prog || !fs_prog) return 0;

  const auto fs_in = VexaInterfaceVars((GLuint)fs_prog, 0x92E3 /* GL_PROGRAM_INPUT */);
  const auto vs_out = VexaInterfaceVars((GLuint)vs_prog, 0x92E4 /* GL_PROGRAM_OUTPUT */);
  auto in_vs = [&vs_out](const std::string& n) {
    for (const auto& v : vs_out) {
      if (v.first == n) return true;
    }
    return false;
  };
  // If the fragment stage reads something the vertex stage doesn't write,
  // stripping outputs can't fix this pair.
  for (const auto& f : fs_in) {
    if (f.first.compare(0, 3, "gl_") == 0) continue;
    if (!in_vs(f.first)) {
      fprintf(stderr, "[VEXA][PIPE] clone: fs reads %s which vs lacks; not a superset\n",
              f.first.c_str());
      return 0;
    }
  }
  std::vector<std::string> unused;
  for (const auto& v : vs_out) {
    if (v.first.compare(0, 3, "gl_") == 0) continue;
    bool consumed = false;
    for (const auto& f : fs_in) {
      if (f.first == v.first) { consumed = true; break; }
    }
    if (!consumed) unused.push_back(v.first);
  }
  if (unused.empty()) return 0;

  std::string src;
  if (const auto it = g_vexa_prog_sources.find((GLuint)vs_prog); it != g_vexa_prog_sources.end()) {
    for (const auto& s : it->second) {
      if (s.first == GL_VERTEX_SHADER) { src = s.second; break; }
    }
  }
  if (src.empty()) {
    if (const auto it = g_vexa_prog_direct_source.find((GLuint)vs_prog);
        it != g_vexa_prog_direct_source.end() && it->second.first == GL_VERTEX_SHADER) {
      src = it->second.second;
    }
  }
  if (src.empty()) {
    fprintf(stderr, "[VEXA][PIPE] clone: no source for vs prog=%d\n", (int)vs_prog);
    return 0;
  }
  if (!VexaDemoteOutputs(src, unused)) return 0;

  GLuint clone = VexaLoadCloneBinary(src);
  const bool binary_hit = clone != 0;
  if (!binary_hit) {
    const GLuint shader = ::glCreateShader(GL_VERTEX_SHADER);
    const char* sp = src.c_str();
    ::glShaderSource(shader, 1, &sp, nullptr);
    ::glCompileShader(shader);
    GLint compiled = 0;
    ::glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
      GLint len = 0;
      ::glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
      std::string log(static_cast<size_t>(len > 0 ? len : 1), '\0');
      if (len > 0) ::glGetShaderInfoLog(shader, len, nullptr, log.data());
      fprintf(stderr, "[VEXA][PIPE] clone compile failed vs=%d: %.200s\n", (int)vs_prog, log.c_str());
      ::glDeleteShader(shader);
      return 0;
    }
    clone = ::glCreateProgram();
    ::glProgramParameteri(clone, GL_PROGRAM_SEPARABLE, GL_TRUE);
    if (VexaCloneCacheEnabled()) ::glProgramParameteri(clone, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    ::glAttachShader(clone, shader);
    ::glLinkProgram(clone);
    ::glDetachShader(clone, shader);
    ::glDeleteShader(shader);
    GLint linked = 0;
    ::glGetProgramiv(clone, GL_LINK_STATUS, &linked);
    if (!linked) {
      fprintf(stderr, "[VEXA][PIPE] clone link failed vs=%d clone=%u\n", (int)vs_prog, clone);
      ::glDeleteProgram(clone);
      return 0;
    }
    VexaRememberCloneBinary(clone, src);
  }

  // Uniform locations differ between programs; remember the mapping so
  // glProgramUniform* calls on the original can be mirrored to the clone.
  {
    GLint n = 0;
    ::glGetProgramInterfaceiv((GLuint)vs_prog, GL_UNIFORM, GL_ACTIVE_RESOURCES, &n);
    auto& lm = g_vexa_clone_locs[clone];
    const GLenum props[] = {GL_LOCATION, GL_ARRAY_SIZE, GL_TYPE, GL_NAME_LENGTH};
    for (GLint i = 0; i < n; ++i) {
      GLint vals[4] = {-1, 1, GL_FLOAT, 0};
      ::glGetProgramResourceiv((GLuint)vs_prog, GL_UNIFORM, (GLuint)i, 4, props, 4, nullptr, vals);
      if (vals[0] < 0) continue;
      if (vals[3] <= 0) continue;
      std::string uname(static_cast<size_t>(vals[3]), '\0');
      GLsizei name_length = 0;
      ::glGetProgramResourceName((GLuint)vs_prog, GL_UNIFORM, (GLuint)i, vals[3], &name_length, uname.data());
      uname.resize(static_cast<size_t>(name_length));
      // Map every array element: element locations are not guaranteed to be
      // consecutive, so look each one up by name on both programs.
      for (GLint e = 0; e < vals[1]; ++e) {
        std::string ename = uname;
        if (vals[1] > 1) {
          const auto bracket = ename.find("[0]");
          const auto index = "[" + std::to_string(e) + "]";
          if (bracket == std::string::npos) ename += index;
          else ename.replace(bracket, 3, index);
        }
        const GLint oloc = ::glGetUniformLocation((GLuint)vs_prog, ename.c_str());
        if (oloc >= 0) {
          lm[oloc] = ::glGetUniformLocation(clone, ename.c_str());
          VexaCopyUniform(static_cast<GLuint>(vs_prog), clone, oloc, lm[oloc], static_cast<GLenum>(vals[2]));
        }
      }
    }
  }
  // Uniform-block bindings are program state; copy them across.
  {
    GLint n = 0;
    ::glGetProgramInterfaceiv((GLuint)vs_prog, GL_UNIFORM_BLOCK, GL_ACTIVE_RESOURCES, &n);
    for (GLint i = 0; i < n; ++i) {
      GLuint idx = (GLuint)i;
      GLint binding = -1;
      ::glGetActiveUniformBlockiv((GLuint)vs_prog, idx, GL_UNIFORM_BLOCK_BINDING, &binding);
      char bname[128] = {};
      ::glGetActiveUniformBlockName((GLuint)vs_prog, idx, (GLsizei)sizeof(bname), nullptr, bname);
      const GLuint cidx = ::glGetUniformBlockIndex(clone, bname);
      if (cidx != GL_INVALID_INDEX && binding >= 0) {
        ::glUniformBlockBinding(clone, cidx, (GLuint)binding);
      }
    }
  }

  VexaProgramContext().OwnedClones[pipe] = {static_cast<GLuint>(vs_prog), clone};
  g_vexa_clones_of_prog[(GLuint)vs_prog].push_back(clone);
  ::glUseProgramStages(pipe, GL_VERTEX_SHADER_BIT, clone);
  while (::glGetError() != GL_NO_ERROR) {
  }
  const double cache_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cache_start).count();
  fprintf(stderr, "[VEXA][PIPE] pipeline=%d vs=%d clone=%u stripped=%zu inst=%d grass=%d binary_hit=%d cache=%s build_ms=%.3f\n",
          (int)pipe, (int)vs_prog, clone, unused.size(),
          src.find("gl_InstanceID") != std::string::npos ? 1 : 0,
          src.find("_GrassBuffer") != std::string::npos ? 1 : 0, binary_hit ? 1 : 0,
          g_vexa_clone_hit_source, cache_ms);
  return clone;
}

// Validates each bound pipeline once. On failure, tries to repair it with a
// stripped-output vertex clone (see VexaBuildStrippedVertexProgram).
static void VexaPreDraw() {
  VEXA_PROGRAM_LOCK();
  VexaRefreshProgramGeneration();
  if (!g_vexa_intended_pipe) return;
  int st = g_vexa_pipe_state[g_vexa_intended_pipe];
  if (st != 0) return;
  ::glValidateProgramPipeline(g_vexa_intended_pipe);
  GLint valid = 0;
  ::glGetProgramPipelineiv(g_vexa_intended_pipe, GL_VALIDATE_STATUS, &valid);
  if (valid) {
    g_vexa_pipe_state[g_vexa_intended_pipe] = 1;
    return;
  }
  const GLuint clone = VexaBuildStrippedVertexProgram(g_vexa_intended_pipe);
  g_vexa_pipe_state[g_vexa_intended_pipe] = clone ? 2 : -1;
  if (!clone) {
    fprintf(stderr, "[VEXA][PIPE] pipeline=%u invalid (interface mismatch)\n",
            static_cast<unsigned>(g_vexa_intended_pipe));
  }
}

// ETS2 keeps a program pipeline bound and glUseProgram(0). glUniform* then
// applies to program 0, and glGetUniformLocation(0) is rejected, so sampler
// and material values stay at zero and the shaded frame is black.
// Link-status cache belongs to the program share group.

static GLuint VexaUniformTarget(GLuint program) {
  VEXA_PROGRAM_LOCK();
  // Hot path: every glUniform* under a bound pipeline lands here with 0.
  if (!program && !g_vexa_diag_curprog) return VexaStageProgram();
  auto linked = [](GLuint candidate) {
    if (!candidate) return false;
    if (g_vexa_linked_programs.count(candidate)) return true;
    GLint status = GL_FALSE;
    ::glGetProgramiv(candidate, GL_LINK_STATUS, &status);
    while (::glGetError() != GL_NO_ERROR) {
    }
    if (status == GL_TRUE) g_vexa_linked_programs.insert(candidate);
    return status == GL_TRUE;
  };
  if (linked(program)) return program;
  if (linked(g_vexa_diag_curprog)) return g_vexa_diag_curprog;
  return VexaStageProgram();
}

static GLint VexaGL_glGetUniformLocation(GLuint program, const GLchar* name) {
  const GLuint target = VexaUniformTarget(program);
  static int logs = 0;
  if (logs++ < 8) {
    fprintf(stderr, "[VEXA][UNI] GetUniformLocation(%u) -> %u name=%s\n", program, target, name ? name : "");
  }
  if (!target) return -1;
  return ::glGetUniformLocation(target, name);
}

static void VexaGL_glUniformBlockBinding(GLuint program, GLuint blockIndex, GLuint bindingPoint) {
  VEXA_PROGRAM_LOCK();
  ::glUniformBlockBinding(program, blockIndex, bindingPoint);
  // Mirror to clone programs (block bindings are per-program state, and the
  // guest only knows about the original).
  if (const auto it = g_vexa_clones_of_prog.find(program); it != g_vexa_clones_of_prog.end()) {
    char bname[128] = {};
    ::glGetActiveUniformBlockName(program, blockIndex, (GLsizei)sizeof(bname), nullptr, bname);
    for (GLuint c : it->second) {
      const GLuint cidx = ::glGetUniformBlockIndex(c, bname);
      if (cidx != GL_INVALID_INDEX) ::glUniformBlockBinding(c, cidx, bindingPoint);
    }
  }
}

static void VexaGL_glUseProgramStages(GLuint pipeline, GLbitfield stages, GLuint program) {
  VEXA_PROGRAM_LOCK();
  VexaRefreshProgramGeneration();
  VexaReleasePipelineClone(pipeline, true);
  // GL_FRAGMENT_SHADER_BIT is 0x2. GL_ALL_SHADER_BITS is all ones.
  if ((stages & 0x2u) != 0 || stages == 0xFFFFFFFFu) g_vexa_stage_program = program;
  else if (!g_vexa_stage_program) g_vexa_stage_program = program;
  g_vexa_pipe_state.erase(pipeline);
  ::glUseProgramStages(pipeline, stages, program);
}

static void VexaGL_glBindProgramPipeline(GLuint pipeline) {
  VEXA_PROGRAM_LOCK();
  g_vexa_stage_program = 0;
  g_vexa_intended_pipe = pipeline;
  ::glBindProgramPipeline(pipeline);
}

static void VexaGL_glActiveShaderProgram(GLuint pipeline, GLuint program) {
  VEXA_PROGRAM_LOCK();
  g_vexa_stage_program = program;
  ::glActiveShaderProgram(pipeline, program);
}

static void VexaGL_glProgramUniform1i(GLuint program, GLint location, GLint v0) {
  VEXA_PROGRAM_LOCK();
  const GLuint target = VexaUniformTarget(program);
  if (target) ::glProgramUniform1i(target, location, v0);
  if (const auto it = g_vexa_clones_of_prog.find(program); it != g_vexa_clones_of_prog.end()) {
    for (GLuint c : it->second) {
      const auto li = g_vexa_clone_locs[c].find(location);
      if (li != g_vexa_clone_locs[c].end() && li->second >= 0) ::glProgramUniform1i(c, li->second, v0);
    }
  }
}

// Scalar glProgramUniform* forms thunk straight to the host; mirror them to
// cloned stage programs the same way the vector/matrix forms are.
#define VEXA_WRAP_PROG_UNI_S(uname, T, decl, ...) \
static void VexaGL_glProgramUniform##uname(GLuint program, GLint location, decl) { \
  VEXA_PROGRAM_LOCK(); \
  ::glProgramUniform##uname(program, location, __VA_ARGS__); \
  if (const auto it = g_vexa_clones_of_prog.find(program); it != g_vexa_clones_of_prog.end()) { \
    for (GLuint c : it->second) { \
      const auto li = g_vexa_clone_locs[c].find(location); \
      if (li != g_vexa_clone_locs[c].end() && li->second >= 0) { \
        ::glProgramUniform##uname(c, li->second, __VA_ARGS__); \
      } \
    } \
  } \
}
#define VEXA_1V(T) T v0
#define VEXA_2V(T) VEXA_1V(T), T v1
#define VEXA_3V(T) VEXA_2V(T), T v2
#define VEXA_4V(T) VEXA_3V(T), T v3
VEXA_WRAP_PROG_UNI_S(1f, GLfloat, VEXA_1V(GLfloat), v0)
VEXA_WRAP_PROG_UNI_S(2f, GLfloat, VEXA_2V(GLfloat), v0, v1)
VEXA_WRAP_PROG_UNI_S(3f, GLfloat, VEXA_3V(GLfloat), v0, v1, v2)
VEXA_WRAP_PROG_UNI_S(4f, GLfloat, VEXA_4V(GLfloat), v0, v1, v2, v3)
VEXA_WRAP_PROG_UNI_S(2i, GLint, VEXA_2V(GLint), v0, v1)
VEXA_WRAP_PROG_UNI_S(3i, GLint, VEXA_3V(GLint), v0, v1, v2)
VEXA_WRAP_PROG_UNI_S(4i, GLint, VEXA_4V(GLint), v0, v1, v2, v3)
VEXA_WRAP_PROG_UNI_S(1ui, GLuint, VEXA_1V(GLuint), v0)
VEXA_WRAP_PROG_UNI_S(2ui, GLuint, VEXA_2V(GLuint), v0, v1)
VEXA_WRAP_PROG_UNI_S(3ui, GLuint, VEXA_3V(GLuint), v0, v1, v2)
VEXA_WRAP_PROG_UNI_S(4ui, GLuint, VEXA_4V(GLuint), v0, v1, v2, v3)

// Applies `apply(clone, mapped_location)` for every clone of `program`.
template<typename F>
static void VexaMirrorUniform(GLuint program, GLint location, F apply) {
  VEXA_PROGRAM_LOCK();
  const auto it = g_vexa_clones_of_prog.find(program);
  if (it == g_vexa_clones_of_prog.end()) return;
  for (GLuint c : it->second) {
    const auto li = g_vexa_clone_locs[c].find(location);
    if (li != g_vexa_clone_locs[c].end() && li->second >= 0) apply(c, li->second);
  }
}

#define VEXA_MIRROR_UNI(suffix, ...) \
  VexaMirrorUniform(g_vexa_diag_curprog ? g_vexa_diag_curprog : target, location, \
    [&](GLuint c, GLint l){ ::glProgramUniform##suffix(c, l, __VA_ARGS__); })
#define VEXA_MIRROR_MAT(n, ...) \
  VexaMirrorUniform(g_vexa_diag_curprog ? g_vexa_diag_curprog : target, location, \
    [&](GLuint c, GLint l){ ::glProgramUniformMatrix##n##fv(c, l, __VA_ARGS__); })
#define VEXA_U1(suffix, type)                                                                                          \
  static void VexaGL_glUniform##suffix(GLint location, type v0) {                                                     \
    if (g_vexa_diag_curprog) {                                                                                         \
      ::glUniform##suffix(location, v0);                                                                              \
      const GLuint target = 0;                                                                                        \
      VEXA_MIRROR_UNI(suffix, v0);                                                                                     \
      return;                                                                                                         \
    }                                                                                                                  \
    const GLuint target = VexaUniformTarget(0);                                                                       \
    if (target) ::glProgramUniform##suffix(target, location, v0);                                                     \
    VEXA_MIRROR_UNI(suffix, v0);                                                                                       \
  }
#define VEXA_UN(suffix, type)                                                                                          \
  static void VexaGL_glUniform##suffix(GLint location, GLsizei count, const type* value) {                            \
    if (g_vexa_diag_curprog) {                                                                                         \
      ::glUniform##suffix(location, count, value);                                                                    \
      const GLuint target = 0;                                                                                        \
      VEXA_MIRROR_UNI(suffix, count, value);                                                                           \
      return;                                                                                                         \
    }                                                                                                                  \
    const GLuint target = VexaUniformTarget(0);                                                                       \
    if (target) ::glProgramUniform##suffix(target, location, count, value);                                           \
    VEXA_MIRROR_UNI(suffix, count, value);                                                                             \
  }
#define VEXA_UMAT(n)                                                                                                   \
  static void VexaGL_glUniformMatrix##n##fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) { \
    if (g_vexa_diag_curprog) {                                                                                         \
      ::glUniformMatrix##n##fv(location, count, transpose, value);                                                    \
      const GLuint target = 0;                                                                                        \
      VEXA_MIRROR_MAT(n, count, transpose, value);                                                                     \
      return;                                                                                                         \
    }                                                                                                                  \
    const GLuint target = VexaUniformTarget(0);                                                                       \
    if (target) ::glProgramUniformMatrix##n##fv(target, location, count, transpose, value);                           \
    VEXA_MIRROR_MAT(n, count, transpose, value);                                                                       \
  }

VEXA_U1(1i, GLint)
VEXA_U1(1f, GLfloat)
#define VEXA_U2(suffix, type)                                                                                          \
  static void VexaGL_glUniform##suffix(GLint location, type v0, type v1) {                                            \
    if (g_vexa_diag_curprog) {                                                                                         \
      ::glUniform##suffix(location, v0, v1);                                                                          \
      const GLuint target = 0;                                                                                        \
      VEXA_MIRROR_UNI(suffix, v0, v1);                                                                                 \
      return;                                                                                                         \
    }                                                                                                                  \
    const GLuint target = VexaUniformTarget(0);                                                                       \
    if (target) ::glProgramUniform##suffix(target, location, v0, v1);                                                 \
    VEXA_MIRROR_UNI(suffix, v0, v1);                                                                                   \
  }
#define VEXA_U3(suffix, type)                                                                                          \
  static void VexaGL_glUniform##suffix(GLint location, type v0, type v1, type v2) {                                   \
    if (g_vexa_diag_curprog) {                                                                                         \
      ::glUniform##suffix(location, v0, v1, v2);                                                                      \
      const GLuint target = 0;                                                                                        \
      VEXA_MIRROR_UNI(suffix, v0, v1, v2);                                                                             \
      return;                                                                                                         \
    }                                                                                                                  \
    const GLuint target = VexaUniformTarget(0);                                                                       \
    if (target) ::glProgramUniform##suffix(target, location, v0, v1, v2);                                             \
    VEXA_MIRROR_UNI(suffix, v0, v1, v2);                                                                               \
  }
#define VEXA_U4(suffix, type)                                                                                          \
  static void VexaGL_glUniform##suffix(GLint location, type v0, type v1, type v2, type v3) {                          \
    if (g_vexa_diag_curprog) {                                                                                         \
      ::glUniform##suffix(location, v0, v1, v2, v3);                                                                  \
      const GLuint target = 0;                                                                                        \
      VEXA_MIRROR_UNI(suffix, v0, v1, v2, v3);                                                                         \
      return;                                                                                                         \
    }                                                                                                                  \
    const GLuint target = VexaUniformTarget(0);                                                                       \
    if (target) ::glProgramUniform##suffix(target, location, v0, v1, v2, v3);                                         \
    VEXA_MIRROR_UNI(suffix, v0, v1, v2, v3);                                                                           \
  }

VEXA_U2(2i, GLint)
VEXA_U2(2f, GLfloat)
VEXA_U3(3i, GLint)
VEXA_U3(3f, GLfloat)
VEXA_U4(4i, GLint)
VEXA_U4(4f, GLfloat)
VEXA_UN(1iv, GLint)
VEXA_UN(1fv, GLfloat)
VEXA_UN(2iv, GLint)
VEXA_UN(2fv, GLfloat)
VEXA_UN(3iv, GLint)
VEXA_UN(3fv, GLfloat)
VEXA_UN(4iv, GLint)
VEXA_UN(4fv, GLfloat)
VEXA_UMAT(2)
VEXA_UMAT(3)
VEXA_UMAT(4)
#undef VEXA_U1
#undef VEXA_U2
#undef VEXA_U3
#undef VEXA_U4
#undef VEXA_UN
#undef VEXA_UMAT

static void fexfn_impl_libGL_glUseProgram(GLuint program) {
  g_vexa_diag_curprog = program;
  ++g_vexa_diag_useprog;
  if (g_vexa_trace_frame) {
    fprintf(stderr, "[VEXA][TRACE] useprog %u pending_err=0x%x\n", program, glGetError());
  }
  ::glUseProgram(program);
}

static void fexfn_impl_libGL_glBindFramebuffer(GLenum target, GLuint framebuffer) {
  const GLuint prev_drawfbo = g_vexa_diag_drawfbo;
  g_vexa_diag_curfbo = framebuffer;
  if (target != GL_READ_FRAMEBUFFER) {
    g_vexa_diag_drawfbo = framebuffer;
    if (VexaGLDebugEnabled()) {
      int n = g_vexa_frame_binds_len;
      if (n < (int)sizeof(g_vexa_frame_binds) - 8) {
        g_vexa_frame_binds_len +=
            snprintf(g_vexa_frame_binds + n, sizeof(g_vexa_frame_binds) - n, "%s%u",
                     n ? ">" : "", framebuffer);
      }
    }
  }
  ++g_vexa_diag_bindfbo;
  if (g_vexa_trace_frame) {
    fprintf(stderr, "[VEXA][TRACE] bindfbo target=%x fbo=%u pending_err=0x%x\n", target, framebuffer, glGetError());
  } else {
    static int early_binds = 0;
    if (early_binds < 60) {
      ++early_binds;
      fprintf(stderr, "[VEXA][GL] early bindfbo #%d target=%x fbo=%u tid=%ld\n", early_binds, target, framebuffer,
              (long)syscall(SYS_gettid));
    }
  }
  // Diagnostic: when switching away from an offscreen draw FBO to the window
  // framebuffer, dump the offscreen target's pixels once. This isolates
  // whether a screen artifact already exists in the offscreen pass or is
  // introduced by the final composite.
  static const bool fbo_dump =
    access("/data/data/com.critical.vexaemulator/files/vexa_fbo_dump.flag", F_OK) == 0;
  if (framebuffer == 0 && prev_drawfbo != 0 && fbo_dump) {
    static int dumped = 0;
    static GLuint seen[16] = {};
    bool seen_before = false;
    for (int i = 0; i < 16 && seen[i]; ++i) seen_before |= seen[i] == prev_drawfbo;
    if (!seen_before) {
      for (int i = 0; i < 16; ++i) if (!seen[i]) { seen[i] = prev_drawfbo; break; }
      fprintf(stderr, "[VEXA][DUMP] transition fbo=%u -> 0\n", prev_drawfbo);
    }
    if (dumped < 60) {
      ++dumped;
      GLint dims[4] = {};
      ::glGetIntegerv(GL_VIEWPORT, dims);
      const GLint w = dims[2] > 0 ? dims[2] : 1920;
      const GLint h = dims[3] > 0 ? dims[3] : 1080;
      std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
      ::glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_drawfbo);
      GLint read_fmt = GL_RGBA, read_type = GL_UNSIGNED_BYTE;
      ::glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &read_fmt);
      ::glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &read_type);
      ::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      const GLenum rd_err = ::glGetError();
      char path[160];
      snprintf(path, sizeof(path),
               "/data/data/com.critical.vexaemulator/files/artifacts/ets2/fbo%u_%dx%d_%d.ppm",
               prev_drawfbo, w, h, dumped);
      if (FILE* f = fopen(path, "wb")) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (GLint yy = h - 1; yy >= 0; --yy) {
          for (GLint xx = 0; xx < w; ++xx) {
            const uint8_t* p = px.data() + (static_cast<size_t>(yy) * w + xx) * 4;
            fputc(p[0], f); fputc(p[1], f); fputc(p[2], f);
          }
        }
        fclose(f);
      }
      fprintf(stderr, "[VEXA][DUMP] readfmt=0x%x readtype=0x%x err=0x%x\n", read_fmt, read_type, rd_err);
      fprintf(stderr, "[VEXA][DUMP] fbo=%u %dx%d -> %s\n", g_vexa_diag_drawfbo, w, h, path);
    }
  }
  ::glBindFramebuffer(target, framebuffer);
}

static void fexfn_impl_libGL_glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
  static int cc_logged = 0;
  if (cc_logged < 12 || g_vexa_trace_frame) {
    ++cc_logged;
    fprintf(stderr, "[VEXA][GL] clearcolor %.2f %.2f %.2f %.2f fbo=%u\n", red, green, blue, alpha,
            g_vexa_diag_curfbo);
  }
  ::glClearColor(red, green, blue, alpha);
}

static void fexfn_impl_libGL_glClear(GLbitfield mask) {
  ++g_vexa_diag_clears;
  if (g_vexa_trace_frame) {
    GLint dfbo = -1;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfbo);
    fprintf(stderr, "[VEXA][TRACE] clear mask=%x dfbo=%d pending_err=0x%x\n", mask, dfbo, glGetError());
  }
  ::glClear(mask);
}

// Desktop GL exposes a few enable/disable caps and tex-parameter names that
// have no GLES 3.x equivalent. Unity GLCore sets some of them every frame and
// the Mali driver answers GL_INVALID_ENUM. This helper logs each distinct value
// once (to drive a desktop->GLES allowlist) and drains the error queue so stale
// state cannot leak into the guest's later glGetError reads.
static void VexaGLEnumCompat(const char* what, GLenum value) {
  // Draining the error queue costs a round trip to the driver, and Unity sets
  // these caps several times per draw. Behind a trace flag only: otherwise the
  // guest's own glGetError sees the error, which is where it belongs anyway.
  if (!GLHostTraceEnabled()) {
    return;
  }
  static GLenum seen[64];
  static int seen_n = 0;
  while (::glGetError() != GL_NO_ERROR) {
  }
  for (int i = 0; i < seen_n; ++i) {
    if (seen[i] == value) {
      return;
    }
  }
  if (seen_n < static_cast<int>(sizeof(seen) / sizeof(seen[0]))) {
    seen[seen_n++] = value;
    fprintf(stderr, "[VEXA][GL] %s 0x%x\n", what, value);
  }
}

static bool VexaIgnoreDesktopCap(GLenum cap) {
  return cap == 0x864F;  // GL_DEPTH_CLAMP is not in GLES
}

static void fexfn_impl_libGL_glEnable(GLenum cap) {
  if (g_vexa_trace_frame) fprintf(stderr, "[VEXA][TRACE] enable cap=%x\n", cap);
  if (VexaIgnoreDesktopCap(cap)) return;
  ::glEnable(cap);
  VexaGLEnumCompat("glEnable", cap);
}

static void fexfn_impl_libGL_glDisable(GLenum cap) {
  if (g_vexa_trace_frame) fprintf(stderr, "[VEXA][TRACE] disable cap=%x\n", cap);
  if (VexaIgnoreDesktopCap(cap)) return;
  ::glDisable(cap);
  VexaGLEnumCompat("glDisable", cap);
}

static void fexfn_impl_libGL_glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
  if (g_vexa_trace_frame) fprintf(stderr, "[VEXA][TRACE] texparamf target=%x pname=%x value=%g\n", target, pname, param);
  ::glTexParameterf(target, pname, param);
  VexaGLEnumCompat("glTexParameterf", pname);
}

static void fexfn_impl_libGL_glTexParameteri(GLenum target, GLenum pname, GLint param) {
  ::glTexParameteri(target, pname, param);
  VexaGLEnumCompat("glTexParameteri", pname);
}

static void VexaGL_glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param) {
  // GLES has no sampler LOD bias. The desktop default of zero changes no
  // sampling behaviour, so omit only that exact request. Nonzero bias still
  // needs shader emulation and must not be silently discarded.
  if (pname == 0x8501 && param == 0.0f) {
    static std::atomic<int> ignored {0};
    if (ignored.fetch_add(1) < 4)
      VEXA_LOGV("[VEXA][SAMPLER] zero LOD bias omitted sampler=%u\n", sampler);
    return;
  }
  static std::atomic<int> logs {0};
  if (logs.fetch_add(1) < 64)
    VEXA_LOGV("[VEXA][SAMPLER] sampler=%u pname=%x value=%g\n", sampler, pname, param);
  ::glSamplerParameterf(sampler, pname, param);
}

static void VexaDumpTbo(GLuint buffer) {
  if (access("/data/data/com.critical.vexaemulator/files/vexa_tbo_dump.flag", F_OK) != 0) return;
  if (!buffer) return;
  GLint prev = 0;
  ::glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &prev);
  ::glBindBuffer(GL_COPY_READ_BUFFER, buffer);
  GLint size = 0;
  ::glGetBufferParameteriv(GL_COPY_READ_BUFFER, GL_BUFFER_SIZE, &size);
  void* p = size > 0 ? ::glMapBufferRange(GL_COPY_READ_BUFFER, 0, size, GL_MAP_READ_BIT) : nullptr;
  if (p) {
    const float* f = static_cast<const float*>(p);
    const size_t nv = static_cast<size_t>(size) / 16;
    size_t zero_v = 0, nan_v = 0;
    for (size_t i = 0; i < nv; ++i) {
      const float x = f[i * 4], y = f[i * 4 + 1], z = f[i * 4 + 2], w = f[i * 4 + 3];
      if (x == 0.f && y == 0.f && z == 0.f && w == 0.f) ++zero_v;
      else if (!(x == x && y == y && z == z && w == w)) ++nan_v;
    }
    fprintf(stderr, "[VEXA][TBO] dump buffer=%u size=%d vec4=%zu zero=%zu nan=%zu\n",
            buffer, size, nv, zero_v, nan_v);
    char path[160];
    snprintf(path, sizeof(path),
             "/data/data/com.critical.vexaemulator/files/artifacts/ets2/tbo%u.bin", buffer);
    if (FILE* fp = fopen(path, "wb")) { fwrite(p, 1, size, fp); fclose(fp); }
    ::glUnmapBuffer(GL_COPY_READ_BUFFER);
  } else {
    fprintf(stderr, "[VEXA][TBO] dump buffer=%u size=%d map failed\n", buffer, size);
  }
  ::glBindBuffer(GL_COPY_READ_BUFFER, static_cast<GLuint>(prev));
}

static GLuint vexa_host_tbo[16];
static int vexa_host_tbo_n = 0;
static bool VexaHostIsTbo(GLuint b) {
  for (int i = 0; i < vexa_host_tbo_n; ++i)
    if (vexa_host_tbo[i] == b) return true;
  return false;
}
static bool VexaTboWriteLog() {
  static int en = -1;
  if (en < 0) en = access("/data/data/com.critical.vexaemulator/files/vexa_tbo_dump.flag", F_OK) == 0;
  return en != 0;
}
static GLint VexaHostBoundBuffer(GLenum target) {
  GLenum pname = 0;
  switch (target) {
    case GL_ARRAY_BUFFER: pname = GL_ARRAY_BUFFER_BINDING; break;
    case GL_ELEMENT_ARRAY_BUFFER: pname = GL_ELEMENT_ARRAY_BUFFER_BINDING; break;
    case GL_COPY_READ_BUFFER: pname = GL_COPY_READ_BUFFER_BINDING; break;
    case GL_COPY_WRITE_BUFFER: pname = GL_COPY_WRITE_BUFFER_BINDING; break;
    case GL_UNIFORM_BUFFER: pname = GL_UNIFORM_BUFFER_BINDING; break;
    case GL_TEXTURE_BUFFER: pname = 0x8C2A; break;
    case GL_PIXEL_UNPACK_BUFFER: pname = GL_PIXEL_UNPACK_BUFFER_BINDING; break;
    case GL_PIXEL_PACK_BUFFER: pname = GL_PIXEL_PACK_BUFFER_BINDING; break;
    case GL_TRANSFORM_FEEDBACK_BUFFER: pname = GL_TRANSFORM_FEEDBACK_BUFFER_BINDING; break;
    case GL_SHADER_STORAGE_BUFFER: pname = GL_SHADER_STORAGE_BUFFER_BINDING; break;
    case GL_ATOMIC_COUNTER_BUFFER: pname = GL_ATOMIC_COUNTER_BUFFER_BINDING; break;
    case GL_DRAW_INDIRECT_BUFFER: pname = GL_DRAW_INDIRECT_BUFFER_BINDING; break;
    case GL_DISPATCH_INDIRECT_BUFFER: pname = GL_DISPATCH_INDIRECT_BUFFER_BINDING; break;
    case 0x8F3E: pname = 0x9193; break; /* GL_PARAMETER_BUFFER/ARB query */
    default: pname = GL_ARRAY_BUFFER_BINDING; break;
  }
  GLint buf = 0;
  ::glGetIntegerv(pname, &buf);
  return buf;
}
static bool VexaTboBoundTarget(GLenum target, GLint* buf_out) {
  const GLint buf = VexaHostBoundBuffer(target);
  if (buf_out) *buf_out = buf;
  return buf > 0 && VexaHostIsTbo(static_cast<GLuint>(buf));
}

static void fexfn_impl_libGL_glBindBuffer(GLenum target, GLuint buffer) {
  ::glBindBuffer(target, buffer);
  if (VexaTboWriteLog() && VexaHostIsTbo(buffer))
    fprintf(stderr, "[libGL-host] tbobind target=%x buf=%u tid=%ld\n", target, buffer, (long)syscall(SYS_gettid));
}
static void fexfn_impl_libGL_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
  if (VexaTboWriteLog()) {
    GLint buf = 0;
    if (VexaTboBoundTarget(target, &buf))
      fprintf(stderr, "[libGL-host] tbodata target=%x buf=%d size=%lld usage=%x data=%p tid=%ld\n",
              target, buf, (long long)size, usage, data, (long)syscall(SYS_gettid));
  }
  ::glBufferData(target, size, data, usage);
}
static void fexfn_impl_libGL_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
  if (VexaTboWriteLog() && VexaTboBoundTarget(target, nullptr))
    fprintf(stderr, "[libGL-host] tbosub target=%x buf=%d off=%lld size=%lld tid=%ld\n",
            target, VexaHostBoundBuffer(target), (long long)offset, (long long)size, (long)syscall(SYS_gettid));
  ::glBufferSubData(target, offset, size, data);
}
static void fexfn_impl_libGL_glBufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
  if (VexaTboWriteLog())
    fprintf(stderr, "[libGL-host] bufstorage target=%x buf=%d size=%lld flags=%x data=%p tid=%ld\n",
            target, VexaHostBoundBuffer(target), (long long)size, flags, data, (long)syscall(SYS_gettid));
  ::glBufferStorage(target, size, data, flags);
}
#if !defined(IS_32BIT_THUNK)
static void fexfn_impl_libGL_glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
  if (VexaTboWriteLog())
    fprintf(stderr, "[libGL-host] bufflush target=%x buf=%d off=%lld len=%lld tid=%ld\n",
            target, VexaHostBoundBuffer(target), (long long)offset, (long long)length, (long)syscall(SYS_gettid));
  ::glFlushMappedBufferRange(target, offset, length);
}
static void* fexfn_impl_libGL_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
  void* p = ::glMapBufferRange(target, offset, length, access);
  if (VexaTboWriteLog())
    fprintf(stderr, "[libGL-host] bufmap target=%x buf=%d off=%lld len=%lld acc=%x tid=%ld ptr=%p\n",
            target, VexaHostBoundBuffer(target), (long long)offset, (long long)length, access, (long)syscall(SYS_gettid), p);
  return p;
}
static GLboolean fexfn_impl_libGL_glUnmapBuffer(GLenum target) {
  const GLint buf = VexaTboWriteLog() ? VexaHostBoundBuffer(target) : 0;
  if (buf)
    fprintf(stderr, "[libGL-host] bufunmap target=%x buf=%d tid=%ld\n",
            target, buf, (long)syscall(SYS_gettid));
  const GLboolean r = ::glUnmapBuffer(target);
  return r;
}
#endif

static void fexfn_impl_libGL_glTexBuffer(GLenum target, GLenum internalformat, GLuint buffer) {
  ::glTexBuffer(target, internalformat, buffer);
  if (buffer && !VexaHostIsTbo(buffer) && vexa_host_tbo_n < 16) vexa_host_tbo[vexa_host_tbo_n++] = buffer;
  if (VexaTboWriteLog()) VexaDumpTbo(buffer);
  if (VexaGLVerbose()) {
    GLenum err = ::glGetError();
    static unsigned logs = 0;
    if (err != GL_NO_ERROR || logs++ < 8)
      fprintf(stderr, "[VEXA][TBO] texbuffer target=%x fmt=%x buffer=%u err=0x%x\n", target, internalformat, buffer, err);
  }
}
static void fexfn_impl_libGL_glTexBufferRange(GLenum target, GLenum internalformat, GLuint buffer, GLintptr offset, GLsizeiptr size) {
  ::glTexBufferRange(target, internalformat, buffer, offset, size);
  if (VexaGLVerbose()) {
    GLenum err = ::glGetError();
    static unsigned logs = 0;
    if (err != GL_NO_ERROR || logs++ < 8) {
      GLint align = -1;
      ::glGetIntegerv(GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT, &align);
      fprintf(stderr, "[VEXA][TBO] texbufferrange target=%x fmt=%x buffer=%u off=%lld size=%lld align=%d err=0x%x\n",
              target, internalformat, buffer, (long long)offset, (long long)size, align, err);
    }
  }
}

static void fexfn_impl_libGL_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  VexaDiagDraw("arrays", mode, count);
  VexaPreDraw();
  ::glDrawArrays(mode, first, count);
  VexaPostDrawDiag();
}

static void fexfn_impl_libGL_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
  VexaDiagDraw("elements", mode, count);
  VexaPreDraw();
  ::glDrawElements(mode, count, type, indices);
  VexaPostDrawDiag();
}

static void fexfn_impl_libGL_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei primcount) {
  VexaDiagDraw("arrays_inst", mode, count * primcount);
  VexaPreDraw();
  ::glDrawArraysInstanced(mode, first, count, primcount);
  VexaPostDrawDiag();
}

static void fexfn_impl_libGL_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                     GLsizei primcount) {
  VexaDiagDraw("elements_inst", mode, count * primcount);
  VexaPreDraw();
  ::glDrawElementsInstanced(mode, count, type, indices, primcount);
  VexaPostDrawDiag();
}

static void fexfn_impl_libGL_glDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) {
  VexaDiagDraw("elements_indirect", mode, -1);
  VexaPreDraw();
  ::glDrawElementsIndirect(mode, type, indirect);
}

static GLenum VexaUnorm16Storage(GLenum format) {
  if (format != GL_R16 && format != GL_RG16 && format != GL_RGBA16) return format;
  const char* extensions = reinterpret_cast<const char*>(::glGetString(GL_EXTENSIONS));
  if (!extensions || std::strstr(extensions, "GL_EXT_texture_norm16") ||
      !std::strstr(extensions, "GL_EXT_color_buffer_float")) return format;
  return format == GL_R16 ? GL_R32F : format == GL_RG16 ? GL_RG32F : GL_RGBA32F;
}

static void fexfn_impl_libGL_glRenderbufferStorage(GLenum target, GLenum format, GLsizei width, GLsizei height) {
  // Desktop accepts unsized packed depth/stencil; GLES requires a sized format.
  if (format == GL_DEPTH_STENCIL) {
    fprintf(stderr, "[VEXA][GL] renderbuffer DEPTH_STENCIL -> DEPTH24_STENCIL8 %dx%d\n", width, height);
    format = GL_DEPTH24_STENCIL8;
  }
  ::glRenderbufferStorage(target, VexaUnorm16Storage(format), width, height);
}

static void fexfn_impl_libGL_glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum format, GLsizei width, GLsizei height) {
  if (format == GL_DEPTH_STENCIL) format = GL_DEPTH24_STENCIL8;
  ::glRenderbufferStorageMultisample(target, samples, VexaUnorm16Storage(format), width, height);
}

static void fexfn_impl_libGL_glDrawArraysIndirect(GLenum mode, const void* indirect) {
  VexaDiagDraw("arrays_indirect", mode, -1);
  VexaPreDraw();
  ::glDrawArraysIndirect(mode, indirect);
}

static void fexfn_impl_libGL_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                      GLint basevertex) {
  VexaDiagDraw("elements_basev", mode, count);
  VexaPreDraw();
  ::glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
  VexaPostDrawDiag();
}

static void fexfn_impl_libGL_glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type,
                                                               const void* indices, GLsizei primcount,
                                                               GLint basevertex) {
  VexaDiagDraw("elements_inst_basev", mode, count * primcount);
  VexaPreDraw();
  ::glDrawElementsInstancedBaseVertex(mode, count, type, indices, primcount, basevertex);
}

static void fexfn_impl_libGL_glDispatchCompute(GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z) {
  ++g_vexa_diag_dispatches;
  if (g_vexa_trace_frame) fprintf(stderr, "[VEXA][TRACE] dispatch groups=%u,%u,%u prog=%u\n", num_groups_x, num_groups_y, num_groups_z, g_vexa_diag_curprog);
  ::glDispatchCompute(num_groups_x, num_groups_y, num_groups_z);
}

static void fexfn_impl_libGL_glDispatchComputeIndirect(GLintptr indirect) {
  ++g_vexa_diag_dispatches;
  if (g_vexa_trace_frame) fprintf(stderr, "[VEXA][TRACE] dispatch_indirect offset=%lld prog=%u\n", (long long)indirect, g_vexa_diag_curprog);
  ::glDispatchComputeIndirect(indirect);
}

static uint64_t g_vexa_diag_barriers = 0;
static void fexfn_impl_libGL_glMemoryBarrier(GLbitfield barriers) {
  ++g_vexa_diag_barriers;
  if ((g_vexa_diag_barriers & 2047) == 0) {
    fprintf(stderr, "[VEXA][GL] barriers=%llu fbo=%u\n", (unsigned long long)g_vexa_diag_barriers,
            g_vexa_diag_curfbo);
  }
  ::glMemoryBarrier(barriers);
}

static void fexfn_impl_libGL_glClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value) {
  static int cb_logged = 0;
  if (cb_logged < 8) {
    ++cb_logged;
    fprintf(stderr, "[VEXA][GL] clearbuffer uiv buf=%x idx=%d fbo=%u\n", buffer, drawbuffer, g_vexa_diag_curfbo);
  }
  ::glClearBufferuiv(buffer, drawbuffer, value);
}

static void fexfn_impl_libGL_glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value) {
  static int cb_logged = 0;
  if (cb_logged < 8) {
    ++cb_logged;
    fprintf(stderr, "[VEXA][GL] clearbuffer fv buf=%x idx=%d v=%.2f,%.2f,%.2f,%.2f fbo=%u\n", buffer, drawbuffer,
            value ? value[0] : 0.f, value ? value[1] : 0.f, value ? value[2] : 0.f, value ? value[3] : 0.f,
            g_vexa_diag_curfbo);
  }
  ::glClearBufferfv(buffer, drawbuffer, value);
}

// Sync/query instrumentation: a stuck render thread usually shows up here.
static uint64_t g_vexa_diag_fences = 0;
static uint64_t g_vexa_diag_waits = 0;
static uint64_t g_vexa_diag_queries = 0;

static GLsync fexfn_impl_libGL_glFenceSync(GLenum condition, GLbitfield flags) {
  GLsync s = ::glFenceSync(condition, flags);
  ++g_vexa_diag_fences;
  if ((g_vexa_diag_fences & 63) == 0 || !s) {
    fprintf(stderr, "[VEXA][GL] fence n=%llu cond=%x -> %p err=%x\n",
            (unsigned long long)g_vexa_diag_fences, condition, s, glGetError());
  }
  return VexaExportSync(s);
}

static void fexfn_impl_libGL_glDeleteSync(GLsync sync) { ::glDeleteSync(VexaImportSync(sync, true)); }
#ifdef IS_32BIT_THUNK
static GLboolean fexfn_impl_libGL_glIsSync(GLsync sync) { return ::glIsSync(VexaImportSync(sync)); }
#endif

static void fexfn_impl_libGL_glBeginQuery(GLenum target, GLuint id) {
  ++g_vexa_diag_queries;
  ::glBeginQuery(target, id);
}

static void fexfn_impl_libGL_glEndQuery(GLenum target) { ::glEndQuery(target); }

static void fexfn_impl_libGL_glGenQueries(GLsizei n, GLuint* ids) { ::glGenQueries(n, ids); }

static void fexfn_impl_libGL_glGetInternalformativ(GLenum target, GLenum internalformat, GLenum pname, GLsizei count, GLint* params) {
  ::glGetInternalformativ(target, VexaUnorm16Storage(internalformat), pname, count, params);
  static unsigned logged;
  if (logged++ < 80 && params && count > 0) {
    fprintf(stderr, "[VEXA][FORMAT] target=%x fmt=%x pname=%x value=%d err=%x\n", target, internalformat, pname, params[0], ::glGetError());
  }
}

static void fexfn_impl_libGL_glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64* params) {
  ::glGetQueryObjectui64v(id, pname, params);
}

static void fexfn_impl_libGL_glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params) {
  ::glGetQueryObjectuiv(id, pname, params);
}

// ----- Bridges for desktop procs missing from the guest-visible surface -----
// Unity probes a wide proc surface at startup; several reflection/uniform/MSAA
// entry points were unresolved, which leaves shaders unusable or silently
// disables paths. Core GLES symbols are called directly; extension-only or
// GLES3.1+ entry points resolve lazily through eglGetProcAddress with a
// functional fallback where possible.

static void fexfn_impl_libGL_glGetUniformiv(GLuint program, GLint location, GLint* params) {
  ::glGetUniformiv(program, location, params);
}

static void fexfn_impl_libGL_glGetUniformIndices(GLuint program, GLsizei uniformCount, const GLchar* const* uniformNames, GLuint* uniformIndices) {
#ifdef IS_32BIT_THUNK
  auto names = VexaExpandStrings32(uniformCount, uniformNames);
  ::glGetUniformIndices(program, uniformCount, names.data(), uniformIndices);
#else
  ::glGetUniformIndices(program, uniformCount, uniformNames, uniformIndices);
#endif
}

static void fexfn_impl_libGL_glGetActiveUniformsiv(GLuint program, GLsizei uniformCount, const GLuint* uniformIndices, GLenum pname, GLint* params) {
  ::glGetActiveUniformsiv(program, uniformCount, uniformIndices, pname, params);
}

static void fexfn_impl_libGL_glGetActiveUniformBlockName(GLuint program, GLuint uniformBlockIndex, GLsizei bufSize, GLsizei* length, GLchar* uniformBlockName) {
  ::glGetActiveUniformBlockName(program, uniformBlockIndex, bufSize, length, uniformBlockName);
}

static void fexfn_impl_libGL_glGetQueryiv(GLenum target, GLenum pname, GLint* params) {
  ::glGetQueryiv(target, pname, params);
}

static void fexfn_impl_libGL_glValidateProgram(GLuint program) {
  ::glValidateProgram(program);
}

static void fexfn_impl_libGL_glGetShaderSource(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* source) {
  ::glGetShaderSource(shader, bufSize, length, source);
}

static void fexfn_impl_libGL_glGetShaderPrecisionFormat(GLenum shaderType, GLenum precisionType, GLint* range, GLint* precision) {
  ::glGetShaderPrecisionFormat(shaderType, precisionType, range, precision);
}

static void fexfn_impl_libGL_glClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
  ::glClearBufferfi(buffer, drawbuffer, depth, stencil);
}

// ---------------------------------------------------------------------------
// VEXA: software BCn fallback. Desktop builds (Unity standalone) upload
// S3TC/RGTC/BPTC textures; Mali GLES has none of them, so glTexStorage2D and
// glCompressedTex*Image* fail with GL_INVALID_ENUM and every texture samples
// black. Substitute an uncompressed storage format and decode on the host.
// ---------------------------------------------------------------------------
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

namespace {
enum class VexaBCKind { None, BC1, BC2, BC3, BC4, BC5, BC6H, BC7 };

struct VexaBCInfo {
  VexaBCKind kind = VexaBCKind::None;
  GLenum storage = 0;  // substituted sized internal format
  GLenum format = 0;   // upload format
  GLenum type = 0;     // upload type
  int bpp = 0;         // bytes per decoded pixel
  int block = 0;       // compressed bytes per 4x4 block
  bool is_signed = false;
  bool pack565 = false;  // opaque BC1: store RGB565 (BC1 endpoints are 565)
};

VexaBCInfo VexaBCLookup(GLenum f) {
  VexaBCInfo i;
  auto rgba = [&](VexaBCKind k, int block, bool srgb) {
    i.kind = k;
    i.storage = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    i.format = GL_RGBA;
    i.type = GL_UNSIGNED_BYTE;
    i.bpp = 4;
    i.block = block;
  };
  switch (f) {
  case 0x83F0:                                                         // DXT1 RGB
    i.kind = VexaBCKind::BC1; i.storage = GL_RGB565; i.format = GL_RGB; i.type = GL_UNSIGNED_SHORT_5_6_5;
    i.bpp = 2; i.block = 8; i.pack565 = true;
    break;
  case 0x83F1: rgba(VexaBCKind::BC1, 8, false); break;                // DXT1 RGBA
  case 0x8C4C: case 0x8C4D: rgba(VexaBCKind::BC1, 8, true); break;    // SRGB DXT1
  case 0x83F2: rgba(VexaBCKind::BC2, 16, false); break;               // DXT3
  case 0x8C4E: rgba(VexaBCKind::BC2, 16, true); break;
  case 0x83F3: rgba(VexaBCKind::BC3, 16, false); break;               // DXT5
  case 0x8C4F: rgba(VexaBCKind::BC3, 16, true); break;
  case 0x8E8C: rgba(VexaBCKind::BC7, 16, false); break;               // BPTC UNORM
  case 0x8E8D: rgba(VexaBCKind::BC7, 16, true); break;
  case 0x8DBB: case 0x8DBC:                                           // RGTC1 (BC4)
    i.kind = VexaBCKind::BC4; i.storage = GL_R8; i.format = GL_RED; i.type = GL_UNSIGNED_BYTE;
    i.bpp = 1; i.block = 8; i.is_signed = (f == 0x8DBC);
    break;
  case 0x8DBD: case 0x8DBE:                                           // RGTC2 (BC5)
    i.kind = VexaBCKind::BC5; i.storage = GL_RG8; i.format = GL_RG; i.type = GL_UNSIGNED_BYTE;
    i.bpp = 2; i.block = 16; i.is_signed = (f == 0x8DBE);
    break;
  case 0x8E8E: case 0x8E8F:                                           // BPTC float (BC6H)
    i.kind = VexaBCKind::BC6H; i.storage = GL_RGB16F; i.format = GL_RGB; i.type = GL_HALF_FLOAT;
    i.bpp = 6; i.block = 16; i.is_signed = (f == 0x8E8E);
    break;
  default: break;
  }
  return i;
}

// Decode a w x h region of BC blocks into tightly packed pixels. The client
// data may itself be a sub-rectangle of a wider image: GL_UNPACK_ROW_LENGTH
// then gives the stored image's width in texels (desktop GL honours it for
// compressed uploads too; we must or the block indices skew into chevrons).
bool VexaBCDecode(const VexaBCInfo& bc, const uint8_t* src, size_t src_size, GLsizei w, GLsizei h,
                  std::vector<uint8_t>& out) {
  if (!src || w <= 0 || h <= 0) {
    return false;
  }
  GLint row_length = 0;
  ::glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
  const int bw = (w + 3) / 4;
  const int bh = (h + 3) / 4;
  // row_length > w: the client data is stored as a wider image and the
  // driver reads it with that stride regardless of imageSize (ETS2 uploads
  // mip levels out of a base-width buffer). The caller must guarantee that
  // many readable bytes; the size check below only covers the tight extent.
  const int stored_bw = row_length > w ? (row_length + 3) / 4 : bw;
  if (src_size < static_cast<size_t>(bw) * bh * bc.block) {
    return false;
  }
  // Every texel of the w x h region is written below; no zero fill needed.
  out.resize(static_cast<size_t>(w) * h * bc.bpp);
  uint8_t tmp[4 * 4 * 6];
  const int tpitch = 4 * (bc.pack565 ? 4 : bc.bpp);
  for (int by = 0; by < bh; ++by) {
    for (int bx = 0; bx < bw; ++bx) {
      const uint8_t* blk = src + (static_cast<size_t>(by) * stored_bw + bx) * bc.block;
      switch (bc.kind) {
      case VexaBCKind::BC1: bcdec_bc1(blk, tmp, tpitch); break;
      case VexaBCKind::BC2: bcdec_bc2(blk, tmp, tpitch); break;
      case VexaBCKind::BC3: bcdec_bc3(blk, tmp, tpitch); break;
      case VexaBCKind::BC4: bcdec_bc4(blk, tmp, tpitch); break;
      case VexaBCKind::BC5: bcdec_bc5(blk, tmp, tpitch); break;
      case VexaBCKind::BC6H: bcdec_bc6h_half(blk, tmp, 4 * 3, bc.is_signed ? 1 : 0); break;
      case VexaBCKind::BC7: bcdec_bc7(blk, tmp, tpitch); break;
      default: return false;
      }
      for (int y = 0; y < 4; ++y) {
        const int py = by * 4 + y;
        if (py >= h) {
          break;
        }
        const int cols = (bx * 4 + 4 <= w) ? 4 : (w - bx * 4);
        if (bc.pack565) {
          uint16_t* dst = reinterpret_cast<uint16_t*>(out.data()) + static_cast<size_t>(py) * w + bx * 4;
          const uint8_t* p = tmp + y * tpitch;
          for (int x = 0; x < cols; ++x, p += 4) {
            dst[x] = static_cast<uint16_t>(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
          }
          continue;
        }
        std::memcpy(out.data() + (static_cast<size_t>(py) * w + bx * 4) * bc.bpp, tmp + y * tpitch,
                    static_cast<size_t>(cols) * bc.bpp);
      }
    }
  }
  return true;
}

// Bytes to map/copy for a BC upload: max(imageSize, the stored-layout span
// implied by GL_UNPACK_ROW_LENGTH). The app may under-report imageSize while
// the driver still strides through the wider client image.
GLsizei VexaBCFetchSize(const VexaBCInfo& bc, GLsizei w, GLsizei h, GLsizei imageSize) {
  GLint rl = 0;
  ::glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rl);
  if (rl <= w) return imageSize;
  const size_t need = static_cast<size_t>((rl + 3) / 4) * ((h + 3) / 4) * bc.block;
  GLint pbo = 0;
  ::glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
  if (pbo) {
    GLint64 bufsize = 0;
    ::glGetBufferParameteri64v(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &bufsize);
    const auto offset = 0; // offset folded in caller; clamp to buffer
    if (bufsize > 0 && need > static_cast<size_t>(bufsize)) {
      return static_cast<GLsizei>(bufsize) > imageSize ? static_cast<GLsizei>(bufsize) : imageSize;
    }
    (void)offset;
  }
  return need > static_cast<size_t>(imageSize) ? static_cast<GLsizei>(need) : imageSize;
}

// Resolves the compressed source, honouring a bound GL_PIXEL_UNPACK_BUFFER.
struct VexaBCSource {
  const uint8_t* ptr = nullptr;
  GLint pbo = 0;
  bool mapped = false;

  VexaBCSource(const void* data, GLsizei size) {
    ::glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    if (pbo == 0) {
      ptr = static_cast<const uint8_t*>(data);
      return;
    }
    const auto offset = reinterpret_cast<GLintptr>(data);
    ptr = static_cast<const uint8_t*>(::glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, offset, size, GL_MAP_READ_BIT));
    mapped = ptr != nullptr;
  }
  ~VexaBCSource() {
    if (mapped) {
      ::glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    }
  }
};

// Tight unpack state for the decoded upload; restores the guest's state after.
struct VexaUnpackScope {
  GLint align = 4, row = 0, skip_px = 0, skip_rows = 0, img_h = 0, skip_img = 0, pbo = 0;
  VexaUnpackScope() {
    ::glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    ::glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
    ::glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_px);
    ::glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
    ::glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &img_h);
    ::glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &skip_img);
    ::glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    ::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    ::glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    ::glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    ::glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    ::glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    ::glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
    if (pbo) {
      ::glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
  }
  ~VexaUnpackScope() {
    ::glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    ::glPixelStorei(GL_UNPACK_ROW_LENGTH, row);
    ::glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_px);
    ::glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
    ::glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, img_h);
    ::glPixelStorei(GL_UNPACK_SKIP_IMAGES, skip_img);
    if (pbo) {
      ::glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    }
  }
};

void VexaBCLogOnce(const char* where, GLenum fmt) {
  if (!VexaGLVerbose()) return;
  static std::atomic<int> logged {0};
  if (logged.fetch_add(1) < 16) {
    VEXA_LOGV("[VEXA][BCN] %s fmt=0x%x -> software decode\n", where, fmt);
  }
}

// Debug: dump the just-uploaded level of the bound 2D texture to a PPM so a
// corrupted decode/upload can be spotted by eye. Enabled by vexa_tex_dump.flag.
void VexaDumpTexture(GLenum target, GLint level, const char* tag) {
  if (target != GL_TEXTURE_2D) return;
  static const bool enabled = access("/data/data/com.critical.vexaemulator/files/vexa_tex_dump.flag", F_OK) == 0;
  if (!enabled) return;
  static int dumped = 0;
  if (dumped >= 300) return;
  GLint tex = 0;
  ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
  if (!tex) return;
  GLint w = 0, h = 0;
  ::glGetTexLevelParameteriv(target, level, GL_TEXTURE_WIDTH, &w);
  ::glGetTexLevelParameteriv(target, level, GL_TEXTURE_HEIGHT, &h);
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
  GLint prev = 0;
  ::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
  GLuint fbo = 0;
  ::glGenFramebuffers(1, &fbo);
  ::glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  ::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, (GLuint)tex, level);
  if (::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
    GLint rfmt = 0, rtype = 0;
    ::glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &rfmt);
    ::glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &rtype);
    if (rfmt == GL_RGBA && rtype == GL_UNSIGNED_BYTE) {
      std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
      ::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      char path[256];
      snprintf(path, sizeof(path),
               "/data/data/com.critical.vexaemulator/files/artifacts/ets2/tex%03d_t%u_l%d_%s.ppm",
               dumped, (unsigned)tex, (int)level, tag);
      if (FILE* f = fopen(path, "wb")) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int yy = h - 1; yy >= 0; --yy) {
          for (int xx = 0; xx < w; ++xx) {
            const uint8_t* p = px.data() + (static_cast<size_t>(yy) * w + xx) * 4;
            fputc(p[0], f);
            fputc(p[1], f);
            fputc(p[2], f);
          }
        }
        fclose(f);
      }
      char apath[256];
      snprintf(apath, sizeof(apath), "%s.alpha.ppm", path);
      if (FILE* f = fopen(apath, "wb")) {
        fprintf(f, "P5\n%d %d\n255\n", w, h);
        for (int yy = h - 1; yy >= 0; --yy) {
          for (int xx = 0; xx < w; ++xx) {
            const uint8_t* p = px.data() + (static_cast<size_t>(yy) * w + xx) * 4;
            fputc(p[3], f);
          }
        }
        fclose(f);
      }
      ++dumped;
    } else {
      fprintf(stderr, "[VEXA][TEXDUMP] skip tex=%u rfmt=0x%x rtype=0x%x\n", (unsigned)tex,
              (unsigned)rfmt, (unsigned)rtype);
    }
  }
  ::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, 0, 0);
  ::glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
  ::glDeleteFramebuffers(1, &fbo);
}
} // namespace

#include "unorm16_texture.inc"

static void VexaGL_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {
  static const bool dump = access("/data/data/com.critical.vexaemulator/files/unity_shader_dump.flag", F_OK) == 0;
  if (dump) {
    static std::vector<GLenum> seen;
    if (std::find(seen.begin(), seen.end(), internalformat) == seen.end()) {
      seen.push_back(internalformat);
      fprintf(stderr, "[VEXA][STORAGE] target=%x format=%x levels=%d size=%dx%d\n", target, internalformat, levels, width, height);
    }
  }
  const VexaBCInfo bc = VexaBCLookup(internalformat);
  if (bc.kind != VexaBCKind::None) {
    VexaBCLogOnce("glTexStorage2D", internalformat);
    internalformat = bc.storage;
  }
  VexaNoteUnorm16Storage(target, internalformat);
  const GLenum storage = target == GL_TEXTURE_2D ? VexaUnorm16Storage(internalformat) : internalformat;
  if (storage != internalformat) fprintf(stderr, "[VEXA][UNORM16] storage %x -> %x %dx%d\n", internalformat, storage, width, height);
  ::glTexStorage2D(target, levels, storage, width, height);
}

static void VexaGL_glTexStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height,
                                  GLsizei depth) {
  const VexaBCInfo bc = VexaBCLookup(internalformat);
  if (bc.kind != VexaBCKind::None) {
    VexaBCLogOnce("glTexStorage3D", internalformat);
    internalformat = bc.storage;
  }
  ::glTexStorage3D(target, levels, internalformat, width, height, depth);
}

static void VexaGL_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                          GLsizei height, GLint border, GLsizei imageSize, const void* data) {
  const VexaBCInfo bc = VexaBCLookup(internalformat);
  if (bc.kind == VexaBCKind::None) {
    ::glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
    return;
  }
  VexaBCLogOnce("glCompressedTexImage2D", internalformat);
  static thread_local std::vector<uint8_t> px;
  bool ok = false;
  {
    const GLsizei fetch = VexaBCFetchSize(bc, width, height, imageSize);
    VexaBCSource src(data, fetch);
    ok = VexaBCDecode(bc, src.ptr, static_cast<size_t>(imageSize), width, height, px);
  }
  VexaUnpackScope unpack;
  ::glTexImage2D(target, level, bc.storage, width, height, 0, bc.format, bc.type, ok ? px.data() : nullptr);
  VexaDumpTexture(target, level, "cimg");
}

static void VexaGL_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                             GLsizei height, GLenum format, GLsizei imageSize, const void* data) {
  const VexaBCInfo bc = VexaBCLookup(format);
  if (bc.kind == VexaBCKind::None) {
    ::glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, imageSize, data);
    return;
  }
  VexaBCLogOnce("glCompressedTexSubImage2D", format);
  {
    static int slog = 0;
    GLint rl = 0, sx = 0, sy = 0;
    if (VexaGLVerbose()) {
      ::glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rl);
      ::glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &sx);
      ::glGetIntegerv(GL_UNPACK_SKIP_ROWS, &sy);
    }
    if (VexaGLVerbose() && slog++ < 60) {
      const size_t tight = ((width + 3) / 4) * ((height + 3) / 4) * bc.block;
      const size_t stored = ((rl > 0 ? rl : width) + 3) / 4 * ((height + 3) / 4) * bc.block;
      fprintf(stderr,
              "[VEXA][BCN] csub rowlen=%d skip=%d,%d off=%d,%d %dx%d imageSize=%d tight=%zu stored=%zu\n",
              (int)rl, (int)sx, (int)sy, (int)xoffset, (int)yoffset, (int)width, (int)height,
              (int)imageSize, tight, stored);
    }
  }
  static thread_local std::vector<uint8_t> px;
  bool ok = false;
  {
    const GLsizei fetch = VexaBCFetchSize(bc, width, height, imageSize);
    VexaBCSource src(data, fetch);
    ok = VexaBCDecode(bc, src.ptr, static_cast<size_t>(imageSize), width, height, px);
  }
  if (!ok) {
    return;
  }
  VexaUnpackScope unpack;
  ::glTexSubImage2D(target, level, xoffset, yoffset, width, height, bc.format, bc.type, px.data());
  VexaDumpTexture(target, level, "csub");
}

static void VexaGL_glCompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                          GLsizei height, GLsizei depth, GLint border, GLsizei imageSize,
                                          const void* data) {
  const VexaBCInfo bc = VexaBCLookup(internalformat);
  if (bc.kind == VexaBCKind::None) {
    ::glCompressedTexImage3D(target, level, internalformat, width, height, depth, border, imageSize, data);
    return;
  }
  VexaBCLogOnce("glCompressedTexImage3D", internalformat);
  static thread_local std::vector<uint8_t> all;
  all.clear();
  bool ok = depth > 0;
  {
    VexaBCSource src(data, imageSize);
    const size_t slice = static_cast<size_t>((width + 3) / 4) * ((height + 3) / 4) * bc.block;
    static thread_local std::vector<uint8_t> px;
    for (GLsizei z = 0; ok && z < depth; ++z) {
      ok = src.ptr && slice * (z + 1) <= static_cast<size_t>(imageSize) &&
           VexaBCDecode(bc, src.ptr + slice * z, slice, width, height, px);
      if (ok) {
        all.insert(all.end(), px.begin(), px.end());
      }
    }
  }
  VexaUnpackScope unpack;
  ::glTexImage3D(target, level, bc.storage, width, height, depth, 0, bc.format, bc.type, ok ? all.data() : nullptr);
}

static void VexaGL_glCompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                             GLsizei width, GLsizei height, GLsizei depth, GLenum format,
                                             GLsizei imageSize, const void* data) {
  const VexaBCInfo bc = VexaBCLookup(format);
  if (bc.kind == VexaBCKind::None) {
    ::glCompressedTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, imageSize, data);
    return;
  }
  VexaBCLogOnce("glCompressedTexSubImage3D", format);
  static thread_local std::vector<uint8_t> all;
  all.clear();
  bool ok = depth > 0;
  {
    VexaBCSource src(data, imageSize);
    const size_t slice = static_cast<size_t>((width + 3) / 4) * ((height + 3) / 4) * bc.block;
    static thread_local std::vector<uint8_t> px;
    for (GLsizei z = 0; ok && z < depth; ++z) {
      ok = src.ptr && slice * (z + 1) <= static_cast<size_t>(imageSize) &&
           VexaBCDecode(bc, src.ptr + slice * z, slice, width, height, px);
      if (ok) {
        all.insert(all.end(), px.begin(), px.end());
      }
    }
  }
  if (!ok) {
    return;
  }
  VexaUnpackScope unpack;
  ::glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, bc.format, bc.type, all.data());
}

static void fexfn_impl_libGL_glCompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void* data) {
  VexaGL_glCompressedTexImage3D(target, level, internalformat, width, height, depth, border, imageSize, data);
}

static void fexfn_impl_libGL_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void* data) {
  VexaGL_glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, imageSize, data);
}

static void fexfn_impl_libGL_glCompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void* data) {
  VexaGL_glCompressedTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, imageSize, data);
}

using VexaProgramBinaryFn = void (*)(GLuint, GLenum, const void*, GLsizei);
static void fexfn_impl_libGL_glProgramBinary(GLuint program, GLenum binaryFormat, const void* binary, GLsizei length) {
  VEXA_PROGRAM_LOCK();
  static auto fn = reinterpret_cast<VexaProgramBinaryFn>(eglGetProcAddress("glProgramBinary"));
  static auto fn_oes = reinterpret_cast<VexaProgramBinaryFn>(eglGetProcAddress("glProgramBinaryOES"));
  if (fn) fn(program, binaryFormat, binary, length);
  else if (fn_oes) fn_oes(program, binaryFormat, binary, length);
  GLint linked = GL_FALSE;
  ::glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked) {
    g_vexa_prog_sources.erase(program);
    g_vexa_prog_direct_source.erase(program);
    g_vexa_linked_programs.erase(program);
    ++VexaProgramContext().Shared->Generation;
    VexaRefreshProgramGeneration();
  }
}

using VexaGetProgramBinaryFn = void (*)(GLuint, GLsizei, GLsizei*, GLenum*, void*);
static void fexfn_impl_libGL_glGetProgramBinary(GLuint program, GLsizei bufSize, GLsizei* length, GLenum* binaryFormat, void* binary) {
  static auto fn = reinterpret_cast<VexaGetProgramBinaryFn>(eglGetProcAddress("glGetProgramBinary"));
  static auto fn_oes = reinterpret_cast<VexaGetProgramBinaryFn>(eglGetProcAddress("glGetProgramBinaryOES"));
  if (fn) { fn(program, bufSize, length, binaryFormat, binary); return; }
  if (fn_oes) { fn_oes(program, bufSize, length, binaryFormat, binary); return; }
  if (length) *length = 0;
  if (binaryFormat) *binaryFormat = 0;
}

#define VEXA_IMPL_PROG_UNIFORM_V(uname, T) \
static void fexfn_impl_libGL_glProgramUniform##uname(GLuint program, GLint location, GLsizei count, const T* value) { \
  VEXA_PROGRAM_LOCK(); \
  using Fn = void (*)(GLuint, GLint, GLsizei, const T*); \
  static Fn fn = reinterpret_cast<Fn>(eglGetProcAddress("glProgramUniform" #uname)); \
  if (fn) fn(program, location, count, value); \
  if (const auto it = g_vexa_clones_of_prog.find(program); it != g_vexa_clones_of_prog.end()) { \
    for (GLuint c : it->second) { \
      const auto li = g_vexa_clone_locs[c].find(location); \
      if (li != g_vexa_clone_locs[c].end() && li->second >= 0 && fn) fn(c, li->second, count, value); \
    } \
  } \
}
VEXA_IMPL_PROG_UNIFORM_V(1iv, GLint)
VEXA_IMPL_PROG_UNIFORM_V(1fv, GLfloat)
VEXA_IMPL_PROG_UNIFORM_V(1uiv, GLuint)
VEXA_IMPL_PROG_UNIFORM_V(2iv, GLint)
VEXA_IMPL_PROG_UNIFORM_V(2fv, GLfloat)
VEXA_IMPL_PROG_UNIFORM_V(2uiv, GLuint)
VEXA_IMPL_PROG_UNIFORM_V(3iv, GLint)
VEXA_IMPL_PROG_UNIFORM_V(3fv, GLfloat)
VEXA_IMPL_PROG_UNIFORM_V(3uiv, GLuint)
VEXA_IMPL_PROG_UNIFORM_V(4iv, GLint)
VEXA_IMPL_PROG_UNIFORM_V(4fv, GLfloat)
VEXA_IMPL_PROG_UNIFORM_V(4uiv, GLuint)
#undef VEXA_IMPL_PROG_UNIFORM_V

#define VEXA_IMPL_PROG_UNIFORM_M(uname) \
static void fexfn_impl_libGL_glProgramUniform##uname(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) { \
  VEXA_PROGRAM_LOCK(); \
  using Fn = void (*)(GLuint, GLint, GLsizei, GLboolean, const GLfloat*); \
  static Fn fn = reinterpret_cast<Fn>(eglGetProcAddress("glProgramUniform" #uname)); \
  if (fn) fn(program, location, count, transpose, value); \
  if (const auto it = g_vexa_clones_of_prog.find(program); it != g_vexa_clones_of_prog.end()) { \
    for (GLuint c : it->second) { \
      const auto li = g_vexa_clone_locs[c].find(location); \
      if (li != g_vexa_clone_locs[c].end() && li->second >= 0 && fn) fn(c, li->second, count, transpose, value); \
    } \
  } \
}
VEXA_IMPL_PROG_UNIFORM_M(Matrix2fv)
VEXA_IMPL_PROG_UNIFORM_M(Matrix3fv)
VEXA_IMPL_PROG_UNIFORM_M(Matrix4fv)
VEXA_IMPL_PROG_UNIFORM_M(Matrix2x3fv)
VEXA_IMPL_PROG_UNIFORM_M(Matrix3x2fv)
VEXA_IMPL_PROG_UNIFORM_M(Matrix2x4fv)
VEXA_IMPL_PROG_UNIFORM_M(Matrix4x2fv)
VEXA_IMPL_PROG_UNIFORM_M(Matrix3x4fv)
VEXA_IMPL_PROG_UNIFORM_M(Matrix4x3fv)
#undef VEXA_IMPL_PROG_UNIFORM_M

// Desktop glTexImage2DMultisample maps 1:1 onto GLES glTexStorage2DMultisample.
using VexaTexStorage2DMSFn = void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean);
static void fexfn_impl_libGL_glTexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations) {
  static auto fn = reinterpret_cast<VexaTexStorage2DMSFn>(eglGetProcAddress("glTexStorage2DMultisample"));
  if (fn) fn(target, samples, internalformat, width, height, fixedsamplelocations);
}

using VexaTexStorage3DMSFn = void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei, GLboolean);
static void fexfn_impl_libGL_glTexStorage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedsamplelocations) {
  static auto fn = reinterpret_cast<VexaTexStorage3DMSFn>(eglGetProcAddress("glTexStorage3DMultisample"));
  if (fn) fn(target, samples, internalformat, width, height, depth, fixedsamplelocations);
}

// GLES lacks DSA texture getters: emulate by (re)binding the texture to its
// most likely target (2D) and reading back through glGetTexParameteriv.
static void VexaGetTexParamEmulated(GLuint texture, GLint level, GLenum pname, GLint* params) {
  GLint prev = 0;
  ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
  if (prev != (GLint)texture) {
    ::glBindTexture(GL_TEXTURE_2D, texture);
  }
  if (level >= 0) {
    ::glGetTexLevelParameteriv(GL_TEXTURE_2D, level, pname, params);
  } else {
    ::glGetTexParameteriv(GL_TEXTURE_2D, pname, params);
  }
  if (prev != (GLint)texture) {
    ::glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev));
  }
}

static void fexfn_impl_libGL_glGetTextureParameteriv(GLuint texture, GLenum pname, GLint* params) {
  using Fn = void (*)(GLuint, GLenum, GLint*);
  static Fn fn = reinterpret_cast<Fn>(eglGetProcAddress("glGetTextureParameteriv"));
  if (fn) { fn(texture, pname, params); return; }
  VexaGetTexParamEmulated(texture, -1, pname, params);
}

static void fexfn_impl_libGL_glGetTextureLevelParameteriv(GLuint texture, GLint level, GLenum pname, GLint* params) {
  using Fn = void (*)(GLuint, GLint, GLenum, GLint*);
  static Fn fn = reinterpret_cast<Fn>(eglGetProcAddress("glGetTextureLevelParameteriv"));
  if (fn) { fn(texture, level, pname, params); return; }
  VexaGetTexParamEmulated(texture, level, pname, params);
}

using VexaMapBufferRangeFn = void* (*)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
static void* fexfn_impl_libGL_glMapBufferOES(GLenum target, GLenum access) {
#ifdef IS_32BIT_THUNK
  GLint length = 0;
  ::glGetBufferParameteriv(target, GL_BUFFER_SIZE, &length);
  GLbitfield flags = access == GL_READ_ONLY ? GL_MAP_READ_BIT :
                    access == GL_WRITE_ONLY ? GL_MAP_WRITE_BIT :
                    access == GL_READ_WRITE ? GL_MAP_READ_BIT | GL_MAP_WRITE_BIT : 0;
  return fexfn_impl_libGL_glMapBufferRange(target, 0, length, flags);
#else

  static auto fn = reinterpret_cast<VexaMapBufferRangeFn>(eglGetProcAddress("glMapBufferRange"));
  if (!fn) {
    return nullptr;
  }
  GLint size = 0;
  ::glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
  if (size <= 0) {
    return nullptr;
  }
  GLbitfield bits = 0;
  switch (access) {
    case 0x88B8 /* GL_READ_ONLY */:  bits = GL_MAP_READ_BIT; break;
    case 0x88B9 /* GL_WRITE_ONLY */: bits = GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT; break;
    default:                        bits = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT; break;
  }
  return fn(target, 0, size, bits);
#endif
}

using VexaBlendBarrierFn = void (*)(void);
static void fexfn_impl_libGL_glBlendBarrierKHR(void) {
  static auto fn = reinterpret_cast<VexaBlendBarrierFn>(eglGetProcAddress("glBlendBarrierKHR"));
  if (fn) fn();
}

using VexaBlendEqSepIFn = void (*)(GLuint, GLenum, GLenum);
static void fexfn_impl_libGL_glBlendEquationSeparatei(GLuint buf, GLenum modeRGB, GLenum modeAlpha) {
  static auto fn = reinterpret_cast<VexaBlendEqSepIFn>(eglGetProcAddress("glBlendEquationSeparatei"));
  static auto fn_oes = reinterpret_cast<VexaBlendEqSepIFn>(eglGetProcAddress("glBlendEquationSeparateiOES"));
  if (fn) { fn(buf, modeRGB, modeAlpha); return; }
  if (fn_oes) { fn_oes(buf, modeRGB, modeAlpha); return; }
  ::glBlendEquationSeparate(modeRGB, modeAlpha);
}

// KHR_debug callbacks cannot cross the host boundary (guest function pointer);
// accepting and dropping them keeps the proc resolvable without crashing.
static void fexfn_impl_libGL_glDebugMessageCallback(guest_layout<GLDEBUGPROC> callback, const void* userParam) {
  (void)callback;
  (void)userParam;
}

static void fexfn_impl_libGL_glDebugMessageControl(GLenum source, GLenum type, GLenum severity, GLsizei count, const GLuint* ids, GLboolean enabled) {
  using Fn = void (*)(GLenum, GLenum, GLenum, GLsizei, const GLuint*, GLboolean);
  static Fn fn = reinterpret_cast<Fn>(eglGetProcAddress("glDebugMessageControl"));
  if (fn) fn(source, type, severity, count, ids, enabled);
}

static void fexfn_impl_libGL_glDebugMessageInsert(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* buf) {
  using Fn = void (*)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar*);
  static Fn fn = reinterpret_cast<Fn>(eglGetProcAddress("glDebugMessageInsert"));
  if (fn) fn(source, type, id, severity, length, buf);
}

static void fexfn_impl_libGL_glGetObjectLabel(GLenum identifier, GLuint name, GLsizei bufSize, GLsizei* length, GLchar* label) {
  using Fn = void (*)(GLenum, GLuint, GLsizei, GLsizei*, GLchar*);
  static Fn fn = reinterpret_cast<Fn>(eglGetProcAddress("glGetObjectLabel"));
  if (fn) { fn(identifier, name, bufSize, length, label); return; }
  if (length) *length = 0;
  if (label && bufSize > 0) label[0] = 0;
}

static void fexfn_impl_libGL_glGetObjectLabelEXT(GLenum type, GLuint object, GLsizei bufSize, GLsizei* length, GLchar* label) {
  using Fn = void (*)(GLenum, GLuint, GLsizei, GLsizei*, GLchar*);
  static Fn fn = reinterpret_cast<Fn>(eglGetProcAddress("glGetObjectLabelEXT"));
  if (fn) { fn(type, object, bufSize, length, label); return; }
  if (length) *length = 0;
  if (label && bufSize > 0) label[0] = 0;
}

static void fexfn_impl_libGL_glLabelObjectEXT(GLenum type, GLuint object, GLsizei length, const GLchar* label) {
  using Fn = void (*)(GLenum, GLuint, GLsizei, const GLchar*);
  static Fn fn = reinterpret_cast<Fn>(eglGetProcAddress("glLabelObjectEXT"));
  if (fn) fn(type, object, length, label);
}

using VexaFBTexture2DMSFn = void (*)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
static void fexfn_impl_libGL_glFramebufferTexture2DMultisampleEXT(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLsizei samples) {
  static auto fn = reinterpret_cast<VexaFBTexture2DMSFn>(eglGetProcAddress("glFramebufferTexture2DMultisampleEXT"));
  if (fn) { fn(target, attachment, textarget, texture, level, samples); return; }
  ::glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

using VexaEGLImageTargetFn = void (*)(GLenum, void*);
static void fexfn_impl_libGL_glEGLImageTargetTexture2DOES(GLenum target, void* image) {
  static auto fn = reinterpret_cast<VexaEGLImageTargetFn>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (fn) fn(target, image);
}

// APPLE multisample resolve: on EXT_multisampled_render_to_texture the resolve
// happens implicitly at framebuffer write-back, so a no-op is correct there.
static void fexfn_impl_libGL_glResolveMultisampleFramebufferAPPLE(void) {}

// ----- Extended draw entry points (desktop names -> GLES equivalents) -----
static void fexfn_impl_libGL_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
  (void)start; (void)end;
  VexaDiagDraw("range_elements", mode, count);
  VexaPreDraw();
  ::glDrawElements(mode, count, type, indices);
  VexaPostDrawDiag();
}

static void fexfn_impl_libGL_glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
  (void)start; (void)end;
  VexaDiagDraw("range_elements_bv", mode, count);
  VexaPreDraw();
  ::glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
  VexaPostDrawDiag();
}

// Mali has no GL_EXT_base_instance. Instanced attribute streams read element
// floor(instance / divisor) + baseinstance, so shift every divisor > 0 vertex
// binding by baseinstance * stride, draw, and restore. Runs natively on the
// host: one guest transition per draw instead of dozens of state queries.
struct VexaSavedVB { GLuint index; GLuint buffer; GLintptr offset; GLint stride; };
static int VexaApplyBaseInstance(GLuint baseinstance, VexaSavedVB* saved, int max_saved) {
  if (!baseinstance) return 0;
  static GLint max_attribs = 0;
  if (max_attribs <= 0) {
    ::glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &max_attribs);
    if (max_attribs <= 0 || max_attribs > 32) max_attribs = 16;
  }
  uint32_t seen = 0;
  int n = 0;
  for (GLint a = 0; a < max_attribs && n < max_saved; ++a) {
    GLint enabled = 0;
    ::glGetVertexAttribiv(static_cast<GLuint>(a), GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
    if (!enabled) continue;
    GLint binding = 0;
    ::glGetVertexAttribiv(static_cast<GLuint>(a), GL_VERTEX_ATTRIB_BINDING, &binding);
    if (binding < 0 || binding >= 32 || (seen & (1u << binding))) continue;
    seen |= 1u << binding;
    GLint divisor = 0;
    ::glGetIntegeri_v(GL_VERTEX_BINDING_DIVISOR, static_cast<GLuint>(binding), &divisor);
    if (divisor <= 0) continue;
    GLint buffer = 0, stride = 0;
    GLint64 offset = 0;
    ::glGetIntegeri_v(GL_VERTEX_BINDING_BUFFER, static_cast<GLuint>(binding), &buffer);
    ::glGetIntegeri_v(GL_VERTEX_BINDING_STRIDE, static_cast<GLuint>(binding), &stride);
    ::glGetInteger64i_v(GL_VERTEX_BINDING_OFFSET, static_cast<GLuint>(binding), &offset);
    if (!buffer || stride <= 0) continue;
    saved[n++] = {static_cast<GLuint>(binding), static_cast<GLuint>(buffer), static_cast<GLintptr>(offset), stride};
    ::glBindVertexBuffer(static_cast<GLuint>(binding), static_cast<GLuint>(buffer),
                         static_cast<GLintptr>(offset) + static_cast<GLintptr>(baseinstance) * stride, stride);
  }
  return n;
}
static void VexaRestoreBaseInstance(const VexaSavedVB* saved, int n) {
  for (int k = 0; k < n; ++k) ::glBindVertexBuffer(saved[k].index, saved[k].buffer, saved[k].offset, saved[k].stride);
}

using VexaDrawArraysInstBIFn = void (*)(GLenum, GLint, GLsizei, GLsizei, GLuint);
static void fexfn_impl_libGL_glDrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei primcount, GLuint baseinstance) {
  VexaDiagDraw("arrays_inst_bi", mode, count * primcount);
  VexaPreDraw();
  VexaSavedVB saved[32];
  const int n = VexaApplyBaseInstance(baseinstance, saved, 32);
  ::glDrawArraysInstanced(mode, first, count, primcount);
  VexaRestoreBaseInstance(saved, n);
  VexaPostDrawDiag();
}

using VexaDrawElemsInstBIFn = void (*)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLuint);
static void fexfn_impl_libGL_glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei primcount, GLuint baseinstance) {
  VexaDiagDraw("elements_inst_bi", mode, count * primcount);
  VexaPreDraw();
  VexaSavedVB saved[32];
  const int n = VexaApplyBaseInstance(baseinstance, saved, 32);
  ::glDrawElementsInstanced(mode, count, type, indices, primcount);
  VexaRestoreBaseInstance(saved, n);
  VexaPostDrawDiag();
}

using VexaDrawElemsInstBVBiIFn = void (*)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLint, GLuint);
static void fexfn_impl_libGL_glDrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei primcount, GLint basevertex, GLuint baseinstance) {
  VexaDiagDraw("elements_inst_bv_bi", mode, count * primcount);
  VexaPreDraw();
  VexaSavedVB saved[32];
  const int n = VexaApplyBaseInstance(baseinstance, saved, 32);
  ::glDrawElementsInstancedBaseVertex(mode, count, type, indices, primcount, basevertex);
  VexaRestoreBaseInstance(saved, n);
  VexaPostDrawDiag();
}

static inline GLintptr VexaPair64(const unsigned* v, int i) {
  return static_cast<GLintptr>(static_cast<uint64_t>(v[2 * i]) | (static_cast<uint64_t>(v[2 * i + 1]) << 32));
}

static auto fexfn_impl_libGL_VEXA_MultiDrawIndirect(unsigned mode, unsigned type, uintptr_t indirect, int drawcount,
                                                    int stride) -> void {
  if (drawcount <= 0) return;
  if (stride == 0) stride = type ? 20 : 16;
  VexaDiagDraw(type ? "multi_elements_indirect" : "multi_arrays_indirect", mode, drawcount);
  VexaPreDraw();
  for (int i = 0; i < drawcount; ++i) {
    const void* cmd = reinterpret_cast<const void*>(indirect + static_cast<uintptr_t>(stride) * i);
    if (type) ::glDrawElementsIndirect(mode, type, cmd);
    else ::glDrawArraysIndirect(mode, cmd);
  }
  VexaPostDrawDiag();
}

static auto fexfn_impl_libGL_VEXA_MultiDrawElementsBV(unsigned mode, unsigned type, const int* counts,
                                                      const unsigned* offsets, const int* basevertex,
                                                      int drawcount) -> void {
  if (drawcount <= 0 || !counts || !offsets) return;
  VexaDiagDraw("multi_elements_bv", mode, drawcount);
  VexaPreDraw();
  for (int i = 0; i < drawcount; ++i) {
    if (counts[i] <= 0) continue;
    ::glDrawElementsBaseVertex(mode, counts[i], type, reinterpret_cast<const void*>(VexaPair64(offsets, i)),
                               basevertex ? basevertex[i] : 0);
  }
  VexaPostDrawDiag();
}

// ARB_multi_bind semantics on GLES. Textures carry their target, which GLES
// cannot query, so remember it per name and re-probe when a bind is rejected.
static const GLenum kVexaTexTargets[] = {GL_TEXTURE_2D, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_3D,
                                         GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_2D_MULTISAMPLE,
                                         GL_TEXTURE_2D_MULTISAMPLE_ARRAY, GL_TEXTURE_BUFFER};
static std::unordered_map<GLuint, GLenum> g_vexa_tex_target;

static GLenum VexaTexBindingQuery(GLenum target) {
  switch (target) {
  case GL_TEXTURE_2D: return GL_TEXTURE_BINDING_2D;
  case GL_TEXTURE_2D_ARRAY: return GL_TEXTURE_BINDING_2D_ARRAY;
  case GL_TEXTURE_CUBE_MAP: return GL_TEXTURE_BINDING_CUBE_MAP;
  case GL_TEXTURE_3D: return GL_TEXTURE_BINDING_3D;
  case GL_TEXTURE_CUBE_MAP_ARRAY: return GL_TEXTURE_BINDING_CUBE_MAP_ARRAY;
  case GL_TEXTURE_2D_MULTISAMPLE: return GL_TEXTURE_BINDING_2D_MULTISAMPLE;
  case GL_TEXTURE_2D_MULTISAMPLE_ARRAY: return GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY;
  case GL_TEXTURE_BUFFER: return GL_TEXTURE_BINDING_BUFFER;
  default: return 0;
  }
}

// Binds `texture` on the active unit to its own target; returns that target.
// prev_out, when given, receives that target's binding before the bind. A
// rejected bind leaves the binding unchanged, so probing is side-effect free.
static GLenum VexaBindTextureAnyTarget(GLuint texture, GLint* prev_out = nullptr) {
  auto try_bind = [&](GLenum t) {
    if (prev_out) ::glGetIntegerv(VexaTexBindingQuery(t), prev_out);
    ::glBindTexture(t, texture);
    return ::glGetError() == GL_NO_ERROR;
  };
  if (const auto it = g_vexa_tex_target.find(texture); it != g_vexa_tex_target.end()) {
    if (try_bind(it->second)) return it->second;
    g_vexa_tex_target.erase(it);
  }
  for (GLenum t : kVexaTexTargets) {
    if (try_bind(t)) {
      g_vexa_tex_target[texture] = t;
      return t;
    }
  }
  return 0;
}

// op: 1 BuffersBase, 2 BuffersRange, 3 Textures, 4 Samplers, 5 VertexBuffers
// (sizes = strides), 6 ImageTextures. A null names array unbinds every slot.
static auto fexfn_impl_libGL_VEXA_BindMany(unsigned op, unsigned target, unsigned first, int count,
                                           const unsigned* names, const unsigned* offsets,
                                           const unsigned* sizes) -> void {
  if (count <= 0) return;
  // Multi-bind leaves the generic buffer binding point and the active texture
  // unit untouched, unlike the single-bind calls it is built from.
  const GLint prev_buffer = (op == 1 || op == 2) ? VexaHostBoundBuffer(target) : 0;
  GLint prev_unit = 0;
  if (op == 3 || op == 6) {
    ::glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
    // Target probing reads glGetError; a stale flag would read as a rejection.
    while (::glGetError() != GL_NO_ERROR) {
    }
  }
  for (int i = 0; i < count; ++i) {
    const GLuint name = names ? names[i] : 0;
    const GLuint slot = first + static_cast<GLuint>(i);
    switch (op) {
    case 1: ::glBindBufferBase(target, slot, name); break;
    case 2:
      if (name && offsets && sizes) ::glBindBufferRange(target, slot, name, VexaPair64(offsets, i), VexaPair64(sizes, i));
      else ::glBindBufferBase(target, slot, name);
      break;
    case 3:
      ::glActiveTexture(GL_TEXTURE0 + slot);
      if (name) {
        VexaBindTextureAnyTarget(name);
      } else {
        for (GLenum t : kVexaTexTargets) ::glBindTexture(t, 0);
      }
      break;
    case 4: ::glBindSampler(slot, name); break;
    case 5:
      ::glBindVertexBuffer(slot, name, name && offsets ? VexaPair64(offsets, i) : 0,
                           name && sizes ? static_cast<GLsizei>(sizes[2 * i]) : 16);
      break;
    case 6: {
      if (!name) {
        ::glBindImageTexture(slot, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
        break;
      }
      // Spec: level 0, layered, READ_WRITE, the texture's own internal format.
      // Read the format through the active unit and restore its binding.
      GLint prev_tex = 0;
      const GLenum tex_target = VexaBindTextureAnyTarget(name, &prev_tex);
      GLint format = GL_RGBA8;
      if (tex_target) {
        const GLenum level_target = tex_target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X : tex_target;
        ::glGetTexLevelParameteriv(level_target, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
        ::glBindTexture(tex_target, static_cast<GLuint>(prev_tex));
      }
      ::glBindImageTexture(slot, name, 0, GL_TRUE, 0, GL_READ_WRITE, static_cast<GLenum>(format));
      break;
    }
    default: return;
    }
  }
  if (op == 1 || op == 2) ::glBindBuffer(target, static_cast<GLuint>(prev_buffer));
  if (op == 3 || op == 6) ::glActiveTexture(static_cast<GLenum>(prev_unit));
}

using VexaMultiDrawArraysFn = void (*)(GLenum, const GLint*, const GLsizei*, GLsizei);
static void fexfn_impl_libGL_glMultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
  GLsizei total = 0;
  for (GLsizei i = 0; i < drawcount; ++i) total += count[i];
  VexaDiagDraw("multi_arrays", mode, total);
  static auto fn = reinterpret_cast<VexaMultiDrawArraysFn>(eglGetProcAddress("glMultiDrawArraysEXT"));
  if (fn) { fn(mode, first, count, drawcount); return; }
  for (GLsizei i = 0; i < drawcount; ++i) {
    if (count[i] > 0) { VexaPreDraw(); ::glDrawArrays(mode, first[i], count[i]); }
  }
}

using VexaDrawTFFn = void (*)(GLenum, GLuint);
static void fexfn_impl_libGL_glDrawTransformFeedback(GLenum mode, GLuint id) {
  VexaDiagDraw("tf", mode, 0);
  static auto fn = reinterpret_cast<VexaDrawTFFn>(eglGetProcAddress("glDrawTransformFeedback"));
  if (fn) fn(mode, id);
}

using VexaDrawTFInstFn = void (*)(GLenum, GLuint, GLsizei);
static void fexfn_impl_libGL_glDrawTransformFeedbackInstanced(GLenum mode, GLuint id, GLsizei primcount) {
  VexaDiagDraw("tf_inst", mode, 0);
  static auto fn = reinterpret_cast<VexaDrawTFInstFn>(eglGetProcAddress("glDrawTransformFeedbackInstanced"));
  if (fn) fn(mode, id, primcount);
}

using VexaDrawTFStreamFn = void (*)(GLenum, GLuint, GLuint);
static void fexfn_impl_libGL_glDrawTransformFeedbackStream(GLenum mode, GLuint id, GLuint stream) {
  VexaDiagDraw("tf_stream", mode, 0);
  static auto fn = reinterpret_cast<VexaDrawTFStreamFn>(eglGetProcAddress("glDrawTransformFeedbackStream"));
  if (fn) fn(mode, id, stream);
}

using VexaDrawTFStreamInstFn = void (*)(GLenum, GLuint, GLuint, GLsizei);
static void fexfn_impl_libGL_glDrawTransformFeedbackStreamInstanced(GLenum mode, GLuint id, GLuint stream, GLsizei primcount) {
  VexaDiagDraw("tf_stream_inst", mode, 0);
  static auto fn = reinterpret_cast<VexaDrawTFStreamInstFn>(eglGetProcAddress("glDrawTransformFeedbackStreamInstanced"));
  if (fn) fn(mode, id, stream, primcount);
}

struct VexaFFMesh {
  GLuint vbo = 0;
  GLsizei vertex_count = 0;
};

static GLuint g_vexa_ff_program = 0;
static GLint g_vexa_ff_mvp = -1;
static GLint g_vexa_ff_color = -1;
static GLuint g_vexa_ff_next = 1;
static GLuint g_vexa_ff_immediate_vbo = 0;
static std::unordered_map<GLuint, VexaFFMesh> g_vexa_ff_meshes;

static bool VexaFFEnsureProgram() {
  if (g_vexa_ff_program != 0) {
    return true;
  }
  const char* vertex =
    "attribute vec3 aPos;\n"
    "attribute vec3 aNrm;\n"
    "uniform mat4 uMvp;\n"
    "varying vec3 vNrm;\n"
    "void main() {\n"
    "  gl_Position = uMvp * vec4(aPos, 1.0);\n"
    "  vNrm = aNrm;\n"
    "}\n";
  const char* fragment =
    "precision mediump float;\n"
    "varying vec3 vNrm;\n"
    "uniform vec4 uColor;\n"
    "void main() {\n"
    "  vec3 n = normalize(vNrm);\n"
    "  float light = max(dot(n, normalize(vec3(0.25, 0.45, 0.85))), 0.18);\n"
    "  gl_FragColor = vec4(uColor.rgb * light, uColor.a);\n"
    "}\n";
  const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(vs, 1, &vertex, nullptr);
  glShaderSource(fs, 1, &fragment, nullptr);
  glCompileShader(vs);
  glCompileShader(fs);
  g_vexa_ff_program = glCreateProgram();
  glAttachShader(g_vexa_ff_program, vs);
  glAttachShader(g_vexa_ff_program, fs);
  glBindAttribLocation(g_vexa_ff_program, 0, "aPos");
  glBindAttribLocation(g_vexa_ff_program, 1, "aNrm");
  glLinkProgram(g_vexa_ff_program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint linked = 0;
  glGetProgramiv(g_vexa_ff_program, GL_LINK_STATUS, &linked);
  if (g_vexa_ff_program == 0 || linked != GL_TRUE) {
    char log[512] {};
    if (g_vexa_ff_program != 0) {
      glGetProgramInfoLog(g_vexa_ff_program, sizeof(log), nullptr, log);
    }
    fprintf(stderr, "[VEXA][FF] program link failed %s\n", log);
    g_vexa_ff_program = 0;
    return false;
  }
  g_vexa_ff_mvp = glGetUniformLocation(g_vexa_ff_program, "uMvp");
  g_vexa_ff_color = glGetUniformLocation(g_vexa_ff_program, "uColor");
  glGenBuffers(1, &g_vexa_ff_immediate_vbo);
  fprintf(stderr, "[VEXA][FF] program %u mvp %d color %d\n", g_vexa_ff_program, g_vexa_ff_mvp, g_vexa_ff_color);
  return true;
}

static void VexaFFBindMeshAttribs() {
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(0));
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
}

static auto fexfn_impl_libGL_VEXA_FFUpload(const float* vertices, int float_count) -> unsigned {
  if (!vertices || float_count < 18 || (float_count % 6) != 0 || !VexaFFEnsureProgram()) {
    return 0;
  }
  const GLuint id = g_vexa_ff_next++;
  VexaFFMesh mesh;
  mesh.vertex_count = float_count / 6;
  glGenBuffers(1, &mesh.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(float_count) * static_cast<GLsizeiptr>(sizeof(float)), vertices, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  g_vexa_ff_meshes.emplace(id, mesh);
  fprintf(stderr, "[VEXA][FF] mesh %u verts %d\n", id, mesh.vertex_count);
  return id;
}

static auto fexfn_impl_libGL_VEXA_FFDrawList(const unsigned* buffers, const int* counts, const float* colors, int item_count, const float* mvp) -> void {
  if (!buffers || !counts || !colors || !mvp || item_count <= 0 || !VexaFFEnsureProgram()) {
    return;
  }
  if (item_count > 256) {
    item_count = 256;
  }
  glUseProgram(g_vexa_ff_program);
  glUniformMatrix4fv(g_vexa_ff_mvp, 1, GL_FALSE, mvp);
  for (int i = 0; i < item_count; ++i) {
    const auto mesh = g_vexa_ff_meshes.find(buffers[i]);
    if (mesh == g_vexa_ff_meshes.end() || counts[i] <= 0) {
      continue;
    }
    glUniform4fv(g_vexa_ff_color, 1, colors + (i * 4));
    glBindBuffer(GL_ARRAY_BUFFER, mesh->second.vbo);
    VexaFFBindMeshAttribs();
    glDrawArrays(GL_TRIANGLES, 0, counts[i]);
  }
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static auto fexfn_impl_libGL_VEXA_FFDrawImmediate(const float* vertices, int float_count, const float* mvp, const float* color) -> void {
  if (!vertices || !mvp || !color || float_count < 18 || (float_count % 6) != 0 || !VexaFFEnsureProgram()) {
    return;
  }
  glBindBuffer(GL_ARRAY_BUFFER, g_vexa_ff_immediate_vbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(float_count) * static_cast<GLsizeiptr>(sizeof(float)), vertices, GL_DYNAMIC_DRAW);
  VexaFFBindMeshAttribs();
  glUseProgram(g_vexa_ff_program);
  glUniformMatrix4fv(g_vexa_ff_mvp, 1, GL_FALSE, mvp);
  glUniform4fv(g_vexa_ff_color, 1, color);
  glDrawArrays(GL_TRIANGLES, 0, float_count / 6);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static auto fexfn_impl_libGL_VEXA_FFDelete(unsigned buffer) -> void {
  const auto mesh = g_vexa_ff_meshes.find(buffer);
  if (mesh == g_vexa_ff_meshes.end()) {
    return;
  }
  glDeleteBuffers(1, &mesh->second.vbo);
  g_vexa_ff_meshes.erase(mesh);
}
#endif

#include "thunkgen_host_libGL.inl"

static void VexaRebindUploadThunks() {
  static bool done = false;
  if (done) return;
  done = true;
  // Direct guest imports call fexldr_ptr, which init points at the raw GLES
  // entry. glXGetProcAddress returns the wrappers, but ETS2 also calls the
  // imported symbols. Point those at the wrappers before the first upload.
  fexldr_ptr_libGL_glTexImage2D = VexaGL_glTexImage2D;
  fexldr_ptr_libGL_glTexImage3D = VexaGL_glTexImage3D;
  fexldr_ptr_libGL_glTexSubImage2D = VexaGL_glTexSubImage2D;
  fexldr_ptr_libGL_glTexSubImage3D = VexaGL_glTexSubImage3D;
  fexldr_ptr_libGL_glPixelStorei = VexaGL_glPixelStorei;
  fexldr_ptr_libGL_glGetUniformLocation = VexaGL_glGetUniformLocation;
  fexldr_ptr_libGL_glUniform1i = VexaGL_glUniform1i;
  fexldr_ptr_libGL_glUniform1f = VexaGL_glUniform1f;
  fexldr_ptr_libGL_glUniform1iv = VexaGL_glUniform1iv;
  fexldr_ptr_libGL_glUniform1fv = VexaGL_glUniform1fv;
  fexldr_ptr_libGL_glUniform2i = VexaGL_glUniform2i;
  fexldr_ptr_libGL_glUniform2f = VexaGL_glUniform2f;
  fexldr_ptr_libGL_glUniform2iv = VexaGL_glUniform2iv;
  fexldr_ptr_libGL_glUniform2fv = VexaGL_glUniform2fv;
  fexldr_ptr_libGL_glUniform3i = VexaGL_glUniform3i;
  fexldr_ptr_libGL_glUniform3f = VexaGL_glUniform3f;
  fexldr_ptr_libGL_glUniform3iv = VexaGL_glUniform3iv;
  fexldr_ptr_libGL_glUniform3fv = VexaGL_glUniform3fv;
  fexldr_ptr_libGL_glUniform4i = VexaGL_glUniform4i;
  fexldr_ptr_libGL_glUniform4f = VexaGL_glUniform4f;
  fexldr_ptr_libGL_glUniform4iv = VexaGL_glUniform4iv;
  fexldr_ptr_libGL_glUniform4fv = VexaGL_glUniform4fv;
  fexldr_ptr_libGL_glUniformMatrix2fv = VexaGL_glUniformMatrix2fv;
  fexldr_ptr_libGL_glUniformMatrix3fv = VexaGL_glUniformMatrix3fv;
  fexldr_ptr_libGL_glUniformMatrix4fv = VexaGL_glUniformMatrix4fv;
  fexldr_ptr_libGL_glProgramUniform1i = VexaGL_glProgramUniform1i;
  fexldr_ptr_libGL_glProgramUniform1f = VexaGL_glProgramUniform1f;
  fexldr_ptr_libGL_glProgramUniform2f = VexaGL_glProgramUniform2f;
  fexldr_ptr_libGL_glProgramUniform3f = VexaGL_glProgramUniform3f;
  fexldr_ptr_libGL_glProgramUniform4f = VexaGL_glProgramUniform4f;
  fexldr_ptr_libGL_glProgramUniform2i = VexaGL_glProgramUniform2i;
  fexldr_ptr_libGL_glProgramUniform3i = VexaGL_glProgramUniform3i;
  fexldr_ptr_libGL_glProgramUniform4i = VexaGL_glProgramUniform4i;
  fexldr_ptr_libGL_glUseProgramStages = VexaGL_glUseProgramStages;
  fexldr_ptr_libGL_glUniformBlockBinding = VexaGL_glUniformBlockBinding;
  fexldr_ptr_libGL_glBindProgramPipeline = VexaGL_glBindProgramPipeline;
  fexldr_ptr_libGL_glActiveShaderProgram = VexaGL_glActiveShaderProgram;
}

#if defined(BUILD_ANDROID) && defined(VEXA_ENABLE_RUNTIME_SHADER_TRANSLATOR)
bool ConsumeShaderDriverErrorBudget() {
  int32_t remaining = ShaderDriverErrorLogBudget.load(std::memory_order_acquire);
  while (remaining > 0 &&
         !ShaderDriverErrorLogBudget.compare_exchange_weak(
             remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  return remaining > 0;
}

void LogShaderDriverError(const char* phase, GLuint object, const char* info) {
  MaybeResetShaderStateForNewLaunch();
  if (!ConsumeShaderDriverErrorBudget()) {
    return;
  }

  VEXA_LOGV(
               "[libGL-host] shader-driver-%s object=%u %s\n",
               phase ? phase : "unknown",
               static_cast<unsigned>(object),
               info ? info : "");

  if (FILE* f = std::fopen(ShaderDriverErrorLogPath, "a")) {
    fprintf(f,
                 "[libGL-host] shader-driver-%s object=%u %s\n",
                 phase ? phase : "unknown",
                 static_cast<unsigned>(object),
                 info ? info : "");
    std::fclose(f);
  }
}

void LogFailedShaderCompile(GLuint shader) {
  GLint status = GL_TRUE;
  ::glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (status == GL_TRUE) {
    return;
  }

  GLint log_len = 0;
  ::glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
  std::string info;
  if (log_len > 1) {
    info.resize(static_cast<size_t>(log_len));
    GLsizei written = 0;
    ::glGetShaderInfoLog(shader, log_len, &written, info.data());
    if (written >= 0 && static_cast<size_t>(written) < info.size()) {
      info.resize(static_cast<size_t>(written));
    }
  }

  LogShaderDriverError("compile-failed", shader, info.empty() ? "(empty shader info log)" : info.c_str());
}

bool LogProgramAttachedShaders(GLuint program) {
  GLint attached_count = 0;
  ::glGetProgramiv(program, GL_ATTACHED_SHADERS, &attached_count);
  if (attached_count <= 0) {
    LogShaderDriverError("link-attach", program, "attached_shaders=0");
    return false;
  }

  std::vector<GLuint> shaders(static_cast<size_t>(attached_count));
  GLsizei written = 0;
  ::glGetAttachedShaders(program, attached_count, &written, shaders.data());

  bool has_fragment = false;
  for (GLsizei i = 0; i < written; ++i) {
    GLint shader_type = 0;
    GLint compile_status = GL_FALSE;
    GLint source_len = 0;
    ::glGetShaderiv(shaders[static_cast<size_t>(i)], GL_SHADER_TYPE, &shader_type);
    ::glGetShaderiv(shaders[static_cast<size_t>(i)], GL_COMPILE_STATUS, &compile_status);
    ::glGetShaderiv(shaders[static_cast<size_t>(i)], GL_SHADER_SOURCE_LENGTH, &source_len);

    if (shader_type == GL_FRAGMENT_SHADER) {
      has_fragment = true;
    }

    char detail[196] {};
    std::snprintf(detail, sizeof(detail),
                  "shader=%u type=0x%x compiled=%d source_len=%d",
                  static_cast<unsigned>(shaders[static_cast<size_t>(i)]),
                  static_cast<unsigned>(shader_type),
                  compile_status == GL_TRUE ? 1 : 0,
                  static_cast<int>(source_len));
    LogShaderDriverError("link-attach", program, detail);
  }

  if (!has_fragment) {
    LogShaderDriverError("link-missing-fragment", program, "no GL_FRAGMENT_SHADER attached at link time");
  }
  return has_fragment;
}

using GLProgramParameteriFn = void (*)(GLuint, GLenum, GLint);

static GLProgramParameteriFn ResolveProgramParameteriFn() {
  static GLProgramParameteriFn fn = []() -> GLProgramParameteriFn {
    if (auto p = reinterpret_cast<GLProgramParameteriFn>(eglGetProcAddress("glProgramParameteri"))) {
      return p;
    }
    if (auto p = reinterpret_cast<GLProgramParameteriFn>(eglGetProcAddress("glProgramParameteriEXT"))) {
      return p;
    }
    if (auto p = reinterpret_cast<GLProgramParameteriFn>(eglGetProcAddress("glProgramParameteriARB"))) {
      return p;
    }
    return nullptr;
  }();
  return fn;
}

bool TryRelinkAsSeparableProgram(GLuint program) {
  auto program_param = ResolveProgramParameteriFn();
  if (!program_param) {
    LogShaderDriverError("separable-link-retry-skip", program, "glProgramParameteri* unavailable");
    return false;
  }

  program_param(program, GL_PROGRAM_SEPARABLE, GL_TRUE);
  ::glLinkProgram(program);

  GLint retry_link_status = GL_FALSE;
  ::glGetProgramiv(program, GL_LINK_STATUS, &retry_link_status);
  if (retry_link_status == GL_TRUE) {
    LogShaderDriverError("separable-link-retry-ok", program,
                         "linked with GL_PROGRAM_SEPARABLE (no fallback fragment)");
    return true;
  }

  GLint retry_log_len = 0;
  ::glGetProgramiv(program, GL_INFO_LOG_LENGTH, &retry_log_len);
  std::string retry_info;
  if (retry_log_len > 1) {
    retry_info.resize(static_cast<size_t>(retry_log_len));
    GLsizei retry_written = 0;
    ::glGetProgramInfoLog(program, retry_log_len, &retry_written, retry_info.data());
    if (retry_written >= 0 && static_cast<size_t>(retry_written) < retry_info.size()) {
      retry_info.resize(static_cast<size_t>(retry_written));
    }
  }
  LogShaderDriverError("separable-link-retry-failed", program,
                       retry_info.empty() ? "(empty program info log)" : retry_info.c_str());
  return false;
}

bool TryRelinkWithFallbackFragment(GLuint program) {
  constexpr const char* kFallbackFragmentSource =
      "#version 320 es\n"
      "precision mediump float;\n"
      "out mediump vec4 vexaFallbackColor;\n"
      "void main() {\n"
      "  vexaFallbackColor = vec4(1.0);\n"
      "}\n";

  GLuint fallback_fragment = ::glCreateShader(GL_FRAGMENT_SHADER);
  if (fallback_fragment == 0) {
    LogShaderDriverError("fallback-frag-create-failed", program, "glCreateShader(GL_FRAGMENT_SHADER) returned 0");
    return false;
  }

  const GLchar* source_ptr = kFallbackFragmentSource;
  ::glShaderSource(fallback_fragment, 1, &source_ptr, nullptr);
  ::glCompileShader(fallback_fragment);

  GLint compile_status = GL_FALSE;
  ::glGetShaderiv(fallback_fragment, GL_COMPILE_STATUS, &compile_status);
  if (compile_status != GL_TRUE) {
    GLint log_len = 0;
    ::glGetShaderiv(fallback_fragment, GL_INFO_LOG_LENGTH, &log_len);
    std::string info;
    if (log_len > 1) {
      info.resize(static_cast<size_t>(log_len));
      GLsizei written = 0;
      ::glGetShaderInfoLog(fallback_fragment, log_len, &written, info.data());
      if (written >= 0 && static_cast<size_t>(written) < info.size()) {
        info.resize(static_cast<size_t>(written));
      }
    }
    LogShaderDriverError("fallback-frag-compile-failed", program, info.empty() ? "(empty shader info log)" : info.c_str());
    ::glDeleteShader(fallback_fragment);
    return false;
  }

  ::glAttachShader(program, fallback_fragment);
  ::glLinkProgram(program);

  GLint retry_link_status = GL_FALSE;
  ::glGetProgramiv(program, GL_LINK_STATUS, &retry_link_status);
  if (retry_link_status == GL_TRUE) {
    LogShaderDriverError("fallback-frag-link-retry-ok", program, "linked with injected fallback fragment shader");
    ::glDetachShader(program, fallback_fragment);
    ::glDeleteShader(fallback_fragment);
    return true;
  }

  GLint retry_log_len = 0;
  ::glGetProgramiv(program, GL_INFO_LOG_LENGTH, &retry_log_len);
  std::string retry_info;
  if (retry_log_len > 1) {
    retry_info.resize(static_cast<size_t>(retry_log_len));
    GLsizei retry_written = 0;
    ::glGetProgramInfoLog(program, retry_log_len, &retry_written, retry_info.data());
    if (retry_written >= 0 && static_cast<size_t>(retry_written) < retry_info.size()) {
      retry_info.resize(static_cast<size_t>(retry_written));
    }
  }
  LogShaderDriverError("fallback-frag-link-retry-failed", program,
                       retry_info.empty() ? "(empty program info log)" : retry_info.c_str());

  ::glDetachShader(program, fallback_fragment);
  ::glDeleteShader(fallback_fragment);
  return false;
}

void LogFailedProgramLink(GLuint program) {
  GLint status = GL_TRUE;
  ::glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (status == GL_TRUE) {
    return;
  }

  GLint log_len = 0;
  ::glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
  std::string info;
  if (log_len > 1) {
    info.resize(static_cast<size_t>(log_len));
    GLsizei written = 0;
    ::glGetProgramInfoLog(program, log_len, &written, info.data());
    if (written >= 0 && static_cast<size_t>(written) < info.size()) {
      info.resize(static_cast<size_t>(written));
    }
  }

  const bool has_fragment = LogProgramAttachedShaders(program);
  if (!has_fragment) {
    if (TryRelinkAsSeparableProgram(program)) {
      return;
    }
    if (TryRelinkWithFallbackFragment(program)) {
      return;
    }
  }
  LogShaderDriverError("link-failed", program, info.empty() ? "(empty program info log)" : info.c_str());
}
#endif

#if defined(BUILD_ANDROID) && !defined(VEXA_ENABLE_RUNTIME_SHADER_TRANSLATOR)
// Desktop #extension GL_ARB_* : require fails on ESSL. Those features are core in 300/320 es.
static void VexaStripDesktopArbExtensions(std::string* source) {
  std::string out;
  out.reserve(source->size());
  size_t cursor = 0;
  while (cursor < source->size()) {
    size_t end = source->find('\n', cursor);
    if (end == std::string::npos) {
      end = source->size();
    }
    const std::string_view line(source->data() + cursor, end - cursor);
    size_t begin = 0;
    while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t')) {
      ++begin;
    }
    if (line.substr(begin).starts_with("#extension") && line.find("GL_ARB_") != std::string_view::npos) {
      out.append("// vexa stripped ");
      out.append(line);
    } else {
      out.append(line);
    }
    if (end >= source->size()) {
      break;
    }
    out.push_back('\n');
    cursor = end + 1;
  }
  *source = std::move(out);
}

// Desktop separable-program vertex shaders redeclare the built-in output block:
//   out gl_PerVertex { vec4 gl_Position; };
// ESSL rejects that (Mali L0001 "Invalid qualifier 'out' for interface block")
// and already provides gl_Position, so drop the whole declaration.
static void VexaStripPerVertexRedeclaration(std::string* source) {
  constexpr std::string_view kBlock {"gl_PerVertex"};
  size_t pos = 0;
  while ((pos = source->find(kBlock, pos)) != std::string::npos) {
    // Walk back over whitespace to the `out` keyword.
    size_t kw_end = pos;
    auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (kw_end > 0 && is_space((*source)[kw_end - 1])) {
      --kw_end;
    }
    if (kw_end < 3 || source->compare(kw_end - 3, 3, "out") != 0) {
      pos += kBlock.size();
      continue;
    }
    const size_t kw_begin = kw_end - 3;
    const size_t open = source->find('{', pos);
    const size_t close = open == std::string::npos ? std::string::npos : source->find('}', open);
    const size_t semi = close == std::string::npos ? std::string::npos : source->find(';', close);
    if (semi == std::string::npos) {
      break;
    }
    source->erase(kw_begin, semi + 1 - kw_begin);
    pos = kw_begin;
  }
}
#endif

// VEXA_FIXES: Central shader-source bridge used by both glShaderSource and glShaderSourceARB.
//
// A note on the grass shader, since it looks like an easy fix and is not.
// Mali reports GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS == 0, so Unity's
// `Custom/GrassShaderOptimized` - which emits a vertex SSBO indexed by
// gl_InstanceID - cannot link, and Unity draws the grass with a black
// fallback. Rewriting `buffer` to `uniform` gets past the link error and then
// fails with S0018, because ES forbids an unsized array in a uniform block.
// Sizing it does not help either: Unity binds its instance data on the
// shader-storage binding point, so a uniform block at the same binding point
// has nothing behind it. Lowering the reported GL version does not help
// either - Unity picks its shader target from the extension list, not from
// GL_VERSION, and emits `#version 430` regardless. Two dead ends, both tried,
// both reverted. What is left is dumping the source when a shader fails so the
// next failure can be reasoned about from the outside.

#include <mutex>
#include <memory>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>

// VEXA_FIXES: Keep the last source we handed to each shader so a compile failure
// can be dumped in full. The driver only reports a line and column, which is not
// enough to reason about a rewritten shader from the outside.
// Shader diagnostic sources belong to the program share group.

static void VexaRememberShaderSource(GLuint shader, const std::string& source) {
  if (source.empty()) {
    return;
  }
  std::lock_guard lk(VexaShaderSourceMutex);
  if (VexaShaderSourceById.size() > 512) {
    VexaShaderSourceById.clear();
  }
  VexaShaderSourceById[shader] = source;
}

static void VexaDumpFailedShaderSource(GLuint shader, const char* info) {
  std::string source;
  {
    std::lock_guard lk(VexaShaderSourceMutex);
    const auto it = VexaShaderSourceById.find(shader);
    if (it == VexaShaderSourceById.end()) {
      return;
    }
    source = it->second;
  }
  // The dump directory lives in app-private storage and is not created by the
  // runtime, so make it on first use.
  ::mkdir("/data/data/com.critical.vexaemulator/files/artifacts", 0755);
  ::mkdir("/data/data/com.critical.vexaemulator/files/artifacts/shaders", 0755);
  char path[256];
  std::snprintf(path, sizeof(path), "/data/data/com.critical.vexaemulator/files/artifacts/shaders/failed_%u.glsl",
                static_cast<unsigned>(shader));
  FILE* f = fopen(path, "wb");
  if (!f) {
    VEXA_LOGV("[libGL-host] dump failed for shader %u: cannot open %s\n", static_cast<unsigned>(shader), path);
    return;
  }
  fprintf(f, "// driver log: %s\n", info ? info : "");
  fwrite(source.data(), 1, source.size(), f);
  fclose(f);
  VEXA_LOGV("[libGL-host] dumped failed shader %u -> %s (%zu bytes)\n",
          static_cast<unsigned>(shader), path, source.size());
}

static void fexfn_impl_libGL_FEX_glShaderSource(GLuint shader, GLsizei count, uintptr_t strings, const GLint* length) {
  GLHOST_TRACE_SCOPE();
#ifdef IS_32BIT_THUNK
  std::vector<const GLchar*> expanded;
  if (strings && count > 0) {
    const auto guest_strings = reinterpret_cast<const uint32_t*>(strings);
    expanded.reserve(count);
    for (GLsizei i = 0; i < count; ++i)
      expanded.push_back(reinterpret_cast<const GLchar*>(uintptr_t {guest_strings[i]}));
  }
  const auto host_strings = expanded.empty() ? nullptr : expanded.data();
#else
  const auto host_strings = reinterpret_cast<const GLchar* const*>(strings);
#endif
#if defined(BUILD_ANDROID) && defined(VEXA_ENABLE_RUNTIME_SHADER_TRANSLATOR)
  if (host_strings && count > 0) {
    GLint shader_type = 0;
    ::glGetShaderiv(shader, GL_SHADER_TYPE, &shader_type);

    const std::string source = FlattenShaderSource(count, host_strings, length);
    if (!source.empty() && !IsAlreadyESSL(source)) {
      const auto cache_key = MakeShaderCacheKey(static_cast<GLenum>(shader_type), source);
      DumpShaderDebugText(true, static_cast<GLenum>(shader_type), cache_key, "original.glsl", source);
      DumpShaderDebugText(false, static_cast<GLenum>(shader_type), cache_key, "attempt_source.glsl", source);

      std::string translated_source;
      bool cache_hit = false;
      {
        std::lock_guard lk(ShaderSourceCacheMutex);
        const auto it = ShaderSourceCache.find(cache_key);
        if (it != ShaderSourceCache.end()) {
          translated_source = it->second;
          cache_hit = true;
        }
      }
      {
        char detail[128] {};
        std::snprintf(detail, sizeof(detail), "key=%zx source_len=%zu", cache_key, source.size());
        LogShaderTranspileEvent(cache_hit ? "cache-hit" : "cache-miss", static_cast<GLenum>(shader_type), detail);
      }

      if (translated_source.empty() &&
          [&] {
            std::string transpile_error;
            const bool transpile_ok = TranspileDesktopGLSLToESSL(static_cast<GLenum>(shader_type), source, &translated_source, &transpile_error);
            if (!transpile_ok && !transpile_error.empty()) {
              DumpShaderDebugText(false, static_cast<GLenum>(shader_type), cache_key, "transpile_error.txt", transpile_error);
            } else if (!transpile_ok) {
              char detail[128] {};
              std::snprintf(detail, sizeof(detail), "key=%zx empty-error-message", cache_key);
              LogShaderTranspileEvent("compile-failed", static_cast<GLenum>(shader_type), detail);
            }
            return transpile_ok;
          }()) {
        {
          std::lock_guard lk(ShaderSourceCacheMutex);
          ShaderSourceCache[cache_key] = translated_source;
        }
        char detail[128] {};
        std::snprintf(detail, sizeof(detail), "key=%zx translated_len=%zu", cache_key, translated_source.size());
        LogShaderTranspileEvent("compile-ok", static_cast<GLenum>(shader_type), detail);
      }

      if (!translated_source.empty()) {
        translated_source = NormalizeVersionToES(translated_source);
        EnsureRequiredExtensions(&translated_source, static_cast<GLenum>(shader_type));
        DumpShaderDebugText(false, static_cast<GLenum>(shader_type), cache_key, "translated.glsl", translated_source);
        {
          char detail[128] {};
          std::snprintf(detail, sizeof(detail), "key=%zx translated_len=%zu", cache_key, translated_source.size());
          LogShaderTranspileEvent("emit-translated", static_cast<GLenum>(shader_type), detail);
        }
        const GLchar* translated_ptr = translated_source.c_str();
        ::glShaderSource(shader, 1, &translated_ptr, nullptr);
        return;
      }

      std::string fallback_source = NormalizeVersionToES(CollapseBackslashNewlines(source));
      EnsureFragmentPrecision(&fallback_source, static_cast<GLenum>(shader_type));
      EnsureRequiredExtensions(&fallback_source, static_cast<GLenum>(shader_type));
      if (!fallback_source.empty() && fallback_source != source) {
        DumpShaderDebugText(false, static_cast<GLenum>(shader_type), cache_key, "fallback_320es.glsl", fallback_source);
        {
          char detail[128] {};
          std::snprintf(detail, sizeof(detail), "key=%zx fallback_len=%zu", cache_key, fallback_source.size());
          LogShaderTranspileEvent("emit-fallback-320es", static_cast<GLenum>(shader_type), detail);
        }
        const GLchar* fallback_ptr = fallback_source.c_str();
        ::glShaderSource(shader, 1, &fallback_ptr, nullptr);
        return;
      }

      {
        char detail[160] {};
        std::snprintf(detail, sizeof(detail), "key=%zx passthrough translated_empty=1 fallback_changed=%d",
                      cache_key, fallback_source != source ? 1 : 0);
        LogShaderTranspileEvent("passthrough-original", static_cast<GLenum>(shader_type), detail);
      }
    } else if (source.empty()) {
      LogShaderTranspileEvent("skip-empty-source", static_cast<GLenum>(shader_type));
    } else {
      LogShaderTranspileEvent("skip-essl-source", static_cast<GLenum>(shader_type));
    }
  }
#elif defined(BUILD_ANDROID)
  // Mali accepts ESSL up to 320 es. Godot emits desktop #version 330.
  // Unity, once it sees GL 4.3, emits #version 430/450 including compute.
  if (host_strings && count > 0) {
    std::string source;
    for (GLsizei i = 0; i < count; ++i) {
      if (!host_strings[i]) {
        continue;
      }
      if (length && length[i] > 0) {
        source.append(host_strings[i], static_cast<size_t>(length[i]));
      } else {
        source.append(host_strings[i]);
      }
    }
    // Legacy GLSL 1.10 shaders (Warband) ship without a #version directive.
    // Compiling them raw makes the driver default to ESSL 100 anyway, so give
    // them the version line explicitly and let the rewrite below add precision.
    if (!source.empty() && source.find("#version") == std::string::npos) {
      source.insert(0, "#version 100\n");
    }
    const auto version_pos = source.find("#version");
    if (version_pos != std::string::npos) {
      const auto line_end = source.find('\n', version_pos);
      const auto line_len = line_end == std::string::npos ? source.size() - version_pos : line_end - version_pos;
      const auto line = source.substr(version_pos, line_len);
      int desktop_version = 0;
      if (const auto digits = line.find_first_of("0123456789"); digits != std::string::npos) {
        for (size_t i = digits; i < line.size() && line[i] >= '0' && line[i] <= '9'; ++i) {
          desktop_version = desktop_version * 10 + (line[i] - '0');
        }
      }
      GLint shader_type = 0;
      ::glGetShaderiv(shader, GL_SHADER_TYPE, &shader_type);
      const bool desktop = line.find(" es") == std::string::npos;
      const bool compute = shader_type == GL_COMPUTE_SHADER;
      const char* rewritten_version = nullptr;
      if (desktop && (compute || desktop_version >= 400)) {
        rewritten_version = "#version 320 es";
      } else if (desktop && desktop_version >= 130 && desktop_version <= 330) {
        // Heaps/HashLink uses GLSL 150 with in/out interfaces. Like GLSL 330,
        // its vertex/fragment shaders need an ESSL 300 version on Android.
        // Shaders that use texel buffers (samplerBuffer is reserved in ESSL 300)
        // or ARB_separate_shader_objects (layout(location) on fragment inputs is
        // illegal in ESSL 300) need ESSL 320, where both are core. ETS2 does both.
        const bool needs_es_320 =
            source.find("samplerBuffer") != std::string::npos ||
            source.find("imageBuffer") != std::string::npos ||
            source.find("GL_ARB_separate_shader_objects") != std::string::npos;
        rewritten_version = needs_es_320 ? "#version 320 es" : "#version 300 es";
      } else if (desktop && desktop_version >= 100 && desktop_version <= 120) {
        rewritten_version = "#version 100";
      }
      if (rewritten_version) {
        source.replace(version_pos, line_len, rewritten_version);
        // Desktop precision qualifiers have no effect. Leaving lowp/mediump
        // active in ESSL loses coordinate and animation-time precision on Mali.
        auto is_identifier = [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_';
        };
        unsigned precision_promotions = 0;
        for (std::string_view qualifier : {std::string_view{"mediump"}, std::string_view{"lowp"}}) {
          for (size_t pos = 0; (pos = source.find(qualifier, pos)) != std::string::npos;) {
            const size_t end = pos + qualifier.size();
            if ((pos == 0 || !is_identifier(source[pos - 1])) &&
                (end == source.size() || !is_identifier(source[end]))) {
              source.replace(pos, qualifier.size(), "highp");
              ++precision_promotions;
              pos += 5;
            } else {
              pos = end;
            }
          }
        }
        static unsigned precision_logs = 0;
        if (precision_promotions && precision_logs++ < 4) {
          fprintf(stderr, "[VEXA][GL] desktop precision -> highp shader=%u type=%x qualifiers=%u\n",
                  shader, shader_type, precision_promotions);
        }
        if (desktop_version >= 130 && desktop_version < 400) {
          // `sample` is a legal identifier in older desktop GLSL but reserved
          // in ESSL 300. Rename whole identifiers, never substrings.
          auto identifier = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_';
          };
          for (size_t pos = 0; (pos = source.find("sample", pos)) != std::string::npos;) {
            if ((pos == 0 || !identifier(source[pos - 1])) &&
                (pos + 6 == source.size() || !identifier(source[pos + 6]))) {
              source.replace(pos, 6, "vexa_legacy_sample");
              pos += 18;
            } else {
              pos += 6;
            }
          }
          if (shader_type == GL_FRAGMENT_SHADER) {
            // Heaps names its desktop MRT outputs OUTPUT1..OUTPUTn and binds
            // them to attachments 0..n-1. GLES requires those locations in GLSL.
            constexpr std::string_view prefix {"out vec4 OUTPUT"};
            for (size_t pos = 0; (pos = source.find(prefix, pos)) != std::string::npos;) {
              size_t end = pos + prefix.size();
              unsigned output = 0;
              while (end < source.size() && source[end] >= '0' && source[end] <= '9') {
                output = output * 10 + unsigned(source[end++] - '0');
              }
              if (output > 0 && end < source.size() && source[end] == ';') {
                const auto layout = "layout(location=" + std::to_string(output - 1) + ") ";
                source.insert(pos, layout);
                pos = end + layout.size() + 1;
              } else {
                pos = end;
              }
            }
          }
        }
        // Desktop `#extension GL_ARB_* : require` is a hard error on ESSL (Mali
        // P0003). Those features are core in 300/320 es, so strip the directive
        // for every desktop shader we rewrite to ES (ETS2 uses #version 330 with
        // GL_ARB_separate_shader_objects).
        VexaStripDesktopArbExtensions(&source);
        VexaStripPerVertexRedeclaration(&source);
        {
          const bool to_es_100 = std::string_view{rewritten_version} == "#version 100";
          const bool to_es_320 = std::string_view{rewritten_version} == "#version 320 es";
          const size_t insert_at = VexaShaderPrecisionInsertPos(source);
          if (to_es_100) {
            constexpr std::string_view kVexaShaderPrecisionEs100 {
                "precision highp float;\n"
                "precision highp int;\n"};
            source.insert(insert_at, kVexaShaderPrecisionEs100);
          } else {
            source.insert(insert_at, kVexaShaderPrecisionEs300);
            if (to_es_320) {
              source.insert(insert_at + kVexaShaderPrecisionEs300.size(), kVexaShaderPrecisionEs320Extra);
            }
          }
        }
        VEXA_LOGV("[libGL-host] shader version %d -> %s type=0x%x len=%zu binding=%d\n",
                desktop_version,
                rewritten_version,
                static_cast<unsigned>(shader_type),
                source.size(),
                source.find("layout(binding") != std::string::npos ? 1 : 0);
        VexaRewriteGrassBuffer(shader, shader_type, source);
        VexaRememberShaderSource(shader, source);
        const GLchar* rewritten = source.c_str();
        ::glShaderSource(shader, 1, &rewritten, nullptr);
        return;
      }
    }
    VexaRememberShaderSource(shader, source);
  }
#endif
  ::glShaderSource(shader, count, host_strings, length);
}

#ifdef BUILD_ANDROID
// VEXA_FIXES: Export explicit symbol so guest thunk side can target a stable bridge entrypoint.
extern "C" __attribute__((visibility("default")))
void FEX_glShaderSource(GLuint shader, GLsizei count, uintptr_t strings, const GLint* length) {
  fexfn_impl_libGL_FEX_glShaderSource(shader, count, strings, length);
}

// Android glX proc routing references these wrappers before their definitions.
GLuint fexfn_impl_libGL_glCreateShader(GLenum a_0);
void fexfn_impl_libGL_glAttachShader(GLuint a_0, GLuint a_1);
void fexfn_impl_libGL_glDetachShader(GLuint a_0, GLuint a_1);
GLuint fexfn_impl_libGL_glCreateProgram();
void fexfn_impl_libGL_glGetProgramiv(GLuint program, GLenum pname, GLint* value);
void fexfn_impl_libGL_glDeleteProgram(GLuint program);
void fexfn_impl_libGL_glDeleteShader(GLuint shader);
void fexfn_impl_libGL_glDeleteProgramPipelines(GLsizei count, const GLuint* pipelines);
void fexfn_impl_libGL_glCompileShader(GLuint a_0);
void fexfn_impl_libGL_glLinkProgram(GLuint a_0);
void fexfn_impl_libGL_glGetIntegerv(GLenum a_0, GLint* a_1);
GLenum fexfn_impl_libGL_glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
void fexfn_impl_libGL_glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
void fexfn_impl_libGL_glGetSynciv(GLsync sync, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* values);
void fexfn_impl_libGL_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
void fexfn_impl_libGL_glFinish();
void fexfn_impl_libGL_glFlush();
GLXContext fexfn_impl_libGL_glXCreateContext(Display* Display, guest_layout<XVisualInfo*> Info, GLXContext ShareList, Bool Direct);

#endif

#ifdef BUILD_ANDROID
// Godot 3's Linux export is GLES-over-GL and rejects anything below desktop
// OpenGL 3.3, and needs a desktop framebuffer extension name before it will
// create a device. Framebuffer objects are core on this GLES 3.2 driver; only
// the name differs.
//
// GL_ARB_shader_storage_buffer_object is deliberately absent. Unity picks its
// shader target from the extension list rather than GL_VERSION, so with the
// name present it keeps emitting `#version 430` and a vertex SSBO, which this
// Mali cannot link (GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS == 0). Without the name
// it emits the 3.3 uniform-block form instead.
static const char* const kVexaDesktopSpoofExts[] = {
  "GL_ARB_framebuffer_object",
  "GL_EXT_framebuffer_object",
  "GL_ARB_draw_indirect",
  "GL_ARB_compute_shader",
  "GL_ARB_shader_image_load_store",
  // ETS2 (and most desktop engines) refuse to load DXT/BC textures unless the
  // driver advertises S3TC. Mali supports the compressed formats natively
  // through GL_EXT_texture_compression_s3tc, but the GL ES extension string is
  // not mapped to the desktop name, so advertise it explicitly.
  // Keep this entry last: it is the one the runtime log shows being queried.
  "GL_EXT_texture_compression_s3tc",
};
static constexpr unsigned kVexaDesktopSpoofExtCount =
  sizeof(kVexaDesktopSpoofExts) / sizeof(kVexaDesktopSpoofExts[0]);
#endif

const GLubyte* fexfn_impl_libGL_glGetString(GLenum name) {
  GLHOST_TRACE_SCOPE();
#ifdef BUILD_ANDROID
  GLHostLogCurrentEGLState("glGetString");
  // Godot 3's Linux export is GLES-over-GL and rejects anything below desktop
  // OpenGL 3.3. Mali reports "OpenGL ES 3.2", so both the GLES3 and GLES2
  // viability checks fail before a frame is drawn.
  if (name == GL_VERSION) {
    // 4.3 is required: Unity's shader target selection ignores this value and
    // the extension list, and emits `#version 430` either way. Reporting 3.3
    // changes nothing except the driver's expectations, so keep 4.3.
    static const GLubyte kDesktopVersion[] = "4.3.0";
    VEXA_LOGV(
                 "[libGL-host] glGetString(GL_VERSION) desktop spoof => '%s'\n",
                 reinterpret_cast<const char*>(kDesktopVersion));
    return VexaGuestString(kDesktopVersion);
  }
  if (name == GL_SHADING_LANGUAGE_VERSION) {
    static const GLubyte kDesktopShading[] = "4.30";
    VEXA_LOGV(
                 "[libGL-host] glGetString(GL_SHADING_LANGUAGE_VERSION) desktop spoof => '%s'\n",
                 reinterpret_cast<const char*>(kDesktopShading));
    return VexaGuestString(kDesktopShading);
  }
  const GLubyte* result = ::glGetString(name);
  if (!result) {
    static const GLubyte kNoContextString[] = "";
    VEXA_LOGV(
                 "[libGL-host] glGetString(name=0x%x/%s) => null; returning empty string\n",
                 static_cast<unsigned>(name),
                 GLHostGLStringName(name));
    return VexaGuestString(kNoContextString);
  }

  VEXA_LOGV(
               "[libGL-host] glGetString(name=0x%x/%s) => %p text='%.256s'\n",
               static_cast<unsigned>(name),
               GLHostGLStringName(name),
               reinterpret_cast<const void*>(result),
               reinterpret_cast<const char*>(result));
  return VexaGuestString(result);
#else
  return fexldr_ptr_libGL_glGetString(name);
#endif
}

const GLubyte* fexfn_impl_libGL_glGetStringi(GLenum name, GLuint index) {
  GLHOST_TRACE_SCOPE();
#ifdef BUILD_ANDROID
  GLHostLogCurrentEGLState("glGetStringi");
  // GLES2-over-GL refuses to start unless GLAD sees a desktop FBO extension.
  // Framebuffer objects are core on this GLES 3.2 driver; only the name differs.
  // The extra ARB names are the ones Unity checks before it allows compute,
  // shader storage buffers, images, and indirect draws.
  if (name == GL_EXTENSIONS) {
    GLint real_count = 0;
    ::glGetIntegerv(GL_NUM_EXTENSIONS, &real_count);
    if (real_count < 0) {
      real_count = 0;
    }
    const auto real = static_cast<GLuint>(real_count);
    if (index >= real && index < real + kVexaDesktopSpoofExtCount) {
      const char* ext = kVexaDesktopSpoofExts[index - real];
      VEXA_LOGV(
                   "[libGL-host] glGetStringi(GL_EXTENSIONS index=%u) desktop spoof => '%s'\n",
                   static_cast<unsigned>(index),
                   ext);
      return VexaGuestString(reinterpret_cast<const GLubyte*>(ext));
    }
  }
  const GLubyte* result = ::glGetStringi(name, index);
  if (!result) {
    VEXA_LOGV(
                 "[libGL-host] glGetStringi(name=0x%x/%s index=%u) => null\n",
                 static_cast<unsigned>(name),
                 GLHostGLStringName(name),
                 static_cast<unsigned>(index));
    return nullptr;
  }

  VEXA_LOGV(
               "[libGL-host] glGetStringi(name=0x%x/%s index=%u) => %p text='%.256s'\n",
               static_cast<unsigned>(name),
               GLHostGLStringName(name),
               static_cast<unsigned>(index),
               reinterpret_cast<const void*>(result),
               reinterpret_cast<const char*>(result));
  return VexaGuestString(result);
#else
  return fexldr_ptr_libGL_glGetStringi(name, index);
#endif
}

const char* fexfn_impl_libGL_glXQueryExtensionsString(Display* display, int screen) {
  GLHOST_TRACE_SCOPE();
#ifdef BUILD_ANDROID
  static constexpr const char* kGLXExtensions =
    "GLX_ARB_create_context "
    "GLX_ARB_create_context_profile "
    "GLX_EXT_swap_control "
    "GLX_EXT_framebuffer_sRGB";
  GLHostLogCurrentEGLState("glXQueryExtensionsString");
  VEXA_LOGV(
               "[libGL-host] glXQueryExtensionsString(display=%p screen=%d) => '%s'\n",
               reinterpret_cast<void*>(display),
               screen,
               kGLXExtensions);
  return kGLXExtensions;
#else
  return fexldr_ptr_libGL_glXQueryExtensionsString(display, screen);
#endif
}

const char* fexfn_impl_libGL_glXGetClientString(Display* display, int name) {
  GLHOST_TRACE_SCOPE();
#ifdef BUILD_ANDROID
  static constexpr const char* kVendor = "Vexa GLX Compat";
  static constexpr const char* kVersion = "1.4";
  static constexpr const char* kExtensions =
    "GLX_ARB_create_context "
    "GLX_ARB_create_context_profile "
    "GLX_EXT_swap_control "
    "GLX_EXT_framebuffer_sRGB";
  GLHostLogCurrentEGLState("glXGetClientString");
  const char* result = nullptr;
  switch (name) {
    case 1: result = kVendor; break;
    case 2: result = kVersion; break;
    case 3: result = kExtensions; break;
    default: result = kExtensions; break;
  }
  VEXA_LOGV(
               "[libGL-host] glXGetClientString(display=%p name=%d) => '%s'\n",
               reinterpret_cast<void*>(display),
               name,
               result);
  return VexaGuestString(result);
#else
  return fexldr_ptr_libGL_glXGetClientString(display, name);
#endif
}

const char* fexfn_impl_libGL_glXQueryServerString(Display* display, int screen, int name) {
  GLHOST_TRACE_SCOPE();
#ifdef BUILD_ANDROID
  const char* result = fexfn_impl_libGL_glXGetClientString(display, name);
  VEXA_LOGV(
               "[libGL-host] glXQueryServerString(display=%p screen=%d name=%d) => '%s'\n",
               reinterpret_cast<void*>(display),
               screen,
               name,
               result ? result : "<null>");
  return VexaGuestString(result);
#else
  return fexldr_ptr_libGL_glXQueryServerString(display, screen, name);
#endif
}

GLXContext fexfn_impl_libGL_glXCreateNewContext(Display* display, GLXFBConfig config, int render_type, GLXContext share_list, Bool direct) {
  GLHOST_TRACE_SCOPE();
#ifdef BUILD_ANDROID
  GLHostLogCurrentEGLState("glXCreateNewContext");
  const EGLContext current = eglGetCurrentContext();
  GLXContext result = current != EGL_NO_CONTEXT
                        ? reinterpret_cast<GLXContext>(current)
                        : reinterpret_cast<GLXContext>(uintptr_t {1});
  VEXA_LOGV(
               "[libGL-host] glXCreateNewContext(display=%p config=%p render_type=0x%x share=%p direct=%d) => %p\n",
               reinterpret_cast<void*>(display),
               reinterpret_cast<void*>(config),
               render_type,
               reinterpret_cast<void*>(share_list),
               static_cast<int>(direct),
               reinterpret_cast<void*>(result));
  return result;
#else
  return fexldr_ptr_libGL_glXCreateNewContext(display, config, render_type, share_list, direct);
#endif
}

GLXContext fexfn_impl_libGL_glXGetCurrentContext() {
  GLHOST_TRACE_SCOPE();
#ifdef BUILD_ANDROID
  GLHostLogCurrentEGLState("glXGetCurrentContext");
  const EGLContext current = eglGetCurrentContext();
  auto result = current != EGL_NO_CONTEXT ? reinterpret_cast<GLXContext>(current) : nullptr;
  VEXA_LOGV(
               "[libGL-host] glXGetCurrentContext() => %p\n",
               reinterpret_cast<void*>(result));
  return result;
#else
  return fexldr_ptr_libGL_glXGetCurrentContext();
#endif
}

Bool fexfn_impl_libGL_glXMakeCurrent(Display* display, GLXDrawable drawable, GLXContext context) {
  GLHOST_TRACE_SCOPE();
#ifdef BUILD_ANDROID
  GLHostLogCurrentEGLState("glXMakeCurrent(entry)");
  const EGLContext current = eglGetCurrentContext();
  const EGLContext requested = reinterpret_cast<EGLContext>(context);
  VEXA_LOGV(
               "[libGL-host] glXMakeCurrent(display=%p drawable=0x%llx context=%p current=%p)\n",
               reinterpret_cast<void*>(display),
               static_cast<unsigned long long>(drawable),
               reinterpret_cast<void*>(context),
               reinterpret_cast<void*>(current));

  if (requested != EGL_NO_CONTEXT && requested != current) {
    VEXA_LOGV(
                 "[libGL-host] glXMakeCurrent context mismatch requested=%p current=%p (no-op)\n",
                 reinterpret_cast<void*>(requested),
                 reinterpret_cast<void*>(current));
  }
  GLHostLogCurrentEGLState("glXMakeCurrent(exit)");
  return 1;
#else
  return fexldr_ptr_libGL_glXMakeCurrent(display, drawable, context);
#endif
}

// VEXA_FIXES: Android glXGetProcAddress flow prefers shader bridge, then eglGetProcAddress,
// then RTLD_DEFAULT lookup; desktop path keeps upstream GLX behavior.
auto fexfn_impl_libGL_glXGetProcAddress(const GLubyte* name) -> __GLXextFuncPtr {
  GLHOST_TRACE_SCOPE();
  using VoidFn = __GLXextFuncPtr;
  if (!name) {
    VexaAbortThunkNullTarget("glXGetProcAddress", "procname is null", kThunkGLXGetProcAddress);
  }
  const char* const proc_name = reinterpret_cast<const char*>(name);
  GLHostSetLastRequestedProc(proc_name);
  GLHostMarkProgress("glXGetProcAddress");
  if (!proc_name[0]) {
    // VEXA_FIXES: Empty proc names indicate caller bug; log explicitly to catch
    // malformed guest lookups early during runtime bring-up.
    VEXA_LOGV("[libGL-host] glXGetProcAddress received empty proc name\n");
    VexaTraceThunkEvent("<empty>", VEXA_THUNK_EVENT_LOOKUP_MISS, kThunkGLXGetProcAddress, 0, 4);
    return nullptr;
  }
  std::string_view name_sv {proc_name};
#ifdef BUILD_ANDROID
#ifdef IS_32BIT_THUNK
  if (name_sv == "glMapBufferRange") return reinterpret_cast<VoidFn>(fexfn_impl_libGL_glMapBufferRange);
  if (name_sv == "glDeleteBuffers") return reinterpret_cast<VoidFn>(VexaDeleteBuffers32);
  if (name_sv == "glGetBufferPointerv") return reinterpret_cast<VoidFn>(VexaGetBufferPointer32);
  if (name_sv == "glUnmapBuffer") return reinterpret_cast<VoidFn>(fexfn_impl_libGL_glUnmapBuffer);
  if (name_sv == "glFlushMappedBufferRange") return reinterpret_cast<VoidFn>(fexfn_impl_libGL_glFlushMappedBufferRange);
  if (name_sv == "glIsSync") return reinterpret_cast<VoidFn>(fexfn_impl_libGL_glIsSync);
  if (name_sv == "glGetUniformIndices") return reinterpret_cast<VoidFn>(fexfn_impl_libGL_glGetUniformIndices);
  if (name_sv == "glTransformFeedbackVaryings") return reinterpret_cast<VoidFn>(VexaTransformFeedbackVaryings32);
#endif

  VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') lookup\n", proc_name);
  GLHostLogCurrentEGLState("glXGetProcAddress(entry)");
  VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_LOOKUP_FUNCTION, kThunkGLXGetProcAddress);
  if (name_sv == "glGetString" ||
      name_sv == "glGetStringi" ||
      name_sv == "glXQueryExtensionsString" ||
      name_sv == "glXGetClientString" ||
      name_sv == "glXQueryServerString" ||
      name_sv == "glXCreateContext" ||
      name_sv == "glXCreateNewContext" ||
      name_sv == "glXGetCurrentContext" ||
      name_sv == "glXMakeCurrent") {
    VEXA_LOGV(
                 "[libGL-host] glXGetProcAddress('%s') context-sensitive lookup\n",
                 proc_name);
  }
  if (name_sv == "glGetString") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glGetString);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via getstring-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glXQueryExtensionsString") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glXQueryExtensionsString);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via glx-ext-string-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glXGetClientString") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glXGetClientString);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via glx-client-string-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glXQueryServerString") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glXQueryServerString);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via glx-server-string-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glGetStringi") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glGetStringi);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via getstringi-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glXCreateNewContext") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glXCreateNewContext);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via glx-create-new-context-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glXCreateContext") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glXCreateContext);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via glx-create-context-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 11);
    return result;
  }
  if (name_sv == "glXGetCurrentContext") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glXGetCurrentContext);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via glx-get-current-context-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glXMakeCurrent") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glXMakeCurrent);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via glx-make-current-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glShaderSource" || name_sv == "glShaderSourceARB") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_FEX_glShaderSource);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via shader-bridge\n",
                 proc_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 1);
    return result;
  }
  if (name_sv == "glCompileShader") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glCompileShader);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via compile-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 5);
    return result;
  }
  if (name_sv == "glLinkProgram") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glLinkProgram);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via link-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 6);
    return result;
  }
  if (name_sv == "glCreateProgram") return reinterpret_cast<VoidFn>(fexfn_impl_libGL_glCreateProgram);
  if (name_sv == "glGetProgramiv") return reinterpret_cast<VoidFn>(fexfn_impl_libGL_glGetProgramiv);
  if (name_sv == "glDeleteProgram") return reinterpret_cast<VoidFn>(fexfn_impl_libGL_glDeleteProgram);
  if (name_sv == "glDeleteShader") return reinterpret_cast<VoidFn>(fexfn_impl_libGL_glDeleteShader);
  if (name_sv == "glDeleteProgramPipelines") return reinterpret_cast<VoidFn>(fexfn_impl_libGL_glDeleteProgramPipelines);
  if (name_sv == "glCreateShader") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glCreateShader);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via create-shader-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 9);
    return result;
  }
  if (name_sv == "glAttachShader") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glAttachShader);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via attach-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 10);
    return result;
  }
  if (name_sv == "glDetachShader") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glDetachShader);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via detach-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 11);
    return result;
  }
  if (name_sv == "glGetInternalformativ") {
    return reinterpret_cast<VoidFn>(fexfn_impl_libGL_glGetInternalformativ);
  }
  if (name_sv == "glGetIntegerv") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glGetIntegerv);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via getintegerv-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 12);
    return result;
  }
  if (name_sv == "glClientWaitSync") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glClientWaitSync);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via waitsync-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glWaitSync") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glWaitSync);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via waitsync-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glGetSynciv") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glGetSynciv);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via waitsync-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glReadPixels") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glReadPixels);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via io-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glFinish") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glFinish);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via flush-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glFlush") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glFlush);
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via flush-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  // VEXA diag: engines that load every entry point through glXGetProcAddress
  // (Unity) would otherwise get raw driver pointers and bypass the counters.
  {
    struct DiagWrap {
      const char* name;
      VoidFn fn;
    };
    static const DiagWrap kDiagWraps[] = {
        {"glDrawArrays", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glDrawArrays)},
        {"glDrawElements", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glDrawElements)},
        {"glDrawArraysInstanced", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glDrawArraysInstanced)},
        {"glDrawElementsInstanced", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glDrawElementsInstanced)},
        {"glDrawElementsBaseVertex", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glDrawElementsBaseVertex)},
        {"glDrawElementsInstancedBaseVertex",
         reinterpret_cast<VoidFn>(fexfn_impl_libGL_glDrawElementsInstancedBaseVertex)},
        {"glDrawElementsIndirect", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glDrawElementsIndirect)},
        {"glRenderbufferStorage", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glRenderbufferStorage)},
        {"glBindBufferRange", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glBindBufferRange)},
        {"glBindBufferBase", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glBindBufferBase)},
        {"glGetProgramResourceiv", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glGetProgramResourceiv)},
        {"glGetProgramResourceName", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glGetProgramResourceName)},
        {"glGetProgramInterfaceiv", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glGetProgramInterfaceiv)},
        {"glRenderbufferStorageMultisample", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glRenderbufferStorageMultisample)},
        {"glDrawArraysIndirect", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glDrawArraysIndirect)},
        {"glDispatchCompute", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glDispatchCompute)},
        {"glUseProgram", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glUseProgram)},
        {"glBindFramebuffer", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glBindFramebuffer)},
        {"glClear", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glClear)},
        {"glClearColor", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glClearColor)},
        // Desktop-only caps/pnames Unity GLCore sets are rejected by Mali with
        // GL_INVALID_ENUM; route through wrappers that drain the error.
        {"glEnable", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glEnable)},
        {"glDisable", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glDisable)},
        {"glTexParameterf", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glTexParameterf)},
        {"glTexParameteri", reinterpret_cast<VoidFn>(fexfn_impl_libGL_glTexParameteri)},
        {"glSamplerParameterf", reinterpret_cast<VoidFn>(VexaGL_glSamplerParameterf)},
        // Software BCn fallback (Mali has no S3TC/RGTC/BPTC).
        {"glTexStorage2D", reinterpret_cast<VoidFn>(VexaGL_glTexStorage2D)},
        {"glTexImage2D", reinterpret_cast<VoidFn>(VexaGL_glTexImage2D)},
        {"glTexImage3D", reinterpret_cast<VoidFn>(VexaGL_glTexImage3D)},
        {"glTexSubImage2D", reinterpret_cast<VoidFn>(VexaGL_glTexSubImage2D)},
        {"glTexSubImage3D", reinterpret_cast<VoidFn>(VexaGL_glTexSubImage3D)},
        {"glPixelStorei", reinterpret_cast<VoidFn>(VexaGL_glPixelStorei)},
        {"glGetUniformLocation", reinterpret_cast<VoidFn>(VexaGL_glGetUniformLocation)},
        {"glUseProgramStages", reinterpret_cast<VoidFn>(VexaGL_glUseProgramStages)},
        {"glUniformBlockBinding", reinterpret_cast<VoidFn>(VexaGL_glUniformBlockBinding)},
        {"glBindProgramPipeline", reinterpret_cast<VoidFn>(VexaGL_glBindProgramPipeline)},
        {"glActiveShaderProgram", reinterpret_cast<VoidFn>(VexaGL_glActiveShaderProgram)},
        {"glProgramUniform1i", reinterpret_cast<VoidFn>(VexaGL_glProgramUniform1i)},
        {"glProgramUniform1f", reinterpret_cast<VoidFn>(VexaGL_glProgramUniform1f)},
        {"glProgramUniform2f", reinterpret_cast<VoidFn>(VexaGL_glProgramUniform2f)},
        {"glProgramUniform3f", reinterpret_cast<VoidFn>(VexaGL_glProgramUniform3f)},
        {"glProgramUniform4f", reinterpret_cast<VoidFn>(VexaGL_glProgramUniform4f)},
        {"glProgramUniform2i", reinterpret_cast<VoidFn>(VexaGL_glProgramUniform2i)},
        {"glProgramUniform3i", reinterpret_cast<VoidFn>(VexaGL_glProgramUniform3i)},
        {"glProgramUniform4i", reinterpret_cast<VoidFn>(VexaGL_glProgramUniform4i)},
        {"glProgramUniform1ui", reinterpret_cast<VoidFn>(VexaGL_glProgramUniform1ui)},
        {"glProgramUniform2ui", reinterpret_cast<VoidFn>(VexaGL_glProgramUniform2ui)},
        {"glProgramUniform3ui", reinterpret_cast<VoidFn>(VexaGL_glProgramUniform3ui)},
        {"glProgramUniform4ui", reinterpret_cast<VoidFn>(VexaGL_glProgramUniform4ui)},
        {"glUniform1i", reinterpret_cast<VoidFn>(VexaGL_glUniform1i)},
        {"glUniform1f", reinterpret_cast<VoidFn>(VexaGL_glUniform1f)},
        {"glUniform1iv", reinterpret_cast<VoidFn>(VexaGL_glUniform1iv)},
        {"glUniform1fv", reinterpret_cast<VoidFn>(VexaGL_glUniform1fv)},
        {"glUniform2i", reinterpret_cast<VoidFn>(VexaGL_glUniform2i)},
        {"glUniform2f", reinterpret_cast<VoidFn>(VexaGL_glUniform2f)},
        {"glUniform2iv", reinterpret_cast<VoidFn>(VexaGL_glUniform2iv)},
        {"glUniform2fv", reinterpret_cast<VoidFn>(VexaGL_glUniform2fv)},
        {"glUniform3i", reinterpret_cast<VoidFn>(VexaGL_glUniform3i)},
        {"glUniform3f", reinterpret_cast<VoidFn>(VexaGL_glUniform3f)},
        {"glUniform3iv", reinterpret_cast<VoidFn>(VexaGL_glUniform3iv)},
        {"glUniform3fv", reinterpret_cast<VoidFn>(VexaGL_glUniform3fv)},
        {"glUniform4i", reinterpret_cast<VoidFn>(VexaGL_glUniform4i)},
        {"glUniform4f", reinterpret_cast<VoidFn>(VexaGL_glUniform4f)},
        {"glUniform4iv", reinterpret_cast<VoidFn>(VexaGL_glUniform4iv)},
        {"glUniform4fv", reinterpret_cast<VoidFn>(VexaGL_glUniform4fv)},
        {"glUniformMatrix2fv", reinterpret_cast<VoidFn>(VexaGL_glUniformMatrix2fv)},
        {"glUniformMatrix3fv", reinterpret_cast<VoidFn>(VexaGL_glUniformMatrix3fv)},
        {"glUniformMatrix4fv", reinterpret_cast<VoidFn>(VexaGL_glUniformMatrix4fv)},
        {"glDeleteTextures", reinterpret_cast<VoidFn>(VexaGL_glDeleteTextures)},
        {"glTexStorage3D", reinterpret_cast<VoidFn>(VexaGL_glTexStorage3D)},
        {"glCompressedTexImage2D", reinterpret_cast<VoidFn>(VexaGL_glCompressedTexImage2D)},
        {"glCompressedTexImage2DARB", reinterpret_cast<VoidFn>(VexaGL_glCompressedTexImage2D)},
        {"glCompressedTexSubImage2D", reinterpret_cast<VoidFn>(VexaGL_glCompressedTexSubImage2D)},
        {"glCompressedTexSubImage2DARB", reinterpret_cast<VoidFn>(VexaGL_glCompressedTexSubImage2D)},
        {"glCompressedTexImage3D", reinterpret_cast<VoidFn>(VexaGL_glCompressedTexImage3D)},
        {"glCompressedTexImage3DARB", reinterpret_cast<VoidFn>(VexaGL_glCompressedTexImage3D)},
        {"glCompressedTexSubImage3D", reinterpret_cast<VoidFn>(VexaGL_glCompressedTexSubImage3D)},
        {"glCompressedTexSubImage3DARB", reinterpret_cast<VoidFn>(VexaGL_glCompressedTexSubImage3D)},
    };
    for (const auto& w : kDiagWraps) {
      if (name_sv == w.name) {
        VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via diag-wrapper\n", proc_name,
                reinterpret_cast<void*>(w.fn));
        return w.fn;
      }
    }
  }
  auto* android_name = proc_name;
  if (auto egl_proc = reinterpret_cast<VoidFn>(eglGetProcAddress(android_name)); egl_proc) {
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via eglGetProcAddress\n",
                 android_name, reinterpret_cast<void*>(egl_proc));
    VexaTraceThunkEvent(android_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(egl_proc)), 2);
    return egl_proc;
  }
  VEXA_LOGV(
               "[libGL-host] glXGetProcAddress('%s') eglGetProcAddress miss; trying dlsym(RTLD_DEFAULT)\n",
               android_name);
  dlerror();
  auto result = reinterpret_cast<VoidFn>(dlsym(RTLD_DEFAULT, android_name));
  const char* dlsym_error = dlerror();
  if (!result) {
    VEXA_LOGV(
                 "[libGL-host] glXGetProcAddress('%s') => null (dlsym_err=%s)\n",
                 android_name,
                 dlsym_error ? dlsym_error : "<none>");
    GLHostLogCurrentEGLState("glXGetProcAddress(null-result)");
    VexaTraceThunkEvent(android_name, VEXA_THUNK_EVENT_LOOKUP_MISS, kThunkGLXGetProcAddress, 0, 3);
  } else {
    VEXA_LOGV("[libGL-host] glXGetProcAddress('%s') => %p via dlsym\n",
                 android_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(android_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 3);
  }
  return result;
#else
  if (name_sv == "glCompileShaderIncludeARB") {
    return (VoidFn)fexfn_impl_libGL_glCompileShaderIncludeARB;
  } else if (name_sv == "glCreateShaderProgramv") {
    return (VoidFn)fexfn_impl_libGL_glCreateShaderProgramv;
  } else if (name_sv == "glGetBufferPointerv") {
    return (VoidFn)fexfn_impl_libGL_glGetBufferPointerv;
  } else if (name_sv == "glGetBufferPointervARB") {
    return (VoidFn)fexfn_impl_libGL_glGetBufferPointervARB;
  } else if (name_sv == "glGetNamedBufferPointerv") {
    return (VoidFn)fexfn_impl_libGL_glGetNamedBufferPointerv;
  } else if (name_sv == "glGetNamedBufferPointervEXT") {
    return (VoidFn)fexfn_impl_libGL_glGetNamedBufferPointervEXT;
  } else if (name_sv == "glGetPointerv") {
    return (VoidFn)fexfn_impl_libGL_glGetPointerv;
  } else if (name_sv == "glGetPointervEXT") {
    return (VoidFn)fexfn_impl_libGL_glGetPointervEXT;
  } else if (name_sv == "glGetPointeri_vEXT") {
    return (VoidFn)fexfn_impl_libGL_glGetPointeri_vEXT;
  } else if (name_sv == "glGetPointerIndexedvEXT") {
    return (VoidFn)fexfn_impl_libGL_glGetPointerIndexedvEXT;
  } else if (name_sv == "glGetVariantPointervEXT") {
    return (VoidFn)fexfn_impl_libGL_glGetVariantPointervEXT;
  } else if (name_sv == "glGetVertexAttribPointervARB") {
    return (VoidFn)fexfn_impl_libGL_glGetVertexAttribPointervARB;
  } else if (name_sv == "glGetVertexAttribPointerv") {
    return (VoidFn)fexfn_impl_libGL_glGetVertexAttribPointerv;
  } else if (name_sv == "glGetVertexAttribPointervNV") {
    return (VoidFn)fexfn_impl_libGL_glGetVertexAttribPointervNV;
  } else if (name_sv == "glGetVertexArrayPointeri_vEXT") {
    return (VoidFn)fexfn_impl_libGL_glGetVertexArrayPointeri_vEXT;
  } else if (name_sv == "glGetVertexArrayPointervEXT") {
    return (VoidFn)fexfn_impl_libGL_glGetVertexArrayPointervEXT;
  } else if (name_sv == "glShaderSource") {
    return (VoidFn)fexfn_impl_libGL_FEX_glShaderSource;
  } else if (name_sv == "glShaderSourceARB") {
    return (VoidFn)fexfn_impl_libGL_FEX_glShaderSource;
#ifdef IS_32BIT_THUNK
  } else if (name_sv == "glBindBuffersRange") {
    return (VoidFn)fexfn_impl_libGL_glBindBuffersRange;
  } else if (name_sv == "glBindVertexBuffers") {
    return (VoidFn)fexfn_impl_libGL_glBindVertexBuffers;
  } else if (name_sv == "glGetUniformIndices") {
    return (VoidFn)fexfn_impl_libGL_glGetUniformIndices;
  } else if (name_sv == "glVertexArrayVertexBuffers") {
    return (VoidFn)fexfn_impl_libGL_glVertexArrayVertexBuffers;
#endif
  } else if (name_sv == "glXChooseFBConfig") {
    return (VoidFn)fexfn_impl_libGL_glXChooseFBConfig;
  } else if (name_sv == "glXChooseFBConfigSGIX") {
    return (VoidFn)fexfn_impl_libGL_glXChooseFBConfigSGIX;
  } else if (name_sv == "glXGetCurrentDisplay") {
    return (VoidFn)fexfn_impl_libGL_glXGetCurrentDisplay;
  } else if (name_sv == "glXGetCurrentDisplayEXT") {
    return (VoidFn)fexfn_impl_libGL_glXGetCurrentDisplayEXT;
  } else if (name_sv == "glXGetFBConfigs") {
    return (VoidFn)fexfn_impl_libGL_glXGetFBConfigs;
  } else if (name_sv == "glXGetFBConfigFromVisualSGIX") {
    return (VoidFn)fexfn_impl_libGL_glXGetFBConfigFromVisualSGIX;
  } else if (name_sv == "glXGetVisualFromFBConfigSGIX") {
    return (VoidFn)fexfn_impl_libGL_glXGetVisualFromFBConfigSGIX;
  } else if (name_sv == "glXChooseVisual") {
    return (VoidFn)fexfn_impl_libGL_glXChooseVisual;
  } else if (name_sv == "glXCreateContext") {
    return (VoidFn)fexfn_impl_libGL_glXCreateContext;
  } else if (name_sv == "glXCreateGLXPixmap") {
    return (VoidFn)fexfn_impl_libGL_glXCreateGLXPixmap;
  } else if (name_sv == "glXCreateGLXPixmapMESA") {
    return (VoidFn)fexfn_impl_libGL_glXCreateGLXPixmapMESA;
  } else if (name_sv == "glXGetConfig") {
    return (VoidFn)fexfn_impl_libGL_glXGetConfig;
  } else if (name_sv == "glXGetVisualFromFBConfig") {
    return (VoidFn)fexfn_impl_libGL_glXGetVisualFromFBConfig;
#ifdef IS_32BIT_THUNK
  } else if (name_sv == "glXGetSelectedEvent") {
    return (VoidFn)fexfn_impl_libGL_glXGetSelectedEvent;
  } else if (name_sv == "glXGetSelectedEventSGIX") {
    return (VoidFn)fexfn_impl_libGL_glXGetSelectedEventSGIX;
#endif
  }
  return (VoidFn)glXGetProcAddress((const GLubyte*)name);
#endif
}

#ifdef BUILD_ANDROID
extern "C" __attribute__((visibility("default")))
__GLXextFuncPtr glXGetProcAddress(const GLubyte* name) {
  GLHOST_TRACE_SCOPE();
  return fexfn_impl_libGL_glXGetProcAddress(name);
}

extern "C" __attribute__((visibility("default")))
void GL_SetGuestMalloc(uintptr_t guest_target, uintptr_t guest_unpacker) {
  GLHOST_TRACE_SCOPE();
  fexfn_impl_libGL_GL_SetGuestMalloc(guest_target, guest_unpacker);
}

extern "C" __attribute__((visibility("default")))
void GL_SetGuestXSync(uintptr_t guest_target, uintptr_t guest_unpacker) {
  GLHOST_TRACE_SCOPE();
  fexfn_impl_libGL_GL_SetGuestXSync(guest_target, guest_unpacker);
}

extern "C" __attribute__((visibility("default")))
void GL_SetGuestXGetVisualInfo(uintptr_t guest_target, uintptr_t guest_unpacker) {
  GLHOST_TRACE_SCOPE();
  fexfn_impl_libGL_GL_SetGuestXGetVisualInfo(guest_target, guest_unpacker);
}

extern "C" __attribute__((visibility("default")))
void GL_SetGuestXDisplayString(uintptr_t guest_target, uintptr_t guest_unpacker) {
  GLHOST_TRACE_SCOPE();
  fexfn_impl_libGL_GL_SetGuestXDisplayString(guest_target, guest_unpacker);
}

extern "C" __attribute__((visibility("default")))
void glClearDepthfOES(GLclampf depth) {
  GLHOST_TRACE_SCOPE();
  ::glClearDepthf(static_cast<GLfloat>(depth));
}

extern "C" __attribute__((visibility("default")))
void glClearDepth(GLclampd depth) {
  GLHOST_TRACE_SCOPE();
  ::glClearDepthf(static_cast<GLfloat>(depth));
}

extern "C" __attribute__((visibility("default")))
void glPointSize(GLfloat size) {
  GLHOST_TRACE_SCOPE();
  using GLPointSizeFn = void (*)(GLfloat);
  static GLPointSizeFn fn = []() -> GLPointSizeFn {
    if (void* gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL)) {
      if (auto p = reinterpret_cast<GLPointSizeFn>(dlsym(gles, "glPointSize"))) {
        return p;
      }
    }
    if (void* gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL)) {
      if (auto p = reinterpret_cast<GLPointSizeFn>(dlsym(gles, "glPointSize"))) {
        return p;
      }
    }
    return reinterpret_cast<GLPointSizeFn>(eglGetProcAddress("glPointSize"));
  }();

  if (fn) {
    fn(size);
  } else {
    VEXA_LOGV("[libGL-host] glPointSize unresolved; ignoring size=%f\n",
                 static_cast<double>(size));
  }
}

extern "C" __attribute__((visibility("default")))
void glQueryCounter(GLuint id, GLenum target) {
  GLHOST_TRACE_SCOPE();
  using GLQueryCounterFn = void (*)(GLuint, GLenum);
  static GLQueryCounterFn fn = []() -> GLQueryCounterFn {
    if (auto p = reinterpret_cast<GLQueryCounterFn>(eglGetProcAddress("glQueryCounter"))) {
      return p;
    }
    if (auto p = reinterpret_cast<GLQueryCounterFn>(eglGetProcAddress("glQueryCounterEXT"))) {
      return p;
    }
    if (void* gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL)) {
      if (auto p = reinterpret_cast<GLQueryCounterFn>(dlsym(gles, "glQueryCounterEXT"))) {
        return p;
      }
    }
    if (void* gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL)) {
      if (auto p = reinterpret_cast<GLQueryCounterFn>(dlsym(gles, "glQueryCounterEXT"))) {
        return p;
      }
    }
    return reinterpret_cast<GLQueryCounterFn>(dlsym(RTLD_DEFAULT, "glQueryCounterEXT"));
  }();

  if (fn) {
    fn(id, target);
  } else {
    VEXA_LOGV(
                 "[libGL-host] glQueryCounter unresolved; id=%u target=0x%x\n",
                 static_cast<unsigned>(id),
                 static_cast<unsigned>(target));
  }
}

extern "C" __attribute__((visibility("default")))
void glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64* params) {
  GLHOST_TRACE_SCOPE();
  using GLGetQueryObjectui64vFn = void (*)(GLuint, GLenum, GLuint64*);
  static GLGetQueryObjectui64vFn fn = []() -> GLGetQueryObjectui64vFn {
    if (auto p = reinterpret_cast<GLGetQueryObjectui64vFn>(eglGetProcAddress("glGetQueryObjectui64v"))) {
      return p;
    }
    if (auto p = reinterpret_cast<GLGetQueryObjectui64vFn>(eglGetProcAddress("glGetQueryObjectui64vEXT"))) {
      return p;
    }
    if (void* gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL)) {
      if (auto p = reinterpret_cast<GLGetQueryObjectui64vFn>(dlsym(gles, "glGetQueryObjectui64vEXT"))) {
        return p;
      }
    }
    if (void* gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL)) {
      if (auto p = reinterpret_cast<GLGetQueryObjectui64vFn>(dlsym(gles, "glGetQueryObjectui64vEXT"))) {
        return p;
      }
    }
    return reinterpret_cast<GLGetQueryObjectui64vFn>(dlsym(RTLD_DEFAULT, "glGetQueryObjectui64vEXT"));
  }();

  if (fn) {
    fn(id, pname, params);
  } else {
    if (params) {
      *params = 0;
    }
    VEXA_LOGV(
                 "[libGL-host] glGetQueryObjectui64v unresolved; id=%u pname=0x%x\n",
                 static_cast<unsigned>(id),
                 static_cast<unsigned>(pname));
  }
}

extern "C" __attribute__((visibility("default")))
void glBufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
  GLHOST_TRACE_SCOPE();
  GLenum usage = GL_STATIC_DRAW;
#ifdef GL_DYNAMIC_STORAGE_BIT
  if ((flags & GL_DYNAMIC_STORAGE_BIT) != 0) {
    usage = GL_DYNAMIC_DRAW;
  }
#endif
  ::glBufferData(target, size, data, usage);
}

extern "C" __attribute__((visibility("default")))
void glGenVertexArraysAPPLE(GLsizei n, GLuint* arrays) {
  GLHOST_TRACE_SCOPE();
  ::glGenVertexArrays(n, arrays);
}

extern "C" __attribute__((visibility("default")))
void glDeleteVertexArraysAPPLE(GLsizei n, const GLuint* arrays) {
  GLHOST_TRACE_SCOPE();
  ::glDeleteVertexArrays(n, arrays);
}

extern "C" __attribute__((visibility("default")))
void glBindVertexArrayAPPLE(GLuint array) {
  GLHOST_TRACE_SCOPE();
  ::glBindVertexArray(array);
}

extern "C" __attribute__((visibility("default")))
GLboolean glIsVertexArrayAPPLE(GLuint array) {
  GLHOST_TRACE_SCOPE();
  return ::glIsVertexArray(array);
}

extern "C" __attribute__((visibility("default")))
void glBlitFramebufferEXT(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                          GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                          GLbitfield mask, GLenum filter) {
  GLHOST_TRACE_SCOPE();
  VexaBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

extern "C" __attribute__((visibility("default")))
void glDisableClientState(GLenum /*array*/) {
  GLHOST_TRACE_SCOPE();
}
#endif

// TODO: unsigned int *glXEnumerateVideoDevicesNV (Display *dpy, int screen, int *nelements);

#ifdef BUILD_ANDROID
GLenum fexfn_impl_libGL_glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
  GLHOST_TRACE_SCOPE();
  ScopedGLCallTimer timer {"glClientWaitSync"};
  GLenum r = ::glClientWaitSync(VexaImportSync(sync), flags, timeout);
  ++g_vexa_diag_waits;
  // A zero-timeout poll normally returns TIMEOUT_EXPIRED while the GPU is
  // working. Logging each poll adds I/O to a rendering hot path.
  if (GLHostTraceEnabled() || g_vexa_diag_waits <= 4 ||
      (g_vexa_diag_waits & 2047) == 0 || r == GL_WAIT_FAILED) {
    fprintf(stderr, "[VEXA][GL] clientwait n=%llu sync=%p flags=%x tmo=%llu -> %x tid=%ld\n",
            (unsigned long long)g_vexa_diag_waits, sync, flags, (unsigned long long)timeout, r,
            (long)syscall(SYS_gettid));
  }
  return r;
}

void fexfn_impl_libGL_glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
  GLHOST_TRACE_SCOPE();
  ScopedGLCallTimer timer {"glWaitSync"};
  ::glWaitSync(VexaImportSync(sync), flags, timeout);
}

void fexfn_impl_libGL_glGetSynciv(GLsync sync, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* values) {
  GLHOST_TRACE_SCOPE();
  ScopedGLCallTimer timer {"glGetSynciv"};
  ::glGetSynciv(VexaImportSync(sync), pname, bufSize, length, values);
}

void fexfn_impl_libGL_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
  GLHOST_TRACE_SCOPE();
  ScopedGLCallTimer timer {"glReadPixels"};
  static int rp_logged = 0;
  if (rp_logged < 8) {
    ++rp_logged;
    fprintf(stderr, "[VEXA][GL] readpixels %d,%d %dx%d fmt=%x type=%x fbo=%u\n", x, y, width, height, format, type,
            g_vexa_diag_curfbo);
  }
  ::glReadPixels(x, y, width, height, format, type, pixels);
}

void fexfn_impl_libGL_glFinish() {
  GLHOST_TRACE_SCOPE();
  ScopedGLCallTimer timer {"glFinish"};
  ::glFinish();
}

void fexfn_impl_libGL_glFlush() {
  GLHOST_TRACE_SCOPE();
  ScopedGLCallTimer timer {"glFlush"};
  ::glFlush();
}


GLuint fexfn_impl_libGL_glCreateProgram() {
  VEXA_PROGRAM_LOCK();
  VexaCollectProgramState();
  const GLuint program = ::glCreateProgram();
  if (program) {
    g_vexa_prog_sources.erase(program);
    g_vexa_prog_direct_source.erase(program);
    g_vexa_linked_programs.erase(program);
  }
  return program;
}

void fexfn_impl_libGL_glDeleteProgram(GLuint program) {
  VEXA_PROGRAM_LOCK();
  if (!program || !::glIsProgram(program)) { ::glDeleteProgram(program); return; }
  VexaProgramContext().Shared->Deleted.insert(program);
  VexaCollectProgramState();
}

void fexfn_impl_libGL_glGetProgramiv(GLuint program, GLenum pname, GLint* value) {
  VEXA_PROGRAM_LOCK();
  if (pname == GL_DELETE_STATUS && VexaProgramContext().Shared->Deleted.contains(program)) {
    *value = GL_TRUE;
    return;
  }
  ::glGetProgramiv(program, pname, value);
}

void fexfn_impl_libGL_glDeleteShader(GLuint shader) {
  VEXA_PROGRAM_LOCK();
  ::glDeleteShader(shader);
  // A deleted but attached shader remains valid; the driver retains its
  // translated source until the last attachment goes away.
  if (shader) VexaProgramContext().Shared->DeletedShaders.insert(shader);
  VexaCollectProgramState();
}

void fexfn_impl_libGL_glDeleteProgramPipelines(GLsizei count, const GLuint* pipelines) {
  VEXA_PROGRAM_LOCK();
  if (count < 0 || (count && !pipelines)) { ::glDeleteProgramPipelines(count, pipelines); return; }
  ::glDeleteProgramPipelines(count, pipelines);
  for (GLsizei i = 0; i < count; ++i) {
    VexaReleasePipelineClone(pipelines[i], false);
    g_vexa_pipe_state.erase(pipelines[i]);
    if (g_vexa_intended_pipe == pipelines[i]) {
      g_vexa_intended_pipe = 0;
      g_vexa_stage_program = 0;
    }
  }
  VexaCollectProgramState();
}

GLuint fexfn_impl_libGL_glCreateShader(GLenum a_0) {
  VEXA_PROGRAM_LOCK();
  GLHOST_TRACE_SCOPE();
  const GLuint shader = ::glCreateShader(a_0);
  if (shader) {
    VexaShaderSourceById.erase(shader);
    VexaProgramContext().Shared->CompiledSources.erase(shader);
    VexaProgramContext().Shared->DeletedShaders.erase(shader);
  }
  VEXA_LOGV("[libGL-host] glCreateShader(type=0x%x) => %u\n",
               static_cast<unsigned>(a_0), static_cast<unsigned>(shader));
  return shader;
}

void fexfn_impl_libGL_glAttachShader(GLuint a_0, GLuint a_1) {
  GLHOST_TRACE_SCOPE();
  GLint shader_type = 0;
  GLint compile_status = GL_FALSE;
  GLint source_len = 0;
  if (a_1 != 0) {
    ::glGetShaderiv(a_1, GL_SHADER_TYPE, &shader_type);
    ::glGetShaderiv(a_1, GL_COMPILE_STATUS, &compile_status);
    ::glGetShaderiv(a_1, GL_SHADER_SOURCE_LENGTH, &source_len);
  }

  VEXA_LOGV(
               "[libGL-host] glAttachShader(program=%u shader=%u type=0x%x compiled=%d source_len=%d)\n",
               static_cast<unsigned>(a_0),
               static_cast<unsigned>(a_1),
               static_cast<unsigned>(shader_type),
               compile_status == GL_TRUE ? 1 : 0,
               static_cast<int>(source_len));
#if defined(VEXA_ENABLE_RUNTIME_SHADER_TRANSLATOR)
  char detail[224] {};
  std::snprintf(detail, sizeof(detail),
                "program=%u shader=%u type=0x%x compiled=%d source_len=%d",
                static_cast<unsigned>(a_0),
                static_cast<unsigned>(a_1),
                static_cast<unsigned>(shader_type),
                compile_status == GL_TRUE ? 1 : 0,
                static_cast<int>(source_len));
  LogShaderDriverError("attach-call", a_0, detail);
#endif
  ::glAttachShader(a_0, a_1);
  VEXA_LOGV("[libGL-host] glAttachShader done program=%u shader=%u\n",
               static_cast<unsigned>(a_0), static_cast<unsigned>(a_1));
}

void fexfn_impl_libGL_glDetachShader(GLuint a_0, GLuint a_1) {
  GLHOST_TRACE_SCOPE();
  GLint shader_type = 0;
  if (a_1 != 0) {
    ::glGetShaderiv(a_1, GL_SHADER_TYPE, &shader_type);
  }
  VEXA_LOGV(
               "[libGL-host] glDetachShader(program=%u shader=%u type=0x%x)\n",
               static_cast<unsigned>(a_0),
               static_cast<unsigned>(a_1),
               static_cast<unsigned>(shader_type));
#if defined(VEXA_ENABLE_RUNTIME_SHADER_TRANSLATOR)
  char detail[192] {};
  std::snprintf(detail, sizeof(detail),
                "program=%u shader=%u type=0x%x",
                static_cast<unsigned>(a_0),
                static_cast<unsigned>(a_1),
                static_cast<unsigned>(shader_type));
  LogShaderDriverError("detach-call", a_0, detail);
#endif
  ::glDetachShader(a_0, a_1);
}

void fexfn_impl_libGL_glShaderSource(GLuint a_0, GLsizei count, guest_layout<const GLchar* const*> a_2, const GLint* a_3) {
  GLHOST_TRACE_SCOPE();
  fexfn_impl_libGL_FEX_glShaderSource(a_0, count, reinterpret_cast<uintptr_t>(a_2.force_get_host_pointer()), a_3);
}

void fexfn_impl_libGL_glShaderSourceARB(GLuint a_0, GLsizei count, guest_layout<const GLcharARB**> a_2, const GLint* a_3) {
  GLHOST_TRACE_SCOPE();
  fexfn_impl_libGL_FEX_glShaderSource(a_0, count, reinterpret_cast<uintptr_t>(a_2.force_get_host_pointer()), a_3);
}

void fexfn_impl_libGL_glCompileShader(GLuint a_0) {
  VEXA_PROGRAM_LOCK();
  GLHOST_TRACE_SCOPE();
  ::glCompileShader(a_0);
  GLint compiled = GL_FALSE;
  ::glGetShaderiv(a_0, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE) VexaSnapshotCompiledShader(a_0);
  if (compiled != GL_TRUE) {
    static std::atomic<int> logs {0};
    if (logs.fetch_add(1) < 16) {
      GLint shader_type = 0;
      GLint log_len = 0;
      ::glGetShaderiv(a_0, GL_SHADER_TYPE, &shader_type);
      ::glGetShaderiv(a_0, GL_INFO_LOG_LENGTH, &log_len);
      std::string info;
      if (log_len > 1 && log_len < 4096) {
        info.resize(static_cast<size_t>(log_len));
        GLsizei written = 0;
        ::glGetShaderInfoLog(a_0, log_len, &written, info.data());
      }
      VEXA_LOGV("[libGL-host] shader compile failed type=0x%x log=%.240s\n",
              static_cast<unsigned>(shader_type), info.c_str());
      VexaDumpFailedShaderSource(a_0, info.c_str());
    }
  }
#if defined(VEXA_ENABLE_RUNTIME_SHADER_TRANSLATOR)
  LogFailedShaderCompile(a_0);
#endif
}

void fexfn_impl_libGL_glLinkProgram(GLuint a_0) {
  VEXA_PROGRAM_LOCK();
  GLHOST_TRACE_SCOPE();
  VEXA_LOGV("[libGL-host] glLinkProgram(%u) enter\n", static_cast<unsigned>(a_0));
  g_vexa_linked_programs.erase(a_0);
  ::glLinkProgram(a_0);
  GLint failed_status = 0;
  ::glGetProgramiv(a_0, GL_LINK_STATUS, &failed_status);
#ifdef BUILD_ANDROID
  if (failed_status) {
    VexaSnapshotProgramSources(a_0);
    VexaRefreshProgramGeneration();
    VexaLinkGrassBuffer(a_0);
  }
#endif
  if (access("/data/data/com.critical.vexaemulator/files/unity_shader_dump.flag", F_OK) == 0) {
    GLuint shaders[8] = {};
    GLsizei count = 0;
    ::glGetAttachedShaders(a_0, 8, &count, shaders);
    for (int i = 0; i < count; ++i) {
      GLint length = 0;
      ::glGetShaderiv(shaders[i], GL_SHADER_SOURCE_LENGTH, &length);
      if (length <= 0 || length > 1000000) continue;
      std::string source(length, '\0');
      ::glGetShaderSource(shaders[i], length, nullptr, source.data());
      char path[256];
      snprintf(path, sizeof(path), "/data/data/com.critical.vexaemulator/files/artifacts/unity-%sprogram-%u-%d.glsl",
               failed_status ? "" : "failed-", a_0, i);
      if (FILE* file = fopen(path, "wb")) { fwrite(source.data(), 1, source.size(), file); fclose(file); }
    }
  }
  GLint link_status = 0;
  ::glGetProgramiv(a_0, GL_LINK_STATUS, &link_status);
  GLint log_len = 0;
  ::glGetProgramiv(a_0, GL_INFO_LOG_LENGTH, &log_len);
  VEXA_LOGV("[libGL-host] glLinkProgram(%u) status=%d log_len=%d\n",
               static_cast<unsigned>(a_0), link_status, static_cast<int>(log_len));
  if (log_len > 1 && log_len < 8192) {
    std::string info(static_cast<size_t>(log_len), '\0');
    GLsizei written = 0;
    ::glGetProgramInfoLog(a_0, log_len, &written, info.data());
    VEXA_LOGV("[libGL-host] link log: %s\n", info.c_str());
  }
#if defined(VEXA_ENABLE_RUNTIME_SHADER_TRANSLATOR)
  LogFailedProgramLink(a_0);
#endif
}

void fexfn_impl_libGL_glGetIntegerv(GLenum a_0, GLint* a_1) {
  GLHOST_TRACE_SCOPE();
  if (!a_1) {
    return;
  }

  // VEXA_FIXES: GLES drivers often report GL_DOUBLEBUFFER as 0 even when the
  // EGL window surface is presented via eglSwapBuffers. Many desktop clients
  // treat this as a fatal capability mismatch and stop rendering.
  if (a_0 == GL_DOUBLEBUFFER) {
    *a_1 = 1;
    return;
  }
  if (a_0 == GL_MAJOR_VERSION) {
    *a_1 = 4;
    return;
  }
  if (a_0 == GL_MINOR_VERSION) {
    *a_1 = 3;
    return;
  }
  if (a_0 == GL_CONTEXT_PROFILE_MASK) {
    *a_1 = GL_CONTEXT_CORE_PROFILE_BIT;
    return;
  }
  if (a_0 == GL_NUM_EXTENSIONS) {
    GLint real_count = 0;
    ::glGetIntegerv(GL_NUM_EXTENSIONS, &real_count);
    if (real_count < 0) {
      real_count = 0;
    }
    *a_1 = real_count + static_cast<GLint>(kVexaDesktopSpoofExtCount);
    return;
  }

  ::glGetIntegerv(a_0, a_1);
  // VEXA diag: engines may cache the "default framebuffer" from this query.
  if (a_0 == GL_DRAW_FRAMEBUFFER_BINDING || a_0 == GL_READ_FRAMEBUFFER_BINDING) {
    static int fbq_logged = 0;
    if (fbq_logged < 40) {
      ++fbq_logged;
      fprintf(stderr, "[VEXA][GL] guest query %s -> %d tid=%ld\n",
              a_0 == GL_DRAW_FRAMEBUFFER_BINDING ? "DRAW_FRAMEBUFFER_BINDING" : "READ_FRAMEBUFFER_BINDING", *a_1,
              (long)syscall(SYS_gettid));
    }
  }
}
#endif

#ifndef BUILD_ANDROID
void fexfn_impl_libGL_glCompileShaderIncludeARB(GLuint a_0, GLsizei Count, guest_layout<const GLchar* const*> a_2, const GLint* a_3) {
  GLHOST_TRACE_SCOPE();
#ifndef IS_32BIT_THUNK
  auto sources = a_2.force_get_host_pointer();
#else
  auto sources = (const char**)alloca(Count * sizeof(const char*));
  for (GLsizei i = 0; i < Count; ++i) {
    sources[i] = host_layout<const char* const> {a_2.get_pointer()[i]}.data;
  }
#endif
  return fexldr_ptr_libGL_glCompileShaderIncludeARB(a_0, Count, sources, a_3);
}

GLuint fexfn_impl_libGL_glCreateShaderProgramv(GLuint a_0, GLsizei count, guest_layout<const GLchar* const*> a_2) {
  GLHOST_TRACE_SCOPE();
#ifndef IS_32BIT_THUNK
  auto sources = a_2.force_get_host_pointer();
#else
  auto sources = (const char**)alloca(count * sizeof(const char*));
  for (GLsizei i = 0; i < count; ++i) {
    sources[i] = host_layout<const char* const> {a_2.get_pointer()[i]}.data;
  }
#endif
  VEXA_PROGRAM_LOCK();
  VexaCollectProgramState();
  const GLuint prog = fexldr_ptr_libGL_glCreateShaderProgramv(a_0, count, sources);
  if (prog != 0 && sources != nullptr) {
    g_vexa_prog_sources.erase(prog);
    g_vexa_linked_programs.erase(prog);
    std::string joined;
    for (GLsizei i = 0; i < count; ++i) {
      if (sources[i]) joined += sources[i];
    }
    if (!joined.empty()) g_vexa_prog_direct_source[prog] = {a_0, std::move(joined)};
  }
  return prog;
}

void fexfn_impl_libGL_glGetBufferPointerv(GLenum a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  GLHOST_TRACE_SCOPE();
  void* HostOut;
  fexldr_ptr_libGL_glGetBufferPointerv(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetBufferPointervARB(GLenum a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  GLHOST_TRACE_SCOPE();
  void* HostOut;
  fexldr_ptr_libGL_glGetBufferPointervARB(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetNamedBufferPointerv(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  GLHOST_TRACE_SCOPE();
  void* HostOut;
  fexldr_ptr_libGL_glGetNamedBufferPointerv(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetNamedBufferPointervEXT(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  GLHOST_TRACE_SCOPE();
  void* HostOut;
  fexldr_ptr_libGL_glGetNamedBufferPointervEXT(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetPointerv(GLenum a_0, guest_layout<void**> GuestOut) {
  GLHOST_TRACE_SCOPE();
  void* HostOut;
  fexldr_ptr_libGL_glGetPointerv(a_0, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetPointervEXT(GLenum a_0, guest_layout<void**> GuestOut) {
  GLHOST_TRACE_SCOPE();
  void* HostOut;
  fexldr_ptr_libGL_glGetPointervEXT(a_0, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetPointeri_vEXT(GLenum a_0, GLuint a_1, guest_layout<void**> GuestOut) {
  GLHOST_TRACE_SCOPE();
  void* HostOut;
  fexldr_ptr_libGL_glGetPointeri_vEXT(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetPointerIndexedvEXT(GLenum a_0, GLuint a_1, guest_layout<void**> GuestOut) {
  GLHOST_TRACE_SCOPE();
  void* HostOut;
  fexldr_ptr_libGL_glGetPointerIndexedvEXT(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVariantPointervEXT(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  GLHOST_TRACE_SCOPE();
  void* HostOut;
  fexldr_ptr_libGL_glGetVariantPointervEXT(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVertexAttribPointervARB(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  GLHOST_TRACE_SCOPE();
  void* HostOut;
  fexldr_ptr_libGL_glGetVertexAttribPointervARB(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVertexAttribPointerv(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  GLHOST_TRACE_SCOPE();
  void* HostOut;
  fexldr_ptr_libGL_glGetVertexAttribPointerv(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVertexAttribPointervNV(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  GLHOST_TRACE_SCOPE();
  void* HostOut;
  fexldr_ptr_libGL_glGetVertexAttribPointervNV(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVertexArrayPointeri_vEXT(GLuint a_0, GLuint a_1, GLenum a_2, guest_layout<void**> GuestOut) {
  GLHOST_TRACE_SCOPE();
  void* HostOut;
  fexldr_ptr_libGL_glGetVertexArrayPointeri_vEXT(a_0, a_1, a_2, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVertexArrayPointervEXT(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  GLHOST_TRACE_SCOPE();
  void* HostOut;
  fexldr_ptr_libGL_glGetVertexArrayPointervEXT(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glShaderSource(GLuint a_0, GLsizei count, guest_layout<const GLchar* const*> a_2, const GLint* a_3) {
  GLHOST_TRACE_SCOPE();
#ifndef IS_32BIT_THUNK
  auto sources = a_2.force_get_host_pointer();
#else
  auto sources = (const char**)alloca(count * sizeof(const char*));
  for (GLsizei i = 0; i < count; ++i) {
    sources[i] = host_layout<const char* const> {a_2.get_pointer()[i]}.data;
  }
#endif
  return fexldr_ptr_libGL_glShaderSource(a_0, count, sources, a_3);
}

void fexfn_impl_libGL_glShaderSourceARB(GLuint a_0, GLsizei count, guest_layout<const GLcharARB**> a_2, const GLint* a_3) {
  GLHOST_TRACE_SCOPE();
#ifndef IS_32BIT_THUNK
  auto sources = a_2.force_get_host_pointer();
#else
  auto sources = (const char**)alloca(count * sizeof(const char*));
  for (GLsizei i = 0; i < count; ++i) {
    sources[i] = a_2.get_pointer()[i].force_get_host_pointer();
  }
#endif
  return fexldr_ptr_libGL_glShaderSourceARB(a_0, count, sources, a_3);
}
#endif

#ifndef BUILD_ANDROID
// Relocate data to guest heap so it can be called with XFree.
// The memory at the given host location will be de-allocated.
template<typename T>
guest_layout<T*> RelocateArrayToGuestHeap(T* Data, int NumItems) {
  if (!Data) {
    return guest_layout<T*> {.data = 0};
  }

  guest_layout<T*> GuestData;
  GuestData.data = reinterpret_cast<uintptr_t>(GuestMalloc(sizeof(guest_layout<T>) * NumItems));
  for (int Index = 0; Index < NumItems; ++Index) {
    GuestData.get_pointer()[Index] = to_guest(to_host_layout(Data[Index]));
  }
  x11_manager.HostXFree(Data);
  return GuestData;
}

// Maps to a host-side XVisualInfo, which must be XFree'ed by the caller.
static XVisualInfo* LookupHostVisualInfo(Display* HostDisplay, guest_layout<XVisualInfo*> GuestInfo) {
  if (!GuestInfo.data) {
    return nullptr;
  }

  int num_matches;
  auto HostInfo = host_layout<XVisualInfo> {*GuestInfo.get_pointer()}.data;
  auto ret = x11_manager.HostXGetVisualInfo(HostDisplay, uint64_t {VisualScreenMask | VisualIDMask}, &HostInfo, &num_matches);
  if (num_matches != 1) {
    fprintf(stderr, "ERROR: Did not find unique host XVisualInfo\n");
    std::abort();
  }
  return ret;
}

// Maps to a guest-side XVisualInfo and destroys the host argument.
static guest_layout<XVisualInfo*> MapToGuestVisualInfo(Display* HostDisplay, XVisualInfo* HostInfo) {
  if (!HostInfo) {
    return guest_layout<XVisualInfo*> {.data = 0};
  }

  auto guest_display = x11_manager.HostToGuestDisplay(HostDisplay);
#ifndef IS_32BIT_THUNK
  int num_matches;
  auto GuestInfo = to_guest(to_host_layout(*HostInfo));
#else
  GuestStackBumpAllocator GuestStack;
  auto& num_matches = *GuestStack.New<int>();
  auto& GuestInfo = *GuestStack.New<guest_layout<XVisualInfo>>(to_guest(to_host_layout(*HostInfo)));
#endif
  auto ret = x11_manager.GuestXGetVisualInfo(guest_display.get_pointer(), VisualScreenMask | VisualIDMask, &GuestInfo, &num_matches);

  if (num_matches != 1) {
    fprintf(stderr, "ERROR: Did not find unique guest XVisualInfo\n");
    std::abort();
  }

  // We effectively relocated the VisualInfo, so free the original one now
  x11_manager.HostXFree(HostInfo);
  guest_layout<XVisualInfo*> GuestRet;
  GuestRet.data = reinterpret_cast<uintptr_t>(ret);
  return GuestRet;
}

guest_layout<GLXFBConfig*> fexfn_impl_libGL_glXChooseFBConfig(Display* Display, int Screen, const int* Attributes, int* NumItems) {
  GLHOST_TRACE_SCOPE();
  auto ret = fexldr_ptr_libGL_glXChooseFBConfig(Display, Screen, Attributes, NumItems);
  return RelocateArrayToGuestHeap(ret, *NumItems);
}

guest_layout<GLXFBConfigSGIX*> fexfn_impl_libGL_glXChooseFBConfigSGIX(Display* Display, int Screen, int* Attributes, int* NumItems) {
  GLHOST_TRACE_SCOPE();
  auto ret = fexldr_ptr_libGL_glXChooseFBConfigSGIX(Display, Screen, Attributes, NumItems);
  return RelocateArrayToGuestHeap(ret, *NumItems);
}

guest_layout<_XDisplay*> fexfn_impl_libGL_glXGetCurrentDisplay() {
  GLHOST_TRACE_SCOPE();
  auto ret = fexldr_ptr_libGL_glXGetCurrentDisplay();
  return x11_manager.HostToGuestDisplay(ret);
}

guest_layout<_XDisplay*> fexfn_impl_libGL_glXGetCurrentDisplayEXT() {
  GLHOST_TRACE_SCOPE();
  auto ret = fexldr_ptr_libGL_glXGetCurrentDisplayEXT();
  return x11_manager.HostToGuestDisplay(ret);
}

guest_layout<GLXFBConfig*> fexfn_impl_libGL_glXGetFBConfigs(Display* Display, int Screen, int* NumItems) {
  GLHOST_TRACE_SCOPE();
  auto ret = fexldr_ptr_libGL_glXGetFBConfigs(Display, Screen, NumItems);
  return RelocateArrayToGuestHeap(ret, *NumItems);
}

GLXFBConfigSGIX fexfn_impl_libGL_glXGetFBConfigFromVisualSGIX(Display* Display, guest_layout<XVisualInfo*> Info) {
  GLHOST_TRACE_SCOPE();
  auto HostInfo = LookupHostVisualInfo(Display, Info);
  auto ret = fexldr_ptr_libGL_glXGetFBConfigFromVisualSGIX(Display, HostInfo);
  x11_manager.HostXFree(HostInfo);
  return ret;
}

guest_layout<XVisualInfo*> fexfn_impl_libGL_glXGetVisualFromFBConfigSGIX(Display* Display, GLXFBConfigSGIX Config) {
  GLHOST_TRACE_SCOPE();
  return MapToGuestVisualInfo(Display, fexldr_ptr_libGL_glXGetVisualFromFBConfigSGIX(Display, Config));
}

guest_layout<XVisualInfo*> fexfn_impl_libGL_glXChooseVisual(Display* Display, int Screen, int* Attributes) {
  GLHOST_TRACE_SCOPE();
  return MapToGuestVisualInfo(Display, fexldr_ptr_libGL_glXChooseVisual(Display, Screen, Attributes));
}

GLXContext fexfn_impl_libGL_glXCreateContext(Display* Display, guest_layout<XVisualInfo*> Info, GLXContext ShareList, Bool Direct) {
  GLHOST_TRACE_SCOPE();
  VEXA_LOGV(
               "[libGL-host] glXCreateContext(desktop) display=%p guest_visual=%p share=%p direct=%d\n",
               reinterpret_cast<void*>(Display),
               reinterpret_cast<void*>(Info.data),
               reinterpret_cast<void*>(ShareList),
               static_cast<int>(Direct));
  auto HostInfo = LookupHostVisualInfo(Display, Info);
  VEXA_LOGV(
               "[libGL-host] glXCreateContext(desktop) mapped host_visual=%p\n",
               reinterpret_cast<void*>(HostInfo));
  auto ret = fexldr_ptr_libGL_glXCreateContext(Display, HostInfo, ShareList, Direct);
  VEXA_LOGV(
               "[libGL-host] glXCreateContext(desktop) => %p\n",
               reinterpret_cast<void*>(ret));
  x11_manager.HostXFree(HostInfo);
  return ret;
}

GLXPixmap fexfn_impl_libGL_glXCreateGLXPixmap(Display* Display, guest_layout<XVisualInfo*> Info, Pixmap Pixmap) {
  GLHOST_TRACE_SCOPE();
  auto HostInfo = LookupHostVisualInfo(Display, Info);
  auto ret = fexldr_ptr_libGL_glXCreateGLXPixmap(Display, HostInfo, Pixmap);
  x11_manager.HostXFree(HostInfo);
  return ret;
}

GLXPixmap fexfn_impl_libGL_glXCreateGLXPixmapMESA(Display* Display, guest_layout<XVisualInfo*> Info, Pixmap Pixmap, Colormap Colormap) {
  GLHOST_TRACE_SCOPE();
  auto HostInfo = LookupHostVisualInfo(Display, Info);
  auto ret = fexldr_ptr_libGL_glXCreateGLXPixmapMESA(Display, HostInfo, Pixmap, Colormap);
  x11_manager.HostXFree(HostInfo);
  return ret;
}

int fexfn_impl_libGL_glXGetConfig(Display* Display, guest_layout<XVisualInfo*> Info, int Attribute, int* Value) {
  GLHOST_TRACE_SCOPE();
  auto HostInfo = LookupHostVisualInfo(Display, Info);
  auto ret = fexldr_ptr_libGL_glXGetConfig(Display, HostInfo, Attribute, Value);
  x11_manager.HostXFree(HostInfo);
  return ret;
}

guest_layout<XVisualInfo*> fexfn_impl_libGL_glXGetVisualFromFBConfig(Display* Display, GLXFBConfig Config) {
  GLHOST_TRACE_SCOPE();
  return MapToGuestVisualInfo(Display, fexldr_ptr_libGL_glXGetVisualFromFBConfig(Display, Config));
}
#else
namespace {
// VEXA_FIXES: Android GLX compatibility stubs return deterministic placeholder objects
// to satisfy guests expecting GLX/X11 enumeration before EGL-backed context use.
guest_layout<GLXFBConfig*> MakeGuestFBConfigArray(int* NumItems) {
  if (NumItems) {
    *NumItems = 1;
  }
  if (!GuestMalloc) {
    VexaAbortThunkNullTarget("MakeGuestFBConfigArray", "GuestMalloc is unset", kThunkGLGuestMalloc,
                                 static_cast<uint64_t>(reinterpret_cast<uintptr_t>(NumItems)));
  }

  VEXA_LOGV("[libGL-host] MakeGuestFBConfigArray GuestMalloc=%p\n", reinterpret_cast<void*>(GuestMalloc));
  auto* guest_configs = reinterpret_cast<guest_layout<GLXFBConfig>*>(GuestMalloc(sizeof(guest_layout<GLXFBConfig>)));
  guest_configs[0].data = 1;
  guest_layout<GLXFBConfig*> ret;
  ret.data = reinterpret_cast<uintptr_t>(guest_configs);
  return ret;
}

guest_layout<GLXFBConfigSGIX*> MakeGuestFBConfigArraySGIX(int* NumItems) {
  if (NumItems) {
    *NumItems = 1;
  }
  if (!GuestMalloc) {
    return guest_layout<GLXFBConfigSGIX*> {.data = 0};
  }

  auto* guest_configs = reinterpret_cast<guest_layout<GLXFBConfigSGIX>*>(GuestMalloc(sizeof(guest_layout<GLXFBConfigSGIX>)));
  guest_configs[0].data = 1;
  guest_layout<GLXFBConfigSGIX*> ret;
  ret.data = reinterpret_cast<uintptr_t>(guest_configs);
  return ret;
}

guest_layout<XVisualInfo*> MakeGuestVisualInfo() {
  if (!GuestMalloc) {
    VexaAbortThunkNullTarget("MakeGuestVisualInfo", "GuestMalloc is unset", kThunkGLGuestMalloc);
  }

  XVisualInfo host_info {
    .visual = reinterpret_cast<Visual*>(uintptr_t {1}),
    .visualid = 1,
    .screen = 0,
    .depth = 24,
    .c_class = 4,
    .red_mask = 0x00ff0000UL,
    .green_mask = 0x0000ff00UL,
    .blue_mask = 0x000000ffUL,
    .colormap_size = 256,
    .bits_per_rgb = 8,
  };

  auto* guest_info = reinterpret_cast<XVisualInfo*>(GuestMalloc(sizeof(XVisualInfo)));
  *guest_info = host_info;

  guest_layout<XVisualInfo*> ret;
  ret.data = reinterpret_cast<uintptr_t>(guest_info);
  return ret;
}
} // namespace

guest_layout<GLXFBConfig*> fexfn_impl_libGL_glXChooseFBConfig(Display*, int, const int*, int* NumItems) {
  GLHOST_TRACE_SCOPE();
  return MakeGuestFBConfigArray(NumItems);
}

guest_layout<GLXFBConfigSGIX*> fexfn_impl_libGL_glXChooseFBConfigSGIX(Display*, int, int*, int* NumItems) {
  GLHOST_TRACE_SCOPE();
  return MakeGuestFBConfigArraySGIX(NumItems);
}

guest_layout<_XDisplay*> fexfn_impl_libGL_glXGetCurrentDisplay() {
  GLHOST_TRACE_SCOPE();
  return guest_layout<_XDisplay*> {.data = 0};
}

guest_layout<_XDisplay*> fexfn_impl_libGL_glXGetCurrentDisplayEXT() {
  GLHOST_TRACE_SCOPE();
  return guest_layout<_XDisplay*> {.data = 0};
}

guest_layout<GLXFBConfig*> fexfn_impl_libGL_glXGetFBConfigs(Display*, int, int* NumItems) {
  GLHOST_TRACE_SCOPE();
  return MakeGuestFBConfigArray(NumItems);
}

GLXFBConfigSGIX fexfn_impl_libGL_glXGetFBConfigFromVisualSGIX(Display*, guest_layout<XVisualInfo*>) {
  GLHOST_TRACE_SCOPE();
  return reinterpret_cast<GLXFBConfigSGIX>(uintptr_t {1});
}

guest_layout<XVisualInfo*> fexfn_impl_libGL_glXGetVisualFromFBConfigSGIX(Display*, GLXFBConfigSGIX) {
  GLHOST_TRACE_SCOPE();
  return MakeGuestVisualInfo();
}

guest_layout<XVisualInfo*> fexfn_impl_libGL_glXChooseVisual(Display*, int, int*) {
  GLHOST_TRACE_SCOPE();
  return MakeGuestVisualInfo();
}

GLXContext fexfn_impl_libGL_glXCreateContext(Display* Display, guest_layout<XVisualInfo*> Info, GLXContext ShareList, Bool Direct) {
  GLHOST_TRACE_SCOPE();
  GLHostLogCurrentEGLState("glXCreateContext(android-entry)");
  VEXA_LOGV(
               "[libGL-host] glXCreateContext(android) display=%p visual=%p share=%p direct=%d\n",
               reinterpret_cast<void*>(Display),
               reinterpret_cast<void*>(Info.data),
               reinterpret_cast<void*>(ShareList),
               static_cast<int>(Direct));
  VexaTraceThunkEvent("glXCreateContext", VEXA_THUNK_EVENT_ENTER, kThunkGLXCreateContext,
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(eglGetCurrentDisplay())),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(eglGetCurrentSurface(EGL_DRAW))),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(eglGetCurrentContext())));
  auto context = eglGetCurrentContext();
  auto result = context != EGL_NO_CONTEXT ? reinterpret_cast<GLXContext>(context) : reinterpret_cast<GLXContext>(uintptr_t {1});
  if (context == EGL_NO_CONTEXT) {
    VEXA_LOGV(
                 "[libGL-host] glXCreateContext(android) no current EGL context; returning placeholder=%p\n",
                 reinterpret_cast<void*>(result));
  } else {
    VEXA_LOGV(
                 "[libGL-host] glXCreateContext(android) using current EGL context=%p\n",
                 reinterpret_cast<void*>(context));
  }
  GLHostLogCurrentEGLState("glXCreateContext(android-exit)");
  VexaTraceThunkEvent("glXCreateContext", VEXA_THUNK_EVENT_EXIT, kThunkGLXCreateContext,
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)));
  return result;
}

GLXPixmap fexfn_impl_libGL_glXCreateGLXPixmap(Display*, guest_layout<XVisualInfo*>, Pixmap Pixmap) {
  GLHOST_TRACE_SCOPE();
  return static_cast<GLXPixmap>(Pixmap != 0 ? Pixmap : 1);
}

GLXPixmap fexfn_impl_libGL_glXCreateGLXPixmapMESA(Display*, guest_layout<XVisualInfo*>, Pixmap Pixmap, Colormap) {
  GLHOST_TRACE_SCOPE();
  return static_cast<GLXPixmap>(Pixmap != 0 ? Pixmap : 1);
}

int fexfn_impl_libGL_glXGetConfig(Display*, guest_layout<XVisualInfo*>, int, int* Value) {
  GLHOST_TRACE_SCOPE();
  if (Value) {
    *Value = 0;
  }
  return 0;
}

guest_layout<XVisualInfo*> fexfn_impl_libGL_glXGetVisualFromFBConfig(Display*, GLXFBConfig) {
  GLHOST_TRACE_SCOPE();
  return MakeGuestVisualInfo();
}
#endif

// VEXA_FIXES: These legacy 32-bit desktop GLX helper shims rely on symbols not
// emitted by the Android GL thunk allowlist path.
#if defined(IS_32BIT_THUNK) && !defined(BUILD_ANDROID)
void fexfn_impl_libGL_glBindBuffersRange(GLenum a_0, GLuint a_1, GLsizei Count, const GLuint* a_3, guest_layout<const int*> Offsets,
                                         guest_layout<const int*> Sizes) {
  GLHOST_TRACE_SCOPE();
  auto HostOffsets = (GLintptr*)alloca(Count * sizeof(GLintptr));
  auto HostSizes = (GLsizeiptr*)alloca(Count * sizeof(GLsizeiptr));
  for (int i = 0; i < Count; ++i) {
    HostOffsets[i] = Offsets.get_pointer()[i].data;
    HostSizes[i] = Sizes.get_pointer()[i].data;
  }
  return fexldr_ptr_libGL_glBindBuffersRange(a_0, a_1, Count, a_3, HostOffsets, HostSizes);
}

void fexfn_impl_libGL_glBindVertexBuffers(GLuint a_0, GLsizei count, const GLuint* a_2, guest_layout<const int*> Offsets, const GLsizei* a_4) {
  GLHOST_TRACE_SCOPE();
  auto HostOffsets = (GLintptr*)alloca(count * sizeof(GLintptr));
  for (int i = 0; i < count; ++i) {
    HostOffsets[i] = Offsets.get_pointer()[i].data;
  }
  fexldr_ptr_libGL_glBindVertexBuffers(a_0, count, a_2, HostOffsets, a_4);
}

void fexfn_impl_libGL_glGetUniformIndices(GLuint a_0, GLsizei Count, guest_layout<const GLchar* const*> Names, GLuint* a_3) {
  GLHOST_TRACE_SCOPE();
  auto HostNames = (const GLchar**)alloca(Count * sizeof(GLintptr));
  for (int i = 0; i < Count; ++i) {
    HostNames[i] = host_layout<const char* const> {Names.get_pointer()[i]}.data;
  }
  fexldr_ptr_libGL_glGetUniformIndices(a_0, Count, HostNames, a_3);
}

void fexfn_impl_libGL_glVertexArrayVertexBuffers(GLuint a_0, GLuint a_1, GLsizei count, const GLuint* a_3, guest_layout<const int*> Offsets,
                                                 const GLsizei* a_5) {
  GLHOST_TRACE_SCOPE();
  auto HostOffsets = (GLintptr*)alloca(count * sizeof(GLintptr));
  for (int i = 0; i < count; ++i) {
    HostOffsets[i] = Offsets.get_pointer()[i].data;
  }
  fexldr_ptr_libGL_glVertexArrayVertexBuffers(a_0, a_1, count, a_3, HostOffsets, a_5);
}

void fexfn_impl_libGL_glXGetSelectedEvent(Display* Display, GLXDrawable Drawable, guest_layout<uint32_t*> Mask) {
  GLHOST_TRACE_SCOPE();
  unsigned long HostMask;
  fexldr_ptr_libGL_glXGetSelectedEvent(Display, Drawable, &HostMask);
  *Mask.get_pointer() = HostMask;
}
void fexfn_impl_libGL_glXGetSelectedEventSGIX(Display* Display, GLXDrawable Drawable, guest_layout<uint32_t*> Mask) {
  GLHOST_TRACE_SCOPE();
  unsigned long HostMask;
  fexldr_ptr_libGL_glXGetSelectedEventSGIX(Display, Drawable, &HostMask);
  *Mask.get_pointer() = HostMask;
}
#endif

#if defined(BUILD_ANDROID)
// The runtime window's BufferQueue producer accepts a single api connect();
// our EGL window surface already holds it. A guest that moves to Vulkan
// (Unity probes GLX first) needs the api slot back, otherwise the producer's
// second connect() returns BAD_VALUE and vkCreateAndroidSurfaceKHR fails
// with EINVAL. The Vulkan thunk resolves this through dlsym before creating
// its VkSurfaceKHR.
extern "C" __attribute__((visibility("default")))
void Vexa_ReleaseGlxWindowApi(ANativeWindow* window) {
  if (!window) return;
  std::lock_guard lock(g_vexa_context_mutex);
  if (!g_vexa_glx.ready || g_vexa_glx.window != window) return;
  eglMakeCurrent(g_vexa_glx.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (g_vexa_glx.surface != EGL_NO_SURFACE) {
    eglDestroySurface(g_vexa_glx.display, g_vexa_glx.surface);
    g_vexa_glx.surface = EGL_NO_SURFACE;
  }
  VexaDisconnectWindowApi(window);
  g_vexa_glx.ready = false;
  fprintf(stderr, "[VEXA][GLX] released window api for Vulkan takeover\n");
}
#endif

#if defined(BUILD_ANDROID)
extern "C" ExportEntry* fexthunks_exports_libGL() {
  if (!fexldr_init_libGL()) return nullptr;
  // Direct ELF PLT calls (e.g. Dead Cells sdl.hdll -> glGetString) and i386
  // guest packers route through fexfn_unpack_libGL_<name> -> fexldr_ptr_libGL_<name>.
  // Any proc in FEX_ANDROID_GL_PROC_LIST_FOR_THUNKGEN that isn't marked
  // custom_host_impl in libGL_interface.cpp must have its fexldr_ptr_libGL_<name>
  // bound to the same VEXA wrapper returned by glXGetProcAddress.
#define VEXA_BIND_HOST_WRAPPER(name) \
  fexldr_ptr_libGL_##name = reinterpret_cast<fexldr_type_libGL_##name*>( \
    fexfn_impl_libGL_glXGetProcAddress(reinterpret_cast<const GLubyte*>(#name)));
  VEXA_BIND_HOST_WRAPPER(glGetString)
  VEXA_BIND_HOST_WRAPPER(glGetStringi)
  VEXA_BIND_HOST_WRAPPER(glGetIntegerv)
  VEXA_BIND_HOST_WRAPPER(glGetProgramInterfaceiv)
  VEXA_BIND_HOST_WRAPPER(glGetProgramResourceName)
  VEXA_BIND_HOST_WRAPPER(glGetProgramResourceiv)
  VEXA_BIND_HOST_WRAPPER(glBindBufferBase)
  VEXA_BIND_HOST_WRAPPER(glBindBufferRange)
  VEXA_BIND_HOST_WRAPPER(glGetInternalformativ)
  VEXA_BIND_HOST_WRAPPER(glCreateProgram)
  VEXA_BIND_HOST_WRAPPER(glGetProgramiv)
  VEXA_BIND_HOST_WRAPPER(glDeleteProgram)
  VEXA_BIND_HOST_WRAPPER(glDeleteShader)
  VEXA_BIND_HOST_WRAPPER(glDeleteProgramPipelines)
  VEXA_BIND_HOST_WRAPPER(glCreateShader)
  VEXA_BIND_HOST_WRAPPER(glCompileShader)
  VEXA_BIND_HOST_WRAPPER(glAttachShader)
  VEXA_BIND_HOST_WRAPPER(glDetachShader)
  VEXA_BIND_HOST_WRAPPER(glLinkProgram)
  VEXA_BIND_HOST_WRAPPER(glFlush)
  VEXA_BIND_HOST_WRAPPER(glWaitSync)
  VEXA_BIND_HOST_WRAPPER(glDeleteTextures)
  VEXA_BIND_HOST_WRAPPER(glTexImage2D)
  VEXA_BIND_HOST_WRAPPER(glTexImage3D)
  VEXA_BIND_HOST_WRAPPER(glTexSubImage2D)
  VEXA_BIND_HOST_WRAPPER(glSamplerParameterf)
  VEXA_BIND_HOST_WRAPPER(glTexSubImage3D)
  VEXA_BIND_HOST_WRAPPER(glPixelStorei)
  VEXA_BIND_HOST_WRAPPER(glGetUniformLocation)
  VEXA_BIND_HOST_WRAPPER(glUseProgramStages)
  VEXA_BIND_HOST_WRAPPER(glBindProgramPipeline)
  VEXA_BIND_HOST_WRAPPER(glActiveShaderProgram)
  VEXA_BIND_HOST_WRAPPER(glProgramUniform1i)
  VEXA_BIND_HOST_WRAPPER(glUniform1i)
  VEXA_BIND_HOST_WRAPPER(glUniform1f)
  VEXA_BIND_HOST_WRAPPER(glUniform1iv)
  VEXA_BIND_HOST_WRAPPER(glUniform1fv)
  VEXA_BIND_HOST_WRAPPER(glUniform2i)
  VEXA_BIND_HOST_WRAPPER(glUniform2f)
  VEXA_BIND_HOST_WRAPPER(glUniform2iv)
  VEXA_BIND_HOST_WRAPPER(glUniform2fv)
  VEXA_BIND_HOST_WRAPPER(glUniform3i)
  VEXA_BIND_HOST_WRAPPER(glUniform3f)
  VEXA_BIND_HOST_WRAPPER(glUniform3iv)
  VEXA_BIND_HOST_WRAPPER(glUniform3fv)
  VEXA_BIND_HOST_WRAPPER(glUniform4i)
  VEXA_BIND_HOST_WRAPPER(glUniform4f)
  VEXA_BIND_HOST_WRAPPER(glUniform4iv)
  VEXA_BIND_HOST_WRAPPER(glUniform4fv)
  VEXA_BIND_HOST_WRAPPER(glUniformMatrix2fv)
  VEXA_BIND_HOST_WRAPPER(glUniformMatrix3fv)
  VEXA_BIND_HOST_WRAPPER(glUniformMatrix4fv)
  VEXA_BIND_HOST_WRAPPER(glTexStorage2D)
  VEXA_BIND_HOST_WRAPPER(glTexStorage3D)
  VEXA_BIND_HOST_WRAPPER(glCompressedTexImage2D)
#ifdef IS_32BIT_THUNK
  VEXA_BIND_HOST_WRAPPER(glMapBufferRange)
  VEXA_BIND_HOST_WRAPPER(glDeleteBuffers)
  VEXA_BIND_HOST_WRAPPER(glGetBufferPointerv)
  VEXA_BIND_HOST_WRAPPER(glUnmapBuffer)
  VEXA_BIND_HOST_WRAPPER(glFlushMappedBufferRange)
  VEXA_BIND_HOST_WRAPPER(glIsSync)
  VEXA_BIND_HOST_WRAPPER(glTransformFeedbackVaryings)
#endif
#undef VEXA_BIND_HOST_WRAPPER
  return exports;
}
#else
EXPORTS(libGL)
#endif
