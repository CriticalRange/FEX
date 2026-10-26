// VEXA: GPU compute BCn decode for the GL host thunk.
// Compute shaders are ported from bionic-vulkan-wrapper's s3tc.comp/bc7.comp/
// bc6.comp decode approach but reproduce bcdec's integer arithmetic exactly so
// the uploaded bytes match the CPU path bit-for-bit.
/*
 * Copyright (c) 2020-2024 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

// ---------------------------------------------------------------------------
// Table buffer layout (uint32 words, byte-packed 4 cells per word):
//   [0,256)    BC7 partition sets, numPartitions==2 (64 sets x 4 words)
//   [256,512)  BC7 partition sets, numPartitions==3
//   [512,640)  BC6H partition sets (32 x 4)
//   [640,644)  aWeight2
//   [644,652)  aWeight3
//   [652,668)  aWeight4
//   [668,672)  BC7 actual_bits_count[2][8] packed bytes
//   [672,686)  BC6H actual_bits_count[4][14] packed bytes
// ---------------------------------------------------------------------------
static const uint32_t kVexaBcnTableWords[] = {
0x01010080u, 0x01010000u, 0x01010000u, 0x81010000u, 0x01000080u, 0x01000000u, 0x01000000u, 0x81000000u, 0x01010180u, 0x01010100u, 0x01010100u, 0x81010100u, 0x01000080u, 0x01010000u, 0x01010000u, 0x81010100u, 0x00000080u, 0x01000000u, 0x01000000u, 0x81010000u, 0x01010080u, 0x01010100u, 0x01010100u, 0x81010101u, 0x01000080u, 0x01010000u, 0x01010100u, 0x81010101u, 0x00000080u, 0x01000000u, 0x01010000u, 0x81010100u, 0x00000080u, 0x00000000u, 0x01000000u, 0x81010000u, 0x01010080u, 0x01010100u, 0x01010101u, 0x81010101u, 0x00000080u, 0x01000000u, 0x01010100u, 0x81010101u, 0x00000080u, 0x00000000u, 0x01000000u, 0x81010100u, 0x01000080u, 0x01010100u, 0x01010101u, 0x81010101u, 0x00000080u, 0x00000000u, 0x01010101u, 0x81010101u, 0x00000080u, 0x01010101u, 0x01010101u, 0x81010101u, 0x00000080u, 0x00000000u, 0x00000000u, 0x81010101u, 0x00000080u, 0x00000001u, 0x00010101u, 0x81010101u, 0x01810180u, 0x01000000u, 0x00000000u, 0x00000000u, 0x00000080u, 0x00000000u, 0x00000081u, 0x00010101u, 0x01810180u, 0x01010000u, 0x01000000u, 0x00000000u, 0x01810080u, 0x01000000u, 0x00000000u, 0x00000000u, 0x00000080u, 0x00000001u, 0x00000181u, 0x00010101u, 0x00000080u, 0x00000000u, 0x00000081u, 0x00000101u, 0x01010180u, 0x01010000u, 0x01010000u, 0x81000000u, 0x01810080u, 0x01000000u, 0x01000000u, 0x00000000u, 0x00000080u, 0x00000001u, 0x00000081u, 0x00000101u, 0x00810180u, 0x00010100u, 0x00010100u, 0x00010100u, 0x01810080u, 0x00010100u, 0x00010100u, 0x00000101u, 0x01000080u, 0x01010100u, 0x00010181u, 0x00000001u, 0x00000080u, 0x01010101u, 0x01010181u, 0x00000000u, 0x01810180u, 0x01000000u, 0x00000001u, 0x00010101u, 0x01810080u, 0x01000001u, 0x01000001u, 0x00000101u, 0x01000180u, 0x01000100u, 0x01000100u, 0x81000100u, 0x00000080u, 0x01010101u, 0x00000000u, 0x81010101u, 0x01000180u, 0x00810001u, 0x01000100u, 0x00010001u, 0x01010080u, 0x01010000u, 0x00000181u, 0x00000101u, 0x01810080u, 0x00000101u, 0x01010000u, 0x00000101u, 0x01000180u, 0x01000100u, 0x00010081u, 0x00010001u, 0x00010180u, 0x01000001u, 0x00010100u, 0x81000001u, 0x01000180u, 0x00010001u, 0x00010001u, 0x81000100u, 0x01810180u, 0x01010000u, 0x00000101u, 0x00010101u, 0x01000080u, 0x01010000u, 0x00000181u, 0x00000001u, 0x01810080u, 0x00010000u, 0x00000100u, 0x00000101u, 0x01810080u, 0x01010001u, 0x01000101u, 0x00000101u, 0x00810180u, 0x01000001u, 0x01000001u, 0x00010100u, 0x01010080u, 0x00000101u, 0x00000101u, 0x81010000u, 0x00010180u, 0x00010100u, 0x01000001u, 0x81000001u, 0x00000080u, 0x00810100u, 0x00010100u, 0x00000000u, 0x00000180u, 0x00810101u, 0x00000100u, 0x00000000u, 0x00810080u, 0x01010100u, 0x00010000u, 0x00000000u, 0x00000080u, 0x00810000u, 0x01010100u, 0x00010000u, 0x00000080u, 0x00000100u, 0x00010181u, 0x00000100u, 0x00010180u, 0x00000101u, 0x01000001u, 0x81010000u, 0x01010080u, 0x00010100u, 0x00000101u, 0x81000001u, 0x00810180u, 0x01010000u, 0x01000001u, 0x00000101u, 0x01810080u, 0x01000001u, 0x00000101u, 0x00010100u, 0x00010180u, 0x00000101u, 0x00000101u, 0x81000001u, 0x00010180u, 0x01010000u, 0x01010000u, 0x81000001u, 0x01010180u, 0x00010101u, 0x00000001u, 0x81000000u, 0x01000080u, 0x00000001u, 0x00010101u, 0x81010100u, 0x00000080u, 0x01010101u, 0x01010000u, 0x81010000u, 0x01810080u, 0x01010000u, 0x01010101u, 0x00000000u, 0x00810080u, 0x00010000u, 0x00010101u, 0x00010101u, 0x00000180u, 0x00000100u, 0x01010100u, 0x81010100u, 0x81010080u, 0x01010000u, 0x01020200u, 0x82020202u, 0x81000080u, 0x01010000u, 0x01010282u, 0x01020202u, 0x00000080u, 0x01000002u, 0x01010282u, 0x81010202u, 0x82020280u, 0x02020000u, 0x01010000u, 0x81010100u, 0x00000080u, 0x00000000u, 0x02020181u, 0x82020101u, 0x81010080u, 0x01010000u, 0x02020000u, 0x82020000u, 0x82020080u, 0x02020000u, 0x01010101u, 0x81010101u, 0x01010080u, 0x01010000u, 0x01010282u, 0x81010202u, 0x00000080u, 0x00000000u, 0x01010181u, 0x82020202u, 0x00000080u, 0x01010101u, 0x01010181u, 0x82020202u, 0x00000080u, 0x01810101u, 0x02020202u, 0x82020202u, 0x02010080u, 0x02810000u, 0x02010000u, 0x82010000u, 0x02010180u, 0x02810100u, 0x02010100u, 0x82010100u, 0x02020180u, 0x02028100u, 0x02020100u, 0x82020100u, 0x81010080u, 0x02010100u, 0x02020101u, 0x82020201u, 0x81010080u, 0x01000002u, 0x00000282u, 0x00020202u, 0x81000080u, 0x01010000u, 0x02010100u, 0x82020101u, 0x81010180u, 0x01010000u, 0x01000082u, 0x00000202u, 0x00000080u, 0x02020101u, 0x02020181u, 0x82020101u, 0x82020080u, 0x02020000u, 0x02020000u, 0x81010101u, 0x81010180u, 0x01010100u, 0x02020200u, 0x82020200u, 0x81000080u, 0x01000000u, 0x01020282u, 0x01020202u, 0x00000080u, 0x01810000u, 0x02020100u, 0x82020100u, 0x00000080u, 0x00000101u, 0x00810282u, 0x00010202u, 0x82020180u, 0x02028100u, 0x01010000u, 0x00000000u, 0x02010080u, 0x02010000u, 0x02020181u, 0x82020202u, 0x00010180u, 0x01820201u, 0x01020281u, 0x00010100u, 0x00000080u, 0x00810100u, 0x01820201u, 0x01020201u, 0x02020080u, 0x02000101u, 0x02000181u, 0x82020000u, 0x00010180u, 0x00018100u, 0x02000002u, 0x82020202u, 0x01010080u, 0x02020100u, 0x02820100u, 0x81010000u, 0x00000080u, 0x00000002u, 0x01010282u, 0x81020202u, 0x00000080u, 0x02000000u, 0x02020181u, 0x82020201u, 0x82020280u, 0x02020000u, 0x02010000u, 0x81010000u, 0x81010080u, 0x02010000u, 0x02020000u, 0x82020200u, 0x00020180u, 0x00028100u, 0x00820100u, 0x00020100u, 0x00000080u, 0x01810101u, 0x02820202u, 0x00000000u, 0x00020180u, 0x01000201u, 0x02810082u, 0x00020100u, 0x00020180u, 0x02010002u, 0x01008281u, 0x00020100u, 0x01010080u, 0x00000202u, 0x02820101u, 0x81010000u, 0x01010080u, 0x02820101u, 0x00000202u, 0x81010000u, 0x81000180u, 0x01000100u, 0x02020202u, 0x82020202u, 0x00000080u, 0x00000000u, 0x01020182u, 0x81020102u, 0x02020080u, 0x02028101u, 0x02020000u, 0x82020101u, 0x82020080u, 0x01010000u, 0x02020000u, 0x81010000u, 0x00020280u, 0x01820201u, 0x00020200u, 0x81020201u, 0x01000180u, 0x02820202u, 0x02020202u, 0x81000100u, 0x00000080u, 0x01020102u, 0x01020182u, 0x81020102u, 0x81000180u, 0x01000100u, 0x01000100u, 0x82020202u, 0x82020280u, 0x01010100u, 0x02020200u, 0x81010100u, 0x02000080u, 0x02018101u, 0x02000000u, 0x82010101u, 0x00000080u, 0x02018102u, 0x02010102u, 0x82010102u, 0x02020280u, 0x01018100u, 0x01010100u, 0x82020200u, 0x02000080u, 0x02010101u, 0x02010181u, 0x82000000u, 0x00010180u, 0x00018100u, 0x00010100u, 0x82020202u, 0x00000080u, 0x00000000u, 0x02810102u, 0x82010102u, 0x00010180u, 0x00018100u, 0x02020202u, 0x82020202u, 0x02020080u, 0x01010000u, 0x01810000u, 0x82020000u, 0x02020080u, 0x02020101u, 0x02020181u, 0x82020000u, 0x00000080u, 0x00000000u, 0x00000000u, 0x82018102u, 0x82000080u, 0x01000000u, 0x02000000u, 0x81000000u, 0x02020280u, 0x02020201u, 0x02020200u, 0x82020281u, 0x81000180u, 0x02020202u, 0x02020202u, 0x82020202u, 0x81010180u, 0x01010002u, 0x01000282u, 0x00020202u, 0x01010080u, 0x01010000u, 0x01010000u, 0x81010000u, 0x01000080u, 0x01000000u, 0x01000000u, 0x81000000u, 0x01010180u, 0x01010100u, 0x01010100u, 0x81010100u, 0x01000080u, 0x01010000u, 0x01010000u, 0x81010100u, 0x00000080u, 0x01000000u, 0x01000000u, 0x81010000u, 0x01010080u, 0x01010100u, 0x01010100u, 0x81010101u, 0x01000080u, 0x01010000u, 0x01010100u, 0x81010101u, 0x00000080u, 0x01000000u, 0x01010000u, 0x81010100u, 0x00000080u, 0x00000000u, 0x01000000u, 0x81010000u, 0x01010080u, 0x01010100u, 0x01010101u, 0x81010101u, 0x00000080u, 0x01000000u, 0x01010100u, 0x81010101u, 0x00000080u, 0x00000000u, 0x01000000u, 0x81010100u, 0x01000080u, 0x01010100u, 0x01010101u, 0x81010101u, 0x00000080u, 0x00000000u, 0x01010101u, 0x81010101u, 0x00000080u, 0x01010101u, 0x01010101u, 0x81010101u, 0x00000080u, 0x00000000u, 0x00000000u, 0x81010101u, 0x00000080u, 0x00000001u, 0x00010101u, 0x81010101u, 0x01810180u, 0x01000000u, 0x00000000u, 0x00000000u, 0x00000080u, 0x00000000u, 0x00000081u, 0x00010101u, 0x01810180u, 0x01010000u, 0x01000000u, 0x00000000u, 0x01810080u, 0x01000000u, 0x00000000u, 0x00000000u, 0x00000080u, 0x00000001u, 0x00000181u, 0x00010101u, 0x00000080u, 0x00000000u, 0x00000081u, 0x00000101u, 0x01010180u, 0x01010000u, 0x01010000u, 0x81000000u, 0x01810080u, 0x01000000u, 0x01000000u, 0x00000000u, 0x00000080u, 0x00000001u, 0x00000081u, 0x00000101u, 0x00810180u, 0x00010100u, 0x00010100u, 0x00010100u, 0x01810080u, 0x00010100u, 0x00010100u, 0x00000101u, 0x01000080u, 0x01010100u, 0x00010181u, 0x00000001u, 0x00000080u, 0x01010101u, 0x01010181u, 0x00000000u, 0x01810180u, 0x01000000u, 0x00000001u, 0x00010101u, 0x01810080u, 0x01000001u, 0x01000001u, 0x00000101u, 0x00000000u, 0x00000015u, 0x0000002bu, 0x00000040u, 0x00000000u, 0x00000009u, 0x00000012u, 0x0000001bu, 0x00000025u, 0x0000002eu, 0x00000037u, 0x00000040u, 0x00000000u, 0x00000004u, 0x00000009u, 0x0000000du, 0x00000011u, 0x00000015u, 0x0000001au, 0x0000001eu, 0x00000022u, 0x00000026u, 0x0000002bu, 0x0000002fu, 0x00000033u, 0x00000037u, 0x0000003cu, 0x00000040u, 0x07050604u, 0x05070705u, 0x00000000u, 0x05070806u, 0x0b0b070au, 0x0808090bu, 0x0b0a0608u, 0x0605100cu, 0x05040405u, 0x06050506u, 0x0408090au, 0x05040605u, 0x06050504u, 0x090a0605u, 0x06050408u, 0x05050404u, 0x06060505u, 0x0408090au
};

static const char* kVexaBcnSrcS3tc = R"GLSL(#version 310 es
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, binding = 0) readonly buffer Src { uint data[]; } srcB;
layout(std430, binding = 1) writeonly buffer Dst { uint data[]; } dstB;
uniform uint uSrcWordOff;
uniform uint uSrcSliceWords;
uniform uint uSrcStride;
uniform uint uBlocksW;
uniform uint uBlocksH;
uniform uint uRowWords;
uniform uint uKind;    // 0 BC1, 1 BC2, 2 BC3, 3 BC4, 4 BC5
uniform uint uPack565;

uint getb(uvec2 v, int pos, int n) {
  uint r = pos >= 32 ? (v.y >> (pos - 32)) : (v.x >> pos);
  if (pos < 32 && pos + n > 32) r |= v.y << (32 - pos);
  return r & ((1u << n) - 1u);
}

uvec4 color_block(uvec2 blk, bool onlyOpaque) {
  uint c0 = blk.x & 0xffffu;
  uint c1 = blk.x >> 16;
  uint r0 = c0 >> 11, g0 = (c0 >> 5) & 63u, b0 = c0 & 31u;
  uint r1 = c1 >> 11, g1 = (c1 >> 5) & 63u, b1 = c1 & 31u;
  uvec4 ref;
  ref.x = 0xFF000000u | (((b0 * 527u + 23u) >> 6) << 16) | (((g0 * 259u + 33u) >> 6) << 8) | ((r0 * 527u + 23u) >> 6);
  ref.y = 0xFF000000u | (((b1 * 527u + 23u) >> 6) << 16) | (((g1 * 259u + 33u) >> 6) << 8) | ((r1 * 527u + 23u) >> 6);
  if (c0 > c1 || onlyOpaque) {
    ref.z = 0xFF000000u | ((((2u * b0 + b1) * 351u + 61u) >> 7) << 16) | ((((2u * g0 + g1) * 2763u + 1039u) >> 11) << 8) | (((2u * r0 + r1) * 351u + 61u) >> 7);
    ref.w = 0xFF000000u | ((((b0 + 2u * b1) * 351u + 61u) >> 7) << 16) | ((((g0 + 2u * g1) * 2763u + 1039u) >> 11) << 8) | (((r0 + 2u * r1) * 351u + 61u) >> 7);
  } else {
    ref.z = 0xFF000000u | ((((b0 + b1) * 1053u + 125u) >> 8) << 16) | ((((g0 + g1) * 4145u + 1019u) >> 11) << 8) | (((r0 + r1) * 1053u + 125u) >> 8);
    ref.w = 0u;
  }
  return ref;
}

uint smooth_alpha(uvec2 blk, int i) {
  uint a0 = blk.x & 0xFFu;
  uint a1 = (blk.x >> 8) & 0xFFu;
  uint idx = getb(blk, 16 + 3 * i, 3);
  uint a;
  if (a0 > a1) {
    uint A2 = (6u * a0 + a1) / 7u;
    uint A3 = (5u * a0 + 2u * a1) / 7u;
    uint A4 = (4u * a0 + 3u * a1) / 7u;
    uint A5 = (3u * a0 + 4u * a1) / 7u;
    uint A6 = (2u * a0 + 5u * a1) / 7u;
    uint A7 = (a0 + 6u * a1) / 7u;
    a = idx == 0u ? a0 : idx == 1u ? a1 : idx == 2u ? A2 : idx == 3u ? A3 : idx == 4u ? A4 : idx == 5u ? A5 : idx == 6u ? A6 : A7;
  } else {
    uint A2 = (4u * a0 + a1) / 5u;
    uint A3 = (3u * a0 + 2u * a1) / 5u;
    uint A4 = (2u * a0 + 3u * a1) / 5u;
    uint A5 = (a0 + 4u * a1) / 5u;
    a = idx == 0u ? a0 : idx == 1u ? a1 : idx == 2u ? A2 : idx == 3u ? A3 : idx == 4u ? A4 : idx == 5u ? A5 : idx == 6u ? 0u : 255u;
  }
  return a;
}

void main() {
  uint bx = gl_GlobalInvocationID.x;
  uint by = gl_GlobalInvocationID.y;
  uint bz = gl_GlobalInvocationID.z;
  if (bx >= uBlocksW || by >= uBlocksH) return;
  bool twoWords = (uKind == 0u || uKind == 3u);
  uint base = uSrcWordOff + bz * uSrcSliceWords + (by * uSrcStride + bx) * (twoWords ? 2u : 4u);
  uvec2 p01 = uvec2(srcB.data[base], srcB.data[base + 1u]);
  uvec2 p23 = twoWords ? uvec2(0u) : uvec2(srcB.data[base + 2u], srcB.data[base + 3u]);

  uint px[16];
  if (uKind == 0u || uKind == 1u || uKind == 2u) {
    uvec2 cblk = uKind == 0u ? p01 : p23;
    uvec4 ref = color_block(cblk, uKind != 0u);
    for (int i = 0; i < 16; ++i) {
      uint c = ref[(cblk.y >> (2 * i)) & 3u];
      if (uKind == 1u) {
        c = (c & 0xFFFFFFu) | ((getb(p01, 4 * i, 4) * 17u) << 24);
      } else if (uKind == 2u) {
        c = (c & 0xFFFFFFu) | (smooth_alpha(p01, i) << 24);
      }
      px[i] = c;
    }
    if (uPack565 != 0u) {
      uint rowBase = bz * uBlocksH * 4u * uRowWords;
      for (int y = 0; y < 4; ++y) {
        uint w0 = 0u, w1 = 0u;
        for (int x = 0; x < 4; ++x) {
          uint c = px[y * 4 + x];
          uint v = ((c & 0xF8u) << 8) | ((c & 0xFC00u) >> 5) | ((c & 0xF80000u) >> 19);
          if (x < 2) w0 |= v << (16 * x); else w1 |= v << (16 * (x - 2));
        }
        uint row = rowBase + (by * 4u + uint(y)) * uRowWords + bx * 2u;
        dstB.data[row] = w0;
        dstB.data[row + 1u] = w1;
      }
      return;
    }
    uint rowBase = bz * uBlocksH * 4u * uRowWords;
    for (int y = 0; y < 4; ++y) {
      uint row = rowBase + (by * 4u + uint(y)) * uRowWords + bx * 4u;
      for (int x = 0; x < 4; ++x) dstB.data[row + uint(x)] = px[y * 4 + x];
    }
    return;
  }
  // BC4 / BC5
  uint r0v[16];
  uint r1v[16];
  for (int i = 0; i < 16; ++i) {
    r0v[i] = smooth_alpha(p01, i);
    if (uKind == 4u) r1v[i] = smooth_alpha(p23, i);
  }
  uint rowBase = bz * uBlocksH * 4u * uRowWords;
  for (int y = 0; y < 4; ++y) {
    if (uKind == 3u) {
      uint w = r0v[y * 4] | (r0v[y * 4 + 1] << 8) | (r0v[y * 4 + 2] << 16) | (r0v[y * 4 + 3] << 24);
      dstB.data[rowBase + (by * 4u + uint(y)) * uRowWords + bx] = w;
    } else {
      uint w0 = r0v[y * 4] | (r1v[y * 4] << 8) | (r0v[y * 4 + 1] << 16) | (r1v[y * 4 + 1] << 24);
      uint w1 = r0v[y * 4 + 2] | (r1v[y * 4 + 2] << 8) | (r0v[y * 4 + 3] << 16) | (r1v[y * 4 + 3] << 24);
      uint row = rowBase + (by * 4u + uint(y)) * uRowWords + bx * 2u;
      dstB.data[row] = w0;
      dstB.data[row + 1u] = w1;
    }
  }
}
)GLSL";

static const char* kVexaBcnSrcBc7 = R"GLSL(#version 310 es
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, binding = 0) readonly buffer Src { uint data[]; } srcB;
layout(std430, binding = 1) writeonly buffer Dst { uint data[]; } dstB;
layout(std430, binding = 2) readonly buffer Tbl { uint data[]; } tblB;
uniform uint uSrcWordOff;
uniform uint uSrcSliceWords;
uniform uint uSrcStride;
uniform uint uBlocksW;
uniform uint uBlocksH;
uniform uint uRowWords;

uint ext(uvec4 v, int pos, int n) {
  int w = pos >> 5;
  int off = pos & 31;
  uint w0 = w == 0 ? v.x : (w == 1 ? v.y : (w == 2 ? v.z : v.w));
  uint w1 = w == 0 ? v.y : (w == 1 ? v.z : (w == 2 ? v.w : 0u));
  uint r = off == 0 ? w0 : ((w0 >> off) | (w1 << (32 - off)));
  return r & ((n >= 32) ? 0xffffffffu : ((1u << n) - 1u));
}

int bs_bits(inout uvec4 v, inout int pos, int n) {
  uint r = ext(v, pos, n);
  pos += n;
  return int(r);
}

int interp(int a, int b, int wbase, int index) {
  int w = int(tblB.data[wbase + index]);
  return (a * (64 - w) + b * w + 32) >> 6;
}

uint cell(uint setBase, int i, int j) {
  return (tblB.data[setBase + uint(i)] >> (uint(j) * 8u)) & 0xFFu;
}

uint abc7(int which, int mode) {
  return (tblB.data[668u + uint(which * 2 + (mode >> 2))] >> (uint(mode & 3) * 8u)) & 0xFFu;
}

void main() {
  uint bx = gl_GlobalInvocationID.x;
  uint by = gl_GlobalInvocationID.y;
  uint bz = gl_GlobalInvocationID.z;
  if (bx >= uBlocksW || by >= uBlocksH) return;
  uint base = uSrcWordOff + bz * uSrcSliceWords + (by * uSrcStride + bx) * 4u;
  uvec4 v = uvec4(srcB.data[base], srcB.data[base + 1u], srcB.data[base + 2u], srcB.data[base + 3u]);
  int pos = 0;

  int mode = 0;
  while (mode < 8 && bs_bits(v, pos, 1) == 0) ++mode;

  uint outPx[16];
  if (mode >= 8) {
    uint rowBase = bz * uBlocksH * 4u * uRowWords;
    for (int y = 0; y < 4; ++y) {
      uint row = rowBase + (by * 4u + uint(y)) * uRowWords + bx * 4u;
      for (int x = 0; x < 4; ++x) dstB.data[row + uint(x)] = 0u;
    }
    return;
  }

  int partition = 0;
  int numPartitions = 1;
  int rotation = 0;
  int indexSelectionBit = 0;
  if (mode == 0 || mode == 1 || mode == 2 || mode == 3 || mode == 7) {
    numPartitions = (mode == 0 || mode == 2) ? 3 : 2;
    partition = bs_bits(v, pos, mode == 0 ? 4 : 6);
  }
  int numEndpoints = numPartitions * 2;
  if (mode == 4 || mode == 5) {
    rotation = bs_bits(v, pos, 2);
    if (mode == 4) indexSelectionBit = bs_bits(v, pos, 1);
  }

  int endpoints[24];
  for (int i = 0; i < 24; ++i) endpoints[i] = 0;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < numEndpoints; ++j)
      endpoints[j * 4 + i] = bs_bits(v, pos, int(abc7(0, mode)));
  if (abc7(1, mode) > 0u)
    for (int j = 0; j < numEndpoints; ++j)
      endpoints[j * 4 + 3] = bs_bits(v, pos, int(abc7(1, mode)));

  const int sModeHasPBits = 0xCB; // 0b11001011
  if (mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7) {
    for (int i = 0; i < numEndpoints; ++i)
      for (int j = 0; j < 4; ++j)
        endpoints[i * 4 + j] <<= 1;
    if (mode == 1) {
      int p0 = bs_bits(v, pos, 1);
      int p1 = bs_bits(v, pos, 1);
      for (int k = 0; k < 3; ++k) {
        endpoints[0 * 4 + k] |= p0;
        endpoints[1 * 4 + k] |= p0;
        endpoints[2 * 4 + k] |= p1;
        endpoints[3 * 4 + k] |= p1;
      }
    } else if ((sModeHasPBits & (1 << mode)) != 0) {
      for (int i = 0; i < numEndpoints; ++i) {
        int p = bs_bits(v, pos, 1);
        for (int k = 0; k < 4; ++k) endpoints[i * 4 + k] |= p;
      }
    }
  }

  for (int i = 0; i < numEndpoints; ++i) {
    int j = int(abc7(0, mode)) + ((sModeHasPBits >> mode) & 1);
    for (int k = 0; k < 3; ++k) {
      endpoints[i * 4 + k] <<= (8 - j);
      endpoints[i * 4 + k] |= (endpoints[i * 4 + k] >> j);
    }
    j = int(abc7(1, mode)) + ((sModeHasPBits >> mode) & 1);
    endpoints[i * 4 + 3] <<= (8 - j);
    endpoints[i * 4 + 3] |= (endpoints[i * 4 + 3] >> j);
  }
  if (abc7(1, mode) == 0u)
    for (int j = 0; j < numEndpoints; ++j) endpoints[j * 4 + 3] = 0xFF;

  int indexBits = (mode == 0 || mode == 1) ? 3 : (mode == 6 ? 4 : 2);
  int indexBits2 = mode == 4 ? 3 : (mode == 5 ? 2 : 0);
  int wbase = indexBits == 2 ? 640 : (indexBits == 3 ? 644 : 652);
  int wbase2 = indexBits2 == 2 ? 640 : 644;

  int indices[16];
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      int partitionSet;
      if (numPartitions == 1) partitionSet = ((i | j) != 0) ? 0 : 128;
      else {
        uint setBase = (numPartitions == 2 ? 0u : 256u) + uint(partition) * 4u;
        partitionSet = int(cell(setBase, i, j));
      }
      int ib = indexBits;
      if ((partitionSet & 0x80) != 0) --ib;
      indices[i * 4 + j] = bs_bits(v, pos, ib);
    }
  }

  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      int partitionSet;
      if (numPartitions == 1) partitionSet = ((i | j) != 0) ? 0 : 128;
      else {
        uint setBase = (numPartitions == 2 ? 0u : 256u) + uint(partition) * 4u;
        partitionSet = int(cell(setBase, i, j));
      }
      partitionSet &= 0x03;
      int index = indices[i * 4 + j];
      int r, g, b, a;
      int ep = partitionSet * 2;
      if (indexBits2 == 0) {
        r = interp(endpoints[ep * 4 + 0], endpoints[ep * 4 + 4 + 0], wbase, index);
        g = interp(endpoints[ep * 4 + 1], endpoints[ep * 4 + 4 + 1], wbase, index);
        b = interp(endpoints[ep * 4 + 2], endpoints[ep * 4 + 4 + 2], wbase, index);
        a = interp(endpoints[ep * 4 + 3], endpoints[ep * 4 + 4 + 3], wbase, index);
      } else {
        int index2 = bs_bits(v, pos, (i | j) != 0 ? indexBits2 : indexBits2 - 1);
        if (indexSelectionBit == 0) {
          r = interp(endpoints[ep * 4 + 0], endpoints[ep * 4 + 4 + 0], wbase, index);
          g = interp(endpoints[ep * 4 + 1], endpoints[ep * 4 + 4 + 1], wbase, index);
          b = interp(endpoints[ep * 4 + 2], endpoints[ep * 4 + 4 + 2], wbase, index);
          a = interp(endpoints[ep * 4 + 3], endpoints[ep * 4 + 4 + 3], wbase2, index2);
        } else {
          r = interp(endpoints[ep * 4 + 0], endpoints[ep * 4 + 4 + 0], wbase2, index2);
          g = interp(endpoints[ep * 4 + 1], endpoints[ep * 4 + 4 + 1], wbase2, index2);
          b = interp(endpoints[ep * 4 + 2], endpoints[ep * 4 + 4 + 2], wbase2, index2);
          a = interp(endpoints[ep * 4 + 3], endpoints[ep * 4 + 4 + 3], wbase, index);
        }
      }
      int t;
      if (rotation == 1) { t = a; a = r; r = t; }
      else if (rotation == 2) { t = a; a = g; g = t; }
      else if (rotation == 3) { t = a; a = b; b = t; }
      outPx[i * 4 + j] = uint(r) | (uint(g) << 8) | (uint(b) << 16) | (uint(a) << 24);
    }
  }

  uint rowBase = bz * uBlocksH * 4u * uRowWords;
  for (int y = 0; y < 4; ++y) {
    uint row = rowBase + (by * 4u + uint(y)) * uRowWords + bx * 4u;
    for (int x = 0; x < 4; ++x) dstB.data[row + uint(x)] = outPx[y * 4 + x];
  }
}
)GLSL";

static const char* kVexaBcnSrcBc6h = R"GLSL(#version 310 es
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, binding = 0) readonly buffer Src { uint data[]; } srcB;
layout(std430, binding = 1) writeonly buffer Dst { uint data[]; } dstB;
layout(std430, binding = 2) readonly buffer Tbl { uint data[]; } tblB;
uniform uint uSrcWordOff;
uniform uint uSrcSliceWords;
uniform uint uSrcStride;
uniform uint uBlocksW;
uniform uint uBlocksH;
uniform uint uRowWords;
uniform uint uIsSigned;

uint extw(uvec4 v, int pos, int n) {
  int w = pos >> 5;
  int off = pos & 31;
  uint w0 = w == 0 ? v.x : (w == 1 ? v.y : (w == 2 ? v.z : v.w));
  uint w1 = w == 0 ? v.y : (w == 1 ? v.z : (w == 2 ? v.w : 0u));
  uint r = off == 0 ? w0 : ((w0 >> off) | (w1 << (32 - off)));
  return r & ((n >= 32) ? 0xffffffffu : ((1u << n) - 1u));
}

int bs_bits(inout uvec4 v, inout int pos, int n) {
  uint r = extw(v, pos, n);
  pos += n;
  return int(r);
}

int bs_bits_r(inout uvec4 v, inout int pos, int n) {
  int bits = bs_bits(v, pos, n);
  int r = 0;
  for (int i = 0; i < n; ++i) {
    r <<= 1;
    r |= (bits & 1);
    bits >>= 1;
  }
  return r;
}

int extend_sign(int val, int bits) {
  return (val << (32 - bits)) >> (32 - bits);
}

int transform_inverse(int val, int a0, int bits, bool isSigned) {
  val = (val + a0) & ((1 << bits) - 1);
  if (isSigned) val = extend_sign(val, bits);
  return val;
}

int unquantize(int val, int bits, bool isSigned) {
  int unq;
  if (!isSigned) {
    if (bits >= 15) unq = val;
    else if (val == 0) unq = 0;
    else if (val == (1 << bits) - 1) unq = 0xFFFF;
    else unq = ((val << 16) + 0x8000) >> bits;
  } else {
    if (bits >= 16) {
      unq = val;
    } else {
      int s = 0;
      if (val < 0) { s = 1; val = -val; }
      if (val == 0) unq = 0;
      else if (val >= (1 << (bits - 1)) - 1) unq = 0x7FFF;
      else unq = ((val << 15) + 0x4000) >> (bits - 1);
      if (s != 0) unq = -unq;
    }
  }
  return unq;
}

int interp6(int a, int b, int wbase, int index) {
  int w = int(tblB.data[wbase + index]);
  return (a * (64 - w) + b * w + 32) >> 6;
}

int finish_unquantize(int val, bool isSigned) {
  if (!isSigned) return (val * 31) >> 6;
  val = val < 0 ? -(((-val) * 31) >> 5) : (val * 31) >> 5;
  int s = 0;
  if (val < 0) { s = 0x8000; val = -val; }
  return s | val;
}

uint cell6(uint setBase, int i, int j) {
  return (tblB.data[setBase + uint(i)] >> (uint(j) * 8u)) & 0xFFu;
}

uint abc6(int c, int mode) {
  int idx = c * 14 + mode;
  return (tblB.data[672u + uint(idx >> 2)] >> (uint(idx & 3) * 8u)) & 0xFFu;
}

void main() {
  uint bx = gl_GlobalInvocationID.x;
  uint by = gl_GlobalInvocationID.y;
  uint bz = gl_GlobalInvocationID.z;
  if (bx >= uBlocksW || by >= uBlocksH) return;
  uint base = uSrcWordOff + bz * uSrcSliceWords + (by * uSrcStride + bx) * 4u;
  uvec4 v = uvec4(srcB.data[base], srcB.data[base + 1u], srcB.data[base + 2u], srcB.data[base + 3u]);
  int pos = 0;
  bool isSigned = uIsSigned != 0u;

  int r[4]; int g[4]; int b[4];
  for (int i = 0; i < 4; ++i) { r[i] = 0; g[i] = 0; b[i] = 0; }

  int mode = bs_bits(v, pos, 2);
  if (mode > 1) mode |= bs_bits(v, pos, 3) << 2;

  int partition = 0;
  switch (mode) {
  case 0: {
    g[2] |= bs_bits(v, pos, 1) << 4;
    b[2] |= bs_bits(v, pos, 1) << 4;
    b[3] |= bs_bits(v, pos, 1) << 4;
    r[0] |= bs_bits(v, pos, 10);
    g[0] |= bs_bits(v, pos, 10);
    b[0] |= bs_bits(v, pos, 10);
    r[1] |= bs_bits(v, pos, 5);
    g[3] |= bs_bits(v, pos, 1) << 4;
    g[2] |= bs_bits(v, pos, 4);
    g[1] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1);
    g[3] |= bs_bits(v, pos, 4);
    b[1] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1) << 1;
    b[2] |= bs_bits(v, pos, 4);
    r[2] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1) << 2;
    r[3] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1) << 3;
    partition = bs_bits(v, pos, 5);
    mode = 0;
  } break;
  case 1: {
    g[2] |= bs_bits(v, pos, 1) << 5;
    g[3] |= bs_bits(v, pos, 1) << 4;
    g[3] |= bs_bits(v, pos, 1) << 5;
    r[0] |= bs_bits(v, pos, 7);
    b[3] |= bs_bits(v, pos, 1);
    b[3] |= bs_bits(v, pos, 1) << 1;
    b[2] |= bs_bits(v, pos, 1) << 4;
    g[0] |= bs_bits(v, pos, 7);
    b[2] |= bs_bits(v, pos, 1) << 5;
    b[3] |= bs_bits(v, pos, 1) << 2;
    g[2] |= bs_bits(v, pos, 1) << 4;
    b[0] |= bs_bits(v, pos, 7);
    b[3] |= bs_bits(v, pos, 1) << 3;
    b[3] |= bs_bits(v, pos, 1) << 5;
    b[3] |= bs_bits(v, pos, 1) << 4;
    r[1] |= bs_bits(v, pos, 6);
    g[2] |= bs_bits(v, pos, 4);
    g[1] |= bs_bits(v, pos, 6);
    g[3] |= bs_bits(v, pos, 4);
    b[1] |= bs_bits(v, pos, 6);
    b[3] |= bs_bits(v, pos, 4);
    r[2] |= bs_bits(v, pos, 6);
    r[3] |= bs_bits(v, pos, 6);
    partition = bs_bits(v, pos, 5);
    mode = 1;
  } break;
  case 2: {
    r[0] |= bs_bits(v, pos, 10);
    g[0] |= bs_bits(v, pos, 10);
    b[0] |= bs_bits(v, pos, 10);
    r[1] |= bs_bits(v, pos, 5);
    r[0] |= bs_bits(v, pos, 1) << 10;
    g[2] |= bs_bits(v, pos, 4);
    g[1] |= bs_bits(v, pos, 4);
    g[0] |= bs_bits(v, pos, 1) << 10;
    b[3] |= bs_bits(v, pos, 1);
    g[3] |= bs_bits(v, pos, 4);
    b[1] |= bs_bits(v, pos, 4);
    b[0] |= bs_bits(v, pos, 1) << 10;
    b[3] |= bs_bits(v, pos, 1) << 1;
    b[2] |= bs_bits(v, pos, 4);
    r[2] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1) << 2;
    r[3] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1) << 3;
    partition = bs_bits(v, pos, 5);
    mode = 2;
  } break;
  case 6: {
    r[0] |= bs_bits(v, pos, 10);
    g[0] |= bs_bits(v, pos, 10);
    b[0] |= bs_bits(v, pos, 10);
    r[1] |= bs_bits(v, pos, 4);
    r[0] |= bs_bits(v, pos, 1) << 10;
    g[3] |= bs_bits(v, pos, 1) << 4;
    g[2] |= bs_bits(v, pos, 4);
    g[1] |= bs_bits(v, pos, 5);
    g[0] |= bs_bits(v, pos, 1) << 10;
    g[3] |= bs_bits(v, pos, 4);
    b[1] |= bs_bits(v, pos, 4);
    b[0] |= bs_bits(v, pos, 1) << 10;
    b[3] |= bs_bits(v, pos, 1) << 1;
    b[2] |= bs_bits(v, pos, 4);
    r[2] |= bs_bits(v, pos, 4);
    b[3] |= bs_bits(v, pos, 1);
    b[3] |= bs_bits(v, pos, 1) << 2;
    r[3] |= bs_bits(v, pos, 4);
    g[2] |= bs_bits(v, pos, 1) << 4;
    b[3] |= bs_bits(v, pos, 1) << 3;
    partition = bs_bits(v, pos, 5);
    mode = 3;
  } break;
  case 10: {
    r[0] |= bs_bits(v, pos, 10);
    g[0] |= bs_bits(v, pos, 10);
    b[0] |= bs_bits(v, pos, 10);
    r[1] |= bs_bits(v, pos, 4);
    r[0] |= bs_bits(v, pos, 1) << 10;
    b[2] |= bs_bits(v, pos, 1) << 4;
    g[2] |= bs_bits(v, pos, 4);
    g[1] |= bs_bits(v, pos, 4);
    g[0] |= bs_bits(v, pos, 1) << 10;
    b[3] |= bs_bits(v, pos, 1);
    g[3] |= bs_bits(v, pos, 4);
    b[1] |= bs_bits(v, pos, 5);
    b[0] |= bs_bits(v, pos, 1) << 10;
    b[2] |= bs_bits(v, pos, 1) << 4;
    r[2] |= bs_bits(v, pos, 4);
    b[3] |= bs_bits(v, pos, 1) << 1;
    b[3] |= bs_bits(v, pos, 1) << 2;
    r[3] |= bs_bits(v, pos, 4);
    b[3] |= bs_bits(v, pos, 1) << 4;
    b[3] |= bs_bits(v, pos, 1) << 3;
    partition = bs_bits(v, pos, 5);
    mode = 4;
  } break;
  case 14: {
    r[0] |= bs_bits(v, pos, 9);
    b[2] |= bs_bits(v, pos, 1) << 4;
    g[0] |= bs_bits(v, pos, 9);
    g[2] |= bs_bits(v, pos, 1) << 4;
    b[0] |= bs_bits(v, pos, 9);
    b[3] |= bs_bits(v, pos, 1) << 4;
    r[1] |= bs_bits(v, pos, 5);
    g[3] |= bs_bits(v, pos, 1) << 4;
    g[2] |= bs_bits(v, pos, 4);
    g[1] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1);
    g[3] |= bs_bits(v, pos, 4);
    b[1] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1) << 1;
    b[2] |= bs_bits(v, pos, 4);
    r[2] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1) << 2;
    r[3] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1) << 3;
    partition = bs_bits(v, pos, 5);
    mode = 5;
  } break;
  case 18: {
    r[0] |= bs_bits(v, pos, 8);
    g[3] |= bs_bits(v, pos, 1) << 4;
    b[2] |= bs_bits(v, pos, 1) << 4;
    g[0] |= bs_bits(v, pos, 8);
    b[3] |= bs_bits(v, pos, 1) << 2;
    g[2] |= bs_bits(v, pos, 1) << 4;
    b[0] |= bs_bits(v, pos, 8);
    b[3] |= bs_bits(v, pos, 1) << 3;
    b[3] |= bs_bits(v, pos, 1) << 4;
    r[1] |= bs_bits(v, pos, 6);
    g[2] |= bs_bits(v, pos, 4);
    g[1] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1);
    g[3] |= bs_bits(v, pos, 4);
    b[1] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1) << 1;
    b[2] |= bs_bits(v, pos, 4);
    r[2] |= bs_bits(v, pos, 6);
    b[3] |= bs_bits(v, pos, 1) << 2;
    r[3] |= bs_bits(v, pos, 6);
    partition = bs_bits(v, pos, 5);
    mode = 6;
  } break;
  case 22: {
    r[0] |= bs_bits(v, pos, 8);
    b[3] |= bs_bits(v, pos, 1);
    b[2] |= bs_bits(v, pos, 1) << 4;
    g[0] |= bs_bits(v, pos, 8);
    g[2] |= bs_bits(v, pos, 1) << 5;
    g[2] |= bs_bits(v, pos, 1) << 4;
    b[0] |= bs_bits(v, pos, 8);
    g[3] |= bs_bits(v, pos, 1) << 5;
    b[3] |= bs_bits(v, pos, 1) << 4;
    r[1] |= bs_bits(v, pos, 5);
    g[3] |= bs_bits(v, pos, 1) << 4;
    g[2] |= bs_bits(v, pos, 4);
    g[1] |= bs_bits(v, pos, 6);
    g[3] |= bs_bits(v, pos, 4);
    b[1] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1) << 1;
    b[2] |= bs_bits(v, pos, 4);
    r[2] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1) << 2;
    r[3] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1) << 3;
    partition = bs_bits(v, pos, 5);
    mode = 7;
  } break;
  case 26: {
    r[0] |= bs_bits(v, pos, 8);
    b[3] |= bs_bits(v, pos, 1) << 1;
    b[2] |= bs_bits(v, pos, 1) << 4;
    g[0] |= bs_bits(v, pos, 8);
    b[2] |= bs_bits(v, pos, 1) << 5;
    g[2] |= bs_bits(v, pos, 1) << 4;
    b[0] |= bs_bits(v, pos, 8);
    b[3] |= bs_bits(v, pos, 1) << 5;
    b[3] |= bs_bits(v, pos, 1) << 4;
    r[1] |= bs_bits(v, pos, 5);
    g[3] |= bs_bits(v, pos, 1) << 4;
    g[2] |= bs_bits(v, pos, 4);
    g[1] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1);
    g[3] |= bs_bits(v, pos, 4);
    b[1] |= bs_bits(v, pos, 6);
    b[2] |= bs_bits(v, pos, 4);
    r[2] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1) << 2;
    r[3] |= bs_bits(v, pos, 5);
    b[3] |= bs_bits(v, pos, 1) << 3;
    partition = bs_bits(v, pos, 5);
    mode = 8;
  } break;
  case 30: {
    r[0] |= bs_bits(v, pos, 6);
    g[3] |= bs_bits(v, pos, 1) << 4;
    b[3] |= bs_bits(v, pos, 1);
    b[3] |= bs_bits(v, pos, 1) << 1;
    b[2] |= bs_bits(v, pos, 1) << 4;
    g[0] |= bs_bits(v, pos, 6);
    g[2] |= bs_bits(v, pos, 1) << 5;
    b[2] |= bs_bits(v, pos, 1) << 5;
    b[3] |= bs_bits(v, pos, 1) << 2;
    g[2] |= bs_bits(v, pos, 1) << 4;
    b[0] |= bs_bits(v, pos, 6);
    g[3] |= bs_bits(v, pos, 1) << 5;
    b[3] |= bs_bits(v, pos, 1) << 3;
    b[3] |= bs_bits(v, pos, 1) << 5;
    b[3] |= bs_bits(v, pos, 1) << 4;
    r[1] |= bs_bits(v, pos, 6);
    g[2] |= bs_bits(v, pos, 4);
    g[1] |= bs_bits(v, pos, 6);
    g[3] |= bs_bits(v, pos, 4);
    b[1] |= bs_bits(v, pos, 6);
    b[2] |= bs_bits(v, pos, 4);
    r[2] |= bs_bits(v, pos, 6);
    r[3] |= bs_bits(v, pos, 6);
    partition = bs_bits(v, pos, 5);
    mode = 9;
  } break;
  case 3: {
    r[0] |= bs_bits(v, pos, 10);
    g[0] |= bs_bits(v, pos, 10);
    b[0] |= bs_bits(v, pos, 10);
    r[1] |= bs_bits(v, pos, 10);
    g[1] |= bs_bits(v, pos, 10);
    b[1] |= bs_bits(v, pos, 10);
    mode = 10;
  } break;
  case 7: {
    r[0] |= bs_bits(v, pos, 10);
    g[0] |= bs_bits(v, pos, 10);
    b[0] |= bs_bits(v, pos, 10);
    r[1] |= bs_bits(v, pos, 9);
    r[0] |= bs_bits(v, pos, 1) << 10;
    g[1] |= bs_bits(v, pos, 9);
    g[0] |= bs_bits(v, pos, 1) << 10;
    b[1] |= bs_bits(v, pos, 9);
    b[0] |= bs_bits(v, pos, 1) << 10;
    mode = 11;
  } break;
  case 11: {
    r[0] |= bs_bits(v, pos, 10);
    g[0] |= bs_bits(v, pos, 10);
    b[0] |= bs_bits(v, pos, 10);
    r[1] |= bs_bits(v, pos, 8);
    r[0] |= bs_bits_r(v, pos, 2) << 10;
    g[1] |= bs_bits(v, pos, 8);
    g[0] |= bs_bits_r(v, pos, 2) << 10;
    b[1] |= bs_bits(v, pos, 8);
    b[0] |= bs_bits_r(v, pos, 2) << 10;
    mode = 12;
  } break;
  case 15: {
    r[0] |= bs_bits(v, pos, 10);
    g[0] |= bs_bits(v, pos, 10);
    b[0] |= bs_bits(v, pos, 10);
    r[1] |= bs_bits(v, pos, 4);
    r[0] |= bs_bits_r(v, pos, 6) << 10;
    g[1] |= bs_bits(v, pos, 4);
    g[0] |= bs_bits_r(v, pos, 6) << 10;
    b[1] |= bs_bits(v, pos, 4);
    b[0] |= bs_bits_r(v, pos, 6) << 10;
    mode = 13;
  } break;
  default: {
    uint rowBaseZ = bz * uBlocksH * 4u * uRowWords;
    for (int y = 0; y < 4; ++y) {
      uint row = rowBaseZ + (by * 4u + uint(y)) * uRowWords + bx * 6u;
      for (int x = 0; x < 6; ++x) dstB.data[row + uint(x)] = 0u;
    }
    return;
  }
  }

  int numPartitions = mode >= 10 ? 0 : 1;
  int actualBits0Mode = int(abc6(0, mode));
  if (isSigned) {
    r[0] = extend_sign(r[0], actualBits0Mode);
    g[0] = extend_sign(g[0], actualBits0Mode);
    b[0] = extend_sign(b[0], actualBits0Mode);
  }
  if ((mode != 9 && mode != 10) || isSigned) {
    for (int i = 1; i < (numPartitions + 1) * 2; ++i) {
      r[i] = extend_sign(r[i], int(abc6(1, mode)));
      g[i] = extend_sign(g[i], int(abc6(2, mode)));
      b[i] = extend_sign(b[i], int(abc6(3, mode)));
    }
  }
  if (mode != 9 && mode != 10) {
    for (int i = 1; i < (numPartitions + 1) * 2; ++i) {
      r[i] = transform_inverse(r[i], r[0], actualBits0Mode, isSigned);
      g[i] = transform_inverse(g[i], g[0], actualBits0Mode, isSigned);
      b[i] = transform_inverse(b[i], b[0], actualBits0Mode, isSigned);
    }
  }
  for (int i = 0; i < (numPartitions + 1) * 2; ++i) {
    r[i] = unquantize(r[i], actualBits0Mode, isSigned);
    g[i] = unquantize(g[i], actualBits0Mode, isSigned);
    b[i] = unquantize(b[i], actualBits0Mode, isSigned);
  }

  int wbase = mode >= 10 ? 652 : 644;
  uint hbuf[48];
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      int partitionSet;
      if (mode >= 10) partitionSet = ((i | j) != 0) ? 0 : 128;
      else partitionSet = int(cell6(512u + uint(partition) * 4u, i, j));
      int ib = mode >= 10 ? 4 : 3;
      if ((partitionSet & 0x80) != 0) --ib;
      partitionSet &= 0x01;
      int index = bs_bits(v, pos, ib);
      int ep = partitionSet * 2;
      int o = (i * 4 + j) * 3;
      hbuf[o] = uint(finish_unquantize(interp6(r[ep], r[ep + 1], wbase, index), isSigned));
      hbuf[o + 1] = uint(finish_unquantize(interp6(g[ep], g[ep + 1], wbase, index), isSigned));
      hbuf[o + 2] = uint(finish_unquantize(interp6(b[ep], b[ep + 1], wbase, index), isSigned));
    }
  }

  uint rowBase = bz * uBlocksH * 4u * uRowWords;
  for (int y = 0; y < 4; ++y) {
    uint row = rowBase + (by * 4u + uint(y)) * uRowWords + bx * 6u;
    int o = y * 12;
    dstB.data[row] = (hbuf[o] & 0xFFFFu) | (hbuf[o + 1] << 16);
    dstB.data[row + 1u] = (hbuf[o + 2] & 0xFFFFu) | (hbuf[o + 3] << 16);
    dstB.data[row + 2u] = (hbuf[o + 4] & 0xFFFFu) | (hbuf[o + 5] << 16);
    dstB.data[row + 3u] = (hbuf[o + 6] & 0xFFFFu) | (hbuf[o + 7] << 16);
    dstB.data[row + 4u] = (hbuf[o + 8] & 0xFFFFu) | (hbuf[o + 9] << 16);
    dstB.data[row + 5u] = (hbuf[o + 10] & 0xFFFFu) | (hbuf[o + 11] << 16);
  }
}
)GLSL";

// ---------------------------------------------------------------------------
// Host-side driver for the compute decoders.
// ---------------------------------------------------------------------------
struct VexaBcnGpuCtx {
  EGLContext ctx = nullptr;
  GLuint progS3tc = 0, progBc7 = 0, progBc6h = 0;
  GLuint srcBuf = 0, dstBuf = 0, tblBuf = 0;
  GLsizeiptr srcCap = 0, dstCap = 0;
  bool failed = false;
  bool tried = false;
  GLint locS3tc[8] = {};
  GLint locBc7[6] = {};
  GLint locBc6[7] = {};
};

static thread_local std::unordered_map<EGLContext, VexaBcnGpuCtx> VexaBcnGpuStates;

static bool VexaBcnGpuDisabled() {
  static const bool off = access("/data/data/com.critical.vexaemulator/files/vexa_bcn_gpu_off.flag", F_OK) == 0;
  return off;
}
static bool VexaBcnValidateEnabled() {
  static const bool on = access("/data/data/com.critical.vexaemulator/files/vexa_bcn_validate.flag", F_OK) == 0;
  return on;
}

static GLuint VexaBcnCompile(GLenum type, const char* src) {
  GLuint sh = ::glCreateShader(type);
  ::glShaderSource(sh, 1, &src, nullptr);
  ::glCompileShader(sh);
  GLint ok = GL_FALSE;
  ::glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024] = {};
    ::glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
    fprintf(stderr, "[VEXA][BCN-GPU] shader compile failed: %s\n", log);
    ::glDeleteShader(sh);
    return 0;
  }
  return sh;
}

static bool VexaBcnBuildCtx(VexaBcnGpuCtx& c) {
  if (c.tried) return !c.failed;
  c.tried = true;
  auto build = [&](const char* src, const char* const* names, GLint* locs, int n) -> GLuint {
    GLuint sh = VexaBcnCompile(GL_COMPUTE_SHADER, src);
    if (!sh) return 0;
    GLuint prog = ::glCreateProgram();
    ::glAttachShader(prog, sh);
    ::glLinkProgram(prog);
    ::glDeleteShader(sh);
    GLint ok = GL_FALSE;
    ::glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
      char log[1024] = {};
      ::glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
      fprintf(stderr, "[VEXA][BCN-GPU] link failed: %s\n", log);
      ::glDeleteProgram(prog);
      return 0;
    }
    for (int i = 0; i < n; ++i) locs[i] = ::glGetUniformLocation(prog, names[i]);
    return prog;
  };
  static const char* const s3tcNames[] = {"uSrcWordOff", "uSrcSliceWords", "uSrcStride", "uBlocksW",
                                          "uBlocksH",     "uRowWords",      "uKind",      "uPack565"};
  static const char* const bc7Names[] = {"uSrcWordOff", "uSrcSliceWords", "uSrcStride", "uBlocksW", "uBlocksH", "uRowWords"};
  static const char* const bc6Names[] = {"uSrcWordOff", "uSrcSliceWords", "uSrcStride", "uBlocksW",
                                         "uBlocksH",    "uRowWords",      "uIsSigned"};
  c.progS3tc = build(kVexaBcnSrcS3tc, s3tcNames, c.locS3tc, 8);
  c.progBc7 = build(kVexaBcnSrcBc7, bc7Names, c.locBc7, 6);
  c.progBc6h = build(kVexaBcnSrcBc6h, bc6Names, c.locBc6, 7);
  if (!c.progS3tc || !c.progBc7 || !c.progBc6h) {
    c.failed = true;
    return false;
  }
  GLuint bufs[3] = {};
  ::glGenBuffers(3, bufs);
  c.srcBuf = bufs[0];
  c.dstBuf = bufs[1];
  c.tblBuf = bufs[2];
  GLint prev = 0;
  ::glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &prev);
  ::glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.tblBuf);
  ::glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(kVexaBcnTableWords), kVexaBcnTableWords, GL_STATIC_DRAW);
  ::glBindBuffer(GL_SHADER_STORAGE_BUFFER, (GLuint)prev);
  if (::glGetError() != GL_NO_ERROR) {
    fprintf(stderr, "[VEXA][BCN-GPU] table buffer setup failed\n");
    c.failed = true;
    return false;
  }
  return true;
}

// Saves and restores every GL state the GPU decode touches. The guest may use
// program pipelines; we only switch CURRENT_PROGRAM so the bound pipeline
// stays active when we restore program 0.
struct VexaBcnGpuSave {
  GLint prog = 0;
  GLint ssbo[3] = {};
  GLint ssboGeneric = 0;
  GLint pbo = 0;
  GLint copyRead = 0;
  GLint alignV = 4, row = 0, skpx = 0, skrow = 0, imgh = 0, skimg = 0;
  VexaBcnGpuSave() {
    ::glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    for (int i = 0; i < 3; ++i) ::glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, i, &ssbo[i]);
    ::glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &ssboGeneric);
    ::glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    ::glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &copyRead);
    ::glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignV);
    ::glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
    ::glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skpx);
    ::glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skrow);
    ::glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &imgh);
    ::glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &skimg);
  }
  ~VexaBcnGpuSave() {
    ::glUseProgram((GLuint)prog);
    for (int i = 0; i < 3; ++i) ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, (GLuint)ssbo[i]);
    ::glBindBuffer(GL_SHADER_STORAGE_BUFFER, (GLuint)ssboGeneric);
    ::glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)pbo);
    ::glBindBuffer(GL_COPY_READ_BUFFER, (GLuint)copyRead);
    ::glPixelStorei(GL_UNPACK_ALIGNMENT, alignV);
    ::glPixelStorei(GL_UNPACK_ROW_LENGTH, row);
    ::glPixelStorei(GL_UNPACK_SKIP_PIXELS, skpx);
    ::glPixelStorei(GL_UNPACK_SKIP_ROWS, skrow);
    ::glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, imgh);
    ::glPixelStorei(GL_UNPACK_SKIP_IMAGES, skimg);
  }
};

static std::atomic<uint64_t> VexaBcnValCount {0}, VexaBcnValBad {0};
static std::atomic<uint64_t> VexaBcnCpuNs {0}, VexaBcnGpuNs {0};

// Decodes one w x h x depth region of BC blocks through the GPU into the
// context's output buffer. Returns false when the GPU path is unavailable or
// failed and the caller should use the CPU decoder.
static bool VexaBcnGpuDecode(const VexaBCInfo& bc, const void* data, GLsizei imageSize, GLsizei w, GLsizei h,
                             GLsizei depth, VexaBcnGpuCtx& c) {
  const int bw = (w + 3) / 4;
  const int bh = (h + 3) / 4;
  GLint row_length = 0;
  ::glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
  const int stored_bw = row_length > w ? (row_length + 3) / 4 : bw;

  GLuint prog = 0;
  const GLint* loc = nullptr;
  switch (bc.kind) {
  case VexaBCKind::BC1:
  case VexaBCKind::BC2:
  case VexaBCKind::BC3:
  case VexaBCKind::BC4:
  case VexaBCKind::BC5:
    prog = c.progS3tc;
    loc = c.locS3tc;
    break;
  case VexaBCKind::BC7:
    prog = c.progBc7;
    loc = c.locBc7;
    break;
  case VexaBCKind::BC6H:
    prog = c.progBc6h;
    loc = c.locBc6;
    break;
  default:
    return false;
  }

  const GLsizeiptr dstNeed = static_cast<GLsizeiptr>(bw) * 4 * bc.bpp * bh * 4 * depth;
  if (dstNeed > c.dstCap) {
    ::glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.dstBuf);
    ::glBufferData(GL_SHADER_STORAGE_BUFFER, dstNeed * 2, nullptr, GL_STREAM_COPY);
    GLint64 actual = 0;
    ::glGetBufferParameteri64v(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &actual);
    if (actual < dstNeed) {
      fprintf(stderr, "[VEXA][BCN-GPU] dst alloc %lld failed\n", (long long)dstNeed);
      c.failed = true;
      return false;
    }
    c.dstCap = actual;
  }

  // Source: bound PIXEL_UNPACK_BUFFER becomes SSBO 0 directly, otherwise the
  // client span is staged through the context's own buffer with orphaning.
  uint32_t srcWordOff = 0;
  GLint pbo = 0;
  ::glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
  if (pbo) {
    const auto offset = reinterpret_cast<uintptr_t>(data);
    if (offset & 3) {
      return false;
    }
    srcWordOff = static_cast<uint32_t>(offset / 4);
    ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, (GLuint)pbo);
  } else {
    const GLsizei fetch = VexaBCFetchSize(bc, w, h, imageSize) * std::max<GLsizei>(depth, 1);
    if (fetch > c.srcCap) {
      ::glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.srcBuf);
      ::glBufferData(GL_SHADER_STORAGE_BUFFER, fetch * 2, nullptr, GL_STREAM_DRAW);
      GLint64 actual = 0;
      ::glGetBufferParameteri64v(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &actual);
      if (actual < fetch) {
        c.failed = true;
        return false;
      }
      c.srcCap = actual;
    }
    ::glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.srcBuf);
    ::glBufferData(GL_SHADER_STORAGE_BUFFER, c.srcCap, nullptr, GL_STREAM_DRAW); // orphan
    ::glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, fetch, data);
    ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, c.srcBuf);
  }
  ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, c.dstBuf);
  ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, c.tblBuf);

  const auto t0 = std::chrono::steady_clock::now();
  ::glUseProgram(prog);
  ::glUniform1ui(loc[0], srcWordOff);
  const uint32_t sliceBytes = static_cast<uint32_t>(stored_bw) * bh * bc.block;
  ::glUniform1ui(loc[1], sliceBytes / 4);
  ::glUniform1ui(loc[2], static_cast<uint32_t>(stored_bw));
  ::glUniform1ui(loc[3], static_cast<uint32_t>(bw));
  ::glUniform1ui(loc[4], static_cast<uint32_t>(bh));
  const uint32_t rowWords = static_cast<uint32_t>(bw) * 4 * bc.bpp / 4;
  ::glUniform1ui(loc[5], rowWords);
  if (bc.kind == VexaBCKind::BC6H) {
    ::glUniform1ui(loc[6], bc.is_signed ? 1u : 0u);
  } else if (loc == c.locS3tc) {
    uint32_t kind = 0;
    switch (bc.kind) {
    case VexaBCKind::BC1: kind = 0; break;
    case VexaBCKind::BC2: kind = 1; break;
    case VexaBCKind::BC3: kind = 2; break;
    case VexaBCKind::BC4: kind = 3; break;
    case VexaBCKind::BC5: kind = 4; break;
    default: break;
    }
    ::glUniform1ui(loc[6], kind);
    ::glUniform1ui(loc[7], bc.pack565 ? 1u : 0u);
  }
  ::glDispatchCompute((bw + 7) / 8, (bh + 7) / 8, depth);
  ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_PIXEL_BUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT |
                    GL_BUFFER_UPDATE_BARRIER_BIT);
  const auto gpuNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
  VexaBcnGpuNs += static_cast<uint64_t>(gpuNs);

  if (::glGetError() != GL_NO_ERROR) {
    fprintf(stderr, "[VEXA][BCN-GPU] GL error during dispatch; falling back to CPU permanently\n");
    c.failed = true;
    return false;
  }

  if (VexaBcnValidateEnabled() && VexaBcnValCount.load() < 64) {
    static thread_local std::vector<uint8_t> px;
    static thread_local std::vector<uint8_t> gpu;
    const auto c0 = std::chrono::steady_clock::now();
    const bool ok = VexaBCDecode(bc, pbo ? nullptr : static_cast<const uint8_t*>(data), static_cast<size_t>(imageSize), w,
                                 h, px);
    // For the PBO case the CPU reference must be decoded from the mapped buffer.
    const uint8_t* mapped = nullptr;
    bool unmap = false;
    if (pbo && !ok) {
      const GLsizei fetch = VexaBCFetchSize(bc, w, h, imageSize);
      mapped = static_cast<const uint8_t*>(
        ::glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, reinterpret_cast<GLintptr>(data), fetch, GL_MAP_READ_BIT));
      if (mapped) {
        unmap = VexaBCDecode(bc, mapped, static_cast<size_t>(imageSize), w, h, px);
      }
    }
    const auto cpuNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - c0).count();
    VexaBcnCpuNs += static_cast<uint64_t>(cpuNs);
    const size_t rowBytes = static_cast<size_t>(bw) * 4 * bc.bpp;
    gpu.resize(rowBytes * bh * 4 * depth);
    ::glBindBuffer(GL_COPY_READ_BUFFER, c.dstBuf);
    void* gmap = ::glMapBufferRange(GL_COPY_READ_BUFFER, 0, gpu.size(), GL_MAP_READ_BIT);
    bool match = false;
    size_t first = SIZE_MAX;
    if (gmap && (pbo ? unmap : ok)) {
      std::memcpy(gpu.data(), gmap, gpu.size());
      ::glUnmapBuffer(GL_COPY_READ_BUFFER);
      if (unmap) ::glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      match = true;
      const size_t tightRow = static_cast<size_t>(w) * bc.bpp;
      size_t off = 0;
      for (GLsizei z = 0; z < depth && match; ++z) {
        for (GLsizei y = 0; y < h && match; ++y) {
          const size_t go = (static_cast<size_t>(z) * bh * 4 + y) * rowBytes;
          const size_t co = (static_cast<size_t>(z) * h + y) * tightRow;
          if (std::memcmp(gpu.data() + go, px.data() + co, tightRow) != 0) {
            match = false;
            first = go;
          }
          (void)off;
        }
      }
    } else {
      if (gmap) ::glUnmapBuffer(GL_COPY_READ_BUFFER);
      if (unmap) ::glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    }
    const uint64_t n = VexaBcnValCount.fetch_add(1) + 1;
    if (!match) VexaBcnValBad.fetch_add(1);
    fprintf(stderr, "[VEXA][BCN-GPU] validate kind=%d %dx%dx%d %s%s\n", (int)bc.kind, (int)w, (int)h, (int)depth,
            match ? "ok" : "MISMATCH", match ? "" : (first == SIZE_MAX ? " (compare failed)" : ""));
    fprintf(stderr, "[VEXA][BCN-GPU] validate summary n=%llu bad=%llu cpu_ms=%.2f gpu_ms=%.2f\n",
            (unsigned long long)n, (unsigned long long)VexaBcnValBad.load(), VexaBcnCpuNs.load() / 1.0e6,
            VexaBcnGpuNs.load() / 1.0e6);
  }
  return true;
}

// Uploads the decoded image from the context dst buffer through
// GL_PIXEL_UNPACK_BUFFER. Unpack state is configured for the padded layout.
static void VexaBcnGpuUpload(GLenum target, GLint level, bool sub, GLint xoffset, GLint yoffset, GLint zoffset,
                             const VexaBCInfo& bc, GLsizei w, GLsizei h, GLsizei depth, const VexaBcnGpuCtx& c) {
  const int bw = (w + 3) / 4;
  const int bh = (h + 3) / 4;
  ::glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  ::glPixelStorei(GL_UNPACK_ROW_LENGTH, bw * 4);
  ::glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  ::glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
  ::glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, bh * 4);
  ::glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
  ::glBindBuffer(GL_PIXEL_UNPACK_BUFFER, c.dstBuf);
  const void* zero = nullptr;
  if (sub) {
    if (target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY || depth > 1) {
      ::glTexSubImage3D(target, level, xoffset, yoffset, zoffset, w, h, depth, bc.format, bc.type, zero);
    } else {
      ::glTexSubImage2D(target, level, xoffset, yoffset, w, h, bc.format, bc.type, zero);
    }
  } else {
    if (depth > 1) {
      ::glTexImage3D(target, level, bc.storage, w, h, depth, 0, bc.format, bc.type, zero);
    } else {
      ::glTexImage2D(target, level, bc.storage, w, h, 0, bc.format, bc.type, zero);
    }
  }
}

static bool VexaBcnGpuEligible(const VexaBCInfo& bc, GLsizei w, GLsizei h) {
  if (VexaBcnGpuDisabled() || bc.kind == VexaBCKind::None) return false;
  const int64_t blocks = static_cast<int64_t>((w + 3) / 4) * ((h + 3) / 4);
  if (blocks < 256) return false;
  EGLContext ctx = eglGetCurrentContext();
  if (ctx == EGL_NO_CONTEXT) return false;
  VexaBcnGpuCtx& c = VexaBcnGpuStates[ctx];
  c.ctx = ctx;
  if (!VexaBcnBuildCtx(c)) return false;
  return !c.failed;
}

static bool VexaBcnGpuUploadImage(GLenum target, GLint level, bool sub, GLint xoffset, GLint yoffset, GLint zoffset,
                                  const VexaBCInfo& bc, GLsizei w, GLsizei h, GLsizei depth, GLsizei imageSize,
                                  const void* data) {
  if (!VexaBcnGpuEligible(bc, w, h)) return false;
  VexaBcnGpuCtx& c = VexaBcnGpuStates[eglGetCurrentContext()];
  VexaBcnGpuSave save;
  if (!VexaBcnGpuDecode(bc, data, imageSize, w, h, depth, c)) {
    return false;
  }
  VexaBcnGpuUpload(target, level, sub, xoffset, yoffset, zoffset, bc, w, h, depth, c);
  return true;
}
