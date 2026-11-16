// Metal backend. Compiled with ARC (-fobjc-arc).
//
// Shaders arrive as SPIR-V and are transpiled to Metal Shading Language when a
// pipeline is created, because that is the first moment the bind-group layouts
// are known: Metal has flat per-stage binding tables, so each (group, binding)
// pair is assigned a compact buffer/texture/sampler index here.
#include "rhi/rhi.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <spirv_msl.hpp>

extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* pool);

namespace ace::rhi {
namespace {

constexpr u32 kVertexBufferIndex = 30; // last slot of Metal's buffer table; uniforms grow from 0

MTLPixelFormat toMTL(Format f) {
  switch (f) {
    case Format::R8: return MTLPixelFormatR8Unorm;
    case Format::R32F: return MTLPixelFormatR32Float;
    case Format::RGBA8: return MTLPixelFormatRGBA8Unorm;
    case Format::RGBA8_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
    case Format::BGRA8: return MTLPixelFormatBGRA8Unorm;
    case Format::BGRA8_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
    case Format::RGBA16F: return MTLPixelFormatRGBA16Float;
    case Format::RGBA32F: return MTLPixelFormatRGBA32Float;
    case Format::D32F: return MTLPixelFormatDepth32Float;
    case Format::Undefined: break;
  }
  return MTLPixelFormatInvalid;
}

struct Texture {
  id<MTLTexture> tex = nil;
  TextureDesc desc;
};
struct Shader {
  std::vector<u32> spirv;
  ShaderStage stage = ShaderStage::Vertex;
};
struct BindGroup {
  BindGroupDesc desc;
  BindGroupLayoutDesc layout;
};
struct Pipeline {
  id<MTLRenderPipelineState> state = nil;
  id<MTLDepthStencilState> depth = nil;
  MTLCullMode cull = MTLCullModeNone;
  u8 slot[kMaxBindGroups][kMaxBindings] = {}; // Metal table index of each binding
  BindingType type[kMaxBindGroups][kMaxBindings] = {};
};

class MetalDevice;

class MetalCommandList final : public CommandList {
public:
  explicit MetalCommandList(MetalDevice& device) : dev_(device) {}
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

  id<MTLCommandBuffer> cmd = nil;

private:
  MetalDevice& dev_;
  id<MTLRenderCommandEncoder> enc_ = nil;
  Pipeline* pipeline_ = nullptr;
  id<MTLBuffer> indexBuffer_ = nil;
  u64 indexOffset_ = 0;
  bool index32_ = true;
  u32 passWidth_ = 0, passHeight_ = 0;
};

class MetalDevice final : public Device {
public:
  explicit MetalDevice(const DeviceDesc& desc) : list_(*this), window_(desc.window) {
    device_ = MTLCreateSystemDefaultDevice();
    ACE_ASSERT(device_, "no Metal device");
    queue_ = [device_ newCommandQueue];
    view_ = SDL_Metal_CreateView(window_);
    layer_ = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view_);
    layer_.device = device_;
    layer_.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
    layer_.framebufferOnly = YES;
#if TARGET_OS_OSX
    layer_.displaySyncEnabled = desc.vsync;
#endif
    backbuffer_.id = textures_.add(Texture{});
    ACE_INFO("Metal device: %s", device_.name.UTF8String);
  }
  ~MetalDevice() override {
    waitIdle();
    SDL_Metal_DestroyView(view_);
  }

  Backend backend() const override { return Backend::Metal; }

  BufferHandle createBuffer(const BufferDesc& desc) override {
    id<MTLBuffer> b = [device_ newBufferWithLength:desc.size options:MTLResourceStorageModeShared];
    ACE_ASSERT(b, "buffer allocation of %llu bytes failed", desc.size);
    b.label = @(desc.name);
    return {buffers_.add(std::move(b))};
  }

  TextureHandle createTexture(const TextureDesc& desc) override {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:toMTL(desc.format)
                                                                                 width:desc.width
                                                                                height:desc.height
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    if (desc.usage & TextureUsage_RenderTarget) {
      d.usage |= MTLTextureUsageRenderTarget;
      d.storageMode = MTLStorageModePrivate;
    }
    Texture t;
    t.tex = [device_ newTextureWithDescriptor:d];
    ACE_ASSERT(t.tex, "texture allocation failed (%s)", desc.name);
    t.tex.label = @(desc.name);
    t.desc = desc;
    return {textures_.add(std::move(t))};
  }

  SamplerHandle createSampler(const SamplerDesc& desc) override {
    MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
    MTLSamplerMinMagFilter f = desc.filter == Filter::Linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    MTLSamplerAddressMode a = desc.address == AddressMode::Repeat ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
    d.minFilter = d.magFilter = f;
    d.sAddressMode = d.tAddressMode = d.rAddressMode = a;
    return {samplers_.add([device_ newSamplerStateWithDescriptor:d])};
  }

  ShaderHandle createShader(const ShaderDesc& desc) override {
    ACE_ASSERT(!desc.spirv.empty() && desc.spirv.size() % 4 == 0, "shader %s has no SPIR-V", desc.name);
    Shader s;
    s.stage = desc.stage;
    s.spirv.resize(desc.spirv.size() / 4);
    memcpy(s.spirv.data(), desc.spirv.data(), desc.spirv.size());
    return {shaders_.add(std::move(s))};
  }

  BindGroupLayoutHandle createBindGroupLayout(const BindGroupLayoutDesc& desc) override {
    return {layouts_.add(BindGroupLayoutDesc(desc))};
  }

  BindGroupHandle createBindGroup(const BindGroupDesc& desc) override {
    return {groups_.add(BindGroup{desc, layouts_.get(desc.layout.id)})};
  }

  PipelineHandle createPipeline(const PipelineDesc& desc) override {
    Pipeline p;
    // Assign flat Metal indices: one running counter per table, across all groups.
    u32 nextBuffer = 0, nextTexture = 0, nextSampler = 0;
    for (u32 g = 0; g < kMaxBindGroups; g++) {
      if (!desc.bindGroupLayouts[g]) continue;
      const BindGroupLayoutDesc& layout = layouts_.get(desc.bindGroupLayouts[g].id);
      for (u32 b = 0; b < kMaxBindings; b++) {
        p.type[g][b] = layout.bindings[b];
        switch (layout.bindings[b]) {
          case BindingType::UniformBuffer:
          case BindingType::StorageBuffer: p.slot[g][b] = (u8)nextBuffer++; break;
          case BindingType::Texture: p.slot[g][b] = (u8)nextTexture++; break;
          case BindingType::Sampler: p.slot[g][b] = (u8)nextSampler++; break;
          case BindingType::None: break;
        }
      }
    }
    ACE_ASSERT(nextBuffer < kVertexBufferIndex && nextSampler <= 16, "pipeline %s binds too many resources for Metal", desc.name);

    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.label = @(desc.name);
    d.vertexFunction = compile(shaders_.get(desc.vertexShader.id), p, desc.name);
    d.fragmentFunction = compile(shaders_.get(desc.fragmentShader.id), p, desc.name);
    if (!d.vertexFunction || !d.fragmentFunction) return {};

    if (desc.attributeCount) {
      MTLVertexDescriptor* vd = [MTLVertexDescriptor new];
      for (u32 i = 0; i < desc.attributeCount; i++) {
        const VertexAttribute& a = desc.attributes[i];
        static const MTLVertexFormat formats[] = {MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4,
                                                  MTLVertexFormatUChar4Normalized};
        vd.attributes[a.location].format = formats[(int)a.format];
        vd.attributes[a.location].offset = a.offset;
        vd.attributes[a.location].bufferIndex = kVertexBufferIndex;
      }
      vd.layouts[kVertexBufferIndex].stride = desc.vertexStride;
      d.vertexDescriptor = vd;
    }

    for (u32 i = 0; i < desc.colorCount; i++) {
      MTLRenderPipelineColorAttachmentDescriptor* c = d.colorAttachments[i];
      c.pixelFormat = toMTL(desc.colorFormats[i]);
      if (desc.blend[i] != BlendMode::Opaque) {
        c.blendingEnabled = YES;
        c.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        c.destinationRGBBlendFactor = desc.blend[i] == BlendMode::Alpha ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOne;
        c.sourceAlphaBlendFactor = MTLBlendFactorOne;
        c.destinationAlphaBlendFactor = desc.blend[i] == BlendMode::Alpha ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOne;
      }
    }
    d.depthAttachmentPixelFormat = toMTL(desc.depthFormat);

    NSError* error = nil;
    p.state = [device_ newRenderPipelineStateWithDescriptor:d error:&error];
    if (!p.state) {
      ACE_ERROR("pipeline %s: %s", desc.name, error.localizedDescription.UTF8String);
      return {};
    }

    MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
    if (desc.depthFormat != Format::Undefined) {
      static const MTLCompareFunction ops[] = {MTLCompareFunctionAlways, MTLCompareFunctionLess,
                                               MTLCompareFunctionLessEqual, MTLCompareFunctionEqual};
      ds.depthCompareFunction = ops[(int)desc.depthCompare];
      ds.depthWriteEnabled = desc.depthWrite;
    }
    p.depth = [device_ newDepthStencilStateWithDescriptor:ds];
    p.cull = desc.cull == CullMode::None ? MTLCullModeNone : desc.cull == CullMode::Back ? MTLCullModeBack : MTLCullModeFront;
    return {pipelines_.add(std::move(p))};
  }

  // ARC keeps a resource alive while a command buffer still references it, so
  // dropping our reference is already a safe deferred destroy.
  void destroy(BufferHandle h) override { buffers_.remove(h.id); }
  void destroy(TextureHandle h) override { textures_.remove(h.id); }
  void destroy(SamplerHandle h) override { samplers_.remove(h.id); }
  void destroy(ShaderHandle h) override { shaders_.remove(h.id); }
  void destroy(BindGroupLayoutHandle h) override { layouts_.remove(h.id); }
  void destroy(BindGroupHandle h) override { groups_.remove(h.id); }
  void destroy(PipelineHandle h) override { pipelines_.remove(h.id); }

  void writeBuffer(BufferHandle buffer, u64 offset, const void* data, u64 size) override {
    id<MTLBuffer> b = buffers_.get(buffer.id);
    ACE_ASSERT(offset + size <= b.length, "writeBuffer out of range");
    memcpy((u8*)b.contents + offset, data, size);
  }

  void writeTexture(TextureHandle texture, const void* pixels) override {
    Texture& t = textures_.get(texture.id);
    [t.tex replaceRegion:MTLRegionMake2D(0, 0, t.desc.width, t.desc.height)
             mipmapLevel:0
               withBytes:pixels
             bytesPerRow:t.desc.width * bytesPerPixel(t.desc.format)];
  }

  bool readTexture(TextureHandle texture, std::vector<u8>& out) override {
    Texture& t = textures_.get(texture.id);
    if (!t.tex || isDepthFormat(t.desc.format)) return false;
    u32 rowBytes = t.desc.width * bytesPerPixel(t.desc.format);
    u64 size = (u64)rowBytes * t.desc.height;
    id<MTLBuffer> staging = [device_ newBufferWithLength:size options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit copyFromTexture:t.tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(t.desc.width, t.desc.height, 1)
                 toBuffer:staging destinationOffset:0 destinationBytesPerRow:rowBytes destinationBytesPerImage:size];
    [blit endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    out.assign((u8*)staging.contents, (u8*)staging.contents + size);
    return true;
  }

  CommandList* beginFrame() override {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (w <= 0 || h <= 0) return nullptr;
    waitIdle(); // one frame in flight: see Device::beginFrame
    pool_ = objc_autoreleasePoolPush();
    if ((int)layer_.drawableSize.width != w || (int)layer_.drawableSize.height != h) layer_.drawableSize = CGSizeMake(w, h);
    drawable_ = [layer_ nextDrawable];
    if (!drawable_) {
      objc_autoreleasePoolPop(pool_);
      return nullptr;
    }
    width_ = (u32)w;
    height_ = (u32)h;
    Texture& bb = textures_.get(backbuffer_.id);
    bb.tex = drawable_.texture;
    bb.desc = TextureDesc{width_, height_, Format::BGRA8_SRGB, TextureUsage_RenderTarget, "backbuffer"};
    list_.cmd = [queue_ commandBuffer];
    return &list_;
  }

  void endFrame() override {
    [list_.cmd presentDrawable:drawable_];
    [list_.cmd commit];
    lastCommit_ = list_.cmd;
    list_.cmd = nil;
    drawable_ = nil;
    textures_.get(backbuffer_.id).tex = nil;
    objc_autoreleasePoolPop(pool_);
  }

  TextureHandle backbuffer() override { return backbuffer_; }
  Format backbufferFormat() const override { return Format::BGRA8_SRGB; }
  u32 backbufferWidth() const override { return width_; }
  u32 backbufferHeight() const override { return height_; }
  void waitIdle() override {
    if (lastCommit_) [lastCommit_ waitUntilCompleted];
  }

  Pool<id<MTLBuffer>> buffers_;
  Pool<Texture> textures_;
  Pool<id<MTLSamplerState>> samplers_;
  Pool<Shader> shaders_;
  Pool<BindGroupLayoutDesc> layouts_;
  Pool<BindGroup> groups_;
  Pool<Pipeline> pipelines_;

private:
  id<MTLFunction> compile(const Shader& shader, const Pipeline& p, const char* name) {
    std::string msl;
    try {
      spirv_cross::CompilerMSL compiler(shader.spirv);
      spirv_cross::CompilerMSL::Options opts;
      opts.set_msl_version(2, 3);
#if TARGET_OS_OSX
      opts.platform = spirv_cross::CompilerMSL::Options::macOS;
#else
      opts.platform = spirv_cross::CompilerMSL::Options::iOS;
      opts.ios_support_base_vertex_instance = true; // SV_VertexID lowers to vertex id minus base vertex (A9+)
#endif
      compiler.set_msl_options(opts);
      spv::ExecutionModel model = shader.stage == ShaderStage::Vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;
      for (u32 g = 0; g < kMaxBindGroups; g++) {
        for (u32 b = 0; b < kMaxBindings; b++) {
          if (p.type[g][b] == BindingType::None) continue;
          spirv_cross::MSLResourceBinding rb;
          rb.stage = model;
          rb.desc_set = g;
          rb.binding = b;
          rb.msl_buffer = rb.msl_texture = rb.msl_sampler = p.slot[g][b];
          compiler.add_msl_resource_binding(rb);
        }
      }
      msl = compiler.compile();
    } catch (const spirv_cross::CompilerError& e) {
      ACE_ERROR("pipeline %s: SPIR-V to MSL failed: %s", name, e.what());
      return nil;
    }
    NSError* error = nil;
    id<MTLLibrary> lib = [device_ newLibraryWithSource:@(msl.c_str()) options:nil error:&error];
    if (!lib) {
      ACE_ERROR("pipeline %s: MSL compile failed: %s\n%s", name, error.localizedDescription.UTF8String, msl.c_str());
      return nil;
    }
    return [lib newFunctionWithName:@"main0"];
  }

  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLCommandBuffer> lastCommit_ = nil;
  id<CAMetalDrawable> drawable_ = nil;
  CAMetalLayer* layer_ = nil;
  SDL_MetalView view_ = nullptr;
  MetalCommandList list_;
  SDL_Window* window_ = nullptr;
  TextureHandle backbuffer_;
  void* pool_ = nullptr;
  u32 width_ = 0, height_ = 0;
};

void MetalCommandList::beginRenderPass(const RenderPassDesc& desc) {
  MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
  for (u32 i = 0; i < desc.colorCount; i++) {
    const ColorAttachment& c = desc.color[i];
    Texture& t = dev_.textures_.get(c.texture.id);
    rp.colorAttachments[i].texture = t.tex;
    rp.colorAttachments[i].loadAction = c.load == LoadOp::Clear ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.colorAttachments[i].storeAction = MTLStoreActionStore;
    rp.colorAttachments[i].clearColor = MTLClearColorMake(c.clear[0], c.clear[1], c.clear[2], c.clear[3]);
    passWidth_ = t.desc.width;
    passHeight_ = t.desc.height;
  }
  if (desc.depth) {
    Texture& t = dev_.textures_.get(desc.depth.id);
    rp.depthAttachment.texture = t.tex;
    rp.depthAttachment.loadAction = desc.depthLoad == LoadOp::Clear ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    rp.depthAttachment.clearDepth = desc.depthClear;
    passWidth_ = t.desc.width;
    passHeight_ = t.desc.height;
  }
  enc_ = [cmd renderCommandEncoderWithDescriptor:rp];
  enc_.label = @(desc.name);
  [enc_ setFrontFacingWinding:MTLWindingCounterClockwise];
  pipeline_ = nullptr;
}

void MetalCommandList::endRenderPass() {
  [enc_ endEncoding];
  enc_ = nil;
}

void MetalCommandList::setViewport(f32 x, f32 y, f32 w, f32 h) {
  [enc_ setViewport:(MTLViewport){x, y, w, h, 0.0, 1.0}];
}

void MetalCommandList::setScissor(i32 x, i32 y, u32 w, u32 h) {
  // Metal rejects scissor rects outside the attachment; clamp like the other APIs do.
  i32 x0 = std::clamp(x, 0, (i32)passWidth_), y0 = std::clamp(y, 0, (i32)passHeight_);
  i32 x1 = std::clamp(x + (i32)w, x0, (i32)passWidth_), y1 = std::clamp(y + (i32)h, y0, (i32)passHeight_);
  [enc_ setScissorRect:(MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)}];
}

void MetalCommandList::setPipeline(PipelineHandle pipeline) {
  pipeline_ = &dev_.pipelines_.get(pipeline.id);
  [enc_ setRenderPipelineState:pipeline_->state];
  [enc_ setDepthStencilState:pipeline_->depth];
  [enc_ setCullMode:pipeline_->cull];
}

void MetalCommandList::setBindGroup(u32 index, BindGroupHandle group, std::span<const u32> dynamicOffsets) {
  ACE_ASSERT(pipeline_, "setBindGroup must come after setPipeline");
  BindGroup& g = dev_.groups_.get(group.id);
  u32 dynamic = 0;
  for (u32 b = 0; b < kMaxBindings; b++) {
    const BindGroupEntry& e = g.desc.entries[b];
    u32 slot = pipeline_->slot[index][b];
    switch (g.layout.bindings[b]) {
      case BindingType::UniformBuffer:
      case BindingType::StorageBuffer: {
        u64 offset = e.offset;
        if (g.layout.bindings[b] == BindingType::UniformBuffer && dynamic < dynamicOffsets.size()) offset += dynamicOffsets[dynamic++];
        id<MTLBuffer> buf = dev_.buffers_.get(e.buffer.id);
        [enc_ setVertexBuffer:buf offset:offset atIndex:slot];
        [enc_ setFragmentBuffer:buf offset:offset atIndex:slot];
        break;
      }
      case BindingType::Texture: {
        id<MTLTexture> tex = dev_.textures_.get(e.texture.id).tex;
        [enc_ setVertexTexture:tex atIndex:slot];
        [enc_ setFragmentTexture:tex atIndex:slot];
        break;
      }
      case BindingType::Sampler: {
        id<MTLSamplerState> s = dev_.samplers_.get(e.sampler.id);
        [enc_ setVertexSamplerState:s atIndex:slot];
        [enc_ setFragmentSamplerState:s atIndex:slot];
        break;
      }
      case BindingType::None: break;
    }
  }
}

void MetalCommandList::setVertexBuffer(BufferHandle buffer, u64 offset) {
  [enc_ setVertexBuffer:dev_.buffers_.get(buffer.id) offset:offset atIndex:kVertexBufferIndex];
}

void MetalCommandList::setIndexBuffer(BufferHandle buffer, bool index32, u64 offset) {
  indexBuffer_ = dev_.buffers_.get(buffer.id);
  index32_ = index32;
  indexOffset_ = offset;
}

void MetalCommandList::draw(u32 vertexCount, u32 firstVertex) {
  [enc_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:firstVertex vertexCount:vertexCount];
}

void MetalCommandList::drawIndexed(u32 indexCount, u32 firstIndex, i32 baseVertex) {
  [enc_ drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                   indexCount:indexCount
                    indexType:index32_ ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                  indexBuffer:indexBuffer_
            indexBufferOffset:indexOffset_ + (u64)firstIndex * (index32_ ? 4 : 2)
                instanceCount:1
                   baseVertex:baseVertex
                 baseInstance:0];
}

} // namespace

std::unique_ptr<Device> createMetalDevice(const DeviceDesc& desc) { return std::make_unique<MetalDevice>(desc); }

} // namespace ace::rhi
