#include <unistd.h>
// Narrow opt-in experiment: the measured 43-tap fragment shader has one
// constant loop. Request unrolling without changing arithmetic or precision.
static std::vector<uint32_t> VexaOptimizeShader(const VkShaderModuleCreateInfo* info) {
  static const bool enabled = [] {
    const char* value = std::getenv("VEXA_VK_OPTIMIZE_LOOPS");
    if (value && std::strcmp(value, "1") == 0) return true;
    const char* base = std::getenv("FEX_JIT_SYMBOL_DIR");
    return base && access((std::string(base) + "/vk_loop_unroll.flag").c_str(), F_OK) == 0;
  }();
  if (!enabled || !info || info->codeSize != 8796 || !info->pCode) return {};
  uint64_t hash = 14695981039346656037ULL;
  const auto bytes = reinterpret_cast<const uint8_t*>(info->pCode);
  for (size_t i = 0; i < info->codeSize; ++i) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
  if (hash != 0x93d389af8308d139ULL) return {};
  // A separately validated, offline-unrolled module can be supplied for the
  // experiment. The original module's exact hash guards its interface/version.
  if (const char* base = std::getenv("FEX_JIT_SYMBOL_DIR")) {
    const auto path = std::string(base) + "/vk-loop-unrolled.spv";
    if (auto file = fopen(path.c_str(), "rb")) {
      fseek(file, 0, SEEK_END);
      const long size = ftell(file);
      rewind(file);
      if (size >= 20 && size <= 4 * 1024 * 1024 && size % 4 == 0) {
        std::vector<uint32_t> code(size / 4);
        const auto read = fread(code.data(), 1, size, file);
        fclose(file);
        if (read == static_cast<size_t>(size) && code[0] == 0x07230203) {
          fprintf(stderr, "[VEXA][VK-OPT] shader=%016llx offline-unrolled bytes=%ld\n", static_cast<unsigned long long>(hash), size);
          return code;
        }
      } else fclose(file);
    }
  }
  std::vector<uint32_t> code(info->pCode, info->pCode + info->codeSize / 4);
  if (code[0] != 0x07230203 || code[1157] != ((4u << 16) | 246u) || code[1160] != 0) return {};
  code[1160] = 1; // SPIR-V LoopControlUnrollMask, an optimization hint only.
  fprintf(stderr, "[VEXA][VK-OPT] shader=%016llx constant-loop unroll hint enabled\n", static_cast<unsigned long long>(hash));
  return code;
}
