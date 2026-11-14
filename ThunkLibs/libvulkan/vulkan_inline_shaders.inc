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
  if (!VexaStageIsInline(stage)) return VK_SUCCESS;
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
