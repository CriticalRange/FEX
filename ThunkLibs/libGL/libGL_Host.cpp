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
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#define GL_GLEXT_PROTOTYPES 1
#define GLX_GLXEXT_PROTOTYPES 1

#include "glcorearb.h"

#ifdef BUILD_ANDROID
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#ifdef HYMOBILE_ENABLE_RUNTIME_SHADER_TRANSLATOR
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

#ifdef BUILD_ANDROID
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

#if defined(BUILD_ANDROID) && defined(HYMOBILE_ENABLE_RUNTIME_SHADER_TRANSLATOR)
std::mutex ShaderSourceCacheMutex;
std::unordered_map<size_t, std::string> ShaderSourceCache;
std::atomic<uint32_t> ShaderTranspileLogBudget {256};
std::once_flag ShaderDumpDirInitOnce;
bool ShaderDumpDirsReady {false};

constexpr const char* ShaderDumpRootDir = "/sdcard/HyMobile.Android/shaders";
constexpr const char* ShaderDumpOriginalDir = "/sdcard/HyMobile.Android/shaders/original";
constexpr const char* ShaderDumpTranslatedDir = "/sdcard/HyMobile.Android/shaders/translated";

bool ConsumeShaderTranspileLogBudget() {
  uint32_t remaining = ShaderTranspileLogBudget.load(std::memory_order_acquire);
  while (remaining > 0 &&
         !ShaderTranspileLogBudget.compare_exchange_weak(
             remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  return remaining > 0;
}

void LogShaderTranspileEvent(const char* event, GLenum shader_type, const char* detail = nullptr) {
  if (!ConsumeShaderTranspileLogBudget()) {
    return;
  }

  std::fprintf(stderr, "[libGL-host] shader-transpile %s type=0x%x%s%s\n",
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

std::string NormalizeVersionToES(std::string_view source) {
  if (source.empty()) {
    return {};
  }

  constexpr std::string_view target_version {"#version 320 es"};

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

  int version_number = 0;
  if (!ParseGLSLVersionNumber(current_version_line, &version_number) || version_number <= 320) {
    return std::string {source};
  }

  std::string normalized;
  normalized.reserve(source.size() + 16);
  normalized.append(source.substr(0, version_pos));
  normalized.append(target_version);
  if (line_end != std::string_view::npos) {
    normalized.append(source.substr(line_end));
  } else {
    normalized.push_back('\n');
  }

  return normalized;
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
  const auto compilation =
      compiler.CompileGlslToSpv(std::string {source}, kind, "hymobile_guest_shader.glsl", options);
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

  if (shader_type == GL_FRAGMENT_SHADER && out_source->find("precision ") == std::string::npos) {
    out_source->insert(0, "precision highp float;\nprecision highp int;\n");
  }

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
#ifdef BUILD_ANDROID
  std::fprintf(stderr, "[libGL-host] GL_SetGuestMalloc target=0x%llx unpacker=0x%llx\n",
               (unsigned long long)GuestTarget, (unsigned long long)GuestUnpacker);
  (void)GuestTarget;
  (void)GuestUnpacker;
  GuestMalloc = &AndroidGuestMallocFallback;
#else
  MakeHostTrampolineForGuestFunctionAt(GuestTarget, GuestUnpacker, &GuestMalloc);
#endif
}

#ifdef BUILD_ANDROID
static void fexfn_impl_libGL_GL_SetGuestXGetVisualInfo(uintptr_t, uintptr_t) {
}

static void fexfn_impl_libGL_GL_SetGuestXSync(uintptr_t, uintptr_t) {
}

static void fexfn_impl_libGL_GL_SetGuestXDisplayString(uintptr_t, uintptr_t) {
}
#else
static void fexfn_impl_libGL_GL_SetGuestXGetVisualInfo(uintptr_t GuestTarget, uintptr_t GuestUnpacker) {
  MakeHostTrampolineForGuestFunctionAt(GuestTarget, GuestUnpacker, &x11_manager.GuestXGetVisualInfo);
}

static void fexfn_impl_libGL_GL_SetGuestXSync(uintptr_t GuestTarget, uintptr_t GuestUnpacker) {
  MakeHostTrampolineForGuestFunctionAt(GuestTarget, GuestUnpacker, &x11_manager.GuestXSync);
}

static void fexfn_impl_libGL_GL_SetGuestXDisplayString(uintptr_t GuestTarget, uintptr_t GuestUnpacker) {
  MakeHostTrampolineForGuestFunctionAt(GuestTarget, GuestUnpacker, &x11_manager.GuestXDisplayString);
}
#endif

#include "thunkgen_host_libGL.inl"

static void fexfn_impl_libGL_FEX_glShaderSource(GLuint shader, GLsizei count, uintptr_t strings, const GLint* length) {
  const auto host_strings = reinterpret_cast<const GLchar* const*>(strings);
#if defined(BUILD_ANDROID) && defined(HYMOBILE_ENABLE_RUNTIME_SHADER_TRANSLATOR)
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

      const std::string fallback_source = NormalizeVersionToES(source);
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
extern "C" __attribute__((visibility("default")))
void FEX_glShaderSource(GLuint shader, GLsizei count, uintptr_t strings, const GLint* length) {
  fexfn_impl_libGL_FEX_glShaderSource(shader, count, strings, length);
}
#endif

auto fexfn_impl_libGL_glXGetProcAddress(const GLubyte* name) -> __GLXextFuncPtr {
  using VoidFn = __GLXextFuncPtr;
  if (!name) {
    HyMobileAbortThunkNullTarget("glXGetProcAddress", "procname is null", kThunkGLXGetProcAddress);
  }
  std::string_view name_sv {reinterpret_cast<const char*>(name)};
#ifdef BUILD_ANDROID
  HyMobileTraceThunkEvent(reinterpret_cast<const char*>(name), HYMOBILE_THUNK_EVENT_LOOKUP_FUNCTION, kThunkGLXGetProcAddress);
  if (name_sv == "glShaderSource" || name_sv == "glShaderSourceARB") {
    auto result = reinterpret_cast<VoidFn>(fexfn_impl_libGL_FEX_glShaderSource);
    HyMobileTraceThunkEvent(reinterpret_cast<const char*>(name), HYMOBILE_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)), 1);
    return result;
  }

  auto* android_name = reinterpret_cast<const char*>(name);
  if (auto egl_proc = reinterpret_cast<VoidFn>(eglGetProcAddress(android_name)); egl_proc) {
    HyMobileTraceThunkEvent(android_name, HYMOBILE_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
                            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(egl_proc)), 2);
    return egl_proc;
  }
  auto result = reinterpret_cast<VoidFn>(dlsym(RTLD_DEFAULT, android_name));
  if (!result) {
    HyMobileTraceThunkEvent(android_name, HYMOBILE_THUNK_EVENT_LOOKUP_MISS, kThunkGLXGetProcAddress, 0, 3);
  } else {
    HyMobileTraceThunkEvent(android_name, HYMOBILE_THUNK_EVENT_EXIT, kThunkGLXGetProcAddress,
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

// TODO: unsigned int *glXEnumerateVideoDevicesNV (Display *dpy, int screen, int *nelements);

#ifdef BUILD_ANDROID
void fexfn_impl_libGL_glShaderSource(GLuint a_0, GLsizei count, guest_layout<const GLchar* const*> a_2, const GLint* a_3) {
  fexfn_impl_libGL_FEX_glShaderSource(a_0, count, reinterpret_cast<uintptr_t>(a_2.force_get_host_pointer()), a_3);
}

void fexfn_impl_libGL_glShaderSourceARB(GLuint a_0, GLsizei count, guest_layout<const GLcharARB**> a_2, const GLint* a_3) {
  fexfn_impl_libGL_FEX_glShaderSource(a_0, count, reinterpret_cast<uintptr_t>(a_2.force_get_host_pointer()), a_3);
}
#endif

#ifndef BUILD_ANDROID
void fexfn_impl_libGL_glCompileShaderIncludeARB(GLuint a_0, GLsizei Count, guest_layout<const GLchar* const*> a_2, const GLint* a_3) {
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
  void* HostOut;
  fexldr_ptr_libGL_glGetBufferPointerv(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetBufferPointervARB(GLenum a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetBufferPointervARB(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetNamedBufferPointerv(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetNamedBufferPointerv(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetNamedBufferPointervEXT(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetNamedBufferPointervEXT(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetPointerv(GLenum a_0, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetPointerv(a_0, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetPointervEXT(GLenum a_0, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetPointervEXT(a_0, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetPointeri_vEXT(GLenum a_0, GLuint a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetPointeri_vEXT(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetPointerIndexedvEXT(GLenum a_0, GLuint a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetPointerIndexedvEXT(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVariantPointervEXT(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetVariantPointervEXT(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVertexAttribPointervARB(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetVertexAttribPointervARB(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVertexAttribPointerv(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetVertexAttribPointerv(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVertexAttribPointervNV(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetVertexAttribPointervNV(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVertexArrayPointeri_vEXT(GLuint a_0, GLuint a_1, GLenum a_2, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetVertexArrayPointeri_vEXT(a_0, a_1, a_2, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glGetVertexArrayPointervEXT(GLuint a_0, GLenum a_1, guest_layout<void**> GuestOut) {
  void* HostOut;
  fexldr_ptr_libGL_glGetVertexArrayPointervEXT(a_0, a_1, &HostOut);
  *GuestOut.get_pointer() = to_guest(to_host_layout(HostOut));
}

void fexfn_impl_libGL_glShaderSource(GLuint a_0, GLsizei count, guest_layout<const GLchar* const*> a_2, const GLint* a_3) {
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
  auto ret = fexldr_ptr_libGL_glXChooseFBConfig(Display, Screen, Attributes, NumItems);
  return RelocateArrayToGuestHeap(ret, *NumItems);
}

guest_layout<GLXFBConfigSGIX*> fexfn_impl_libGL_glXChooseFBConfigSGIX(Display* Display, int Screen, int* Attributes, int* NumItems) {
  auto ret = fexldr_ptr_libGL_glXChooseFBConfigSGIX(Display, Screen, Attributes, NumItems);
  return RelocateArrayToGuestHeap(ret, *NumItems);
}

guest_layout<_XDisplay*> fexfn_impl_libGL_glXGetCurrentDisplay() {
  auto ret = fexldr_ptr_libGL_glXGetCurrentDisplay();
  return x11_manager.HostToGuestDisplay(ret);
}

guest_layout<_XDisplay*> fexfn_impl_libGL_glXGetCurrentDisplayEXT() {
  auto ret = fexldr_ptr_libGL_glXGetCurrentDisplayEXT();
  return x11_manager.HostToGuestDisplay(ret);
}

guest_layout<GLXFBConfig*> fexfn_impl_libGL_glXGetFBConfigs(Display* Display, int Screen, int* NumItems) {
  auto ret = fexldr_ptr_libGL_glXGetFBConfigs(Display, Screen, NumItems);
  return RelocateArrayToGuestHeap(ret, *NumItems);
}

GLXFBConfigSGIX fexfn_impl_libGL_glXGetFBConfigFromVisualSGIX(Display* Display, guest_layout<XVisualInfo*> Info) {
  auto HostInfo = LookupHostVisualInfo(Display, Info);
  auto ret = fexldr_ptr_libGL_glXGetFBConfigFromVisualSGIX(Display, HostInfo);
  x11_manager.HostXFree(HostInfo);
  return ret;
}

guest_layout<XVisualInfo*> fexfn_impl_libGL_glXGetVisualFromFBConfigSGIX(Display* Display, GLXFBConfigSGIX Config) {
  return MapToGuestVisualInfo(Display, fexldr_ptr_libGL_glXGetVisualFromFBConfigSGIX(Display, Config));
}

guest_layout<XVisualInfo*> fexfn_impl_libGL_glXChooseVisual(Display* Display, int Screen, int* Attributes) {
  return MapToGuestVisualInfo(Display, fexldr_ptr_libGL_glXChooseVisual(Display, Screen, Attributes));
}

GLXContext fexfn_impl_libGL_glXCreateContext(Display* Display, guest_layout<XVisualInfo*> Info, GLXContext ShareList, Bool Direct) {
  auto HostInfo = LookupHostVisualInfo(Display, Info);
  auto ret = fexldr_ptr_libGL_glXCreateContext(Display, HostInfo, ShareList, Direct);
  x11_manager.HostXFree(HostInfo);
  return ret;
}

GLXPixmap fexfn_impl_libGL_glXCreateGLXPixmap(Display* Display, guest_layout<XVisualInfo*> Info, Pixmap Pixmap) {
  auto HostInfo = LookupHostVisualInfo(Display, Info);
  auto ret = fexldr_ptr_libGL_glXCreateGLXPixmap(Display, HostInfo, Pixmap);
  x11_manager.HostXFree(HostInfo);
  return ret;
}

GLXPixmap fexfn_impl_libGL_glXCreateGLXPixmapMESA(Display* Display, guest_layout<XVisualInfo*> Info, Pixmap Pixmap, Colormap Colormap) {
  auto HostInfo = LookupHostVisualInfo(Display, Info);
  auto ret = fexldr_ptr_libGL_glXCreateGLXPixmapMESA(Display, HostInfo, Pixmap, Colormap);
  x11_manager.HostXFree(HostInfo);
  return ret;
}

int fexfn_impl_libGL_glXGetConfig(Display* Display, guest_layout<XVisualInfo*> Info, int Attribute, int* Value) {
  auto HostInfo = LookupHostVisualInfo(Display, Info);
  auto ret = fexldr_ptr_libGL_glXGetConfig(Display, HostInfo, Attribute, Value);
  x11_manager.HostXFree(HostInfo);
  return ret;
}

guest_layout<XVisualInfo*> fexfn_impl_libGL_glXGetVisualFromFBConfig(Display* Display, GLXFBConfig Config) {
  return MapToGuestVisualInfo(Display, fexldr_ptr_libGL_glXGetVisualFromFBConfig(Display, Config));
}
#else
namespace {
guest_layout<GLXFBConfig*> MakeGuestFBConfigArray(int* NumItems) {
  if (NumItems) {
    *NumItems = 1;
  }
  if (!GuestMalloc) {
    HyMobileAbortThunkNullTarget("MakeGuestFBConfigArray", "GuestMalloc is unset", kThunkGLGuestMalloc,
                                 static_cast<uint64_t>(reinterpret_cast<uintptr_t>(NumItems)));
  }

  std::fprintf(stderr, "[libGL-host] MakeGuestFBConfigArray GuestMalloc=%p\n", reinterpret_cast<void*>(GuestMalloc));
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
    HyMobileAbortThunkNullTarget("MakeGuestVisualInfo", "GuestMalloc is unset", kThunkGLGuestMalloc);
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
  return MakeGuestFBConfigArray(NumItems);
}

guest_layout<GLXFBConfigSGIX*> fexfn_impl_libGL_glXChooseFBConfigSGIX(Display*, int, int*, int* NumItems) {
  return MakeGuestFBConfigArraySGIX(NumItems);
}

guest_layout<_XDisplay*> fexfn_impl_libGL_glXGetCurrentDisplay() {
  return guest_layout<_XDisplay*> {.data = 0};
}

guest_layout<_XDisplay*> fexfn_impl_libGL_glXGetCurrentDisplayEXT() {
  return guest_layout<_XDisplay*> {.data = 0};
}

guest_layout<GLXFBConfig*> fexfn_impl_libGL_glXGetFBConfigs(Display*, int, int* NumItems) {
  return MakeGuestFBConfigArray(NumItems);
}

GLXFBConfigSGIX fexfn_impl_libGL_glXGetFBConfigFromVisualSGIX(Display*, guest_layout<XVisualInfo*>) {
  return reinterpret_cast<GLXFBConfigSGIX>(uintptr_t {1});
}

guest_layout<XVisualInfo*> fexfn_impl_libGL_glXGetVisualFromFBConfigSGIX(Display*, GLXFBConfigSGIX) {
  return MakeGuestVisualInfo();
}

guest_layout<XVisualInfo*> fexfn_impl_libGL_glXChooseVisual(Display*, int, int*) {
  return MakeGuestVisualInfo();
}

GLXContext fexfn_impl_libGL_glXCreateContext(Display*, guest_layout<XVisualInfo*>, GLXContext, Bool) {
  HyMobileTraceThunkEvent("glXCreateContext", HYMOBILE_THUNK_EVENT_ENTER, kThunkGLXCreateContext,
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(eglGetCurrentDisplay())),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(eglGetCurrentSurface(EGL_DRAW))),
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(eglGetCurrentContext())));
  auto context = eglGetCurrentContext();
  auto result = context != EGL_NO_CONTEXT ? reinterpret_cast<GLXContext>(context) : reinterpret_cast<GLXContext>(uintptr_t {1});
  HyMobileTraceThunkEvent("glXCreateContext", HYMOBILE_THUNK_EVENT_EXIT, kThunkGLXCreateContext,
                          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(result)));
  return result;
}

GLXPixmap fexfn_impl_libGL_glXCreateGLXPixmap(Display*, guest_layout<XVisualInfo*>, Pixmap Pixmap) {
  return static_cast<GLXPixmap>(Pixmap != 0 ? Pixmap : 1);
}

GLXPixmap fexfn_impl_libGL_glXCreateGLXPixmapMESA(Display*, guest_layout<XVisualInfo*>, Pixmap Pixmap, Colormap) {
  return static_cast<GLXPixmap>(Pixmap != 0 ? Pixmap : 1);
}

int fexfn_impl_libGL_glXGetConfig(Display*, guest_layout<XVisualInfo*>, int, int* Value) {
  if (Value) {
    *Value = 0;
  }
  return 0;
}

guest_layout<XVisualInfo*> fexfn_impl_libGL_glXGetVisualFromFBConfig(Display*, GLXFBConfig) {
  return MakeGuestVisualInfo();
}
#endif

#ifdef IS_32BIT_THUNK
void fexfn_impl_libGL_glBindBuffersRange(GLenum a_0, GLuint a_1, GLsizei Count, const GLuint* a_3, guest_layout<const int*> Offsets,
                                         guest_layout<const int*> Sizes) {
  auto HostOffsets = (GLintptr*)alloca(Count * sizeof(GLintptr));
  auto HostSizes = (GLsizeiptr*)alloca(Count * sizeof(GLsizeiptr));
  for (int i = 0; i < Count; ++i) {
    HostOffsets[i] = Offsets.get_pointer()[i].data;
    HostSizes[i] = Sizes.get_pointer()[i].data;
  }
  return fexldr_ptr_libGL_glBindBuffersRange(a_0, a_1, Count, a_3, HostOffsets, HostSizes);
}

void fexfn_impl_libGL_glBindVertexBuffers(GLuint a_0, GLsizei count, const GLuint* a_2, guest_layout<const int*> Offsets, const GLsizei* a_4) {
  auto HostOffsets = (GLintptr*)alloca(count * sizeof(GLintptr));
  for (int i = 0; i < count; ++i) {
    HostOffsets[i] = Offsets.get_pointer()[i].data;
  }
  fexldr_ptr_libGL_glBindVertexBuffers(a_0, count, a_2, HostOffsets, a_4);
}

void fexfn_impl_libGL_glGetUniformIndices(GLuint a_0, GLsizei Count, guest_layout<const GLchar* const*> Names, GLuint* a_3) {
  auto HostNames = (const GLchar**)alloca(Count * sizeof(GLintptr));
  for (int i = 0; i < Count; ++i) {
    HostNames[i] = host_layout<const char* const> {Names.get_pointer()[i]}.data;
  }
  fexldr_ptr_libGL_glGetUniformIndices(a_0, Count, HostNames, a_3);
}

void fexfn_impl_libGL_glVertexArrayVertexBuffers(GLuint a_0, GLuint a_1, GLsizei count, const GLuint* a_3, guest_layout<const int*> Offsets,
                                                 const GLsizei* a_5) {
  auto HostOffsets = (GLintptr*)alloca(count * sizeof(GLintptr));
  for (int i = 0; i < count; ++i) {
    HostOffsets[i] = Offsets.get_pointer()[i].data;
  }
  fexldr_ptr_libGL_glVertexArrayVertexBuffers(a_0, a_1, count, a_3, HostOffsets, a_5);
}

void fexfn_impl_libGL_glXGetSelectedEvent(Display* Display, GLXDrawable Drawable, guest_layout<uint32_t*> Mask) {
  unsigned long HostMask;
  fexldr_ptr_libGL_glXGetSelectedEvent(Display, Drawable, &HostMask);
  *Mask.get_pointer() = HostMask;
}
void fexfn_impl_libGL_glXGetSelectedEventSGIX(Display* Display, GLXDrawable Drawable, guest_layout<uint32_t*> Mask) {
  unsigned long HostMask;
  fexldr_ptr_libGL_glXGetSelectedEventSGIX(Display, Drawable, &HostMask);
  *Mask.get_pointer() = HostMask;
}
#endif

EXPORTS(libGL)
