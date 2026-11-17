// Render Hardware Interface: the only API the rest of the engine uses to talk to
// the GPU. Each backend (Metal, Vulkan, D3D12) implements Device + CommandList.
//
// Design rules:
//  - Resources are plain integer handles; id 0 is always "null".
//  - Binding follows the bind-group model (WebGPU / Vulkan descriptor sets): a
//    pipeline declares up to kMaxBindGroups layouts, draws bind matching groups.
//  - Every uniform-buffer binding takes a dynamic offset, so per-draw constants
//    come from one big per-frame buffer instead of thousands of small ones.
//  - Shaders are authored in Slang with explicit [[vk::binding(binding, group)]].
#pragma once
#include "core/base.h"
#include <memory>

struct SDL_Window;

namespace ace::rhi {

enum class Backend { Metal, Vulkan, D3D12 };
const char* backendName(Backend b);

template <typename Tag> struct Handle {
  u32 id = 0;
  explicit operator bool() const { return id != 0; }
  bool operator==(const Handle&) const = default;
};
using BufferHandle = Handle<struct BufferTag>;
using TextureHandle = Handle<struct TextureTag>;
using SamplerHandle = Handle<struct SamplerTag>;
using ShaderHandle = Handle<struct ShaderTag>;
using BindGroupLayoutHandle = Handle<struct BindGroupLayoutTag>;
using BindGroupHandle = Handle<struct BindGroupTag>;
using PipelineHandle = Handle<struct PipelineTag>;

constexpr u32 kMaxBindGroups = 4;
constexpr u32 kMaxBindings = 8; // per group
constexpr u32 kMaxColorTargets = 4;
constexpr u32 kMaxVertexAttributes = 8;

enum class Format { Undefined, R8, R32F, RGBA8, RGBA8_SRGB, BGRA8, BGRA8_SRGB, RGBA16F, RGBA32F, D32F };
bool isDepthFormat(Format f);
u32 bytesPerPixel(Format f);

enum TextureUsage : u32 { TextureUsage_Sampled = 1, TextureUsage_RenderTarget = 2 };

struct TextureDesc {
  u32 width = 1, height = 1;
  Format format = Format::RGBA8;
  u32 usage = TextureUsage_Sampled;
  const char* name = "";
};

enum BufferUsage : u32 { BufferUsage_Vertex = 1, BufferUsage_Index = 2, BufferUsage_Uniform = 4, BufferUsage_Storage = 8 };

// All buffers are CPU-writable through Device::writeBuffer. shortcut: no
// GPU-private/staged buffers yet; revisit when large static meshes show up in profiles.
struct BufferDesc {
  u64 size = 0;
  u32 usage = BufferUsage_Vertex;
  const char* name = "";
};

enum class Filter { Nearest, Linear };
enum class AddressMode { Repeat, ClampToEdge };
struct SamplerDesc {
  Filter filter = Filter::Nearest;
  AddressMode address = AddressMode::Repeat;
};

enum class ShaderStage : u32 { Vertex = 1, Fragment = 2 };
struct ShaderDesc {
  ShaderStage stage = ShaderStage::Vertex;
  std::span<const u8> spirv; // Vulkan + Metal (transpiled)
  std::span<const u8> dxil;  // D3D12
  const char* name = "";
};

enum class BindingType { None, UniformBuffer, StorageBuffer, Texture, Sampler };
struct BindGroupLayoutDesc {
  // Index in this array == [[vk::binding(N, group)]] in the shader.
  BindingType bindings[kMaxBindings] = {};
};

struct BindGroupEntry {
  BufferHandle buffer; // UniformBuffer / StorageBuffer
  u64 offset = 0;
  u64 size = 0; // bytes visible to the shader (UniformBuffer); 0 = whole buffer
  TextureHandle texture;
  SamplerHandle sampler;
};
struct BindGroupDesc {
  BindGroupLayoutHandle layout;
  BindGroupEntry entries[kMaxBindings] = {};
};

enum class VertexFormat { Float2, Float3, Float4, UByte4Norm };
struct VertexAttribute {
  u32 location = 0;
  VertexFormat format = VertexFormat::Float3;
  u32 offset = 0;
};
enum class CullMode { None, Back, Front };
enum class BlendMode { Opaque, Alpha, Additive };
enum class CompareOp { Always, Less, LessEqual, Equal };

struct PipelineDesc {
  ShaderHandle vertexShader, fragmentShader;
  VertexAttribute attributes[kMaxVertexAttributes] = {};
  u32 attributeCount = 0;
  u32 vertexStride = 0;
  BindGroupLayoutHandle bindGroupLayouts[kMaxBindGroups] = {};
  Format colorFormats[kMaxColorTargets] = {};
  u32 colorCount = 1;
  Format depthFormat = Format::Undefined;
  bool depthWrite = true;
  CompareOp depthCompare = CompareOp::Less;
  CullMode cull = CullMode::Back; // front faces are counter-clockwise
  BlendMode blend[kMaxColorTargets] = {}; // per color target
  const char* name = "";
};

enum class LoadOp { Clear, Load };
struct ColorAttachment {
  TextureHandle texture;
  LoadOp load = LoadOp::Clear;
  f32 clear[4] = {0, 0, 0, 1};
};
struct RenderPassDesc {
  ColorAttachment color[kMaxColorTargets] = {};
  u32 colorCount = 1;
  TextureHandle depth;
  LoadOp depthLoad = LoadOp::Clear;
  f32 depthClear = 1.0f;
  const char* name = "";
};

class CommandList {
public:
  virtual ~CommandList() = default;
  virtual void beginRenderPass(const RenderPassDesc& desc) = 0;
  virtual void endRenderPass() = 0;
  virtual void setViewport(f32 x, f32 y, f32 w, f32 h) = 0;
  virtual void setScissor(i32 x, i32 y, u32 w, u32 h) = 0;
  virtual void setPipeline(PipelineHandle pipeline) = 0;
  // `dynamicOffsets` has one entry per UniformBuffer binding of the group, in binding order.
  virtual void setBindGroup(u32 index, BindGroupHandle group, std::span<const u32> dynamicOffsets = {}) = 0;
  virtual void setVertexBuffer(BufferHandle buffer, u64 offset = 0) = 0;
  virtual void setIndexBuffer(BufferHandle buffer, bool index32 = true, u64 offset = 0) = 0;
  virtual void draw(u32 vertexCount, u32 firstVertex = 0) = 0;
  virtual void drawIndexed(u32 indexCount, u32 firstIndex = 0, i32 baseVertex = 0) = 0;
};

struct DeviceDesc {
  Backend backend = Backend::Metal;
  SDL_Window* window = nullptr;
  bool vsync = true;
  bool validation = false;
};

class Device {
public:
  virtual ~Device() = default;
  virtual Backend backend() const = 0;

  virtual BufferHandle createBuffer(const BufferDesc& desc) = 0;
  virtual TextureHandle createTexture(const TextureDesc& desc) = 0;
  virtual SamplerHandle createSampler(const SamplerDesc& desc) = 0;
  virtual ShaderHandle createShader(const ShaderDesc& desc) = 0;
  virtual BindGroupLayoutHandle createBindGroupLayout(const BindGroupLayoutDesc& desc) = 0;
  virtual BindGroupHandle createBindGroup(const BindGroupDesc& desc) = 0;
  virtual PipelineHandle createPipeline(const PipelineDesc& desc) = 0;

  // Destruction is deferred until the GPU can no longer be using the resource.
  virtual void destroy(BufferHandle h) = 0;
  virtual void destroy(TextureHandle h) = 0;
  virtual void destroy(SamplerHandle h) = 0;
  virtual void destroy(ShaderHandle h) = 0;
  virtual void destroy(BindGroupLayoutHandle h) = 0;
  virtual void destroy(BindGroupHandle h) = 0;
  virtual void destroy(PipelineHandle h) = 0;

  virtual void writeBuffer(BufferHandle buffer, u64 offset, const void* data, u64 size) = 0;
  // Uploads the full image (tightly packed rows, top row first).
  virtual void writeTexture(TextureHandle texture, const void* pixels) = 0;
  // Blocking read-back of a color texture rendered in a *previous* frame. For
  // screenshots, tests and editor picking; not for per-frame use.
  virtual bool readTexture(TextureHandle texture, std::vector<u8>& outPixels) = 0;

  // Frame: beginFrame() -> record on the returned list -> endFrame() presents.
  // Returns null when there is nothing to draw to (minimised window).
  // beginFrame waits for the previous frame's GPU work, so buffers may be
  // rewritten every frame without tearing.
  // shortcut: one frame in flight; move to per-frame resource rings if CPU/GPU
  // overlap ever shows up as the bottleneck.
  // present = false: an offscreen frame that never touches the window (no
  // backbuffer). Works while the window is hidden, minimised or the display is
  // asleep, which is what screenshots and automated tests need.
  virtual CommandList* beginFrame(bool present = true) = 0;
  virtual void endFrame() = 0;
  virtual TextureHandle backbuffer() = 0; // valid between beginFrame and endFrame
  virtual Format backbufferFormat() const = 0;
  virtual u32 backbufferWidth() const = 0;
  virtual u32 backbufferHeight() const = 0;
  virtual void waitIdle() = 0;
};

// Picks the platform default: Metal on Apple, D3D12 on Windows, Vulkan elsewhere.
Backend defaultBackend();
bool backendAvailable(Backend b);
// SDL window flag the backend needs at window creation (SDL_WINDOW_METAL / _VULKAN / 0).
u64 windowFlagsFor(Backend b);
std::unique_ptr<Device> createDevice(const DeviceDesc& desc);

// Slot map used by the backends to store their resource structs behind handles.
template <typename T> class Pool {
public:
  u32 add(T&& value) {
    if (!free_.empty()) {
      u32 id = free_.back();
      free_.pop_back();
      items_[id] = std::move(value);
      return id;
    }
    if (items_.empty()) items_.emplace_back(); // slot 0 = null handle
    items_.push_back(std::move(value));
    return (u32)items_.size() - 1;
  }
  T& get(u32 id) {
    ACE_ASSERT(id != 0 && id < items_.size(), "invalid RHI handle %u", id);
    return items_[id];
  }
  template <typename F> void each(F&& fn) { // live and freed slots alike; freed ones hold T{}
    for (size_t i = 1; i < items_.size(); i++) fn(items_[i]);
  }
  void remove(u32 id) {
    if (id == 0) return;
    items_[id] = T{};
    free_.push_back(id);
  }
private:
  std::vector<T> items_;
  std::vector<u32> free_;
};

} // namespace ace::rhi
