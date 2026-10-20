// Android/x64 swapchain pre-rotation.
// A guest that keeps preTransform=IDENTITY on a rotated display (landscape
// game, portrait panel) makes the compositor rotate every frame. On MT6989
// that HWC path costs ~10 ms per frame and caps Unity at ~69 fps, while GL
// gets pre-rotated buffers from the EGL driver. The guest renders into
// private images instead; one host draw per present rotates them into a
// swapchain created with preTransform=currentTransform.
// Kill switch: files/vk_prerotate_off.flag.
#include <atomic>
#include <memory>

// Generated with glslc -O --target-env=vulkan1.0 -mfmt=num from:
//   vert: fullscreen triangle from gl_VertexIndex.
//   frag: layout(push_constant) { ivec2 size; int mode; };
//         p = ivec2(gl_FragCoord.xy);
//         s = mode == 0 ? ivec2(p.y, size.y - 1 - p.x)   // ROTATE_90 (clockwise)
//           : mode == 1 ? ivec2(size.x - 1 - p.y, p.x)   // ROTATE_270
//                       : size - 1 - p;                  // ROTATE_180
//         color = texelFetch(src, s, 0);
static const uint32_t kVexaRotVert[] = {
  0x07230203,0x00010000,0x000d000a,0x0000002c,
  0x00000000,0x00020011,0x00000001,0x0006000b,
  0x00000001,0x4c534c47,0x6474732e,0x3035342e,
  0x00000000,0x0003000e,0x00000000,0x00000001,
  0x0007000f,0x00000000,0x00000004,0x6e69616d,
  0x00000000,0x0000000c,0x0000001d,0x00040047,
  0x0000000c,0x0000000b,0x0000002a,0x00050048,
  0x0000001b,0x00000000,0x0000000b,0x00000000,
  0x00050048,0x0000001b,0x00000001,0x0000000b,
  0x00000001,0x00050048,0x0000001b,0x00000002,
  0x0000000b,0x00000003,0x00050048,0x0000001b,
  0x00000003,0x0000000b,0x00000004,0x00030047,
  0x0000001b,0x00000002,0x00020013,0x00000002,
  0x00030021,0x00000003,0x00000002,0x00030016,
  0x00000006,0x00000020,0x00040017,0x00000007,
  0x00000006,0x00000002,0x00040015,0x0000000a,
  0x00000020,0x00000001,0x00040020,0x0000000b,
  0x00000001,0x0000000a,0x0004003b,0x0000000b,
  0x0000000c,0x00000001,0x0004002b,0x0000000a,
  0x0000000e,0x00000001,0x0004002b,0x0000000a,
  0x00000010,0x00000002,0x00040017,0x00000017,
  0x00000006,0x00000004,0x00040015,0x00000018,
  0x00000020,0x00000000,0x0004002b,0x00000018,
  0x00000019,0x00000001,0x0004001c,0x0000001a,
  0x00000006,0x00000019,0x0006001e,0x0000001b,
  0x00000017,0x00000006,0x0000001a,0x0000001a,
  0x00040020,0x0000001c,0x00000003,0x0000001b,
  0x0004003b,0x0000001c,0x0000001d,0x00000003,
  0x0004002b,0x0000000a,0x0000001e,0x00000000,
  0x0004002b,0x00000006,0x00000020,0x40000000,
  0x0004002b,0x00000006,0x00000022,0x3f800000,
  0x0004002b,0x00000006,0x00000025,0x00000000,
  0x00040020,0x00000029,0x00000003,0x00000017,
  0x0005002c,0x00000007,0x0000002b,0x00000022,
  0x00000022,0x00050036,0x00000002,0x00000004,
  0x00000000,0x00000003,0x000200f8,0x00000005,
  0x0004003d,0x0000000a,0x0000000d,0x0000000c,
  0x000500c4,0x0000000a,0x0000000f,0x0000000d,
  0x0000000e,0x000500c7,0x0000000a,0x00000011,
  0x0000000f,0x00000010,0x0004006f,0x00000006,
  0x00000012,0x00000011,0x000500c7,0x0000000a,
  0x00000014,0x0000000d,0x00000010,0x0004006f,
  0x00000006,0x00000015,0x00000014,0x00050050,
  0x00000007,0x00000016,0x00000012,0x00000015,
  0x0005008e,0x00000007,0x00000021,0x00000016,
  0x00000020,0x00050083,0x00000007,0x00000024,
  0x00000021,0x0000002b,0x00050051,0x00000006,
  0x00000026,0x00000024,0x00000000,0x00050051,
  0x00000006,0x00000027,0x00000024,0x00000001,
  0x00070050,0x00000017,0x00000028,0x00000026,
  0x00000027,0x00000025,0x00000022,0x00050041,
  0x00000029,0x0000002a,0x0000001d,0x0000001e,
  0x0003003e,0x0000002a,0x00000028,0x000100fd,
  0x00010038
};
static const uint32_t kVexaRotFrag[] = {
  0x07230203,0x00010000,0x000d000a,0x00000058,
  0x00000000,0x00020011,0x00000001,0x0006000b,
  0x00000001,0x4c534c47,0x6474732e,0x3035342e,
  0x00000000,0x0003000e,0x00000000,0x00000001,
  0x0007000f,0x00000004,0x00000004,0x6e69616d,
  0x00000000,0x0000000d,0x00000048,0x00030010,
  0x00000004,0x00000007,0x00040047,0x0000000d,
  0x0000000b,0x0000000f,0x00050048,0x00000013,
  0x00000000,0x00000023,0x00000000,0x00050048,
  0x00000013,0x00000001,0x00000023,0x00000008,
  0x00030047,0x00000013,0x00000002,0x00040047,
  0x00000048,0x0000001e,0x00000000,0x00040047,
  0x0000004c,0x00000022,0x00000000,0x00040047,
  0x0000004c,0x00000021,0x00000000,0x00020013,
  0x00000002,0x00030021,0x00000003,0x00000002,
  0x00040015,0x00000006,0x00000020,0x00000001,
  0x00040017,0x00000007,0x00000006,0x00000002,
  0x00030016,0x0000000a,0x00000020,0x00040017,
  0x0000000b,0x0000000a,0x00000004,0x00040020,
  0x0000000c,0x00000001,0x0000000b,0x0004003b,
  0x0000000c,0x0000000d,0x00000001,0x00040017,
  0x0000000e,0x0000000a,0x00000002,0x0004001e,
  0x00000013,0x00000007,0x00000006,0x00040020,
  0x00000014,0x00000009,0x00000013,0x0004003b,
  0x00000014,0x00000015,0x00000009,0x0004002b,
  0x00000006,0x00000016,0x00000001,0x00040020,
  0x00000017,0x00000009,0x00000006,0x0004002b,
  0x00000006,0x0000001a,0x00000000,0x00020014,
  0x0000001b,0x00040015,0x00000020,0x00000020,
  0x00000000,0x0004002b,0x00000020,0x00000021,
  0x00000001,0x0004002b,0x00000020,0x00000028,
  0x00000000,0x00040020,0x0000003e,0x00000009,
  0x00000007,0x00040020,0x00000047,0x00000003,
  0x0000000b,0x0004003b,0x00000047,0x00000048,
  0x00000003,0x00090019,0x00000049,0x0000000a,
  0x00000001,0x00000000,0x00000000,0x00000000,
  0x00000001,0x00000000,0x0003001b,0x0000004a,
  0x00000049,0x00040020,0x0000004b,0x00000000,
  0x0000004a,0x0004003b,0x0000004b,0x0000004c,
  0x00000000,0x0005002c,0x00000007,0x00000057,
  0x00000016,0x00000016,0x00050036,0x00000002,
  0x00000004,0x00000000,0x00000003,0x000200f8,
  0x00000005,0x0004003d,0x0000000b,0x0000000f,
  0x0000000d,0x0007004f,0x0000000e,0x00000010,
  0x0000000f,0x0000000f,0x00000000,0x00000001,
  0x0004006e,0x00000007,0x00000011,0x00000010,
  0x00050041,0x00000017,0x00000018,0x00000015,
  0x00000016,0x0004003d,0x00000006,0x00000019,
  0x00000018,0x000500aa,0x0000001b,0x0000001c,
  0x00000019,0x0000001a,0x000300f7,0x0000001f,
  0x00000000,0x000400fa,0x0000001c,0x0000001e,
  0x0000002d,0x000200f8,0x0000001e,0x00050051,
  0x00000006,0x00000024,0x00000011,0x00000001,
  0x00060041,0x00000017,0x00000025,0x00000015,
  0x0000001a,0x00000021,0x0004003d,0x00000006,
  0x00000026,0x00000025,0x00050082,0x00000006,
  0x00000027,0x00000026,0x00000016,0x00050051,
  0x00000006,0x0000002a,0x00000011,0x00000000,
  0x00050082,0x00000006,0x0000002b,0x00000027,
  0x0000002a,0x00050050,0x00000007,0x0000002c,
  0x00000024,0x0000002b,0x000200f9,0x0000001f,
  0x000200f8,0x0000002d,0x000500aa,0x0000001b,
  0x00000030,0x00000019,0x00000016,0x000300f7,
  0x00000033,0x00000000,0x000400fa,0x00000030,
  0x00000032,0x0000003d,0x000200f8,0x00000032,
  0x00060041,0x00000017,0x00000034,0x00000015,
  0x0000001a,0x00000028,0x0004003d,0x00000006,
  0x00000035,0x00000034,0x00050082,0x00000006,
  0x00000036,0x00000035,0x00000016,0x00050051,
  0x00000006,0x00000038,0x00000011,0x00000001,
  0x00050082,0x00000006,0x00000039,0x00000036,
  0x00000038,0x00050051,0x00000006,0x0000003b,
  0x00000011,0x00000000,0x00050050,0x00000007,
  0x0000003c,0x00000039,0x0000003b,0x000200f9,
  0x00000033,0x000200f8,0x0000003d,0x00050041,
  0x0000003e,0x0000003f,0x00000015,0x0000001a,
  0x0004003d,0x00000007,0x00000040,0x0000003f,
  0x00050082,0x00000007,0x00000042,0x00000040,
  0x00000057,0x00050082,0x00000007,0x00000044,
  0x00000042,0x00000011,0x000200f9,0x00000033,
  0x000200f8,0x00000033,0x000700f5,0x00000007,
  0x00000055,0x0000003c,0x00000032,0x00000044,
  0x0000003d,0x000200f9,0x0000001f,0x000200f8,
  0x0000001f,0x000700f5,0x00000007,0x00000056,
  0x0000002c,0x0000001e,0x00000055,0x00000033,
  0x0004003d,0x0000004a,0x0000004d,0x0000004c,
  0x00040064,0x00000049,0x0000004f,0x0000004d,
  0x0007005f,0x0000000b,0x00000050,0x0000004f,
  0x00000056,0x00000002,0x0000001a,0x0003003e,
  0x00000048,0x00000050,0x000100fd,0x00010038
};

#define VEXA_ROT_FNS(X)                                                                                                        \
  X(vkGetSwapchainImagesKHR) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory)         \
  X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateRenderPass) X(vkDestroyRenderPass) \
  X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateSampler) X(vkDestroySampler) X(vkCreateDescriptorSetLayout)         \
  X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets)            \
  X(vkUpdateDescriptorSets) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateShaderModule)                      \
  X(vkDestroyShaderModule) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateSemaphore) X(vkDestroySemaphore)       \
  X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateCommandPool) X(vkDestroyCommandPool)       \
  X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdBeginRenderPass)   \
  X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDraw) X(vkQueueSubmit)  \
  X(vkGetDeviceQueue)

struct VexaRotFns {
#define VEXA_ROT_DECL(n) PFN_##n n {};
  VEXA_ROT_FNS(VEXA_ROT_DECL)
#undef VEXA_ROT_DECL
  bool Load(VkDevice device) {
#define VEXA_ROT_LOAD(n)                                                         \
  n = reinterpret_cast<PFN_##n>(LDR_PTR(vkGetDeviceProcAddr)(device, #n)); \
  if (!n) return false;
    VEXA_ROT_FNS(VEXA_ROT_LOAD)
#undef VEXA_ROT_LOAD
    return true;
  }
};

static std::atomic<bool> VexaRotFailed {};
static std::mutex VexaRotMutex;
// Queues created with flags == 0, per device, so a VkQueue can be mapped back
// to its family without hooking every queue getter.
static std::unordered_map<VkDevice, std::vector<std::pair<uint32_t, uint32_t>>> VexaRotDeviceQueues;

static void VexaRotRecordDevice(VkDevice device, const VkDeviceCreateInfo* info) {
  std::vector<std::pair<uint32_t, uint32_t>> families;
  for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i) {
    const auto& queue = info->pQueueCreateInfos[i];
    if (!queue.flags) families.emplace_back(queue.queueFamilyIndex, queue.queueCount);
  }
  std::lock_guard lock {VexaRotMutex};
  VexaRotDeviceQueues[device] = std::move(families);
}

struct VexaRotSwapchain {
  VexaRotFns Fns;
  VkDevice Device {};
  VkExtent2D Guest {}, Real {};
  int32_t Mode {};
  bool Recorded {}, Lost {};
  std::vector<VkImage> Images;
  std::vector<VkDeviceMemory> Memory;
  std::vector<VkImageView> Views, Targets;
  std::vector<VkFramebuffer> Framebuffers;
  std::vector<VkDescriptorSet> Sets;
  std::vector<VkSemaphore> Done;
  std::vector<VkFence> Fences;
  std::vector<VkCommandBuffer> Commands;
  VkRenderPass Pass {};
  VkSampler Sampler {};
  VkDescriptorSetLayout SetLayout {};
  VkDescriptorPool Pool {};
  VkPipelineLayout Layout {};
  VkPipeline Pipeline {};
  VkCommandPool CommandPool {};

  ~VexaRotSwapchain() {
    if (!Fns.vkDestroyFence) return;
    std::vector<VkFence> live;
    for (auto fence : Fences) if (fence) live.push_back(fence);
    if (!Lost && !live.empty()) Fns.vkWaitForFences(Device, live.size(), live.data(), VK_TRUE, UINT64_MAX);
    if (CommandPool) Fns.vkDestroyCommandPool(Device, CommandPool, nullptr);
    for (auto fence : live) Fns.vkDestroyFence(Device, fence, nullptr);
    for (auto semaphore : Done) if (semaphore) Fns.vkDestroySemaphore(Device, semaphore, nullptr);
    if (Pipeline) Fns.vkDestroyPipeline(Device, Pipeline, nullptr);
    if (Layout) Fns.vkDestroyPipelineLayout(Device, Layout, nullptr);
    if (Pool) Fns.vkDestroyDescriptorPool(Device, Pool, nullptr);
    if (SetLayout) Fns.vkDestroyDescriptorSetLayout(Device, SetLayout, nullptr);
    if (Sampler) Fns.vkDestroySampler(Device, Sampler, nullptr);
    for (auto framebuffer : Framebuffers) if (framebuffer) Fns.vkDestroyFramebuffer(Device, framebuffer, nullptr);
    if (Pass) Fns.vkDestroyRenderPass(Device, Pass, nullptr);
    for (auto view : Targets) if (view) Fns.vkDestroyImageView(Device, view, nullptr);
    for (auto view : Views) if (view) Fns.vkDestroyImageView(Device, view, nullptr);
    for (auto image : Images) if (image) Fns.vkDestroyImage(Device, image, nullptr);
    for (auto memory : Memory) if (memory) Fns.vkFreeMemory(Device, memory, nullptr);
  }

  bool Init(VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& info) {
    if (!Fns.Load(Device)) return false;
    uint32_t count {};
    if (Fns.vkGetSwapchainImagesKHR(Device, swapchain, &count, nullptr) != VK_SUCCESS || !count) return false;
    std::vector<VkImage> real(count);
    if (Fns.vkGetSwapchainImagesKHR(Device, swapchain, &count, real.data()) != VK_SUCCESS) return false;
    Images.resize(count);
    Memory.resize(count);
    Views.resize(count);
    Targets.resize(count);
    Framebuffers.resize(count);
    Sets.resize(count);
    Done.resize(count);
    Fences.resize(count);

    VkPhysicalDeviceMemoryProperties memory {};
    LDR_PTR(vkGetPhysicalDeviceMemoryProperties)(g_vexa_physical_device, &memory);
    VkImageCreateInfo image {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = info.imageFormat;
    image.extent = {Guest.width, Guest.height, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = info.imageUsage | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = info.imageSharingMode;
    image.queueFamilyIndexCount = info.queueFamilyIndexCount;
    image.pQueueFamilyIndices = info.pQueueFamilyIndices;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageViewCreateInfo view {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.imageFormat;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (uint32_t i = 0; i < count; ++i) {
      if (Fns.vkCreateImage(Device, &image, nullptr, &Images[i]) != VK_SUCCESS) return false;
      VkMemoryRequirements requirements {};
      Fns.vkGetImageMemoryRequirements(Device, Images[i], &requirements);
      uint32_t type = UINT32_MAX;
      for (uint32_t t = 0; t < memory.memoryTypeCount && type == UINT32_MAX; ++t) {
        if ((requirements.memoryTypeBits & (1u << t)) && (memory.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = t;
      }
      for (uint32_t t = 0; t < memory.memoryTypeCount && type == UINT32_MAX; ++t) {
        if (requirements.memoryTypeBits & (1u << t)) type = t;
      }
      VkMemoryAllocateInfo allocate {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
      if (type == UINT32_MAX || Fns.vkAllocateMemory(Device, &allocate, nullptr, &Memory[i]) != VK_SUCCESS ||
          Fns.vkBindImageMemory(Device, Images[i], Memory[i], 0) != VK_SUCCESS) {
        return false;
      }
      view.image = Images[i];
      if (Fns.vkCreateImageView(Device, &view, nullptr, &Views[i]) != VK_SUCCESS) return false;
      view.image = real[i];
      if (Fns.vkCreateImageView(Device, &view, nullptr, &Targets[i]) != VK_SUCCESS) return false;
    }

    VkAttachmentDescription attachment {};
    attachment.format = info.imageFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference color {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    // The present-wait semaphores are waited at ALL_COMMANDS, which chains
    // into this dependency before the UNDEFINED -> attachment transition.
    VkSubpassDependency dependency {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0};
    VkRenderPassCreateInfo pass {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = 1;
    pass.pAttachments = &attachment;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 1;
    pass.pDependencies = &dependency;
    if (Fns.vkCreateRenderPass(Device, &pass, nullptr, &Pass) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo framebuffer {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = Pass;
    framebuffer.attachmentCount = 1;
    framebuffer.width = Real.width;
    framebuffer.height = Real.height;
    framebuffer.layers = 1;
    for (uint32_t i = 0; i < count; ++i) {
      framebuffer.pAttachments = &Targets[i];
      if (Fns.vkCreateFramebuffer(Device, &framebuffer, nullptr, &Framebuffers[i]) != VK_SUCCESS) return false;
    }

    VkSamplerCreateInfo sampler {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (Fns.vkCreateSampler(Device, &sampler, nullptr, &Sampler) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding binding {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &Sampler};
    VkDescriptorSetLayoutCreateInfo setLayout {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 1, &binding};
    if (Fns.vkCreateDescriptorSetLayout(Device, &setLayout, nullptr, &SetLayout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize size {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count};
    VkDescriptorPoolCreateInfo pool {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, count, 1, &size};
    if (Fns.vkCreateDescriptorPool(Device, &pool, nullptr, &Pool) != VK_SUCCESS) return false;
    std::vector<VkDescriptorSetLayout> layouts(count, SetLayout);
    VkDescriptorSetAllocateInfo sets {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, Pool, count, layouts.data()};
    if (Fns.vkAllocateDescriptorSets(Device, &sets, Sets.data()) != VK_SUCCESS) return false;
    std::vector<VkDescriptorImageInfo> sources(count);
    std::vector<VkWriteDescriptorSet> writes(count);
    for (uint32_t i = 0; i < count; ++i) {
      sources[i] = {Sampler, Views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[i].dstSet = Sets[i];
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[i].pImageInfo = &sources[i];
    }
    Fns.vkUpdateDescriptorSets(Device, count, writes.data(), 0, nullptr);

    VkPushConstantRange push {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 3 * sizeof(int32_t)};
    VkPipelineLayoutCreateInfo layout {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &SetLayout, 1, &push};
    if (Fns.vkCreatePipelineLayout(Device, &layout, nullptr, &Layout) != VK_SUCCESS) return false;
    VkShaderModule modules[2] {};
    VkShaderModuleCreateInfo module {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof(kVexaRotVert), kVexaRotVert};
    bool ok = Fns.vkCreateShaderModule(Device, &module, nullptr, &modules[0]) == VK_SUCCESS;
    module.codeSize = sizeof(kVexaRotFrag);
    module.pCode = kVexaRotFrag;
    ok = ok && Fns.vkCreateShaderModule(Device, &module, nullptr, &modules[1]) == VK_SUCCESS;
    if (ok) {
      VkPipelineShaderStageCreateInfo stages[2] {};
      stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, modules[0], "main"};
      stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, modules[1], "main"};
      VkPipelineVertexInputStateCreateInfo input {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo assembly {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
      assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkViewport viewport {0, 0, float(Real.width), float(Real.height), 0, 1};
      VkRect2D scissor {{0, 0}, Real};
      VkPipelineViewportStateCreateInfo viewports {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, &viewport, 1, &scissor};
      VkPipelineRasterizationStateCreateInfo raster {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
      raster.polygonMode = VK_POLYGON_MODE_FILL;
      raster.cullMode = VK_CULL_MODE_NONE;
      raster.lineWidth = 1.0f;
      VkPipelineMultisampleStateCreateInfo multisample {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
      multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineColorBlendAttachmentState blendAttachment {};
      blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
      VkPipelineColorBlendStateCreateInfo blend {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      blend.attachmentCount = 1;
      blend.pAttachments = &blendAttachment;
      VkGraphicsPipelineCreateInfo pipeline {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      pipeline.stageCount = 2;
      pipeline.pStages = stages;
      pipeline.pVertexInputState = &input;
      pipeline.pInputAssemblyState = &assembly;
      pipeline.pViewportState = &viewports;
      pipeline.pRasterizationState = &raster;
      pipeline.pMultisampleState = &multisample;
      pipeline.pColorBlendState = &blend;
      pipeline.layout = Layout;
      pipeline.renderPass = Pass;
      ok = Fns.vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &Pipeline) == VK_SUCCESS;
    }
    for (auto shader : modules) if (shader) Fns.vkDestroyShaderModule(Device, shader, nullptr);
    if (!ok) return false;

    VkSemaphoreCreateInfo semaphore {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    for (uint32_t i = 0; i < count; ++i) {
      if (Fns.vkCreateSemaphore(Device, &semaphore, nullptr, &Done[i]) != VK_SUCCESS ||
          Fns.vkCreateFence(Device, &fence, nullptr, &Fences[i]) != VK_SUCCESS) {
        return false;
      }
    }
    return true;
  }

  // The present queue is first known here; record one static command buffer
  // per image on its family.
  bool Record(VkQueue queue) {
    std::vector<std::pair<uint32_t, uint32_t>> families;
    {
      std::lock_guard lock {VexaRotMutex};
      if (const auto it = VexaRotDeviceQueues.find(Device); it != VexaRotDeviceQueues.end()) families = it->second;
    }
    uint32_t family = UINT32_MAX;
    for (const auto& [index, count] : families) {
      for (uint32_t i = 0; i < count && family == UINT32_MAX; ++i) {
        VkQueue candidate {};
        Fns.vkGetDeviceQueue(Device, index, i, &candidate);
        if (candidate == queue) family = index;
      }
    }
    if (family == UINT32_MAX) return false;
    VkCommandPoolCreateInfo pool {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, family};
    if (Fns.vkCreateCommandPool(Device, &pool, nullptr, &CommandPool) != VK_SUCCESS) return false;
    Commands.resize(Images.size());
    VkCommandBufferAllocateInfo allocate {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, CommandPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          static_cast<uint32_t>(Commands.size())};
    if (Fns.vkAllocateCommandBuffers(Device, &allocate, Commands.data()) != VK_SUCCESS) return false;
    const int32_t constants[3] {static_cast<int32_t>(Guest.width), static_cast<int32_t>(Guest.height), Mode};
    for (size_t i = 0; i < Commands.size(); ++i) {
      const auto cmd = Commands[i];
      VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      if (Fns.vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) return false;
      // The guest left its image in PRESENT_SRC; restore that afterwards so
      // its next frame can transition from either PRESENT_SRC or UNDEFINED.
      VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = Images[i];
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      Fns.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
      VkRenderPassBeginInfo pass {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      pass.renderPass = Pass;
      pass.framebuffer = Framebuffers[i];
      pass.renderArea = {{0, 0}, Real};
      Fns.vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
      Fns.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, Pipeline);
      Fns.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, Layout, 0, 1, &Sets[i], 0, nullptr);
      Fns.vkCmdPushConstants(cmd, Layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), constants);
      Fns.vkCmdDraw(cmd, 3, 1, 0, 0);
      Fns.vkCmdEndRenderPass(cmd);
      barrier.srcAccessMask = 0;
      barrier.dstAccessMask = 0;
      barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      Fns.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
      if (Fns.vkEndCommandBuffer(cmd) != VK_SUCCESS) return false;
    }
    Recorded = true;
    fprintf(stderr, "[VEXA][VK] pre-rotation recorded family=%u images=%zu\n", family, Commands.size());
    return true;
  }
};

static std::unordered_map<VkSwapchainKHR, std::unique_ptr<VexaRotSwapchain>> VexaRotSwapchains;

// Returns the shader mode for a guest swapchain that needs host rotation, or -1.
static int32_t VexaRotMode(const VkSwapchainCreateInfoKHR& info, const VkSurfaceCapabilitiesKHR& caps) {
  if (VexaRotFailed || info.preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR || info.flags || info.imageArrayLayers != 1 ||
      !(caps.supportedTransforms & caps.currentTransform) || !(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
    return -1;
  }
  int32_t mode = -1;
  switch (caps.currentTransform) {
  case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: mode = 0; break;
  case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: mode = 1; break;
  case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: mode = 2; break;
  default: return -1;
  }
  if (access("/data/data/com.critical.vexaemulator/files/vk_prerotate_off.flag", F_OK) == 0) return -1;
  VkFormatProperties format {};
  LDR_PTR(vkGetPhysicalDeviceFormatProperties)(g_vexa_physical_device, info.imageFormat, &format);
  constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  return (format.optimalTilingFeatures & needed) == needed ? mode : -1;
}

static bool VexaRotCreate(VkDevice device, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& guest, VkExtent2D real, int32_t mode) {
  auto rot = std::make_unique<VexaRotSwapchain>();
  rot->Device = device;
  rot->Guest = guest.imageExtent;
  rot->Real = real;
  rot->Mode = mode;
  if (!rot->Init(swapchain, guest)) return false;
  fprintf(stderr, "[VEXA][VK] pre-rotation mode=%d guest %ux%u -> swapchain %ux%u images=%zu\n", mode, guest.imageExtent.width,
          guest.imageExtent.height, real.width, real.height, rot->Images.size());
  std::lock_guard lock {VexaRotMutex};
  VexaRotSwapchains[swapchain] = std::move(rot);
  return true;
}

static void VexaRotDestroy(VkSwapchainKHR swapchain) {
  std::unique_ptr<VexaRotSwapchain> rot;
  {
    std::lock_guard lock {VexaRotMutex};
    const auto it = VexaRotSwapchains.find(swapchain);
    if (it == VexaRotSwapchains.end()) return;
    rot = std::move(it->second);
    VexaRotSwapchains.erase(it);
  }
}

static bool VexaRotImages(VkSwapchainKHR swapchain, uint32_t* count, VkImage* images, VkResult& result) {
  std::lock_guard lock {VexaRotMutex};
  const auto it = VexaRotSwapchains.find(swapchain);
  if (it == VexaRotSwapchains.end()) return false;
  const auto& own = it->second->Images;
  const uint32_t total = own.size();
  if (!images) {
    *count = total;
    result = VK_SUCCESS;
    return true;
  }
  const uint32_t written = std::min(*count, total);
  std::copy_n(own.begin(), written, images);
  *count = written;
  result = written < total ? VK_INCOMPLETE : VK_SUCCESS;
  return true;
}

// Submits the rotation draws for every rotated swapchain in the present and
// returns their completion semaphores, which replace the guest's waits.
static VkResult VexaRotSubmit(VkQueue queue, const VkPresentInfoKHR& info, std::vector<VkSemaphore>& waits) {
  std::vector<std::pair<VexaRotSwapchain*, uint32_t>> work;
  {
    std::lock_guard lock {VexaRotMutex};
    if (VexaRotSwapchains.empty()) return VK_SUCCESS;
    for (uint32_t i = 0; i < info.swapchainCount; ++i) {
      if (const auto it = VexaRotSwapchains.find(info.pSwapchains[i]); it != VexaRotSwapchains.end()) {
        work.emplace_back(it->second.get(), info.pImageIndices[i]);
      }
    }
  }
  if (work.empty()) return VK_SUCCESS;
  const std::vector<VkPipelineStageFlags> stages(info.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  bool first = true;
  for (const auto& [rot, index] : work) {
    if (!rot->Recorded && !rot->Record(queue)) {
      // Let the guest rebuild its swapchain on the unrotated path.
      VexaRotFailed = true;
      fprintf(stderr, "[VEXA][VK] pre-rotation disabled: command recording failed\n");
      return VK_ERROR_OUT_OF_DATE_KHR;
    }
    if (index >= rot->Commands.size()) return VK_ERROR_OUT_OF_DATE_KHR;
    auto& fns = rot->Fns;
    fns.vkWaitForFences(rot->Device, 1, &rot->Fences[index], VK_TRUE, UINT64_MAX);
    fns.vkResetFences(rot->Device, 1, &rot->Fences[index]);
    VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (first) {
      submit.waitSemaphoreCount = info.waitSemaphoreCount;
      submit.pWaitSemaphores = info.pWaitSemaphores;
      submit.pWaitDstStageMask = stages.data();
    }
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &rot->Commands[index];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &rot->Done[index];
    const VkResult result = fns.vkQueueSubmit(queue, 1, &submit, rot->Fences[index]);
    if (result != VK_SUCCESS) {
      rot->Lost = true;
      return result;
    }
    first = false;
    waits.push_back(rot->Done[index]);
  }
  return VK_SUCCESS;
}
