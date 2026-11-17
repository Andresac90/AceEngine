// Vulkan backend (Android, Linux, Windows; macOS through MoltenVK for testing).
//
// Targets Vulkan 1.1 with classic render passes so it runs on older Android
// devices. Conventions are made to match Metal/D3D: the viewport is flipped
// (negative height) so +Y is up in clip space on every backend.
//
// Image layouts between passes: sampled render targets rest in
// SHADER_READ_ONLY_OPTIMAL, the backbuffer in PRESENT_SRC, depth in
// DEPTH_STENCIL_ATTACHMENT_OPTIMAL. Render passes do the transitions.
#include "rhi/rhi.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <map>
#include <vulkan/vulkan.h>

namespace ace::rhi {
namespace {

#define VK_CHECK(expr)                                                         \
  do {                                                                         \
    VkResult vk_result_ = (expr);                                              \
    ACE_ASSERT(vk_result_ == VK_SUCCESS, "%s failed: VkResult %d", #expr, (int)vk_result_); \
  } while (0)

VkFormat toVk(Format f) {
  switch (f) {
    case Format::R8: return VK_FORMAT_R8_UNORM;
    case Format::R32F: return VK_FORMAT_R32_SFLOAT;
    case Format::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::BGRA8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::D32F: return VK_FORMAT_D32_SFLOAT;
    case Format::Undefined: break;
  }
  return VK_FORMAT_UNDEFINED;
}

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  u64 size = 0;
};
struct Texture {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE; // null for swapchain images
  VkImageView view = VK_NULL_HANDLE;
  TextureDesc desc;
  bool isBackbuffer = false;
};
struct Layout {
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  BindGroupLayoutDesc desc;
  u32 dynamicCount = 0;
};
struct BindGroup {
  VkDescriptorSet set = VK_NULL_HANDLE;
  u32 dynamicCount = 0;
};
struct Pipeline {
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
};

// Identifies a render pass object: attachment formats plus where each
// attachment's contents come from (which fixes the initial layout).
struct PassKey {
  VkFormat color[kMaxColorTargets] = {};
  u8 colorLoad[kMaxColorTargets] = {};
  u8 colorIsBackbuffer[kMaxColorTargets] = {};
  u32 colorCount = 0;
  VkFormat depth = VK_FORMAT_UNDEFINED;
  u8 depthLoad = 0;
  bool operator<(const PassKey& o) const { return memcmp(this, &o, sizeof(PassKey)) < 0; }
};

class VulkanDevice;

class VulkanCommandList final : public CommandList {
public:
  explicit VulkanCommandList(VulkanDevice& device) : dev_(device) {}
  void beginRenderPass(const RenderPassDesc& desc) override;
  void endRenderPass() override;
  void setViewport(f32 x, f32 y, f32 w, f32 h) override;
  void setScissor(i32 x, i32 y, u32 w, u32 h) override;
  void setPipeline(PipelineHandle pipeline) override;
  void setBindGroup(u32 index, BindGroupHandle group, std::span<const u32> dynamicOffsets) override;
  void setVertexBuffer(BufferHandle buffer, u64 offset) override;
  void setIndexBuffer(BufferHandle buffer, bool index32, u64 offset) override;
  void draw(u32 vertexCount, u32 firstVertex) override;
  void drawIndexed(u32 indexCount, u32 firstIndex, i32 baseVertex) override;

  VkCommandBuffer cmd = VK_NULL_HANDLE;

private:
  VulkanDevice& dev_;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  u32 passWidth_ = 0, passHeight_ = 0;
};

class VulkanDevice final : public Device {
public:
  explicit VulkanDevice(const DeviceDesc& desc) : list_(*this), window_(desc.window) {
    createInstance(desc.validation);
    ACE_ASSERT(SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_), "SDL_Vulkan_CreateSurface: %s", SDL_GetError());
    pickPhysicalDevice();
    createDevice();

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queueFamily_;
    VK_CHECK(vkCreateCommandPool(device_, &pool, nullptr, &commandPool_));
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = commandPool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device_, &alloc, &list_.cmd));

    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(device_, &fence, nullptr, &frameFence_));
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(device_, &sem, nullptr, &acquired_));

    // shortcut: one fixed-size descriptor pool; grow by chaining pools if a
    // project ever exhausts it (allocation failure asserts with a clear message).
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4096},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, 8192}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = 8192;
    dp.poolSizeCount = 4;
    dp.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(device_, &dp, nullptr, &descriptorPool_));

    VkDescriptorSetLayoutCreateInfo empty{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &empty, nullptr, &emptyLayout_));

    backbuffer_.id = textures_.add(Texture{});
    createSwapchain();
  }

  ~VulkanDevice() override {
    vkDeviceWaitIdle(device_);
    flushDeletions();
    destroySwapchain();
    // Release whatever the application still holds (freed slots are null handles,
    // which Vulkan ignores).
    pipelines_.each([&](Pipeline& p) {
      vkDestroyPipeline(device_, p.pipeline, nullptr);
      vkDestroyPipelineLayout(device_, p.layout, nullptr);
    });
    layouts_.each([&](Layout& l) { vkDestroyDescriptorSetLayout(device_, l.layout, nullptr); });
    shaders_.each([&](VkShaderModule& m) { vkDestroyShaderModule(device_, m, nullptr); });
    samplers_.each([&](VkSampler& s) { vkDestroySampler(device_, s, nullptr); });
    buffers_.each([&](Buffer& b) {
      vkDestroyBuffer(device_, b.buffer, nullptr);
      vkFreeMemory(device_, b.memory, nullptr);
    });
    textures_.each([&](Texture& t) {
      if (t.isBackbuffer) return; // owned by the swapchain
      vkDestroyImageView(device_, t.view, nullptr);
      vkDestroyImage(device_, t.image, nullptr);
      vkFreeMemory(device_, t.memory, nullptr);
    });
    for (auto& [key, pass] : passes_) vkDestroyRenderPass(device_, pass, nullptr);
    vkDestroyDescriptorSetLayout(device_, emptyLayout_, nullptr);
    vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    vkDestroySemaphore(device_, acquired_, nullptr);
    vkDestroyFence(device_, frameFence_, nullptr);
    vkDestroyCommandPool(device_, commandPool_, nullptr);
    vkDestroyDevice(device_, nullptr);
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    vkDestroyInstance(instance_, nullptr);
  }

  Backend backend() const override { return Backend::Vulkan; }

  // ------------------------------------------------------------ resources

  BufferHandle createBuffer(const BufferDesc& desc) override {
    Buffer b;
    b.size = std::max<u64>(desc.size, 4);
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = b.size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (desc.usage & BufferUsage_Vertex) info.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (desc.usage & BufferUsage_Index) info.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (desc.usage & BufferUsage_Uniform) info.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (desc.usage & BufferUsage_Storage) info.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VK_CHECK(vkCreateBuffer(device_, &info, nullptr, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.buffer, &req);
    b.memory = allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkBindBufferMemory(device_, b.buffer, b.memory, 0));
    VK_CHECK(vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    return {buffers_.add(std::move(b))};
  }

  TextureHandle createTexture(const TextureDesc& desc) override {
    Texture t;
    t.desc = desc;
    bool depth = isDepthFormat(desc.format);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = toVk(desc.format);
    info.extent = {desc.width, desc.height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!depth) info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc.usage & TextureUsage_RenderTarget) info.usage |= depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VK_CHECK(vkCreateImage(device_, &info, nullptr, &t.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, t.image, &req);
    t.memory = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkBindImageMemory(device_, t.image, t.memory, 0));
    t.view = createView(t.image, info.format, depth);
    // Put the image in its resting layout so it is always legal to sample or load.
    if (!depth) {
      VkCommandBuffer cmd = beginOneShot();
      transition(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      endOneShot(cmd);
    }
    return {textures_.add(std::move(t))};
  }

  SamplerHandle createSampler(const SamplerDesc& desc) override {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = info.minFilter = desc.filter == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = info.addressModeV = info.addressModeW =
        desc.address == AddressMode::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler s;
    VK_CHECK(vkCreateSampler(device_, &info, nullptr, &s));
    return {samplers_.add(std::move(s))};
  }

  ShaderHandle createShader(const ShaderDesc& desc) override {
    ACE_ASSERT(!desc.spirv.empty() && desc.spirv.size() % 4 == 0, "shader %s has no SPIR-V", desc.name);
    std::vector<u32> words(desc.spirv.size() / 4); // guarantees the alignment Vulkan requires
    memcpy(words.data(), desc.spirv.data(), desc.spirv.size());
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = desc.spirv.size();
    info.pCode = words.data();
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(device_, &info, nullptr, &m));
    return {shaders_.add(std::move(m))};
  }

  BindGroupLayoutHandle createBindGroupLayout(const BindGroupLayoutDesc& desc) override {
    Layout l;
    l.desc = desc;
    VkDescriptorSetLayoutBinding bindings[kMaxBindings];
    u32 count = 0;
    for (u32 b = 0; b < kMaxBindings; b++) {
      if (desc.bindings[b] == BindingType::None) continue;
      VkDescriptorSetLayoutBinding& vb = bindings[count++];
      vb = {};
      vb.binding = b;
      vb.descriptorType = descriptorType(desc.bindings[b]);
      vb.descriptorCount = 1;
      vb.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      if (desc.bindings[b] == BindingType::UniformBuffer) l.dynamicCount++;
    }
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = count;
    info.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &info, nullptr, &l.layout));
    return {layouts_.add(std::move(l))};
  }

  BindGroupHandle createBindGroup(const BindGroupDesc& desc) override {
    Layout& layout = layouts_.get(desc.layout.id);
    BindGroup g;
    g.dynamicCount = layout.dynamicCount;
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = descriptorPool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &layout.layout;
    VkResult r = vkAllocateDescriptorSets(device_, &alloc, &g.set);
    ACE_ASSERT(r == VK_SUCCESS, "descriptor pool exhausted (VkResult %d): too many live bind groups", (int)r);

    VkWriteDescriptorSet writes[kMaxBindings];
    VkDescriptorBufferInfo buffers[kMaxBindings];
    VkDescriptorImageInfo images[kMaxBindings];
    u32 count = 0;
    for (u32 b = 0; b < kMaxBindings; b++) {
      BindingType type = layout.desc.bindings[b];
      if (type == BindingType::None) continue;
      const BindGroupEntry& e = desc.entries[b];
      VkWriteDescriptorSet& w = writes[count];
      w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = g.set;
      w.dstBinding = b;
      w.descriptorCount = 1;
      w.descriptorType = descriptorType(type);
      if (type == BindingType::UniformBuffer || type == BindingType::StorageBuffer) {
        buffers[count] = {buffers_.get(e.buffer.id).buffer, e.offset, e.size ? e.size : VK_WHOLE_SIZE};
        w.pBufferInfo = &buffers[count];
      } else if (type == BindingType::Texture) {
        images[count] = {VK_NULL_HANDLE, textures_.get(e.texture.id).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        w.pImageInfo = &images[count];
      } else {
        images[count] = {samplers_.get(e.sampler.id), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        w.pImageInfo = &images[count];
      }
      count++;
    }
    vkUpdateDescriptorSets(device_, count, writes, 0, nullptr);
    return {groups_.add(std::move(g))};
  }

  PipelineHandle createPipeline(const PipelineDesc& desc) override {
    Pipeline p;
    VkDescriptorSetLayout sets[kMaxBindGroups];
    u32 setCount = 0;
    for (u32 g = 0; g < kMaxBindGroups; g++) {
      sets[g] = desc.bindGroupLayouts[g] ? layouts_.get(desc.bindGroupLayouts[g].id).layout : emptyLayout_;
      if (desc.bindGroupLayouts[g]) setCount = g + 1;
    }
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = setCount;
    li.pSetLayouts = sets;
    VK_CHECK(vkCreatePipelineLayout(device_, &li, nullptr, &p.layout));

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = shaders_.get(desc.vertexShader.id);
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shaders_.get(desc.fragmentShader.id);

    VkVertexInputBindingDescription binding{0, desc.vertexStride, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[kMaxVertexAttributes];
    static const VkFormat formats[] = {VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM};
    for (u32 i = 0; i < desc.attributeCount; i++)
      attrs[i] = {desc.attributes[i].location, 0, formats[(int)desc.attributes[i].format], desc.attributes[i].offset};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (desc.attributeCount) {
      vi.vertexBindingDescriptionCount = 1;
      vi.pVertexBindingDescriptions = &binding;
      vi.vertexAttributeDescriptionCount = desc.attributeCount;
      vi.pVertexAttributeDescriptions = attrs;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = desc.cull == CullMode::None ? VK_CULL_MODE_NONE : desc.cull == CullMode::Back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // matches Metal because the viewport is flipped
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (desc.depthFormat != Format::Undefined) {
      static const VkCompareOp ops[] = {VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_EQUAL};
      ds.depthTestEnable = VK_TRUE;
      ds.depthWriteEnable = desc.depthWrite;
      ds.depthCompareOp = ops[(int)desc.depthCompare];
    }
    VkPipelineColorBlendAttachmentState blends[kMaxColorTargets] = {};
    for (u32 i = 0; i < desc.colorCount; i++) {
      VkPipelineColorBlendAttachmentState& b = blends[i];
      b.colorWriteMask = 0xF;
      if (desc.blend[i] != BlendMode::Opaque) {
        VkBlendFactor dst = desc.blend[i] == BlendMode::Alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
        b.blendEnable = VK_TRUE;
        b.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        b.dstColorBlendFactor = dst;
        b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        b.dstAlphaBlendFactor = dst;
        b.colorBlendOp = b.alphaBlendOp = VK_BLEND_OP_ADD;
      }
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = desc.colorCount;
    cb.pAttachments = blends;
    VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamics;

    // Any render pass with the same formats is compatible with this pipeline.
    PassKey key;
    key.colorCount = desc.colorCount;
    for (u32 i = 0; i < desc.colorCount; i++) key.color[i] = toVk(desc.colorFormats[i]);
    key.depth = toVk(desc.depthFormat);

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cb;
    info.pDynamicState = &dyn;
    info.layout = p.layout;
    info.renderPass = renderPass(key);
    VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &p.pipeline);
    if (r != VK_SUCCESS) {
      ACE_ERROR("pipeline %s: vkCreateGraphicsPipelines failed (VkResult %d)", desc.name, (int)r);
      vkDestroyPipelineLayout(device_, p.layout, nullptr);
      return {};
    }
    return {pipelines_.add(std::move(p))};
  }

  // Destruction waits until the frame that may still use the object has finished.
  void destroy(BufferHandle h) override {
    if (!h) return;
    Buffer b = buffers_.get(h.id);
    buffers_.remove(h.id);
    deletions_.push_back([this, b] {
      vkDestroyBuffer(device_, b.buffer, nullptr);
      vkFreeMemory(device_, b.memory, nullptr);
    });
  }
  void destroy(TextureHandle h) override {
    if (!h) return;
    Texture t = textures_.get(h.id);
    textures_.remove(h.id);
    deletions_.push_back([this, t] {
      vkDestroyImageView(device_, t.view, nullptr);
      vkDestroyImage(device_, t.image, nullptr);
      vkFreeMemory(device_, t.memory, nullptr);
    });
  }
  void destroy(SamplerHandle h) override {
    if (!h) return;
    VkSampler s = samplers_.get(h.id);
    samplers_.remove(h.id);
    deletions_.push_back([this, s] { vkDestroySampler(device_, s, nullptr); });
  }
  void destroy(ShaderHandle h) override {
    if (!h) return;
    VkShaderModule m = shaders_.get(h.id);
    shaders_.remove(h.id);
    deletions_.push_back([this, m] { vkDestroyShaderModule(device_, m, nullptr); });
  }
  void destroy(BindGroupLayoutHandle h) override {
    if (!h) return;
    VkDescriptorSetLayout l = layouts_.get(h.id).layout;
    layouts_.remove(h.id);
    deletions_.push_back([this, l] { vkDestroyDescriptorSetLayout(device_, l, nullptr); });
  }
  void destroy(BindGroupHandle h) override {
    if (!h) return;
    VkDescriptorSet s = groups_.get(h.id).set;
    groups_.remove(h.id);
    deletions_.push_back([this, s] { vkFreeDescriptorSets(device_, descriptorPool_, 1, &s); });
  }
  void destroy(PipelineHandle h) override {
    if (!h) return;
    Pipeline p = pipelines_.get(h.id);
    pipelines_.remove(h.id);
    deletions_.push_back([this, p] {
      vkDestroyPipeline(device_, p.pipeline, nullptr);
      vkDestroyPipelineLayout(device_, p.layout, nullptr);
    });
  }

  void writeBuffer(BufferHandle buffer, u64 offset, const void* data, u64 size) override {
    Buffer& b = buffers_.get(buffer.id);
    ACE_ASSERT(offset + size <= b.size, "writeBuffer out of range");
    memcpy((u8*)b.mapped + offset, data, size);
  }

  void writeTexture(TextureHandle texture, const void* pixels) override {
    Texture& t = textures_.get(texture.id);
    u64 size = (u64)t.desc.width * t.desc.height * bytesPerPixel(t.desc.format);
    BufferHandle staging = createBuffer({size, 0, "staging"});
    writeBuffer(staging, 0, pixels, size);
    VkCommandBuffer cmd = beginOneShot();
    transition(cmd, t.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region = fullImage(t);
    vkCmdCopyBufferToImage(cmd, buffers_.get(staging.id).buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transition(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    endOneShot(cmd);
    destroy(staging);
  }

  bool readTexture(TextureHandle texture, std::vector<u8>& out) override {
    Texture& t = textures_.get(texture.id);
    if (!t.image || t.isBackbuffer || isDepthFormat(t.desc.format)) return false;
    u64 size = (u64)t.desc.width * t.desc.height * bytesPerPixel(t.desc.format);
    BufferHandle staging = createBuffer({size, 0, "readback"});
    VkCommandBuffer cmd = beginOneShot();
    transition(cmd, t.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region = fullImage(t);
    vkCmdCopyImageToBuffer(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffers_.get(staging.id).buffer, 1, &region);
    transition(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    endOneShot(cmd);
    u8* p = (u8*)buffers_.get(staging.id).mapped;
    out.assign(p, p + size);
    destroy(staging);
    return true;
  }

  // ------------------------------------------------------------ frame

  CommandList* beginFrame(bool present) override {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (w <= 0 || h <= 0) return nullptr;
    VK_CHECK(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, UINT64_MAX)); // one frame in flight
    flushDeletions();
    presenting_ = present;
    if (!present) { // offscreen frame: no swapchain image
      width_ = (u32)w;
      height_ = (u32)h;
      textures_.get(backbuffer_.id) = Texture{};
      VK_CHECK(vkResetCommandBuffer(list_.cmd, 0));
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      VK_CHECK(vkBeginCommandBuffer(list_.cmd, &begin));
      inFrame_ = true;
      return &list_;
    }
    if (!swapchain_ || (u32)w != width_ || (u32)h != height_) createSwapchain();
    if (!swapchain_) return nullptr;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &imageIndex_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
      createSwapchain();
      return nullptr; // try again next frame
    }
    ACE_ASSERT(r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR, "vkAcquireNextImageKHR: VkResult %d", (int)r);

    Texture& bb = textures_.get(backbuffer_.id);
    bb.image = swapImages_[imageIndex_];
    bb.view = swapViews_[imageIndex_];
    bb.isBackbuffer = true;
    bb.desc = TextureDesc{width_, height_, backbufferFormat_, TextureUsage_RenderTarget, "backbuffer"};

    VK_CHECK(vkResetCommandBuffer(list_.cmd, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(list_.cmd, &begin));
    inFrame_ = true;
    backbufferWritten_ = false;
    return &list_;
  }

  void endFrame() override {
    if (!presenting_) {
      VK_CHECK(vkEndCommandBuffer(list_.cmd));
      inFrame_ = false;
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &list_.cmd;
      VK_CHECK(vkResetFences(device_, 1, &frameFence_));
      VK_CHECK(vkQueueSubmit(queue_, 1, &submit, frameFence_));
      return;
    }
    ACE_ASSERT(backbufferWritten_, "a frame must render to the backbuffer before endFrame");
    VK_CHECK(vkEndCommandBuffer(list_.cmd));
    inFrame_ = false;
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquired_;
    submit.pWaitDstStageMask = &wait;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &list_.cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &rendered_[imageIndex_];
    VK_CHECK(vkResetFences(device_, 1, &frameFence_));
    VK_CHECK(vkQueueSubmit(queue_, 1, &submit, frameFence_));
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &rendered_[imageIndex_];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex_;
    VkResult r = vkQueuePresentKHR(queue_, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) width_ = 0; // forces a rebuild next frame
    else ACE_ASSERT(r == VK_SUCCESS, "vkQueuePresentKHR: VkResult %d", (int)r);
  }

  TextureHandle backbuffer() override { return backbuffer_; }
  Format backbufferFormat() const override { return backbufferFormat_; }
  u32 backbufferWidth() const override { return width_; }
  u32 backbufferHeight() const override { return height_; }

  void waitIdle() override {
    if (inFrame_) return; // commands of this frame are not submitted yet; nothing older is running
    vkDeviceWaitIdle(device_);
    flushDeletions();
  }

  // ------------------------------------------------------------ internals

  VkRenderPass renderPass(const PassKey& key) {
    auto it = passes_.find(key);
    if (it != passes_.end()) return it->second;
    VkAttachmentDescription atts[kMaxColorTargets + 1] = {};
    VkAttachmentReference colorRefs[kMaxColorTargets], depthRef{};
    u32 n = 0;
    for (u32 i = 0; i < key.colorCount; i++) {
      VkImageLayout rest = key.colorIsBackbuffer[i] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      atts[n].format = key.color[i];
      atts[n].samples = VK_SAMPLE_COUNT_1_BIT;
      atts[n].loadOp = key.colorLoad[i] ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
      atts[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      atts[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      atts[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      atts[n].initialLayout = key.colorLoad[i] ? rest : VK_IMAGE_LAYOUT_UNDEFINED;
      atts[n].finalLayout = rest;
      colorRefs[i] = {n, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      n++;
    }
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = key.colorCount;
    sub.pColorAttachments = colorRefs;
    if (key.depth != VK_FORMAT_UNDEFINED) {
      atts[n].format = key.depth;
      atts[n].samples = VK_SAMPLE_COUNT_1_BIT;
      atts[n].loadOp = key.depthLoad ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
      atts[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      atts[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      atts[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      atts[n].initialLayout = key.depthLoad ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
      atts[n].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      depthRef = {n, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
      sub.pDepthStencilAttachment = &depthRef;
      n++;
    }
    // Sampling a target in a later pass must wait for this pass's writes, and vice versa.
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = n;
    info.pAttachments = atts;
    info.subpassCount = 1;
    info.pSubpasses = &sub;
    info.dependencyCount = 2;
    info.pDependencies = deps;
    VkRenderPass pass;
    VK_CHECK(vkCreateRenderPass(device_, &info, nullptr, &pass));
    return passes_[key] = pass;
  }

  VkDevice device_ = VK_NULL_HANDLE;
  Pool<Buffer> buffers_;
  Pool<Texture> textures_;
  Pool<VkSampler> samplers_;
  Pool<VkShaderModule> shaders_;
  Pool<Layout> layouts_;
  Pool<BindGroup> groups_;
  Pool<Pipeline> pipelines_;
  std::vector<std::function<void()>> deletions_;
  TextureHandle backbuffer_;
  bool backbufferWritten_ = false;
  bool presenting_ = true;

private:
  static VkDescriptorType descriptorType(BindingType t) {
    switch (t) {
      case BindingType::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      case BindingType::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      case BindingType::Texture: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      default: return VK_DESCRIPTOR_TYPE_SAMPLER;
    }
  }

  static VkBufferImageCopy fullImage(const Texture& t) {
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {t.desc.width, t.desc.height, 1};
    return region;
  }

  void flushDeletions() {
    // Deleters may queue more work (never do today, but stay safe): swap first.
    std::vector<std::function<void()>> pending;
    pending.swap(deletions_);
    for (auto& fn : pending) fn();
  }

  void createInstance(bool validation) {
    u32 sdlCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlCount);
    ACE_ASSERT(sdlExts, "SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
    std::vector<const char*> exts(sdlExts, sdlExts + sdlCount);
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
#ifdef __APPLE__
    exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME); // MoltenVK is a "portability" driver
    info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    std::vector<const char*> layers;
    if (validation) {
      u32 n = 0;
      vkEnumerateInstanceLayerProperties(&n, nullptr);
      std::vector<VkLayerProperties> available(n);
      vkEnumerateInstanceLayerProperties(&n, available.data());
      bool found = std::any_of(available.begin(), available.end(),
                               [](const VkLayerProperties& l) { return strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0; });
      if (found) layers.push_back("VK_LAYER_KHRONOS_validation");
      else ACE_WARN("Vulkan validation layer not installed; continuing without it");
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pEngineName = "AceEngine";
    app.apiVersion = VK_API_VERSION_1_1;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = (u32)exts.size();
    info.ppEnabledExtensionNames = exts.data();
    info.enabledLayerCount = (u32)layers.size();
    info.ppEnabledLayerNames = layers.data();
    VK_CHECK(vkCreateInstance(&info, nullptr, &instance_));
  }

  void pickPhysicalDevice() {
    u32 n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance_, &n, devices.data());
    int bestScore = -1;
    for (VkPhysicalDevice d : devices) {
      u32 qn = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
      std::vector<VkQueueFamilyProperties> queues(qn);
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, queues.data());
      for (u32 q = 0; q < qn; q++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(d, q, surface_, &present);
        if (!(queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
        if (score > bestScore) {
          bestScore = score;
          physical_ = d;
          queueFamily_ = q;
        }
        break;
      }
    }
    ACE_ASSERT(physical_, "no Vulkan device can render to this window");
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_, &props);
    ACE_INFO("Vulkan device: %s", props.deviceName);
  }

  void createDevice() {
    u32 n = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> available(n);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, available.data());
    std::vector<const char*> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    for (const VkExtensionProperties& e : available)
      if (strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) exts.push_back("VK_KHR_portability_subset"); // required when present

    VkPhysicalDeviceFeatures supported, enabled{};
    vkGetPhysicalDeviceFeatures(physical_, &supported);
    // Needed for a different blend mode per color target (path tracer accumulation).
    ACE_ASSERT(supported.independentBlend, "this GPU lacks independentBlend, which the renderer requires");
    enabled.independentBlend = VK_TRUE;

    // Slang's SV_VertexID lowering (fullscreen-triangle shaders) uses SPIR-V DrawParameters.
    VkPhysicalDeviceShaderDrawParametersFeatures drawParams{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &drawParams;
    vkGetPhysicalDeviceFeatures2(physical_, &features2);
    ACE_ASSERT(drawParams.shaderDrawParameters, "this GPU lacks shaderDrawParameters, which the engine shaders require");

    f32 priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = queueFamily_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = (u32)exts.size();
    info.ppEnabledExtensionNames = exts.data();
    info.pEnabledFeatures = &enabled;
    info.pNext = &drawParams;
    VK_CHECK(vkCreateDevice(physical_, &info, nullptr, &device_));
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    vkGetPhysicalDeviceMemoryProperties(physical_, &memoryProps_);
  }

  // shortcut: one VkDeviceMemory per resource. Drivers cap the allocation count
  // (often 4096); switch to a sub-allocator (e.g. VMA) before shipping big scenes.
  VkDeviceMemory allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags flags) {
    for (u32 i = 0; i < memoryProps_.memoryTypeCount; i++) {
      if (!(req.memoryTypeBits & (1u << i)) || (memoryProps_.memoryTypes[i].propertyFlags & flags) != flags) continue;
      VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      info.allocationSize = req.size;
      info.memoryTypeIndex = i;
      VkDeviceMemory memory;
      VkResult r = vkAllocateMemory(device_, &info, nullptr, &memory);
      ACE_ASSERT(r == VK_SUCCESS, "out of GPU memory allocating %llu bytes (VkResult %d)", (unsigned long long)req.size, (int)r);
      return memory;
    }
    ACE_ASSERT(false, "no suitable Vulkan memory type");
    return VK_NULL_HANDLE;
  }

  VkImageView createView(VkImage image, VkFormat format, bool depth) {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = {depth ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view;
    VK_CHECK(vkCreateImageView(device_, &info, nullptr, &view));
    return view;
  }

  VkCommandBuffer beginOneShot() {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = commandPool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(device_, &alloc, &cmd));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    return cmd;
  }

  // Blocks until done. Used for uploads and read-backs, which are rare.
  void endOneShot(VkCommandBuffer cmd) {
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue_));
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
  }

  void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // Uploads and read-backs are rare; conservative masks keep this simple and correct.
    b.srcAccessMask = b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  }

  void destroySwapchain() {
    for (VkImageView v : swapViews_) vkDestroyImageView(device_, v, nullptr);
    for (VkSemaphore sem : rendered_) vkDestroySemaphore(device_, sem, nullptr);
    rendered_.clear();
    swapViews_.clear();
    swapImages_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }

  void createSwapchain() {
    vkDeviceWaitIdle(device_);
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps));
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
      extent.width = std::clamp((u32)w, caps.minImageExtent.width, caps.maxImageExtent.width);
      extent.height = std::clamp((u32)h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
      destroySwapchain();
      return;
    }

    u32 n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, formats.data());
    VkSurfaceFormatKHR chosen{};
    for (const VkSurfaceFormatKHR& f : formats) {
      if (f.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
      if (f.format == VK_FORMAT_B8G8R8A8_SRGB || (f.format == VK_FORMAT_R8G8B8A8_SRGB && chosen.format == VK_FORMAT_UNDEFINED)) chosen = f;
    }
    ACE_ASSERT(chosen.format != VK_FORMAT_UNDEFINED, "the display offers no 8-bit sRGB surface format");
    backbufferFormat_ = chosen.format == VK_FORMAT_B8G8R8A8_SRGB ? Format::BGRA8_SRGB : Format::RGBA8_SRGB;

    VkSwapchainKHR old = swapchain_;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && info.minImageCount > caps.maxImageCount) info.minImageCount = caps.maxImageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // shortcut: identity transform; Android should pass caps.currentTransform and
    // pre-rotate in the projection to avoid a compositor blit on rotated devices.
    info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR; // always available; vsync
    info.clipped = VK_TRUE;
    info.oldSwapchain = old;
    VkSwapchainKHR created;
    VK_CHECK(vkCreateSwapchainKHR(device_, &info, nullptr, &created));
    destroySwapchain();
    swapchain_ = created;
    width_ = extent.width;
    height_ = extent.height;
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    swapImages_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapImages_.data());
    for (VkImage image : swapImages_) {
      swapViews_.push_back(createView(image, chosen.format, false));
      VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VkSemaphore s;
      VK_CHECK(vkCreateSemaphore(device_, &sem, nullptr, &s));
      rendered_.push_back(s);
    }
  }

  VulkanCommandList list_;
  SDL_Window* window_ = nullptr;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memoryProps_{};
  u32 queueFamily_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool commandPool_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout emptyLayout_ = VK_NULL_HANDLE;
  VkFence frameFence_ = VK_NULL_HANDLE;
  VkSemaphore acquired_ = VK_NULL_HANDLE;
  std::vector<VkSemaphore> rendered_; // one per swapchain image: a present may still hold it
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  std::vector<VkImage> swapImages_;
  std::vector<VkImageView> swapViews_;
  std::map<PassKey, VkRenderPass> passes_;
  Format backbufferFormat_ = Format::BGRA8_SRGB;
  u32 width_ = 0, height_ = 0, imageIndex_ = 0;
  bool inFrame_ = false;
};

void VulkanCommandList::beginRenderPass(const RenderPassDesc& desc) {
  PassKey key;
  key.colorCount = desc.colorCount;
  VkImageView views[kMaxColorTargets + 1];
  VkClearValue clears[kMaxColorTargets + 1] = {};
  u32 n = 0;
  for (u32 i = 0; i < desc.colorCount; i++) {
    Texture& t = dev_.textures_.get(desc.color[i].texture.id);
    key.color[i] = toVk(t.desc.format);
    key.colorLoad[i] = desc.color[i].load == LoadOp::Load;
    key.colorIsBackbuffer[i] = t.isBackbuffer;
    // The first pass of a frame that touches the backbuffer must clear it: its
    // contents (and layout) are undefined after acquire.
    if (t.isBackbuffer && !dev_.backbufferWritten_) key.colorLoad[i] = 0;
    if (t.isBackbuffer) dev_.backbufferWritten_ = true;
    memcpy(clears[n].color.float32, desc.color[i].clear, sizeof(f32) * 4);
    views[n++] = t.view;
    passWidth_ = t.desc.width;
    passHeight_ = t.desc.height;
  }
  if (desc.depth) {
    Texture& t = dev_.textures_.get(desc.depth.id);
    key.depth = toVk(t.desc.format);
    key.depthLoad = desc.depthLoad == LoadOp::Load;
    clears[n].depthStencil = {desc.depthClear, 0};
    views[n++] = t.view;
    passWidth_ = t.desc.width;
    passHeight_ = t.desc.height;
  }
  VkRenderPass pass = dev_.renderPass(key);

  // shortcut: a framebuffer object per pass per frame; cache by attachment set if
  // it ever shows up in a profile.
  VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fb.renderPass = pass;
  fb.attachmentCount = n;
  fb.pAttachments = views;
  fb.width = passWidth_;
  fb.height = passHeight_;
  fb.layers = 1;
  VkFramebuffer framebuffer;
  VK_CHECK(vkCreateFramebuffer(dev_.device_, &fb, nullptr, &framebuffer));
  VkDevice device = dev_.device_;
  dev_.deletions_.push_back([device, framebuffer] { vkDestroyFramebuffer(device, framebuffer, nullptr); });

  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = pass;
  begin.framebuffer = framebuffer;
  begin.renderArea = {{0, 0}, {passWidth_, passHeight_}};
  begin.clearValueCount = n;
  begin.pClearValues = clears;
  vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  setViewport(0, 0, (f32)passWidth_, (f32)passHeight_);
  setScissor(0, 0, passWidth_, passHeight_);
  layout_ = VK_NULL_HANDLE;
}

void VulkanCommandList::endRenderPass() { vkCmdEndRenderPass(cmd); }

void VulkanCommandList::setViewport(f32 x, f32 y, f32 w, f32 h) {
  VkViewport v{x, y + h, w, -h, 0.0f, 1.0f}; // flipped: +Y up, like Metal and D3D
  vkCmdSetViewport(cmd, 0, 1, &v);
}

void VulkanCommandList::setScissor(i32 x, i32 y, u32 w, u32 h) {
  i32 x0 = std::clamp(x, 0, (i32)passWidth_), y0 = std::clamp(y, 0, (i32)passHeight_);
  i32 x1 = std::clamp(x + (i32)w, x0, (i32)passWidth_), y1 = std::clamp(y + (i32)h, y0, (i32)passHeight_);
  VkRect2D r{{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}};
  vkCmdSetScissor(cmd, 0, 1, &r);
}

void VulkanCommandList::setPipeline(PipelineHandle pipeline) {
  Pipeline& p = dev_.pipelines_.get(pipeline.id);
  layout_ = p.layout;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
}

void VulkanCommandList::setBindGroup(u32 index, BindGroupHandle group, std::span<const u32> dynamicOffsets) {
  ACE_ASSERT(layout_, "setBindGroup must come after setPipeline");
  BindGroup& g = dev_.groups_.get(group.id);
  u32 offsets[kMaxBindings] = {}; // Vulkan needs exactly one offset per dynamic buffer
  for (u32 i = 0; i < g.dynamicCount && i < dynamicOffsets.size(); i++) offsets[i] = dynamicOffsets[i];
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, index, 1, &g.set, g.dynamicCount, offsets);
}

void VulkanCommandList::setVertexBuffer(BufferHandle buffer, u64 offset) {
  vkCmdBindVertexBuffers(cmd, 0, 1, &dev_.buffers_.get(buffer.id).buffer, &offset);
}

void VulkanCommandList::setIndexBuffer(BufferHandle buffer, bool index32, u64 offset) {
  vkCmdBindIndexBuffer(cmd, dev_.buffers_.get(buffer.id).buffer, offset, index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
}

void VulkanCommandList::draw(u32 vertexCount, u32 firstVertex) { vkCmdDraw(cmd, vertexCount, 1, firstVertex, 0); }

void VulkanCommandList::drawIndexed(u32 indexCount, u32 firstIndex, i32 baseVertex) {
  vkCmdDrawIndexed(cmd, indexCount, 1, firstIndex, baseVertex, 0);
}

} // namespace

std::unique_ptr<Device> createVulkanDevice(const DeviceDesc& desc) { return std::make_unique<VulkanDevice>(desc); }

} // namespace ace::rhi
