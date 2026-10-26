// VEXA_FIXES: fix invalid OpConstantComposite instructions whose constituents
// are specialization constants. Some shader generators emit those even though
// the SPIR-V spec forbids mixing; Mali then folds the spec constants to their
// defaults and renders black. Rewriting the opcode to OpSpecConstantComposite
// is word-count neutral and spec-correct for any result type, so we patch the
// instruction word in place. See SPIRV-Tools fix_mali_spec_constant_composite_pass.

#include <cstdint>
#include <cstddef>
#include <vector>

namespace {
constexpr uint32_t kVexaSpirvMagic = 0x07230203;
constexpr uint32_t kVexaOpConstantComposite = 44;
// OpSpecConstantTrue .. OpSpecConstantOp
constexpr uint32_t kVexaOpSpecConstantFirst = 48;
constexpr uint32_t kVexaOpSpecConstantComposite = 51;
constexpr uint32_t kVexaOpSpecConstantLast = 52;

// Returns the number of OpConstantComposite instructions that would be (or
// were) rewritten. When `patch` is non-null the opcode words are fixed in
// place. Returns 0 on any malformed input.
static uint32_t VexaSpecCompositeScan(const uint32_t* words, size_t count, uint32_t* patch) {
  if (!words || count < 5 || words[0] != kVexaSpirvMagic) {
    return 0;
  }
  const uint32_t bound = words[3];
  if (bound == 0 || bound > (1u << 22)) {
    return 0;
  }
  std::vector<bool> spec(bound, false);
  std::vector<char> converted(count, 0);

  // First pass: collect the result ids of all spec-constant instructions.
  for (size_t i = 5; i < count;) {
    const uint32_t inst = words[i];
    const uint32_t wc = inst >> 16;
    const uint32_t op = inst & 0xffff;
    if (wc == 0 || i + wc > count) {
      return 0;
    }
    if (op >= kVexaOpSpecConstantFirst && op <= kVexaOpSpecConstantLast && wc >= 3) {
      const uint32_t id = words[i + 2];
      if (id < bound) {
        spec[id] = true;
      }
    }
    i += wc;
  }

  // Fixpoint: an OpConstantComposite with a spec-constant constituent must be
  // OpSpecConstantComposite; its result id then counts as spec too.
  uint32_t changed = 0;
  bool progress = true;
  while (progress) {
    progress = false;
    for (size_t i = 5; i < count;) {
      const uint32_t inst = words[i];
      const uint32_t wc = inst >> 16;
      const uint32_t op = inst & 0xffff;
      if (wc == 0 || i + wc > count) {
        return 0;
      }
      if (op == kVexaOpConstantComposite && wc >= 3 && !converted[i]) {
        bool has_spec = false;
        for (uint32_t k = 3; k < wc; ++k) {
          const uint32_t id = words[i + k];
          if (id < bound && spec[id]) {
            has_spec = true;
            break;
          }
        }
        if (has_spec) {
          const uint32_t id = words[i + 2];
          converted[i] = 1;
          if (patch) {
            patch[i] = (patch[i] & 0xffff0000u) | kVexaOpSpecConstantComposite;
          }
          if (id < bound && !spec[id]) {
            spec[id] = true;
          }
          ++changed;
          progress = true;
        }
      }
      i += wc;
    }
  }
  return changed;
}
} // namespace

static bool VexaNeedsSpecCompositeFix(const uint32_t* words, size_t count) {
  return VexaSpecCompositeScan(words, count, nullptr) != 0;
}

static uint32_t VexaFixSpecConstantComposites(std::vector<uint32_t>& code) {
  return VexaSpecCompositeScan(code.data(), code.size(), code.data());
}
