// Opt-in Android/x64 queue timestamp sampling. Query reads never wait.
// The span includes original semaphore waits; it is not shader-only time.
#include <array>
#include <memory>

struct VexaGpuQueueProfile {
  static constexpr unsigned Slots = 8;
  VkDevice Device {};
  uint32_t Family {}, Bits {};
  float Period {};
  VkCommandPool Commands {};
  VkQueryPool Queries {};
  std::array<VkCommandBuffer, Slots * 2> Buffers {};
  std::array<VkFence, Slots> Fences {};
  std::array<bool, Slots> Pending {};
  std::array<uint32_t, Slots> Waits {}, Cmds {};
  uint64_t Calls {}, Samples {};
  unsigned Next {};
  double SumMs {}, MaxMs {};
  bool Tried {}, Ready {};

  ~VexaGpuQueueProfile() {
    for (auto fence : Fences) if (fence) LDR_PTR(vkDestroyFence)(Device, fence, nullptr);
    if (Commands) LDR_PTR(vkDestroyCommandPool)(Device, Commands, nullptr);
    if (Queries) LDR_PTR(vkDestroyQueryPool)(Device, Queries, nullptr);
  }

  void Init() {
    Tried = true;
    uint32_t count {};
    LDR_PTR(vkGetPhysicalDeviceQueueFamilyProperties)(g_vexa_physical_device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    LDR_PTR(vkGetPhysicalDeviceQueueFamilyProperties)(g_vexa_physical_device, &count, families.data());
    if (Family >= count || !families[Family].timestampValidBits || !(families[Family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) return;
    Bits = families[Family].timestampValidBits;
    VkPhysicalDeviceProperties props {};
    LDR_PTR(vkGetPhysicalDeviceProperties)(g_vexa_physical_device, &props);
    Period = props.limits.timestampPeriod;
    VkCommandPoolCreateInfo pool {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.queueFamilyIndex = Family;
    if (LDR_PTR(vkCreateCommandPool)(Device, &pool, nullptr, &Commands) != VK_SUCCESS) return;
    VkQueryPoolCreateInfo query {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query.queryCount = Slots * 2;
    if (LDR_PTR(vkCreateQueryPool)(Device, &query, nullptr, &Queries) != VK_SUCCESS) return;
    VkCommandBufferAllocateInfo allocate {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = Commands;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = Buffers.size();
    if (LDR_PTR(vkAllocateCommandBuffers)(Device, &allocate, Buffers.data()) != VK_SUCCESS) return;
    VkFenceCreateInfo fenceInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (auto& fence : Fences) if (LDR_PTR(vkCreateFence)(Device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) return;
    for (unsigned i = 0; i < Buffers.size(); ++i) {
      VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      if (LDR_PTR(vkBeginCommandBuffer)(Buffers[i], &begin) != VK_SUCCESS) return;
      if ((i & 1) == 0) LDR_PTR(vkCmdResetQueryPool)(Buffers[i], Queries, i, 2);
      // BOTTOM isolates the sample from unfinished earlier queue submissions.
      LDR_PTR(vkCmdWriteTimestamp)(Buffers[i], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, Queries, i);
      if (LDR_PTR(vkEndCommandBuffer)(Buffers[i]) != VK_SUCCESS) return;
    }
    Ready = true;
    fprintf(stderr, "[VEXA][VK-GPU] ready family=%u timestamp_bits=%u period_ns=%.3f sample_every=17\n", Family, Bits, Period);
  }

  VkResult Submit(VkQueue queue, uint32_t count, const VkSubmitInfo* infos, VkFence fence) {
    ++Calls;
    if (Calls % 17 || !count) return LDR_PTR(vkQueueSubmit)(queue, count, infos, fence);
    if (!Tried) Init();
    if (!Ready) return LDR_PTR(vkQueueSubmit)(queue, count, infos, fence);
    if (Pending[Next]) {
      // Availability alone can still describe the previous query use until
      // its GPU reset executes. A private fence proves this use completed.
      if (LDR_PTR(vkGetFenceStatus)(Device, Fences[Next]) != VK_SUCCESS) return LDR_PTR(vkQueueSubmit)(queue, count, infos, fence);
      uint64_t values[4] {};
      const auto status = LDR_PTR(vkGetQueryPoolResults)(Device, Queries, Next * 2, 2, sizeof(values), values, 16,
                                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
      if (status != VK_SUCCESS || !values[1] || !values[3]) return LDR_PTR(vkQueueSubmit)(queue, count, infos, fence);
      const uint64_t mask = Bits == 64 ? ~uint64_t {} : (uint64_t {1} << Bits) - 1;
      const double ms = ((values[2] - values[0]) & mask) * static_cast<double>(Period) / 1e6;
      if (Samples < 8 || Samples % 64 < 4) {
        fprintf(stderr, "[VEXA][VK-GPU] sample span_ms=%.4f cmds=%u waits=%u\n", ms, Cmds[Next], Waits[Next]);
      }
      SumMs += ms;
      MaxMs = std::max(MaxMs, ms);
      if (++Samples % 16 == 0) {
        fprintf(stderr, "[VEXA][VK-GPU] queue=%p samples=%llu queue_span_avg_ms=%.4f max_ms=%.4f\n",
                queue, static_cast<unsigned long long>(Samples), SumMs / 16, MaxMs);
        SumMs = MaxMs = 0;
      }
    }
    // Preserve all original submit infos, semaphore chains, and their order.
    // Keep the caller's fence on its work. A separate trailing marker has a
    // private completion fence, never resetting or retaining guest fences.
    std::vector<VkSubmitInfo> submits(count + 1);
    submits.front().sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submits.front().commandBufferCount = 1;
    submits.front().pCommandBuffers = &Buffers[Next * 2];
    std::copy_n(infos, count, submits.begin() + 1);
    Waits[Next] = Cmds[Next] = 0;
    for (uint32_t i = 0; i < count; ++i) { Waits[Next] += infos[i].waitSemaphoreCount; Cmds[Next] += infos[i].commandBufferCount; }
    const auto result = LDR_PTR(vkQueueSubmit)(queue, submits.size(), submits.data(), fence);
    if (result == VK_SUCCESS) {
      VkSubmitInfo end {VK_STRUCTURE_TYPE_SUBMIT_INFO};
      end.commandBufferCount = 1;
      end.pCommandBuffers = &Buffers[Next * 2 + 1];
      if (LDR_PTR(vkResetFences)(Device, 1, &Fences[Next]) != VK_SUCCESS ||
          LDR_PTR(vkQueueSubmit)(queue, 1, &end, Fences[Next]) != VK_SUCCESS) {
        Ready = false;
        fprintf(stderr, "[VEXA][VK-GPU] disabled after marker submission failure\n");
        return result;
      }
      Pending[Next] = true;
      Next = (Next + 1) % Slots;
    }
    return result;
  }
};

static std::mutex VexaGpuProfilesMutex;
static std::unordered_map<VkQueue, std::unique_ptr<VexaGpuQueueProfile>> VexaGpuProfiles;

static void FEXFN_IMPL(vkGetDeviceQueue)(VkDevice device, uint32_t family, uint32_t index, VkQueue* out) {
  LDR_PTR(vkGetDeviceQueue)(device, family, index, out);
  if (VexaVkProfileEnabled() && out && *out) {
    std::lock_guard lock {VexaGpuProfilesMutex};
    if (!VexaGpuProfiles.contains(*out)) {
      auto profile = std::make_unique<VexaGpuQueueProfile>();
      profile->Device = device;
      profile->Family = family;
      VexaGpuProfiles.emplace(*out, std::move(profile));
    }
  }
}

static VkResult FEXFN_IMPL(vkQueueSubmit)(VkQueue queue, uint32_t count, const VkSubmitInfo* infos, VkFence fence) {
  const bool enabled = VexaVkProfileEnabled();
  if (!enabled) return LDR_PTR(vkQueueSubmit)(queue, count, infos, fence);
  VexaGpuQueueProfile* profile {};
  {
    std::lock_guard lock {VexaGpuProfilesMutex};
    const auto found = VexaGpuProfiles.find(queue);
    if (found != VexaGpuProfiles.end()) profile = found->second.get();
  }
  const auto start = std::chrono::steady_clock::now();
  const auto cpuStart = VexaVkWallTime::ThreadCpuNs();
  const auto result = profile ? profile->Submit(queue, count, infos, fence) : LDR_PTR(vkQueueSubmit)(queue, count, infos, fence);
  static thread_local VexaVkWallTime stats;
  stats.Add("submit", start, result, cpuStart);
  return result;
}

static void FEXFN_IMPL(vkDestroyDevice)(VkDevice device, const VkAllocationCallbacks*) {
  VexaDestroyPassProfiles(device);
  {
    std::lock_guard lock {VexaGpuProfilesMutex};
    for (auto it = VexaGpuProfiles.begin(); it != VexaGpuProfiles.end();) {
      if (it->second->Device == device) it = VexaGpuProfiles.erase(it);
      else ++it;
    }
  }
  LDR_PTR(vkDestroyDevice)(device, nullptr);
}
