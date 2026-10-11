// Generated from SDL_gpu.h: SDL GPU is not available on the Vexa Android bridge.
// These stubs let guests that import the GPU API (FNA3D) load, then fail gracefully at device creation.

extern "C" SDL_GPUCommandBuffer * SDL_AcquireGPUCommandBuffer(SDL_GPUDevice *device) { return nullptr; }
extern "C" SDL_GPUCopyPass * SDL_BeginGPUCopyPass(SDL_GPUCommandBuffer *command_buffer) { return nullptr; }
extern "C" SDL_GPURenderPass * SDL_BeginGPURenderPass(SDL_GPUCommandBuffer *command_buffer, const SDL_GPUColorTargetInfo *color_target_infos, Uint32 num_color_targets, const SDL_GPUDepthStencilTargetInfo *depth_stencil_target_info) { return nullptr; }
extern "C" void SDL_BindGPUFragmentSamplers(SDL_GPURenderPass *render_pass, Uint32 first_slot, const SDL_GPUTextureSamplerBinding *texture_sampler_bindings, Uint32 num_bindings) { }
extern "C" void SDL_BindGPUGraphicsPipeline(SDL_GPURenderPass *render_pass, SDL_GPUGraphicsPipeline *graphics_pipeline) { }
extern "C" void SDL_BindGPUIndexBuffer(SDL_GPURenderPass *render_pass, const SDL_GPUBufferBinding *binding, SDL_GPUIndexElementSize index_element_size) { }
extern "C" void SDL_BindGPUVertexBuffers(SDL_GPURenderPass *render_pass, Uint32 first_slot, const SDL_GPUBufferBinding *bindings, Uint32 num_bindings) { }
extern "C" void SDL_BindGPUVertexSamplers(SDL_GPURenderPass *render_pass, Uint32 first_slot, const SDL_GPUTextureSamplerBinding *texture_sampler_bindings, Uint32 num_bindings) { }
extern "C" void SDL_BlitGPUTexture(SDL_GPUCommandBuffer *command_buffer, const SDL_GPUBlitInfo *info) { }
extern "C" bool SDL_CancelGPUCommandBuffer(SDL_GPUCommandBuffer *command_buffer) { return false; }
extern "C" bool SDL_ClaimWindowForGPUDevice(SDL_GPUDevice *device, SDL_Window *window) { return false; }
extern "C" SDL_GPUBuffer * SDL_CreateGPUBuffer(SDL_GPUDevice *device, const SDL_GPUBufferCreateInfo *createinfo) { return nullptr; }
extern "C" SDL_GPUDevice * SDL_CreateGPUDeviceWithProperties(SDL_PropertiesID props) { SDL_SetError("SDL GPU is not supported by the Vexa SDL3 bridge"); return nullptr; }
extern "C" SDL_GPUGraphicsPipeline * SDL_CreateGPUGraphicsPipeline(SDL_GPUDevice *device, const SDL_GPUGraphicsPipelineCreateInfo *createinfo) { return nullptr; }
extern "C" SDL_GPUSampler * SDL_CreateGPUSampler(SDL_GPUDevice *device, const SDL_GPUSamplerCreateInfo *createinfo) { return nullptr; }
extern "C" SDL_GPUShader * SDL_CreateGPUShader(SDL_GPUDevice *device, const SDL_GPUShaderCreateInfo *createinfo) { return nullptr; }
extern "C" SDL_GPUTexture * SDL_CreateGPUTexture(SDL_GPUDevice *device, const SDL_GPUTextureCreateInfo *createinfo) { return nullptr; }
extern "C" SDL_GPUTransferBuffer * SDL_CreateGPUTransferBuffer(SDL_GPUDevice *device, const SDL_GPUTransferBufferCreateInfo *createinfo) { return nullptr; }
extern "C" void SDL_DestroyGPUDevice(SDL_GPUDevice *device) { }
extern "C" void SDL_DownloadFromGPUBuffer(SDL_GPUCopyPass *copy_pass, const SDL_GPUBufferRegion *source, const SDL_GPUTransferBufferLocation *destination) { }
extern "C" void SDL_DownloadFromGPUTexture(SDL_GPUCopyPass *copy_pass, const SDL_GPUTextureRegion *source, const SDL_GPUTextureTransferInfo *destination) { }
extern "C" void SDL_DrawGPUIndexedPrimitives(SDL_GPURenderPass *render_pass, Uint32 num_indices, Uint32 num_instances, Uint32 first_index, Sint32 vertex_offset, Uint32 first_instance) { }
extern "C" void SDL_DrawGPUPrimitives(SDL_GPURenderPass *render_pass, Uint32 num_vertices, Uint32 num_instances, Uint32 first_vertex, Uint32 first_instance) { }
extern "C" void SDL_EndGPUCopyPass(SDL_GPUCopyPass *copy_pass) { }
extern "C" void SDL_EndGPURenderPass(SDL_GPURenderPass *render_pass) { }
extern "C" bool SDL_GPUSupportsProperties(SDL_PropertiesID props) { return false; }
extern "C" Uint32 SDL_GPUTextureFormatTexelBlockSize(SDL_GPUTextureFormat format) { return 0; }
extern "C" bool SDL_GPUTextureSupportsFormat(SDL_GPUDevice *device, SDL_GPUTextureFormat format, SDL_GPUTextureType type, SDL_GPUTextureUsageFlags usage) { return false; }
extern "C" bool SDL_GPUTextureSupportsSampleCount(SDL_GPUDevice *device, SDL_GPUTextureFormat format, SDL_GPUSampleCount sample_count) { return false; }
extern "C" void SDL_GenerateMipmapsForGPUTexture(SDL_GPUCommandBuffer *command_buffer, SDL_GPUTexture *texture) { }
extern "C" SDL_GPUShaderFormat SDL_GetGPUShaderFormats(SDL_GPUDevice *device) { return 0; }
extern "C" void SDL_InsertGPUDebugLabel(SDL_GPUCommandBuffer *command_buffer, const char *text) { }
extern "C" void * SDL_MapGPUTransferBuffer(SDL_GPUDevice *device, SDL_GPUTransferBuffer *transfer_buffer, bool cycle) { return nullptr; }
extern "C" void SDL_PushGPUFragmentUniformData(SDL_GPUCommandBuffer *command_buffer, Uint32 slot_index, const void *data, Uint32 length) { }
extern "C" void SDL_PushGPUVertexUniformData(SDL_GPUCommandBuffer *command_buffer, Uint32 slot_index, const void *data, Uint32 length) { }
extern "C" void SDL_ReleaseGPUBuffer(SDL_GPUDevice *device, SDL_GPUBuffer *buffer) { }
extern "C" void SDL_ReleaseGPUFence(SDL_GPUDevice *device, SDL_GPUFence *fence) { }
extern "C" void SDL_ReleaseGPUGraphicsPipeline(SDL_GPUDevice *device, SDL_GPUGraphicsPipeline *graphics_pipeline) { }
extern "C" void SDL_ReleaseGPUSampler(SDL_GPUDevice *device, SDL_GPUSampler *sampler) { }
extern "C" void SDL_ReleaseGPUShader(SDL_GPUDevice *device, SDL_GPUShader *shader) { }
extern "C" void SDL_ReleaseGPUTexture(SDL_GPUDevice *device, SDL_GPUTexture *texture) { }
extern "C" void SDL_ReleaseGPUTransferBuffer(SDL_GPUDevice *device, SDL_GPUTransferBuffer *transfer_buffer) { }
extern "C" void SDL_SetGPUBlendConstants(SDL_GPURenderPass *render_pass, SDL_FColor blend_constants) { }
extern "C" void SDL_SetGPUScissor(SDL_GPURenderPass *render_pass, const SDL_Rect *scissor) { }
extern "C" void SDL_SetGPUStencilReference(SDL_GPURenderPass *render_pass, Uint8 reference) { }
extern "C" bool SDL_SetGPUSwapchainParameters(SDL_GPUDevice *device, SDL_Window *window, SDL_GPUSwapchainComposition swapchain_composition, SDL_GPUPresentMode present_mode) { return false; }
extern "C" void SDL_SetGPUTextureName(SDL_GPUDevice *device, SDL_GPUTexture *texture, const char *text) { }
extern "C" void SDL_SetGPUViewport(SDL_GPURenderPass *render_pass, const SDL_GPUViewport *viewport) { }
extern "C" bool SDL_SubmitGPUCommandBuffer(SDL_GPUCommandBuffer *command_buffer) { return false; }
extern "C" SDL_GPUFence * SDL_SubmitGPUCommandBufferAndAcquireFence(SDL_GPUCommandBuffer *command_buffer) { return nullptr; }
extern "C" void SDL_UnmapGPUTransferBuffer(SDL_GPUDevice *device, SDL_GPUTransferBuffer *transfer_buffer) { }
extern "C" void SDL_UploadToGPUBuffer(SDL_GPUCopyPass *copy_pass, const SDL_GPUTransferBufferLocation *source, const SDL_GPUBufferRegion *destination, bool cycle) { }
extern "C" void SDL_UploadToGPUTexture(SDL_GPUCopyPass *copy_pass, const SDL_GPUTextureTransferInfo *source, const SDL_GPUTextureRegion *destination, bool cycle) { }
extern "C" bool SDL_WaitAndAcquireGPUSwapchainTexture(SDL_GPUCommandBuffer *command_buffer, SDL_Window *window, SDL_GPUTexture **swapchain_texture, Uint32 *swapchain_texture_width, Uint32 *swapchain_texture_height) { return false; }
extern "C" bool SDL_WaitForGPUFences(SDL_GPUDevice *device, bool wait_all, SDL_GPUFence *const *fences, Uint32 num_fences) { return false; }
extern "C" bool SDL_WaitForGPUIdle(SDL_GPUDevice *device) { return false; }
extern "C" bool SDL_WindowSupportsGPUPresentMode(SDL_GPUDevice *device, SDL_Window *window, SDL_GPUPresentMode present_mode) { return false; }
