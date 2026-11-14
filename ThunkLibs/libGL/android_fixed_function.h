#pragma once

// Guest-side fixed-function path for glxgears.
// Display lists are baked into static meshes. A later glCallList crosses to the
// host once and only updates the current matrix and color. Immediate glBegin/glEnd
// still works, as one host transition per primitive instead of a full GLES setup.
#include "vexa_evaluator_state.h"
#include "raster_position_core.h"
#include "legacy_lighting_wire.h"
#include "pixel_transfer_state.h"
#include "vexa_selection_state.h"
#include "stipple_core.h"
#include <array>
#include <cmath>
#include <functional>
#include <cstdint>
#include <cstring>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <cstdlib>

extern "C" {
unsigned fexfn_pack_VEXA_FFUpload(const float* vertices, int float_count, unsigned mode);
void fexfn_pack_VEXA_FFDrawList(const unsigned* buffers, const int* counts, const float* colors, int item_count, const float* mvp);
void fexfn_pack_VEXA_FFDrawImmediate(const float* vertices, int float_count, const float* mvp, const float* color, unsigned mode);
void fexfn_pack_VEXA_FFDelete(unsigned buffer);
void fexfn_pack_VEXA_SetClipMatrices(const float* model, const float* projection, unsigned mode);
void fexfn_pack_VEXA_MultiDrawIndirect(unsigned mode, unsigned type, uintptr_t indirect, int drawcount, int stride);
void fexfn_pack_VEXA_MultiDrawElementsBV(unsigned mode, unsigned type, const int* counts, const unsigned* offsets,
                                         const int* basevertex, int drawcount);
void fexfn_pack_VEXA_BindMany(unsigned op, unsigned target, unsigned first, int count, const unsigned* names,
                              const unsigned* offsets, const unsigned* sizes);
}

namespace vexa_ff {

constexpr unsigned kModelView = 0x1700;
constexpr unsigned kProjection = 0x1701;
constexpr unsigned kTexture = 0x1702;
constexpr unsigned kCompile = 0x1300;
constexpr unsigned kQuads = 0x0007;
constexpr unsigned kQuadStrip = 0x0008;
constexpr unsigned kTriangles = 0x0004;
constexpr unsigned kTriangleStrip = 0x0005;
constexpr unsigned kTriangleFan = 0x0006;
constexpr unsigned kDiffuse = 0x1201;
constexpr unsigned kAmbientAndDiffuse = 0x1602;

enum class CmdOp : uint8_t {
  Begin,
  End,
  Vertex,
  Normal,
  Material,
  MatrixMode,
  LoadIdentity,
  Translate,
  Rotate,
  Frustum,
  Push,
  Pop,
  Call,
  ListBase,
  CallOffset,
  Callback,
};

struct Cmd {
  CmdOp op = CmdOp::End;
  unsigned mode = 0;
  float a = 0.f;
  float b = 0.f;
  float c = 0.f;
  float d = 0.f;
  float e = 0.f;
  float f = 0.f;
  std::function<void()> callback;
};

struct Batch {
  float color[4] {0.8f, 0.8f, 0.8f, 1.f};
  bool live_color = true;
  std::vector<float> verts;
  unsigned buffer = 0;
  int vertex_count = 0;
  unsigned mode = 4;
  bool mergeable = true;
};

struct DisplayList {
  std::vector<Batch> batches;
  std::vector<Cmd> cmds;
  bool baked = false;
  bool defined = false;
  bool sets_color = false;
  bool sets_normal = false;
  float end_color[4] {0.8f, 0.8f, 0.8f, 1.f};
  float end_normal[3] {0.f, 0.f, 1.f};
};

struct State {
  vexa_stipple::State stipple;
  std::function<void(const std::vector<float>&, const float*, unsigned)> stipple_draw;
  vexa_select::State selection;
  std::function<void(const std::vector<float>&,unsigned)> selection_draw;
  float model[16] {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
  float projection[16] {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
  float texture[16] {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
  float texture_stack[32][16] {};
  int texture_depth = 0;
  float model_stack[32][16] {};
  float projection_stack[32][16] {};
  int model_depth = 0;
  int projection_depth = 0;
  unsigned matrix_mode = kModelView;
  bool inside = false;
  unsigned begin_mode = 0;
  float normal[3] {0.f, 0.f, 1.f};
  float texcoord[4] {0.f, 0.f, 0.f, 1.f};
  float color[4] {0.8f, 0.8f, 0.8f, 1.f};
  std::vector<float> verts;
  bool compiling = false;
  bool compile_execute = false;
  unsigned list_depth = 0;
  bool compile_inside = false;
  vexa_eval::State evaluator;
  vexa_raster::Position raster;
  vexa_legacy_lighting::State lighting;
  vexa_pixel::PixelTransferState pixels;
  bool unpack_swap_bytes = false, unpack_lsb_first = false, pack_lsb_first = false, pack_swap_bytes = false;
  double color_index = 1;
  float clear_index = 0;
  unsigned index_mask = ~0U;
  bool edge_flag = true;
  float accum_clear[4] = {};
  unsigned compile_id = 0;
  unsigned list_base = 0;
  std::vector<Cmd> compile_cmds;
  std::vector<DisplayList> lists {1};
  std::vector<unsigned> scratch_buffers;
  std::vector<int> scratch_counts;
  std::vector<float> scratch_colors;
};

inline bool clip_enabled() {static const bool enabled=[] {const auto v=getenv("VEXA_GL_CLIP_PLANES");return v && !strcmp(v,"1");}();return enabled;}
inline std::unordered_map<uintptr_t,std::shared_ptr<State>>& context_states() {static std::unordered_map<uintptr_t,std::shared_ptr<State>> states;return states;}
inline std::mutex& context_mutex() {static std::mutex mutex;return mutex;}
inline std::shared_ptr<State>& active_state() {static thread_local std::shared_ptr<State> state;return state;}
inline State& S() {static thread_local State fallback;return active_state()?*active_state():fallback;}
inline bool tracked_contexts() {
  static const bool real_x11 = [] { const char* v = getenv("VEXA_REAL_X11"); return v && !strcmp(v, "1"); }();
  return real_x11 || clip_enabled();
}
inline void select_context(uintptr_t context) {
  if (!tracked_contexts()) return;
  if (!context) { active_state().reset(); return; }
  std::lock_guard lock(context_mutex()); auto& slot = context_states()[context];
  if (!slot) slot = std::make_shared<State>();
  active_state() = slot;
}
inline void forget_context(uintptr_t context) {
  if (!tracked_contexts()) return;
  std::lock_guard lock(context_mutex()); context_states().erase(context);
}
inline void sync_clip_matrices() {if(clip_enabled())fexfn_pack_VEXA_SetClipMatrices(S().model,S().projection,S().matrix_mode);}

inline void identity(float* m) {
  std::memset(m, 0, sizeof(float) * 16);
  m[0] = m[5] = m[10] = m[15] = 1.f;
}

inline float* current() {
  return S().matrix_mode == kProjection ? S().projection : S().matrix_mode == kTexture ? S().texture : S().model;
}

inline void multiply(float* a, const float* b) {
  float out[16];
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      out[col * 4 + row] = a[row] * b[col * 4] + a[4 + row] * b[col * 4 + 1] + a[8 + row] * b[col * 4 + 2] + a[12 + row] * b[col * 4 + 3];
    }
  }
  std::memcpy(a, out, sizeof(out));
}

inline void current_mvp(float mvp[16]) {
  std::memcpy(mvp, S().projection, sizeof(float) * 16);
  multiply(mvp, S().model);
}

inline void push_vertex(std::vector<float>& dst, float x, float y, float z, float w, const float normal[3], const float color[4], const float texcoord[4]) {
  dst.push_back(x);
  dst.push_back(y);
  dst.push_back(z);
  dst.push_back(w);
  dst.push_back(normal[0]);
  dst.push_back(normal[1]);
  dst.push_back(normal[2]);
  dst.insert(dst.end(), color, color + 4);
  dst.insert(dst.end(), texcoord, texcoord + 4);
}

inline std::vector<float> expand(unsigned mode, const std::vector<float>& src) {
  std::vector<float> out;
  const int count = static_cast<int>(src.size() / 15);
  auto add = [&](int index) {
    out.insert(out.end(), src.begin() + index * 15, src.begin() + (index + 1) * 15);
  };
  if (mode <= 3 || mode == kTriangles) {
    return src;
  }
  if (mode == kQuads) {
    for (int i = 0; i + 3 < count; i += 4) {
      add(i);
      add(i + 1);
      add(i + 2);
      add(i);
      add(i + 2);
      add(i + 3);
    }
  } else if (mode == kQuadStrip) {
    for (int i = 0; i + 3 < count; i += 2) {
      add(i);
      add(i + 1);
      add(i + 2);
      add(i + 2);
      add(i + 1);
      add(i + 3);
    }
  } else if (mode == kTriangleStrip) {
    for (int i = 0; i + 2 < count; ++i) {
      if ((i & 1) == 0) {
        add(i);
        add(i + 1);
        add(i + 2);
      } else {
        add(i + 1);
        add(i);
        add(i + 2);
      }
    }
  } else if (mode == kTriangleFan && count >= 3) {
    for (int i = 1; i + 1 < count; ++i) {
      add(0);
      add(i);
      add(i + 1);
    }
  }
  return out;
}

inline void draw_immediate(const std::vector<float>& data, const float color[4], unsigned mode) {
  if (data.size() < 15 || !color) {
    return;
  }
  float mvp[16];
  current_mvp(mvp);
#ifdef VEXA_FF_LIGHTING_BRIDGE
  vexa_legacy_lighting::Mat4 model;
  std::copy_n(S().model, 16, model.begin());
  vexa_legacy_lighting::Wire wire{};
  if (vexa_legacy_lighting::build_wire(S().lighting, model, wire, S().texture))
    fexfn_pack_VEXA_FFSetLighting(&wire, sizeof(wire));
#endif
  if (S().stipple_draw) { S().stipple_draw(data, color, mode); return; }
  fexfn_pack_VEXA_FFDrawImmediate(data.data(), static_cast<int>(data.size()), mvp, color, mode <= 3 ? mode : kTriangles);
}

inline void destroy_list(DisplayList& list) {
  for (Batch& batch : list.batches) {
    if (batch.buffer != 0) {
      fexfn_pack_VEXA_FFDelete(batch.buffer);
      batch.buffer = 0;
    }
  }
  list.batches.clear();
  list.cmds.clear();
  list.baked = false;
  list.defined = false;
  list.sets_color = false;
  list.sets_normal = false;
}

inline bool is_matrix_op(CmdOp op) {
  return op == CmdOp::MatrixMode || op == CmdOp::LoadIdentity || op == CmdOp::Translate || op == CmdOp::Rotate || op == CmdOp::Frustum ||
         op == CmdOp::Push || op == CmdOp::Pop || op == CmdOp::Call || op == CmdOp::ListBase || op == CmdOp::CallOffset || op == CmdOp::Callback;
}

inline bool bake_commands(const std::vector<Cmd>& cmds, DisplayList& out) {
  // Color commands may occur between vertices; preserve their ordering on replay.
  for (const auto& cmd : cmds) if (cmd.op == CmdOp::Material) return false;
  float color[4] {0.8f, 0.8f, 0.8f, 1.f};
  float normal[3] {0.f, 0.f, 1.f};
  bool color_from_list = false;
  bool normal_from_list = false;
  bool inside = false;
  unsigned mode = 0;
  std::vector<float> verts;
  std::vector<Batch> batches;

  auto same_color = [](const Batch& batch, const float rgba[4], bool live) {
    return batch.live_color == live && (live || std::memcmp(batch.color, rgba, sizeof(float) * 4) == 0);
  };
  auto flush_prim = [&]() {
    std::vector<float> tris = expand(mode, verts);
    verts.clear();
    inside = false;
    if (tris.size() < 15) {
      return;
    }
    const bool live = !color_from_list;
    if (batches.empty() || !batches.back().mergeable || mode == 2 || mode == 3 || mode == 1 || (mode == kTriangles && (tris.size() / 15) % 3 != 0) || batches.back().mode != (mode <= 3 ? mode : kTriangles) || !same_color(batches.back(), color, live)) {
      Batch batch;
      std::memcpy(batch.color, color, sizeof(batch.color));
      batch.live_color = live;
      batch.mode = mode <= 3 ? mode : kTriangles;
      batch.mergeable = !(mode == 1 || mode == 2 || mode == 3 || (mode == kTriangles && (tris.size() / 15) % 3 != 0));
      batches.push_back(std::move(batch));
    }
    batches.back().verts.insert(batches.back().verts.end(), tris.begin(), tris.end());
  };

  for (const Cmd& cmd : cmds) {
    if (is_matrix_op(cmd.op)) {
      return false;
    }
    switch (cmd.op) {
    case CmdOp::Begin:
      inside = true;
      mode = cmd.mode;
      verts.clear();
      break;
    case CmdOp::Vertex:
      if (inside) {
        const float texcoord[4] {0,0,0,1};
        push_vertex(verts, cmd.a, cmd.b, cmd.c, cmd.d, normal, color, texcoord);
      }
      break;
    case CmdOp::Normal:
      normal[0] = cmd.a;
      normal[1] = cmd.b;
      normal[2] = cmd.c;
      normal_from_list = true;
      break;
    case CmdOp::Material:
      color[0] = cmd.a;
      color[1] = cmd.b;
      color[2] = cmd.c;
      color[3] = cmd.d;
      color_from_list = true;
      break;
    case CmdOp::End:
      flush_prim();
      break;
    default:
      break;
    }
  }

  out.batches = std::move(batches);
  out.sets_color = color_from_list;
  out.sets_normal = normal_from_list;
  std::memcpy(out.end_color, color, sizeof(out.end_color));
  std::memcpy(out.end_normal, normal, sizeof(out.end_normal));
  return true;
}

inline void apply(const Cmd& cmd);

inline void call_list(unsigned list) {
  State& state = S();
  if (state.compiling) {
    Cmd cmd;
    cmd.op = CmdOp::Call;
    cmd.mode = list;
    state.compile_cmds.push_back(cmd);
    if (state.compile_execute) {
      state.compiling = false;
      call_list(list);
      state.compiling = true;
    }
    return;
  }
  if (list >= state.lists.size() || !state.lists[list].defined) {
    return;
  }
  // Recursive lists are legal, but nesting beyond the advertised limit is ignored.
  if (state.list_depth >= 64) return;
  struct DepthScope { unsigned& depth; DepthScope(unsigned& d):depth(d){++depth;} ~DepthScope(){--depth;} } depth(state.list_depth);
  DisplayList& compiled = state.lists[list];
  if (!compiled.baked || state.lighting.lighting_enabled || state.selection.mode != vexa_select::kRender || state.stipple.line || state.stipple.polygon) {
    for (const Cmd& cmd : compiled.cmds) {
      apply(cmd);
    }
    return;
  }

  float mvp[16];
  current_mvp(mvp);
  state.scratch_buffers.clear();
  state.scratch_counts.clear();
  state.scratch_colors.clear();
  for (const Batch& batch : compiled.batches) {
    if (batch.buffer == 0 || batch.vertex_count <= 0) {
      continue;
    }
    state.scratch_buffers.push_back(batch.buffer);
    state.scratch_counts.push_back(batch.vertex_count);
    const float* color = batch.live_color ? state.color : batch.color;
    state.scratch_colors.insert(state.scratch_colors.end(), color, color + 4);
  }
  if (!state.scratch_buffers.empty()) {
#ifdef VEXA_FF_LIGHTING_BRIDGE
    vexa_legacy_lighting::Mat4 model;
    std::copy_n(state.model, 16, model.begin());
    vexa_legacy_lighting::Wire wire{};
    if (vexa_legacy_lighting::build_wire(state.lighting, model, wire, state.texture))
      fexfn_pack_VEXA_FFSetLighting(&wire, sizeof(wire));
#endif
    fexfn_pack_VEXA_FFDrawList(state.scratch_buffers.data(), state.scratch_counts.data(), state.scratch_colors.data(),
                               static_cast<int>(state.scratch_buffers.size()), mvp);
  }
  if (compiled.sets_color) {
    std::memcpy(state.color, compiled.end_color, sizeof(state.color));
  }
  if (compiled.sets_normal) {
    std::memcpy(state.normal, compiled.end_normal, sizeof(state.normal));
  }
}

inline void apply(const Cmd& cmd) {
  State& state = S();
  switch (cmd.op) {
  case CmdOp::Begin:
    state.inside = true;
    state.begin_mode = cmd.mode;
    state.verts.clear();
    break;
  case CmdOp::Vertex:
    if (state.inside) {
      push_vertex(state.verts, cmd.a, cmd.b, cmd.c, cmd.d, state.normal, state.color, state.texcoord);
    }
    break;
  case CmdOp::Normal:
    state.normal[0] = cmd.a;
    state.normal[1] = cmd.b;
    state.normal[2] = cmd.c;
    break;
  case CmdOp::Material:
    state.color[0] = cmd.a;
    state.color[1] = cmd.b;
    state.color[2] = cmd.c;
    state.color[3] = cmd.d;
    if (state.lighting.color_material_enabled) state.lighting.update_color_material(state.color);
    break;
  case CmdOp::End: {
    if(state.selection.mode != vexa_select::kRender && state.selection_draw) {
      state.selection_draw(state.verts,state.begin_mode);
      state.inside=false;state.verts.clear();break;
    }
    std::vector<float> tris = expand(state.begin_mode, state.verts);
    float color[4];
    std::memcpy(color, state.color, sizeof(color));
    state.inside = false;
    state.verts.clear();
    draw_immediate(tris, color, state.begin_mode);
    break;
  }
  case CmdOp::MatrixMode:
    state.matrix_mode = cmd.mode;
    break;
  case CmdOp::LoadIdentity:
    identity(current());
    break;
  case CmdOp::Translate: {
    float m[16];
    identity(m);
    m[12] = cmd.a;
    m[13] = cmd.b;
    m[14] = cmd.c;
    multiply(current(), m);
    break;
  }
  case CmdOp::Rotate: {
    const float length = std::sqrt(cmd.b * cmd.b + cmd.c * cmd.c + cmd.d * cmd.d);
    if (length == 0.f) {
      break;
    }
    const float nx = cmd.b / length;
    const float ny = cmd.c / length;
    const float nz = cmd.d / length;
    const float radians = cmd.a * 0.01745329252f;
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float ic = 1.f - c;
    float m[16] {};
    m[0] = nx * nx * ic + c;
    m[1] = ny * nx * ic + nz * s;
    m[2] = nz * nx * ic - ny * s;
    m[4] = nx * ny * ic - nz * s;
    m[5] = ny * ny * ic + c;
    m[6] = nz * ny * ic + nx * s;
    m[8] = nx * nz * ic + ny * s;
    m[9] = ny * nz * ic - nx * s;
    m[10] = nz * nz * ic + c;
    m[15] = 1.f;
    multiply(current(), m);
    break;
  }
  case CmdOp::Frustum: {
    float m[16] {};
    m[0] = (2.f * cmd.e) / (cmd.b - cmd.a);
    m[5] = (2.f * cmd.e) / (cmd.d - cmd.c);
    m[8] = (cmd.b + cmd.a) / (cmd.b - cmd.a);
    m[9] = (cmd.d + cmd.c) / (cmd.d - cmd.c);
    m[10] = -(cmd.f + cmd.e) / (cmd.f - cmd.e);
    m[11] = -1.f;
    m[14] = -(2.f * cmd.f * cmd.e) / (cmd.f - cmd.e);
    multiply(current(), m);
    break;
  }
  case CmdOp::Push:
    if (state.matrix_mode == kProjection) {
      if (state.projection_depth < 32) {
        std::memcpy(state.projection_stack[state.projection_depth++], state.projection, sizeof(state.projection));
      }
    } else if (state.matrix_mode == kTexture) {
      if (state.texture_depth < 32) std::memcpy(state.texture_stack[state.texture_depth++], state.texture, sizeof(state.texture));
    } else if (state.model_depth < 32) {
      std::memcpy(state.model_stack[state.model_depth++], state.model, sizeof(state.model));
    }
    break;
  case CmdOp::Pop:
    if (state.matrix_mode == kProjection) {
      if (state.projection_depth > 0) {
        std::memcpy(state.projection, state.projection_stack[--state.projection_depth], sizeof(state.projection));
      }
    } else if (state.matrix_mode == kTexture) {
      if (state.texture_depth > 0) std::memcpy(state.texture, state.texture_stack[--state.texture_depth], sizeof(state.texture));
    } else if (state.model_depth > 0) {
      std::memcpy(state.model, state.model_stack[--state.model_depth], sizeof(state.model));
    }
    break;
  case CmdOp::Call:
    call_list(cmd.mode);
    break;
  case CmdOp::ListBase:
    state.list_base = cmd.mode;
    break;
  case CmdOp::CallOffset:
    call_list(state.list_base + cmd.mode);
    break;
  case CmdOp::Callback:
    if (cmd.callback) cmd.callback();
    break;
  }
  if (cmd.op >= CmdOp::MatrixMode && cmd.op <= CmdOp::Pop) sync_clip_matrices();
}

inline void run_or_record(Cmd cmd) {
  if (S().compiling) {
    S().compile_cmds.push_back(cmd);
    if (S().compile_execute) {
      S().compiling = false;
      apply(cmd);
      S().compiling = true;
    }
  } else {
    apply(cmd);
  }
}

inline void run_or_record_callback(std::function<void()> callback) {
  Cmd cmd; cmd.op = CmdOp::Callback; cmd.callback = std::move(callback); run_or_record(std::move(cmd));
}

inline void matrix(const float* values, bool multiply_current) {
  std::array<float,16> copy;
  std::memcpy(copy.data(), values, sizeof(float)*16);
  run_or_record_callback([copy, multiply_current] {
    if (multiply_current) multiply(current(), copy.data());
    else std::memcpy(current(), copy.data(), sizeof(float)*16);
    sync_clip_matrices();
  });
}

inline bool in_begin() { return S().compiling ? S().compile_inside : S().inside; }

inline void begin(unsigned mode) {
  if (S().compiling) S().compile_inside = true;
  Cmd cmd;
  cmd.op = CmdOp::Begin;
  cmd.mode = mode;
  run_or_record(cmd);
}

inline void end() {
  if (S().compiling) S().compile_inside = false;
  Cmd cmd;
  cmd.op = CmdOp::End;
  run_or_record(cmd);
}

inline void vertex4(float x, float y, float z, float w) {
  Cmd cmd;
  cmd.op = CmdOp::Vertex;
  cmd.a = x;
  cmd.b = y;
  cmd.c = z;
  cmd.d = w;
  run_or_record(cmd);
}
inline void vertex(float x, float y, float z) { vertex4(x, y, z, 1.0f); }

inline void normal(float x, float y, float z) {
  Cmd cmd;
  cmd.op = CmdOp::Normal;
  cmd.a = x;
  cmd.b = y;
  cmd.c = z;
  run_or_record(cmd);
}

inline void matrix_mode(unsigned mode) {
  Cmd cmd;
  cmd.op = CmdOp::MatrixMode;
  cmd.mode = mode;
  run_or_record(cmd);
}

inline void load_identity() {
  Cmd cmd;
  cmd.op = CmdOp::LoadIdentity;
  run_or_record(cmd);
}

inline void translate(float x, float y, float z) {
  Cmd cmd;
  cmd.op = CmdOp::Translate;
  cmd.a = x;
  cmd.b = y;
  cmd.c = z;
  run_or_record(cmd);
}

inline void rotate(float angle, float x, float y, float z) {
  Cmd cmd;
  cmd.op = CmdOp::Rotate;
  cmd.a = angle;
  cmd.b = x;
  cmd.c = y;
  cmd.d = z;
  run_or_record(cmd);
}

inline void frustum(float left, float right, float bottom, float top, float znear, float zfar) {
  Cmd cmd;
  cmd.op = CmdOp::Frustum;
  cmd.a = left;
  cmd.b = right;
  cmd.c = bottom;
  cmd.d = top;
  cmd.e = znear;
  cmd.f = zfar;
  run_or_record(cmd);
}

inline void push_matrix() {
  Cmd cmd;
  cmd.op = CmdOp::Push;
  run_or_record(cmd);
}

inline void pop_matrix() {
  Cmd cmd;
  cmd.op = CmdOp::Pop;
  run_or_record(cmd);
}

inline unsigned gen_lists(int count) {
  if (count <= 0) return 0;
  State& state = S();
  const unsigned first = static_cast<unsigned>(state.lists.size());
  state.lists.resize(state.lists.size() + static_cast<size_t>(count > 0 ? count : 0));
  return first;
}

inline void new_list(unsigned list, unsigned mode) {
  State& state = S();
  if (!list || state.compiling || list >= state.lists.size() || (mode != kCompile && mode != 0x1301)) {
    return;
  }
  destroy_list(state.lists[list]);
  state.compiling = true;
  state.compile_execute = mode == 0x1301;
  state.compile_id = list;
  state.compile_cmds.clear();
  state.compile_inside = false;
}

inline void end_list() {
  State& state = S();
  if (!state.compiling || state.compile_id >= state.lists.size()) {
    state.compiling = false;
    return;
  }
  const unsigned id = state.compile_id;
  std::vector<Cmd> cmds = std::move(state.compile_cmds);
  state.compiling = false;
  DisplayList& list = state.lists[id];
  list.defined = true;
  if (!bake_commands(cmds, list)) {
    list.cmds = std::move(cmds);
    list.baked = false;
    fprintf(stderr, "[VEXA][FF] replay list %u cmds %zu\n", id, list.cmds.size());
    return;
  }
  int verts = 0;
  for (Batch& batch : list.batches) {
    if (batch.verts.size() < 15) {
      batch.verts.clear();
      continue;
    }
    batch.vertex_count = static_cast<int>(batch.verts.size() / 15);
    batch.buffer = fexfn_pack_VEXA_FFUpload(batch.verts.data(), static_cast<int>(batch.verts.size()), batch.mode);
    verts += batch.vertex_count;
    batch.verts.clear();
    batch.verts.shrink_to_fit();
  }
  list.baked = true;
  list.cmds = std::move(cmds); // Replay original primitives in selection/feedback mode.
  fprintf(stderr, "[VEXA][FF] baked list %u batches %zu verts %d\n", id, list.batches.size(), verts);
}

inline void delete_lists(unsigned list, int count) {
  State& state = S();
  for (int i = 0; i < count; ++i) {
    const unsigned id = list + static_cast<unsigned>(i);
    if (id < state.lists.size()) {
      destroy_list(state.lists[id]);
    }
  }
}

inline bool is_list(unsigned list) {
  const State& state = S();
  return list && list < state.lists.size() && state.lists[list].defined;
}
inline void list_base(unsigned base) {
  Cmd cmd; cmd.op = CmdOp::ListBase; cmd.mode = base; run_or_record(cmd);
}
inline void call_offset(unsigned offset) {
  Cmd cmd; cmd.op = CmdOp::CallOffset; cmd.mode = offset; run_or_record(cmd);
}

inline void material(unsigned pname, const float* params) {
  if (!params) {
    return;
  }
  if (pname != kDiffuse && pname != kAmbientAndDiffuse) {
    return;
  }
  Cmd cmd;
  cmd.op = CmdOp::Material;
  cmd.a = params[0];
  cmd.b = params[1];
  cmd.c = params[2];
  cmd.d = params[3];
  run_or_record(cmd);
}

inline void init_matrices() { sync_clip_matrices(); }

} // namespace vexa_ff
