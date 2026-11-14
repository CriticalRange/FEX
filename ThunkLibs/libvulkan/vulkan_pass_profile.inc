// Per-primary-command-buffer GPU timings. Previous results are read before
// BeginCommandBuffer, when the caller must have completed that buffer's use.
#include <atomic>
#include <sys/stat.h>
static std::unordered_map<VkShaderModule, uint64_t> VexaShaderHashes;
static std::unordered_map<VkPipeline, uint64_t> VexaFragmentHashes;
static std::mutex VexaShaderMutex;
static void VexaDumpShader(VkShaderModule module, const VkShaderModuleCreateInfo* info) {
  uint64_t hash = 14695981039346656037ULL;
  const auto bytes = reinterpret_cast<const uint8_t*>(info->pCode);
  for (size_t i = 0; i < info->codeSize; ++i) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
  { std::lock_guard lock {VexaShaderMutex}; VexaShaderHashes[module] = hash; }
  if (const char* base = std::getenv("FEX_JIT_SYMBOL_DIR")) {
    const auto directory = std::string(base) + "/vk-shaders";
    mkdir(directory.c_str(), 0700);
    char name[64];
    snprintf(name, sizeof(name), "/shader-%016llx.spv", static_cast<unsigned long long>(hash));
    if (auto file = fopen((directory + name).c_str(), "wb")) { fwrite(info->pCode, 1, info->codeSize, file); fclose(file); }
  }
}
struct VexaGpuOperation {
  const char* Kind;
  uint32_t X, Y, Z;
  VkPipeline Pipeline {};
  uint32_t Binds {};
};
struct VexaPassProfile {
  VkDevice Device {};
  VkCommandPool Pool {};
  VkQueryPool Queries {};
  uint64_t Frames {};
  float Period {};
  uint32_t Bits {};
  bool InPass {};
  bool Primary {};
  VkPipeline LastGraphics {}, LastCompute {};
  uint32_t GraphicsBinds {};
  std::vector<VexaGpuOperation> Operations;
  ~VexaPassProfile() { if (Queries) LDR_PTR(vkDestroyQueryPool)(Device, Queries, nullptr); }
  bool Start(VkCommandBuffer buffer, const char* kind, uint32_t x, uint32_t y, uint32_t z = 0) {
    if (!Primary || !Queries || Operations.size() >= 128) return false;
    LDR_PTR(vkCmdWriteTimestamp)(buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, Queries, Operations.size() * 2);
    Operations.push_back({kind, x, y, z});
    return true;
  }
  void End(VkCommandBuffer buffer) {
    LDR_PTR(vkCmdWriteTimestamp)(buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, Queries, Operations.size() * 2 - 1);
  }
  void Begin(VkCommandBuffer buffer) {
    if (!Queries) {
      VkQueryPoolCreateInfo info {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      info.queryType = VK_QUERY_TYPE_TIMESTAMP;
      info.queryCount = 256;
      VkPhysicalDeviceProperties props {};
      LDR_PTR(vkGetPhysicalDeviceProperties)(g_vexa_physical_device, &props);
      Period = props.limits.timestampPeriod;
      if (LDR_PTR(vkCreateQueryPool)(Device, &info, nullptr, &Queries) != VK_SUCCESS) return;
    }
    if (!Operations.empty() && (Frames < 2 || Frames % 64 == 0)) {
      std::vector<uint64_t> values(Operations.size() * 4);
      const auto result = LDR_PTR(vkGetQueryPoolResults)(Device, Queries, 0, Operations.size() * 2, values.size() * 8,
                                                       values.data(), 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
      if (result == VK_SUCCESS) {
        double total {};
        for (size_t i = 0; i < Operations.size(); ++i) {
          if (!values[i * 4 + 1] || !values[i * 4 + 3]) continue;
          const auto& op = Operations[i];
          const uint64_t mask = Bits == 64 ? ~uint64_t {} : (uint64_t {1} << Bits) - 1;
          const double ms = ((values[i * 4 + 2] - values[i * 4]) & mask) * static_cast<double>(Period) / 1e6;
          total += ms;
          uint64_t fragment {};
          { std::lock_guard lock {VexaShaderMutex}; const auto found = VexaFragmentHashes.find(op.Pipeline); if (found != VexaFragmentHashes.end()) fragment = found->second; }
          fprintf(stderr, "[VEXA][VK-PASS] cmd=%p frame=%llu op=%zu kind=%s size=%ux%ux%u gpu_ms=%.4f binds=%u frag=%016llx\n",
                  buffer, static_cast<unsigned long long>(Frames), i, op.Kind, op.X, op.Y, op.Z, ms, op.Binds, static_cast<unsigned long long>(fragment));
        }
        fprintf(stderr, "[VEXA][VK-PASS] cmd=%p frame=%llu operations=%zu total_ms=%.4f\n",
                buffer, static_cast<unsigned long long>(Frames), Operations.size(), total);
      }
    }
    ++Frames;
    Operations.clear();
    InPass = false;
    LDR_PTR(vkCmdResetQueryPool)(buffer, Queries, 0, 256);
  }
};
static std::mutex VexaPassMutex;
static std::unordered_map<VkCommandBuffer, std::unique_ptr<VexaPassProfile>> VexaPassProfiles;
static std::unordered_map<VkCommandPool, std::pair<VkDevice, uint32_t>> VexaPoolTimestampBits;

static VkResult FEXFN_IMPL(vkCreateCommandPool)(VkDevice device, const VkCommandPoolCreateInfo* info,
                                               const VkAllocationCallbacks* alloc, VkCommandPool* out) {
  const auto result = LDR_PTR(vkCreateCommandPool)(device, info, alloc, out);
  if (result == VK_SUCCESS && VexaVkProfileEnabled() && !(info->flags & VK_COMMAND_POOL_CREATE_PROTECTED_BIT)) {
    uint32_t count {};
    LDR_PTR(vkGetPhysicalDeviceQueueFamilyProperties)(g_vexa_physical_device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    LDR_PTR(vkGetPhysicalDeviceQueueFamilyProperties)(g_vexa_physical_device, &count, families.data());
    if (info->queueFamilyIndex < count) {
      std::lock_guard lock {VexaPassMutex};
      VexaPoolTimestampBits[*out] = {device, families[info->queueFamilyIndex].timestampValidBits};
    }
  }
  return result;
}

static void VexaDestroyPassProfiles(VkDevice device) {
  std::lock_guard lock {VexaPassMutex};
  for (auto it = VexaPassProfiles.begin(); it != VexaPassProfiles.end();) {
    if (it->second->Device == device) it = VexaPassProfiles.erase(it); else ++it;
  }
  for (auto it = VexaPoolTimestampBits.begin(); it != VexaPoolTimestampBits.end();) {
    if (it->second.first == device) it = VexaPoolTimestampBits.erase(it); else ++it;
  }
}
static VkResult FEXFN_IMPL(vkAllocateCommandBuffers)(VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out) {
  const auto result = LDR_PTR(vkAllocateCommandBuffers)(device, info, out);
  if (result == VK_SUCCESS && VexaVkProfileEnabled()) {
    std::lock_guard lock {VexaPassMutex};
    const auto bits = VexaPoolTimestampBits.find(info->commandPool);
    if (bits == VexaPoolTimestampBits.end() || bits->second.first != device || !bits->second.second) return result;
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
      auto profile = std::make_unique<VexaPassProfile>();
      profile->Device = device; profile->Pool = info->commandPool;
      profile->Bits = bits->second.second;
      profile->Primary = info->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      VexaPassProfiles[out[i]] = std::move(profile);
    }
  }
  return result;
}
static void FEXFN_IMPL(vkFreeCommandBuffers)(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer* buffers) {
  if (VexaVkProfileEnabled()) {
    std::lock_guard lock {VexaPassMutex};
    for (uint32_t i = 0; i < count; ++i) VexaPassProfiles.erase(buffers[i]);
  }
  LDR_PTR(vkFreeCommandBuffers)(device, pool, count, buffers);
}
static void FEXFN_IMPL(vkDestroyCommandPool)(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* alloc) {
  if (VexaVkProfileEnabled()) {
    std::lock_guard lock {VexaPassMutex};
    VexaPoolTimestampBits.erase(pool);
    for (auto it = VexaPassProfiles.begin(); it != VexaPassProfiles.end();) {
      if (it->second->Device == device && it->second->Pool == pool) it = VexaPassProfiles.erase(it); else ++it;
    }
  }
  LDR_PTR(vkDestroyCommandPool)(device, pool, alloc);
}
static VkResult FEXFN_IMPL(vkBeginCommandBuffer)(VkCommandBuffer buffer, const VkCommandBufferBeginInfo* info) {
  const auto result = LDR_PTR(vkBeginCommandBuffer)(buffer, info);
  if (result == VK_SUCCESS && VexaVkProfileEnabled()) {
    std::lock_guard lock {VexaPassMutex};
    const auto found = VexaPassProfiles.find(buffer);
    if (found != VexaPassProfiles.end()) {
      found->second->LastGraphics = found->second->LastCompute = 0;
      found->second->GraphicsBinds = 0;
      if (found->second->Primary) found->second->Begin(buffer);
    }
  }
  return result;
}
static void FEXFN_IMPL(vkCmdBeginRenderPass)(VkCommandBuffer buffer, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
  if (VexaVkProfileEnabled()) {
    std::lock_guard lock {VexaPassMutex};
    const auto found = VexaPassProfiles.find(buffer);
    if (found != VexaPassProfiles.end()) found->second->InPass = found->second->Start(buffer, "render", info->renderArea.extent.width, info->renderArea.extent.height);
  }
  LDR_PTR(vkCmdBeginRenderPass)(buffer, info, contents);
}
static void FEXFN_IMPL(vkCmdEndRenderPass)(VkCommandBuffer buffer) {
  LDR_PTR(vkCmdEndRenderPass)(buffer);
  if (VexaVkProfileEnabled()) {
    std::lock_guard lock {VexaPassMutex};
    const auto found = VexaPassProfiles.find(buffer);
    if (found != VexaPassProfiles.end() && found->second->InPass) { found->second->End(buffer); found->second->InPass = false; }
  }
}
static void FEXFN_IMPL(vkCmdDispatch)(VkCommandBuffer buffer, uint32_t x, uint32_t y, uint32_t z) {
  if (!VexaVkProfileEnabled()) { LDR_PTR(vkCmdDispatch)(buffer, x, y, z); return; }
  std::lock_guard lock {VexaPassMutex};
  const auto found = VexaPassProfiles.find(buffer);
  const bool started = found != VexaPassProfiles.end() && found->second->Start(buffer, "compute", x, y, z);
  LDR_PTR(vkCmdDispatch)(buffer, x, y, z);
  if (started) found->second->End(buffer);
}
static void FEXFN_IMPL(vkCmdBindPipeline)(VkCommandBuffer buffer, VkPipelineBindPoint point, VkPipeline pipeline) {
  if (VexaVkProfileEnabled()) {
    std::lock_guard lock {VexaPassMutex};
    const auto found = VexaPassProfiles.find(buffer);
    if (found != VexaPassProfiles.end()) {
      auto& profile = *found->second;
      if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        profile.LastGraphics = pipeline; ++profile.GraphicsBinds;
        if (profile.InPass) { auto& op = profile.Operations.back(); op.Pipeline = pipeline; ++op.Binds; }
      } else if (point == VK_PIPELINE_BIND_POINT_COMPUTE) profile.LastCompute = pipeline;
    }
  }
  LDR_PTR(vkCmdBindPipeline)(buffer, point, pipeline);
}
static void FEXFN_IMPL(vkCmdExecuteCommands)(VkCommandBuffer buffer, uint32_t count, const VkCommandBuffer* secondary) {
  if (VexaVkProfileEnabled()) {
    std::lock_guard lock {VexaPassMutex};
    const auto found = VexaPassProfiles.find(buffer);
    if (found != VexaPassProfiles.end() && found->second->InPass) {
      auto& op = found->second->Operations.back();
      for (uint32_t i = 0; i < count; ++i) {
        const auto child = VexaPassProfiles.find(secondary[i]);
        if (child != VexaPassProfiles.end()) { op.Pipeline = child->second->LastGraphics; op.Binds += child->second->GraphicsBinds; }
      }
    }
  }
  LDR_PTR(vkCmdExecuteCommands)(buffer, count, secondary);
}
static VkResult FEXFN_IMPL(vkCreateShaderModule)(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
// Drivers without maintenance5 (Mali) dereference a null stage module. DXVK
// passes module=VK_NULL_HANDLE plus a chained VkShaderModuleCreateInfo, so
// materialize a real module for such stages.
static bool VexaStageIsInline(const VkPipelineShaderStageCreateInfo& stage) {
  if (stage.module != VK_NULL_HANDLE) return false;
  for (auto* p = static_cast<const VkBaseInStructure*>(stage.pNext); p; p = p->pNext)
    if (p->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) return true;
  return false;
}
static VkResult VexaMaterializeStage(VkDevice device, VkPipelineShaderStageCreateInfo& stage, std::vector<VkShaderModule>& modules) {
  const VkBaseInStructure* previous = nullptr;
  auto* node = static_cast<const VkBaseInStructure*>(stage.pNext);
  while (node && node->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) { previous = node; node = node->pNext; }
  VkShaderModule module = VK_NULL_HANDLE;
  const auto result = FEXFN_IMPL(vkCreateShaderModule)(device, reinterpret_cast<const VkShaderModuleCreateInfo*>(node), nullptr, &module);
  if (result != VK_SUCCESS) return result;
  modules.push_back(module);
  stage.module = module;
  if (previous) const_cast<VkBaseInStructure*>(previous)->pNext = node->pNext;
  else stage.pNext = node->pNext;
  return VK_SUCCESS;
}
static void VexaDestroyStageModules(VkDevice device, const std::vector<VkShaderModule>& modules) {
  if (modules.empty()) return;
  const auto destroy = reinterpret_cast<PFN_vkDestroyShaderModule>(LDR_PTR(vkGetDeviceProcAddr)(device, "vkDestroyShaderModule"));
  for (const auto module : modules) destroy(device, module, nullptr);
}
static VkResult FEXFN_IMPL(vkCreateComputePipelines)(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                    const VkComputePipelineCreateInfo* info, const VkAllocationCallbacks* alloc, VkPipeline* out) {
  std::vector<VkComputePipelineCreateInfo> patched;
  std::vector<VkShaderModule> modules;
  for (uint32_t i = 0; i < count; ++i) {
    if (!VexaStageIsInline(info[i].stage)) continue;
    if (patched.empty()) patched.assign(info, info + count);
    if (const auto result = VexaMaterializeStage(device, patched[i].stage, modules); result != VK_SUCCESS) {
      VexaDestroyStageModules(device, modules);
      return result;
    }
  }
  const auto result = LDR_PTR(vkCreateComputePipelines)(device, cache, count, patched.empty() ? info : patched.data(), alloc, out);
  VexaDestroyStageModules(device, modules);
  return result;
}
static VkResult FEXFN_IMPL(vkCreateGraphicsPipelines)(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                     const VkGraphicsPipelineCreateInfo* info, const VkAllocationCallbacks* alloc, VkPipeline* out) {
  std::vector<VkGraphicsPipelineCreateInfo> patchedInfos;
  std::vector<std::vector<VkPipelineShaderStageCreateInfo>> patchedStages;
  std::vector<VkShaderModule> inlineModules;
  for (uint32_t i = 0; i < count; ++i) {
    bool inlineStage = false;
    for (uint32_t j = 0; j < info[i].stageCount; ++j) inlineStage |= VexaStageIsInline(info[i].pStages[j]);
    if (!inlineStage) continue;
    if (patchedInfos.empty()) { patchedInfos.assign(info, info + count); patchedStages.resize(count); }
    patchedStages[i].assign(info[i].pStages, info[i].pStages + info[i].stageCount);
    patchedInfos[i].pStages = patchedStages[i].data();
    for (auto& stage : patchedStages[i]) {
      if (const auto result = VexaMaterializeStage(device, stage, inlineModules); result != VK_SUCCESS) {
        VexaDestroyStageModules(device, inlineModules);
        return result;
      }
    }
  }
  if (!patchedInfos.empty()) info = patchedInfos.data();
  if (VexaVkProfileEnabled()) {
    static std::atomic<uint64_t> calls {};
    if (++calls <= 8) for (uint32_t i = 0; i < count; ++i) {
      fprintf(stderr, "[VEXA][VK-PIPELINE] flags=0x%x stages=%u samples=%u\n", info[i].flags, info[i].stageCount,
              info[i].pMultisampleState ? static_cast<unsigned>(info[i].pMultisampleState->rasterizationSamples) : 0);
    }
  }
  const auto result = LDR_PTR(vkCreateGraphicsPipelines)(device, cache, count, info, alloc, out);
  VexaDestroyStageModules(device, inlineModules);
  if (result == VK_SUCCESS && VexaVkProfileEnabled()) {
    std::lock_guard lock {VexaShaderMutex};
    for (uint32_t i = 0; i < count; ++i) for (uint32_t j = 0; j < info[i].stageCount; ++j) {
      if (info[i].pStages[j].stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
        const auto shader = VexaShaderHashes.find(info[i].pStages[j].module);
        if (shader != VexaShaderHashes.end()) VexaFragmentHashes[out[i]] = shader->second;
      }
    }
  }
  return result;
}
