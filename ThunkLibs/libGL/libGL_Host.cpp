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
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#define GL_GLEXT_PROTOTYPES 1
#define GLX_GLXEXT_PROTOTYPES 1

#include "glcorearb.h"

// VEXA_FIXES: Android host path uses EGL/GLES headers and a GLX-compat shim layer.
#ifdef BUILD_ANDROID
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
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
using guest_size_t = size_t;
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

#ifdef BUILD_ANDROID
void GLHostLogCurrentEGLState(const char* site) {
  const EGLDisplay display = eglGetCurrentDisplay();
  const EGLSurface draw_surface = eglGetCurrentSurface(EGL_DRAW);
  const EGLSurface read_surface = eglGetCurrentSurface(EGL_READ);
  const EGLContext context = eglGetCurrentContext();
  fprintf(stderr,
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
#endif

void GLHostLogDuration(const char* call, uint64_t elapsed_ns) {
  if (elapsed_ns >= kStallWarnThresholdNs) {
    fprintf(stderr,
                 "[libGL-host] stall-warn call=%s duration_ms=%llu\n",
                 call ? call : "<null>",
                 static_cast<unsigned long long>(elapsed_ns / 1000000ULL));
  } else if (elapsed_ns >= kStallInfoThresholdNs) {
    fprintf(stderr,
                 "[libGL-host] stall-info call=%s duration_ms=%llu\n",
                 call ? call : "<null>",
                 static_cast<unsigned long long>(elapsed_ns / 1000000ULL));
  }
}

struct ScopedGLCallTimer {
  explicit ScopedGLCallTimer(const char* name_)
    : name {name_}
    , start_ns {GLHostNowNs()} {
    GLHostMarkProgress(name);
  }
  ~ScopedGLCallTimer() {
    const uint64_t elapsed_ns = GLHostNowNs() - start_ns;
    GLHostLogDuration(name, elapsed_ns);
    GLHostMarkProgress(name);
  }
  const char* name;
  uint64_t start_ns;
};

std::atomic<uint64_t> GLHostTraceSeq {0};

struct ScopedGLThunkTrace {
  explicit ScopedGLThunkTrace(const char* fn_)
    : fn {fn_ ? fn_ : "<null>"}
    , seq {GLHostTraceSeq.fetch_add(1, std::memory_order_acq_rel) + 1}
    , start_ns {GLHostNowNs()} {
    GLHostMarkProgress(fn);
    fprintf(stderr,
                 "[libGL-host] call-enter seq=%llu fn=%s\n",
                 static_cast<unsigned long long>(seq),
                 fn);
  }

  ~ScopedGLThunkTrace() {
    const uint64_t elapsed_ns = GLHostNowNs() - start_ns;
    fprintf(stderr,
                 "[libGL-host] call-exit seq=%llu fn=%s duration_us=%llu\n",
                 static_cast<unsigned long long>(seq),
                 fn,
                 static_cast<unsigned long long>(elapsed_ns / 1000ULL));
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

  fprintf(stderr,
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

  fprintf(stderr, "[libGL-host] shader-transpile %s type=0x%x%s%s\n",
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

  if (source->find("precision ") != std::string::npos) {
    return;
  }

  constexpr std::string_view kPrecision {"precision highp float;\nprecision highp int;\n"};
  source->insert(FindPrecisionInsertPos(*source), kPrecision);
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
#ifdef BUILD_ANDROID
  // VEXA_FIXES: Android path does not rely on guest-provided malloc trampoline yet.
  fprintf(stderr, "[libGL-host] GL_SetGuestMalloc target=0x%llx unpacker=0x%llx\n",
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

#include "thunkgen_host_libGL.inl"

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

  fprintf(stderr,
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

// VEXA_FIXES: Central shader-source bridge used by both glShaderSource and glShaderSourceARB.
static void fexfn_impl_libGL_FEX_glShaderSource(GLuint shader, GLsizei count, uintptr_t strings, const GLint* length) {
  GLHOST_TRACE_SCOPE();
  const auto host_strings = reinterpret_cast<const GLchar* const*>(strings);
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

const GLubyte* fexfn_impl_libGL_glGetString(GLenum name) {
  GLHOST_TRACE_SCOPE();
#ifdef BUILD_ANDROID
  GLHostLogCurrentEGLState("glGetString");
  const GLubyte* result = ::glGetString(name);
  if (!result) {
    fprintf(stderr,
                 "[libGL-host] glGetString(name=0x%x/%s) => null\n",
                 static_cast<unsigned>(name),
                 GLHostGLStringName(name));
    return nullptr;
  }

  fprintf(stderr,
               "[libGL-host] glGetString(name=0x%x/%s) => %p text='%.256s'\n",
               static_cast<unsigned>(name),
               GLHostGLStringName(name),
               reinterpret_cast<const void*>(result),
               reinterpret_cast<const char*>(result));
  return result;
#else
  return fexldr_ptr_libGL_glGetString(name);
#endif
}

const GLubyte* fexfn_impl_libGL_glGetStringi(GLenum name, GLuint index) {
  GLHOST_TRACE_SCOPE();
#ifdef BUILD_ANDROID
  GLHostLogCurrentEGLState("glGetStringi");
  const GLubyte* result = ::glGetStringi(name, index);
  if (!result) {
    fprintf(stderr,
                 "[libGL-host] glGetStringi(name=0x%x/%s index=%u) => null\n",
                 static_cast<unsigned>(name),
                 GLHostGLStringName(name),
                 static_cast<unsigned>(index));
    return nullptr;
  }

  fprintf(stderr,
               "[libGL-host] glGetStringi(name=0x%x/%s index=%u) => %p text='%.256s'\n",
               static_cast<unsigned>(name),
               GLHostGLStringName(name),
               static_cast<unsigned>(index),
               reinterpret_cast<const void*>(result),
               reinterpret_cast<const char*>(result));
  return result;
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
  fprintf(stderr,
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
  fprintf(stderr,
               "[libGL-host] glXGetClientString(display=%p name=%d) => '%s'\n",
               reinterpret_cast<void*>(display),
               name,
               result);
  return result;
#else
  return fexldr_ptr_libGL_glXGetClientString(display, name);
#endif
}

const char* fexfn_impl_libGL_glXQueryServerString(Display* display, int screen, int name) {
  GLHOST_TRACE_SCOPE();
#ifdef BUILD_ANDROID
  const char* result = fexfn_impl_libGL_glXGetClientString(display, name);
  fprintf(stderr,
               "[libGL-host] glXQueryServerString(display=%p screen=%d name=%d) => '%s'\n",
               reinterpret_cast<void*>(display),
               screen,
               name,
               result ? result : "<null>");
  return result;
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
  fprintf(stderr,
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
  fprintf(stderr,
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
  fprintf(stderr,
               "[libGL-host] glXMakeCurrent(display=%p drawable=0x%llx context=%p current=%p)\n",
               reinterpret_cast<void*>(display),
               static_cast<unsigned long long>(drawable),
               reinterpret_cast<void*>(context),
               reinterpret_cast<void*>(current));

  if (requested != EGL_NO_CONTEXT && requested != current) {
    fprintf(stderr,
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
    fprintf(stderr, "[libGL-host] glXGetProcAddress received empty proc name\n");
    VexaTraceThunkEvent("<empty>", VEXA_THUNK_EVENT_LOOKUP_MISS, kThunkGLXGetProcAddress, 0, 4);
    return nullptr;
  }
  std::string_view name_sv {proc_name};
#ifdef BUILD_ANDROID
  fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') lookup\n", proc_name);
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
    fprintf(stderr,
                 "[libGL-host] glXGetProcAddress('%s') context-sensitive lookup\n",
                 proc_name);
  }
  if (name_sv == "glGetString") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glGetString);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via getstring-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glXQueryExtensionsString") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glXQueryExtensionsString);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via glx-ext-string-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glXGetClientString") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glXGetClientString);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via glx-client-string-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glXQueryServerString") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glXQueryServerString);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via glx-server-string-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glGetStringi") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glGetStringi);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via getstringi-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glXCreateNewContext") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glXCreateNewContext);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via glx-create-new-context-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glXCreateContext") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glXCreateContext);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via glx-create-context-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 11);
    return result;
  }
  if (name_sv == "glXGetCurrentContext") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glXGetCurrentContext);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via glx-get-current-context-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glXMakeCurrent") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glXMakeCurrent);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via glx-make-current-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glShaderSource" || name_sv == "glShaderSourceARB") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_FEX_glShaderSource);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via shader-bridge\n",
                 proc_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 1);
    return result;
  }
  if (name_sv == "glCompileShader") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glCompileShader);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via compile-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 5);
    return result;
  }
  if (name_sv == "glLinkProgram") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glLinkProgram);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via link-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 6);
    return result;
  }
  if (name_sv == "glCreateShader") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glCreateShader);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via create-shader-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 9);
    return result;
  }
  if (name_sv == "glAttachShader") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glAttachShader);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via attach-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 10);
    return result;
  }
  if (name_sv == "glDetachShader") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glDetachShader);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via detach-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 11);
    return result;
  }
  if (name_sv == "glGetIntegerv") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glGetIntegerv);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via getintegerv-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    VexaTraceThunkEvent(proc_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 12);
    return result;
  }
  if (name_sv == "glClientWaitSync") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glClientWaitSync);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via waitsync-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glWaitSync") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glWaitSync);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via waitsync-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glGetSynciv") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glGetSynciv);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via waitsync-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glReadPixels") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glReadPixels);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via io-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glFinish") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glFinish);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via flush-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  if (name_sv == "glFlush") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_glFlush);
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via flush-wrapper\n",
                 proc_name, reinterpret_cast<void*>(result));
    return result;
  }
  auto* android_name = proc_name;
  if (auto egl_proc = reinterpret_cast<VoidFn>(eglGetProcAddress(android_name)); egl_proc) {
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via eglGetProcAddress\n",
                 android_name, reinterpret_cast<void*>(egl_proc));
    VexaTraceThunkEvent(android_name, VEXA_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(egl_proc)), 2);
    return egl_proc;
  }
  fprintf(stderr,
               "[libGL-host] glXGetProcAddress('%s') eglGetProcAddress miss; trying dlsym(RTLD_DEFAULT)\n",
               android_name);
  dlerror();
  auto result = reinterpret_cast<VoidFn>(dlsym(RTLD_DEFAULT, android_name));
  const char* dlsym_error = dlerror();
  if (!result) {
    fprintf(stderr,
                 "[libGL-host] glXGetProcAddress('%s') => null (dlsym_err=%s)\n",
                 android_name,
                 dlsym_error ? dlsym_error : "<none>");
    GLHostLogCurrentEGLState("glXGetProcAddress(null-result)");
    VexaTraceThunkEvent(android_name, VEXA_THUNK_EVENT_LOOKUP_MISS, kThunkGLXGetProcAddress, 0, 3);
  } else {
    fprintf(stderr, "[libGL-host] glXGetProcAddress('%s') => %p via dlsym\n",
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
    fprintf(stderr, "[libGL-host] glPointSize unresolved; ignoring size=%f\n",
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
    fprintf(stderr,
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
    fprintf(stderr,
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
  ::glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
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
  return ::glClientWaitSync(sync, flags, timeout);
}

void fexfn_impl_libGL_glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
  GLHOST_TRACE_SCOPE();
  ScopedGLCallTimer timer {"glWaitSync"};
  ::glWaitSync(sync, flags, timeout);
}

void fexfn_impl_libGL_glGetSynciv(GLsync sync, GLenum pname, GLsizei bufSize, GLsizei* length, GLint* values) {
  GLHOST_TRACE_SCOPE();
  ScopedGLCallTimer timer {"glGetSynciv"};
  ::glGetSynciv(sync, pname, bufSize, length, values);
}

void fexfn_impl_libGL_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
  GLHOST_TRACE_SCOPE();
  ScopedGLCallTimer timer {"glReadPixels"};
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

GLuint fexfn_impl_libGL_glCreateShader(GLenum a_0) {
  GLHOST_TRACE_SCOPE();
  const GLuint shader = fexldr_ptr_libGL_glCreateShader(a_0);
  fprintf(stderr, "[libGL-host] glCreateShader(type=0x%x) => %u\n",
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

  fprintf(stderr,
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
  fexldr_ptr_libGL_glAttachShader(a_0, a_1);
}

void fexfn_impl_libGL_glDetachShader(GLuint a_0, GLuint a_1) {
  GLHOST_TRACE_SCOPE();
  GLint shader_type = 0;
  if (a_1 != 0) {
    ::glGetShaderiv(a_1, GL_SHADER_TYPE, &shader_type);
  }
  fprintf(stderr,
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
  fexldr_ptr_libGL_glDetachShader(a_0, a_1);
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
  GLHOST_TRACE_SCOPE();
  fexldr_ptr_libGL_glCompileShader(a_0);
#if defined(VEXA_ENABLE_RUNTIME_SHADER_TRANSLATOR)
  LogFailedShaderCompile(a_0);
#endif
}

void fexfn_impl_libGL_glLinkProgram(GLuint a_0) {
  GLHOST_TRACE_SCOPE();
  fexldr_ptr_libGL_glLinkProgram(a_0);
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

  if (fexldr_ptr_libGL_glGetIntegerv) {
    fexldr_ptr_libGL_glGetIntegerv(a_0, a_1);
    return;
  }

  ::glGetIntegerv(a_0, a_1);
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
  return fexldr_ptr_libGL_glCreateShaderProgramv(a_0, count, sources);
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
  fprintf(stderr,
               "[libGL-host] glXCreateContext(desktop) display=%p guest_visual=%p share=%p direct=%d\n",
               reinterpret_cast<void*>(Display),
               reinterpret_cast<void*>(Info.data),
               reinterpret_cast<void*>(ShareList),
               static_cast<int>(Direct));
  auto HostInfo = LookupHostVisualInfo(Display, Info);
  fprintf(stderr,
               "[libGL-host] glXCreateContext(desktop) mapped host_visual=%p\n",
               reinterpret_cast<void*>(HostInfo));
  auto ret = fexldr_ptr_libGL_glXCreateContext(Display, HostInfo, ShareList, Direct);
  fprintf(stderr,
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

  fprintf(stderr, "[libGL-host] MakeGuestFBConfigArray GuestMalloc=%p\n", reinterpret_cast<void*>(GuestMalloc));
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
  fprintf(stderr,
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
    fprintf(stderr,
                 "[libGL-host] glXCreateContext(android) no current EGL context; returning placeholder=%p\n",
                 reinterpret_cast<void*>(result));
  } else {
    fprintf(stderr,
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

EXPORTS(libGL)
