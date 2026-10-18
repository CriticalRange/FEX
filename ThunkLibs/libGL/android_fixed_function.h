#pragma once

// Guest-side fixed-function path for glxgears.
// Display lists are baked into static meshes. A later glCallList crosses to the
// host once and only updates the current matrix and color. Immediate glBegin/glEnd
// still works, as one host transition per primitive instead of a full GLES setup.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
unsigned fexfn_pack_VEXA_FFUpload(const float* vertices, int float_count);
void fexfn_pack_VEXA_FFDrawList(const unsigned* buffers, const int* counts, const float* colors, int item_count, const float* mvp);
void fexfn_pack_VEXA_FFDrawImmediate(const float* vertices, int float_count, const float* mvp, const float* color);
void fexfn_pack_VEXA_FFDelete(unsigned buffer);
void fexfn_pack_VEXA_MultiDrawIndirect(unsigned mode, unsigned type, uintptr_t indirect, int drawcount, int stride);
void fexfn_pack_VEXA_MultiDrawElementsBV(unsigned mode, unsigned type, const int* counts, const unsigned* offsets,
                                         const int* basevertex, int drawcount);
void fexfn_pack_VEXA_BindMany(unsigned op, unsigned target, unsigned first, int count, const unsigned* names,
                              const unsigned* offsets, const unsigned* sizes);
}

namespace vexa_ff {

constexpr unsigned kModelView = 0x1700;
constexpr unsigned kProjection = 0x1701;
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
};

struct Batch {
  float color[4] {0.8f, 0.8f, 0.8f, 1.f};
  bool live_color = true;
  std::vector<float> verts;
  unsigned buffer = 0;
  int vertex_count = 0;
};

struct DisplayList {
  std::vector<Batch> batches;
  std::vector<Cmd> cmds;
  bool baked = false;
  bool sets_color = false;
  bool sets_normal = false;
  float end_color[4] {0.8f, 0.8f, 0.8f, 1.f};
  float end_normal[3] {0.f, 0.f, 1.f};
};

struct State {
  float model[16] {};
  float projection[16] {};
  float model_stack[32][16] {};
  float projection_stack[32][16] {};
  int model_depth = 0;
  int projection_depth = 0;
  unsigned matrix_mode = kModelView;
  bool inside = false;
  unsigned begin_mode = 0;
  float normal[3] {0.f, 0.f, 1.f};
  float color[4] {0.8f, 0.8f, 0.8f, 1.f};
  std::vector<float> verts;
  bool compiling = false;
  unsigned compile_id = 0;
  std::vector<Cmd> compile_cmds;
  std::vector<DisplayList> lists {1};
  std::vector<unsigned> scratch_buffers;
  std::vector<int> scratch_counts;
  std::vector<float> scratch_colors;
};

inline State& S() {
  static State state;
  return state;
}

inline void identity(float* m) {
  std::memset(m, 0, sizeof(float) * 16);
  m[0] = m[5] = m[10] = m[15] = 1.f;
}

inline float* current() {
  return S().matrix_mode == kProjection ? S().projection : S().model;
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

inline void push_vertex(std::vector<float>& dst, float x, float y, float z, const float normal[3]) {
  dst.push_back(x);
  dst.push_back(y);
  dst.push_back(z);
  dst.push_back(normal[0]);
  dst.push_back(normal[1]);
  dst.push_back(normal[2]);
}

inline std::vector<float> expand(unsigned mode, const std::vector<float>& src) {
  std::vector<float> out;
  const int count = static_cast<int>(src.size() / 6);
  auto add = [&](int index) {
    out.insert(out.end(), src.begin() + index * 6, src.begin() + (index + 1) * 6);
  };
  if (mode == kTriangles) {
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

inline void draw_immediate(const std::vector<float>& data, const float color[4]) {
  if (data.size() < 18 || !color) {
    return;
  }
  float mvp[16];
  current_mvp(mvp);
  fexfn_pack_VEXA_FFDrawImmediate(data.data(), static_cast<int>(data.size()), mvp, color);
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
  list.sets_color = false;
  list.sets_normal = false;
}

inline bool is_matrix_op(CmdOp op) {
  return op == CmdOp::MatrixMode || op == CmdOp::LoadIdentity || op == CmdOp::Translate || op == CmdOp::Rotate || op == CmdOp::Frustum ||
         op == CmdOp::Push || op == CmdOp::Pop || op == CmdOp::Call;
}

inline bool bake_commands(const std::vector<Cmd>& cmds, DisplayList& out) {
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
    if (tris.size() < 18) {
      return;
    }
    const bool live = !color_from_list;
    if (batches.empty() || !same_color(batches.back(), color, live)) {
      Batch batch;
      std::memcpy(batch.color, color, sizeof(batch.color));
      batch.live_color = live;
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
        push_vertex(verts, cmd.a, cmd.b, cmd.c, normal);
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
    return;
  }
  if (list >= state.lists.size()) {
    return;
  }
  DisplayList& compiled = state.lists[list];
  if (!compiled.baked) {
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
      push_vertex(state.verts, cmd.a, cmd.b, cmd.c, state.normal);
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
    break;
  case CmdOp::End: {
    std::vector<float> tris = expand(state.begin_mode, state.verts);
    float color[4];
    std::memcpy(color, state.color, sizeof(color));
    state.inside = false;
    state.verts.clear();
    draw_immediate(tris, color);
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
    } else if (state.model_depth < 32) {
      std::memcpy(state.model_stack[state.model_depth++], state.model, sizeof(state.model));
    }
    break;
  case CmdOp::Pop:
    if (state.matrix_mode == kProjection) {
      if (state.projection_depth > 0) {
        std::memcpy(state.projection, state.projection_stack[--state.projection_depth], sizeof(state.projection));
      }
    } else if (state.model_depth > 0) {
      std::memcpy(state.model, state.model_stack[--state.model_depth], sizeof(state.model));
    }
    break;
  case CmdOp::Call:
    call_list(cmd.mode);
    break;
  }
}

inline void run_or_record(Cmd cmd) {
  if (S().compiling) {
    S().compile_cmds.push_back(cmd);
  } else {
    apply(cmd);
  }
}

inline void begin(unsigned mode) {
  Cmd cmd;
  cmd.op = CmdOp::Begin;
  cmd.mode = mode;
  run_or_record(cmd);
}

inline void end() {
  Cmd cmd;
  cmd.op = CmdOp::End;
  run_or_record(cmd);
}

inline void vertex(float x, float y, float z) {
  Cmd cmd;
  cmd.op = CmdOp::Vertex;
  cmd.a = x;
  cmd.b = y;
  cmd.c = z;
  run_or_record(cmd);
}

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
  State& state = S();
  const unsigned first = static_cast<unsigned>(state.lists.size());
  state.lists.resize(state.lists.size() + static_cast<size_t>(count > 0 ? count : 0));
  return first;
}

inline void new_list(unsigned list, unsigned mode) {
  State& state = S();
  if (list >= state.lists.size() || mode != kCompile) {
    return;
  }
  destroy_list(state.lists[list]);
  state.compiling = true;
  state.compile_id = list;
  state.compile_cmds.clear();
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
  if (!bake_commands(cmds, list)) {
    list.cmds = std::move(cmds);
    list.baked = false;
    fprintf(stderr, "[VEXA][FF] replay list %u cmds %zu\n", id, list.cmds.size());
    return;
  }
  int verts = 0;
  for (Batch& batch : list.batches) {
    if (batch.verts.size() < 18) {
      batch.verts.clear();
      continue;
    }
    batch.vertex_count = static_cast<int>(batch.verts.size() / 6);
    batch.buffer = fexfn_pack_VEXA_FFUpload(batch.verts.data(), static_cast<int>(batch.verts.size()));
    verts += batch.vertex_count;
    batch.verts.clear();
    batch.verts.shrink_to_fit();
  }
  list.baked = true;
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

inline void init_matrices() {
  static bool ready = false;
  if (!ready) {
    identity(S().model);
    identity(S().projection);
    ready = true;
  }
}

} // namespace vexa_ff
