// Direct3D 12 backend (Windows).
//
// Mapping of the RHI's bind-group model:
//  - Shaders carry explicit registers beside every [[vk::binding(b, g)]]:
//    register(b|t|s <b>, space<g>), so binding index == register number and
//    group == register space. Vertex inputs use semantic ATTRIB<location>.
//  - Uniform buffers are root CBVs and storage buffers root SRVs: binding one is
//    just a GPU address, so dynamic offsets cost nothing.
//  - Textures and samplers of a group live in one descriptor-table block each,
//    written once when the bind group is created.
//  - One root signature per pipeline, built from its bind-group layouts. Bound
//    groups are applied lazily at draw time, so the order of setPipeline and
//    setBindGroup does not matter.
//  - Resource states: every texture rests in one state between passes (shader
//    resource; depth: depth write; swapchain image: present). Passes, uploads
//    and read-backs transition in and back out, so nothing else tracks state.
//
// Coordinate conventions match Metal (y-up clip space, depth 0..1, top-left
// texture origin), so the renderer needs no D3D-specific code.
#include "rhi/rhi.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ace::rhi {
namespace {

#define ACE_HR(expr, what)                                                              \
  do {                                                                                  \
    HRESULT hr_ = (expr);                                                               \
    ACE_ASSERT(SUCCEEDED(hr_), "D3D12: %s failed (HRESULT 0x%08lx)", what, (unsigned long)hr_); \
  } while (0)

constexpr DXGI_FORMAT kSwapchainFormat = DXGI_FORMAT_B8G8R8A8_UNORM; // flip-model swapchains cannot be sRGB...
constexpr DXGI_FORMAT kSwapchainViewFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; // ...their render target views can
constexpr u32 kSwapchainImages = 2;
constexpr D3D12_RESOURCE_STATES kShaderResource = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr u32 kInvalid = ~0u;

DXGI_FORMAT toDXGI(Format f) {
  switch (f) {
    case Format::R8: return DXGI_FORMAT_R8_UNORM;
    case Format::R32F: return DXGI_FORMAT_R32_FLOAT;
    case Format::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::BGRA8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case Format::BGRA8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case Format::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case Format::D32F: return DXGI_FORMAT_D32_FLOAT;
    case Format::Undefined: break;
  }
  return DXGI_FORMAT_UNKNOWN;
}

D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES h{};
  h.Type = type;
  return h;
}

D3D12_RESOURCE_DESC bufferDesc(u64 size) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return d;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  return b;
}

// Descriptor heap with a free list per block size. Blocks are freed through the
// device's deferred-destruction list, never while the GPU may read them.
struct DescriptorHeap {
  ComPtr<ID3D12DescriptorHeap> heap;
  u32 increment = 0, capacity = 0, next = 0;
  std::vector<std::vector<u32>> freeBlocks; // index = block size

  void init(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 count, bool shaderVisible, const char* what) {
    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.Type = type;
    d.NumDescriptors = count;
    d.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ACE_HR(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&heap)), what);
    increment = dev->GetDescriptorHandleIncrementSize(type);
    capacity = count;
  }
  u32 alloc(u32 count) {
    if (count < freeBlocks.size() && !freeBlocks[count].empty()) {
      u32 start = freeBlocks[count].back();
      freeBlocks[count].pop_back();
      return start;
    }
    // shortcut: freed blocks are only reused by blocks of the same size; fine for
    // the handful of layouts the engine uses.
    ACE_ASSERT(next + count <= capacity, "D3D12: descriptor heap full (%u descriptors)", capacity);
    u32 start = next;
    next += count;
    return start;
  }
  void free(u32 start, u32 count) {
    if (count >= freeBlocks.size()) freeBlocks.resize(count + 1);
    freeBlocks[count].push_back(start);
  }
  D3D12_CPU_DESCRIPTOR_HANDLE cpu(u32 i) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)i * increment;
    return h;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE gpu(u32 i) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)i * increment;
    return h;
  }
};

struct Buffer {
  ComPtr<ID3D12Resource> res;
  u8* mapped = nullptr;
  u64 size = 0;
};
struct Texture {
  ComPtr<ID3D12Resource> res;
  TextureDesc desc;
  D3D12_RESOURCE_STATES rest = kShaderResource; // state between passes
  u32 rtv = kInvalid, dsv = kInvalid;
};
struct Shader {
  std::vector<u8> dxil;
  ShaderStage stage = ShaderStage::Vertex;
};
struct BindGroup {
  BindGroupDesc desc;
  BindGroupLayoutDesc layout;
  u32 srvStart = kInvalid, srvCount = 0;
  u32 samplerStart = kInvalid, samplerCount = 0;
};
struct Pipeline {
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> pso;
  BindingType type[kMaxBindGroups][kMaxBindings] = {};
  i8 rootParam[kMaxBindGroups][kMaxBindings]; // root CBV / SRV of each buffer binding
  i8 srvTable[kMaxBindGroups], samplerTable[kMaxBindGroups];
  u32 vertexStride = 0;
  Pipeline() {
    memset(rootParam, -1, sizeof(rootParam));
    memset(srvTable, -1, sizeof(srvTable));
    memset(samplerTable, -1, sizeof(samplerTable));
  }
};

class D3D12Device;

class D3D12CommandList final : public CommandList {
public:
  explicit D3D12CommandList(D3D12Device& device) : dev_(device) {}
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

  void begin(); // start of a frame
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> cl;

private:
  void flush(); // apply pipeline-dependent state before a draw
  D3D12Device& dev_;
  Pipeline* pipeline_ = nullptr;
  ID3D12RootSignature* root_ = nullptr;
  struct Bound {
    BindGroupHandle group;
    u32 offsets[kMaxBindings] = {};
    u32 offsetCount = 0;
  } bound_[kMaxBindGroups];
  u32 dirtyGroups_ = 0;
  BufferHandle vertexBuffer_;
  u64 vertexOffset_ = 0;
  bool vertexDirty_ = false;
  TextureHandle attachments_[kMaxColorTargets + 1];
  u32 attachmentCount_ = 0;
  u32 passWidth_ = 0, passHeight_ = 0;
};

class D3D12Device final : public Device {
public:
  explicit D3D12Device(const DeviceDesc& desc) : list_(*this), window_(desc.window), vsync_(desc.vsync) {
    UINT factoryFlags = 0;
    if (desc.validation) {
      ComPtr<ID3D12Debug> debug;
      if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
      } else {
        ACE_WARN("D3D12 debug layer not available (install the Graphics Tools optional feature)");
      }
    }
    ACE_HR(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_)), "CreateDXGIFactory2");
    // The first hardware adapter that supports D3D12 (feature level 11.0 is enough),
    // discrete GPU first: laptops list the integrated one first otherwise.
    ComPtr<IDXGIFactory6> factory6;
    factory_.As(&factory6);
    auto enumAdapter = [&](UINT i, ComPtr<IDXGIAdapter1>& a) {
      if (factory6) return factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a));
      return factory_->EnumAdapters1(i, &a);
    };
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; enumAdapter(i, adapter) != DXGI_ERROR_NOT_FOUND; i++) {
      DXGI_ADAPTER_DESC1 ad;
      adapter->GetDesc1(&ad);
      if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
      if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
        ACE_INFO("D3D12 device: %s", name);
        break;
      }
    }
    ACE_ASSERT(device_, "no Direct3D 12 capable GPU found");
    if (desc.validation && SUCCEEDED(device_.As(&infoQueue_))) {
      // Clear colors are chosen per pass (RenderPassDesc), not at texture creation,
      // so the "no optimized clear value" performance hint would fire on every clear.
      D3D12_MESSAGE_ID deny[] = {D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                                 D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE};
      D3D12_INFO_QUEUE_FILTER filter{};
      filter.DenyList.NumIDs = (UINT)std::size(deny);
      filter.DenyList.pIDList = deny;
      infoQueue_->AddStorageFilterEntries(&filter);
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ACE_HR(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)), "CreateCommandQueue");
    ACE_HR(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ACE_ASSERT(fenceEvent_, "CreateEvent failed");
    ACE_HR(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&list_.allocator)), "CreateCommandAllocator");
    ACE_HR(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, list_.allocator.Get(), nullptr, IID_PPV_ARGS(&list_.cl)),
           "CreateCommandList");
    list_.cl->Close();
    ACE_HR(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copyAllocator_)), "CreateCommandAllocator");
    ACE_HR(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copyAllocator_.Get(), nullptr, IID_PPV_ARGS(&copyList_)),
           "CreateCommandList");
    copyList_->Close();

    // shortcut: fixed heap sizes; grow (or page) them if a project runs out.
    views_.init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536, true, "SRV heap");
    samplerHeap_.init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048, true, "sampler heap");
    rtvs_.init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024, false, "RTV heap");
    dsvs_.init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 256, false, "DSV heap");

    // Swapchain on the SDL window.
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    ACE_ASSERT(hwnd, "D3D12 needs a Win32 window");
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    swapWidth_ = width_ = (u32)std::max(w, 1);
    swapHeight_ = height_ = (u32)std::max(h, 1);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = swapWidth_;
    sd.Height = swapHeight_;
    sd.Format = kSwapchainFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kSwapchainImages;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc;
    ACE_HR(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc), "CreateSwapChainForHwnd");
    ACE_HR(sc.As(&swapchain_), "IDXGISwapChain3");
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER); // SDL owns fullscreen
    for (u32 i = 0; i < kSwapchainImages; i++) swapRtv_[i] = rtvs_.alloc(1);
    acquireSwapchainImages();
    backbuffer_.id = textures_.add(Texture{});
  }

  ~D3D12Device() override {
    waitIdle();
    collectGarbage();
    if (fenceEvent_) CloseHandle(fenceEvent_);
  }

  Backend backend() const override { return Backend::D3D12; }

  // shortcut: every buffer lives in an upload heap (CPU-writable, see BufferDesc);
  // large static data (meshes, BVH) would read faster from a default heap on
  // discrete GPUs.
  BufferHandle createBuffer(const BufferDesc& desc) override {
    Buffer b;
    b.size = (desc.size + 255) & ~u64(255); // root CBVs read whole 256-byte blocks
    D3D12_HEAP_PROPERTIES hp = heapProps(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC rd = bufferDesc(b.size);
    ACE_HR(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&b.res)),
           desc.name);
    setName(b.res.Get(), desc.name);
    D3D12_RANGE noRead{0, 0};
    ACE_HR(b.res->Map(0, &noRead, (void**)&b.mapped), "Map");
    return {buffers_.add(std::move(b))};
  }

  TextureHandle createTexture(const TextureDesc& desc) override {
    Texture t;
    t.desc = desc;
    bool depth = isDepthFormat(desc.format);
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = desc.width;
    rd.Height = desc.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = toDXGI(desc.format);
    rd.SampleDesc.Count = 1;
    if (depth) {
      // Depth is only ever an attachment in this engine (the shadow map is a color target).
      rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
      t.rest = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else if (desc.usage & TextureUsage_RenderTarget) {
      rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    D3D12_HEAP_PROPERTIES hp = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ACE_HR(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, t.rest, nullptr, IID_PPV_ARGS(&t.res)), desc.name);
    setName(t.res.Get(), desc.name);
    if (depth) {
      t.dsv = dsvs_.alloc(1);
      device_->CreateDepthStencilView(t.res.Get(), nullptr, dsvs_.cpu(t.dsv));
    } else if (desc.usage & TextureUsage_RenderTarget) {
      t.rtv = rtvs_.alloc(1);
      device_->CreateRenderTargetView(t.res.Get(), nullptr, rtvs_.cpu(t.rtv));
    }
    return {textures_.add(std::move(t))};
  }

  SamplerHandle createSampler(const SamplerDesc& desc) override {
    D3D12_SAMPLER_DESC s{};
    s.Filter = desc.filter == Filter::Linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
    D3D12_TEXTURE_ADDRESS_MODE a = desc.address == AddressMode::Repeat ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.AddressU = s.AddressV = s.AddressW = a;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    return {samplers_.add(std::move(s))};
  }

  ShaderHandle createShader(const ShaderDesc& desc) override {
    ACE_ASSERT(!desc.dxil.empty(), "shader %s has no DXIL (shaders are compiled to DXIL only on Windows builds)", desc.name);
    Shader s;
    s.stage = desc.stage;
    s.dxil.assign(desc.dxil.begin(), desc.dxil.end());
    return {shaders_.add(std::move(s))};
  }

  BindGroupLayoutHandle createBindGroupLayout(const BindGroupLayoutDesc& desc) override {
    return {layouts_.add(BindGroupLayoutDesc(desc))};
  }

  BindGroupHandle createBindGroup(const BindGroupDesc& desc) override {
    BindGroup g;
    g.desc = desc;
    g.layout = layouts_.get(desc.layout.id);
    for (u32 b = 0; b < kMaxBindings; b++) {
      if (g.layout.bindings[b] == BindingType::Texture) g.srvCount++;
      if (g.layout.bindings[b] == BindingType::Sampler) g.samplerCount++;
    }
    if (g.srvCount) g.srvStart = views_.alloc(g.srvCount);
    if (g.samplerCount) g.samplerStart = samplerHeap_.alloc(g.samplerCount);
    // Descriptors in binding order, matching the table ranges in createPipeline.
    u32 srv = 0, sampler = 0;
    for (u32 b = 0; b < kMaxBindings; b++) {
      const BindGroupEntry& e = desc.entries[b];
      if (g.layout.bindings[b] == BindingType::Texture) {
        Texture& t = textures_.get(e.texture.id);
        ACE_ASSERT(!isDepthFormat(t.desc.format), "D3D12: depth textures cannot be sampled (%s)", t.desc.name);
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = toDXGI(t.desc.format);
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(t.res.Get(), &sv, views_.cpu(g.srvStart + srv++));
      } else if (g.layout.bindings[b] == BindingType::Sampler) {
        device_->CreateSampler(&samplers_.get(e.sampler.id), samplerHeap_.cpu(g.samplerStart + sampler++));
      }
    }
    return {groups_.add(std::move(g))};
  }

  PipelineHandle createPipeline(const PipelineDesc& desc) override {
    Pipeline p;
    p.vertexStride = desc.vertexStride;
    // Root signature: buffers as root descriptors, then one SRV table and one
    // sampler table per group that has textures / samplers.
    std::vector<D3D12_ROOT_PARAMETER> params;
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges; // reserved up front: params point into it
    ranges.reserve(kMaxBindGroups * kMaxBindings);
    for (u32 g = 0; g < kMaxBindGroups; g++) {
      if (!desc.bindGroupLayouts[g]) continue;
      const BindGroupLayoutDesc& layout = layouts_.get(desc.bindGroupLayouts[g].id);
      size_t srvFirst = ranges.size();
      for (u32 b = 0; b < kMaxBindings; b++) {
        p.type[g][b] = layout.bindings[b];
        if (layout.bindings[b] == BindingType::UniformBuffer || layout.bindings[b] == BindingType::StorageBuffer) {
          D3D12_ROOT_PARAMETER rp{};
          rp.ParameterType = layout.bindings[b] == BindingType::UniformBuffer ? D3D12_ROOT_PARAMETER_TYPE_CBV : D3D12_ROOT_PARAMETER_TYPE_SRV;
          rp.Descriptor.ShaderRegister = b;
          rp.Descriptor.RegisterSpace = g;
          rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
          p.rootParam[g][b] = (i8)params.size();
          params.push_back(rp);
        } else if (layout.bindings[b] == BindingType::Texture) {
          D3D12_DESCRIPTOR_RANGE r{};
          r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
          r.NumDescriptors = 1;
          r.BaseShaderRegister = b;
          r.RegisterSpace = g;
          r.OffsetInDescriptorsFromTableStart = (UINT)(ranges.size() - srvFirst);
          ranges.push_back(r);
        }
      }
      if (ranges.size() > srvFirst) {
        D3D12_ROOT_PARAMETER rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp.DescriptorTable.NumDescriptorRanges = (UINT)(ranges.size() - srvFirst);
        rp.DescriptorTable.pDescriptorRanges = &ranges[srvFirst];
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p.srvTable[g] = (i8)params.size();
        params.push_back(rp);
      }
      size_t samplerFirst = ranges.size();
      for (u32 b = 0; b < kMaxBindings; b++) {
        if (layout.bindings[b] != BindingType::Sampler) continue;
        D3D12_DESCRIPTOR_RANGE r{};
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        r.NumDescriptors = 1;
        r.BaseShaderRegister = b;
        r.RegisterSpace = g;
        r.OffsetInDescriptorsFromTableStart = (UINT)(ranges.size() - samplerFirst);
        ranges.push_back(r);
      }
      if (ranges.size() > samplerFirst) {
        D3D12_ROOT_PARAMETER rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp.DescriptorTable.NumDescriptorRanges = (UINT)(ranges.size() - samplerFirst);
        rp.DescriptorTable.pDescriptorRanges = &ranges[samplerFirst];
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p.samplerTable[g] = (i8)params.size();
        params.push_back(rp);
      }
    }
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = (UINT)params.size();
    rs.pParameters = params.data();
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error))) {
      ACE_ERROR("pipeline %s: root signature: %s", desc.name, error ? (const char*)error->GetBufferPointer() : "?");
      return {};
    }
    if (FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&p.root)))) {
      ACE_ERROR("pipeline %s: CreateRootSignature failed", desc.name);
      drainMessages();
      return {};
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = p.root.Get();
    const Shader& vs = shaders_.get(desc.vertexShader.id);
    const Shader& fs = shaders_.get(desc.fragmentShader.id);
    d.VS = {vs.dxil.data(), vs.dxil.size()};
    d.PS = {fs.dxil.data(), fs.dxil.size()};
    D3D12_INPUT_ELEMENT_DESC inputs[kMaxVertexAttributes] = {};
    for (u32 i = 0; i < desc.attributeCount; i++) {
      static const DXGI_FORMAT formats[] = {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                            DXGI_FORMAT_R8G8B8A8_UNORM};
      const VertexAttribute& a = desc.attributes[i];
      inputs[i].SemanticName = "ATTRIB"; // shaders declare vertex inputs as ATTRIB<location>
      inputs[i].SemanticIndex = a.location;
      inputs[i].Format = formats[(int)a.format];
      inputs[i].AlignedByteOffset = a.offset;
      inputs[i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    }
    d.InputLayout = {inputs, desc.attributeCount};
    d.BlendState.IndependentBlendEnable = TRUE;
    for (u32 i = 0; i < desc.colorCount; i++) {
      D3D12_RENDER_TARGET_BLEND_DESC& bd = d.BlendState.RenderTarget[i];
      bd.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      bd.LogicOp = D3D12_LOGIC_OP_NOOP;
      bd.BlendOp = bd.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      bd.SrcBlend = bd.SrcBlendAlpha = D3D12_BLEND_ONE;
      bd.DestBlend = bd.DestBlendAlpha = D3D12_BLEND_ZERO;
      if (desc.blend[i] != BlendMode::Opaque) { // same factors as the Metal and Vulkan backends
        bool alpha = desc.blend[i] == BlendMode::Alpha;
        bd.BlendEnable = TRUE;
        bd.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        bd.DestBlend = alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ONE;
        bd.SrcBlendAlpha = D3D12_BLEND_ONE;
        bd.DestBlendAlpha = alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ONE;
      }
      d.RTVFormats[i] = toDXGI(desc.colorFormats[i]);
    }
    d.NumRenderTargets = desc.colorCount;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = desc.cull == CullMode::None ? D3D12_CULL_MODE_NONE : desc.cull == CullMode::Back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_FRONT;
    d.RasterizerState.FrontCounterClockwise = TRUE;
    d.RasterizerState.DepthClipEnable = TRUE;
    if (desc.depthFormat != Format::Undefined) {
      static const D3D12_COMPARISON_FUNC ops[] = {D3D12_COMPARISON_FUNC_ALWAYS, D3D12_COMPARISON_FUNC_LESS, D3D12_COMPARISON_FUNC_LESS_EQUAL,
                                                  D3D12_COMPARISON_FUNC_EQUAL};
      d.DepthStencilState.DepthEnable = TRUE;
      d.DepthStencilState.DepthWriteMask = desc.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
      d.DepthStencilState.DepthFunc = ops[(int)desc.depthCompare];
      d.DSVFormat = toDXGI(desc.depthFormat);
    }
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.SampleDesc.Count = 1;
    if (FAILED(device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p.pso)))) {
      ACE_ERROR("pipeline %s: CreateGraphicsPipelineState failed (run with --validation for details)", desc.name);
      drainMessages();
      return {};
    }
    setName(p.pso.Get(), desc.name);
    return {pipelines_.add(std::move(p))};
  }

  // Deferred: the GPU may still be using these until the next beginFrame's wait.
  void destroy(BufferHandle h) override {
    if (!h) return;
    garbage_.push_back(std::move(buffers_.get(h.id).res));
    buffers_.remove(h.id);
  }
  void destroy(TextureHandle h) override {
    if (!h) return;
    Texture& t = textures_.get(h.id);
    garbage_.push_back(std::move(t.res));
    if (t.rtv != kInvalid) freeLater(rtvs_, t.rtv, 1);
    if (t.dsv != kInvalid) freeLater(dsvs_, t.dsv, 1);
    textures_.remove(h.id);
  }
  void destroy(SamplerHandle h) override { samplers_.remove(h.id); } // copied into groups' descriptors
  void destroy(ShaderHandle h) override { shaders_.remove(h.id); }   // copied into pipelines
  void destroy(BindGroupLayoutHandle h) override { layouts_.remove(h.id); }
  void destroy(BindGroupHandle h) override {
    if (!h) return;
    BindGroup& g = groups_.get(h.id);
    if (g.srvCount) freeLater(views_, g.srvStart, g.srvCount);
    if (g.samplerCount) freeLater(samplerHeap_, g.samplerStart, g.samplerCount);
    groups_.remove(h.id);
  }
  void destroy(PipelineHandle h) override {
    if (!h) return;
    Pipeline& p = pipelines_.get(h.id);
    garbage_.push_back(std::move(p.pso));
    garbage_.push_back(std::move(p.root));
    pipelines_.remove(h.id);
  }

  void writeBuffer(BufferHandle buffer, u64 offset, const void* data, u64 size) override {
    Buffer& b = buffers_.get(buffer.id);
    ACE_ASSERT(offset + size <= b.size, "writeBuffer out of range");
    memcpy(b.mapped + offset, data, size);
  }

  // shortcut: synchronous upload through a temporary buffer (waits for the copy);
  // batch uploads into the frame if texture streaming ever matters.
  void writeTexture(TextureHandle texture, const void* pixels) override {
    Texture& t = textures_.get(texture.id);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 rowBytes, total;
    D3D12_RESOURCE_DESC rd = t.res->GetDesc();
    device_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    ComPtr<ID3D12Resource> staging = makeBuffer(D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    u8* dst = nullptr;
    D3D12_RANGE noRead{0, 0};
    ACE_HR(staging->Map(0, &noRead, (void**)&dst), "Map");
    u32 srcRow = t.desc.width * bytesPerPixel(t.desc.format);
    for (u32 y = 0; y < t.desc.height; y++) memcpy(dst + fp.Offset + (u64)y * fp.Footprint.RowPitch, (const u8*)pixels + (u64)y * srcRow, srcRow);
    staging->Unmap(0, nullptr);

    copyCommands([&](ID3D12GraphicsCommandList* cl) {
      D3D12_RESOURCE_BARRIER in = transition(t.res.Get(), t.rest, D3D12_RESOURCE_STATE_COPY_DEST);
      cl->ResourceBarrier(1, &in);
      D3D12_TEXTURE_COPY_LOCATION to{}, from{};
      to.pResource = t.res.Get();
      to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      from.pResource = staging.Get();
      from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      from.PlacedFootprint = fp;
      cl->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      D3D12_RESOURCE_BARRIER out = transition(t.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, t.rest);
      cl->ResourceBarrier(1, &out);
    });
  }

  bool readTexture(TextureHandle texture, std::vector<u8>& out) override {
    Texture& t = textures_.get(texture.id);
    if (!t.res || isDepthFormat(t.desc.format)) return false;
    waitIdle();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 rowBytes, total;
    D3D12_RESOURCE_DESC rd = t.res->GetDesc();
    device_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    ComPtr<ID3D12Resource> staging = makeBuffer(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
    copyCommands([&](ID3D12GraphicsCommandList* cl) {
      D3D12_RESOURCE_BARRIER in = transition(t.res.Get(), t.rest, D3D12_RESOURCE_STATE_COPY_SOURCE);
      cl->ResourceBarrier(1, &in);
      D3D12_TEXTURE_COPY_LOCATION to{}, from{};
      to.pResource = staging.Get();
      to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      to.PlacedFootprint = fp;
      from.pResource = t.res.Get();
      from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      cl->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      D3D12_RESOURCE_BARRIER back = transition(t.res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, t.rest);
      cl->ResourceBarrier(1, &back);
    });
    u8* src = nullptr;
    D3D12_RANGE all{0, (SIZE_T)total};
    ACE_HR(staging->Map(0, &all, (void**)&src), "Map");
    u32 row = t.desc.width * bytesPerPixel(t.desc.format);
    out.resize((size_t)row * t.desc.height);
    for (u32 y = 0; y < t.desc.height; y++) memcpy(out.data() + (size_t)y * row, src + fp.Offset + (u64)y * fp.Footprint.RowPitch, row);
    D3D12_RANGE noWrite{0, 0};
    staging->Unmap(0, &noWrite);
    return true;
  }

  CommandList* beginFrame(bool present) override {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (w <= 0 || h <= 0) return nullptr;
    waitIdle(); // one frame in flight: see Device::beginFrame
    collectGarbage();
    present_ = present;
    Texture& bb = textures_.get(backbuffer_.id);
    if (present) {
      if ((u32)w != swapWidth_ || (u32)h != swapHeight_) resizeSwapchain((u32)w, (u32)h);
      u32 i = swapchain_->GetCurrentBackBufferIndex();
      bb.res = swapImages_[i];
      bb.rtv = swapRtv_[i];
      bb.rest = D3D12_RESOURCE_STATE_PRESENT;
    } else {
      bb.res = nullptr;
      bb.rtv = kInvalid;
    }
    width_ = (u32)w;
    height_ = (u32)h;
    bb.desc = TextureDesc{width_, height_, Format::BGRA8_SRGB, TextureUsage_RenderTarget, "backbuffer"};
    list_.begin();
    return &list_;
  }

  void endFrame() override {
    ACE_HR(list_.cl->Close(), "Close command list");
    ID3D12CommandList* lists[] = {list_.cl.Get()};
    queue_->ExecuteCommandLists(1, lists);
    if (present_) {
      HRESULT hr = swapchain_->Present(vsync_ ? 1 : 0, 0);
      if (FAILED(hr)) ACE_ERROR("D3D12: Present failed (HRESULT 0x%08lx)", (unsigned long)hr);
    }
    ACE_HR(queue_->Signal(fence_.Get(), ++fenceValue_), "Signal");
    textures_.get(backbuffer_.id).res = nullptr;
    drainMessages();
  }

  TextureHandle backbuffer() override { return backbuffer_; }
  Format backbufferFormat() const override { return Format::BGRA8_SRGB; }
  u32 backbufferWidth() const override { return width_; }
  u32 backbufferHeight() const override { return height_; }
  void waitIdle() override {
    if (fence_->GetCompletedValue() >= fenceValue_) return;
    ACE_HR(fence_->SetEventOnCompletion(fenceValue_, fenceEvent_), "SetEventOnCompletion");
    WaitForSingleObject(fenceEvent_, INFINITE);
  }

  Pool<Buffer> buffers_;
  Pool<Texture> textures_;
  Pool<D3D12_SAMPLER_DESC> samplers_;
  Pool<Shader> shaders_;
  Pool<BindGroupLayoutDesc> layouts_;
  Pool<BindGroup> groups_;
  Pool<Pipeline> pipelines_;
  DescriptorHeap views_, samplerHeap_, rtvs_, dsvs_;

private:
  static void setName(ID3D12Object* o, const char* name) {
    if (!name || !*name) return;
    wchar_t w[128];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 128);
    o->SetName(w);
  }

  ComPtr<ID3D12Resource> makeBuffer(D3D12_HEAP_TYPE type, u64 size, D3D12_RESOURCE_STATES state) {
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES hp = heapProps(type);
    D3D12_RESOURCE_DESC rd = bufferDesc(size);
    ACE_HR(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)), "staging buffer");
    return r;
  }

  // Records with `fn` on the copy list, submits it ahead of the frame and waits.
  template <typename F> void copyCommands(F&& fn) {
    ACE_HR(copyAllocator_->Reset(), "Reset allocator");
    ACE_HR(copyList_->Reset(copyAllocator_.Get(), nullptr), "Reset command list");
    fn(copyList_.Get());
    ACE_HR(copyList_->Close(), "Close command list");
    ID3D12CommandList* lists[] = {copyList_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    ACE_HR(queue_->Signal(fence_.Get(), ++fenceValue_), "Signal");
    waitIdle();
    drainMessages();
  }

  void freeLater(DescriptorHeap& heap, u32 start, u32 count) {
    freedDescriptors_.push_back({&heap, start, count});
  }
  void collectGarbage() {
    garbage_.clear();
    for (const FreedDescriptors& f : freedDescriptors_) f.heap->free(f.start, f.count);
    freedDescriptors_.clear();
  }

  void acquireSwapchainImages() {
    for (u32 i = 0; i < kSwapchainImages; i++) {
      ACE_HR(swapchain_->GetBuffer(i, IID_PPV_ARGS(&swapImages_[i])), "GetBuffer");
      D3D12_RENDER_TARGET_VIEW_DESC rv{};
      rv.Format = kSwapchainViewFormat;
      rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      device_->CreateRenderTargetView(swapImages_[i].Get(), &rv, rtvs_.cpu(swapRtv_[i]));
    }
  }
  void resizeSwapchain(u32 w, u32 h) {
    waitIdle();
    for (auto& img : swapImages_) img.Reset();
    textures_.get(backbuffer_.id).res = nullptr;
    ACE_HR(swapchain_->ResizeBuffers(kSwapchainImages, w, h, kSwapchainFormat, 0), "ResizeBuffers");
    swapWidth_ = w;
    swapHeight_ = h;
    acquireSwapchainImages();
  }

  // With --validation: print what the debug layer reported since last time.
  void drainMessages() {
    if (!infoQueue_) return;
    UINT64 n = infoQueue_->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; i++) {
      SIZE_T len = 0;
      infoQueue_->GetMessage(i, nullptr, &len);
      std::vector<u8> mem(len);
      auto* m = (D3D12_MESSAGE*)mem.data();
      if (FAILED(infoQueue_->GetMessage(i, m, &len))) continue;
      if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) ACE_ERROR("D3D12: %s", m->pDescription);
      else if (m->Severity == D3D12_MESSAGE_SEVERITY_WARNING) ACE_WARN("D3D12: %s", m->pDescription);
    }
    infoQueue_->ClearStoredMessages();
  }

  struct FreedDescriptors {
    DescriptorHeap* heap;
    u32 start, count;
  };

  ComPtr<IDXGIFactory4> factory_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12InfoQueue> infoQueue_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<ID3D12Fence> fence_;
  HANDLE fenceEvent_ = nullptr;
  u64 fenceValue_ = 0;
  ComPtr<ID3D12CommandAllocator> copyAllocator_;
  ComPtr<ID3D12GraphicsCommandList> copyList_;
  ComPtr<IDXGISwapChain3> swapchain_;
  ComPtr<ID3D12Resource> swapImages_[kSwapchainImages];
  u32 swapRtv_[kSwapchainImages] = {};
  std::vector<ComPtr<ID3D12Object>> garbage_;
  std::vector<FreedDescriptors> freedDescriptors_;
  D3D12CommandList list_;
  SDL_Window* window_ = nullptr;
  TextureHandle backbuffer_;
  bool vsync_ = true, present_ = false;
  u32 width_ = 0, height_ = 0;          // this frame's backbuffer size
  u32 swapWidth_ = 0, swapHeight_ = 0;  // size the swapchain images were created with (offscreen frames do not resize them)

  friend class D3D12CommandList;
};

void D3D12CommandList::begin() {
  ACE_HR(allocator->Reset(), "Reset allocator");
  ACE_HR(cl->Reset(allocator.Get(), nullptr), "Reset command list");
  ID3D12DescriptorHeap* heaps[] = {dev_.views_.heap.Get(), dev_.samplerHeap_.heap.Get()};
  cl->SetDescriptorHeaps(2, heaps);
  cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  pipeline_ = nullptr;
  root_ = nullptr;
  for (Bound& b : bound_) b = Bound{};
  dirtyGroups_ = 0;
  vertexBuffer_ = {};
  vertexDirty_ = false;
}

void D3D12CommandList::beginRenderPass(const RenderPassDesc& desc) {
  D3D12_RESOURCE_BARRIER barriers[kMaxColorTargets + 1];
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[kMaxColorTargets];
  u32 nb = 0;
  attachmentCount_ = 0;
  for (u32 i = 0; i < desc.colorCount; i++) {
    Texture& t = dev_.textures_.get(desc.color[i].texture.id);
    ACE_ASSERT(t.res && t.rtv != kInvalid, "render pass %s: color %u is not a render target", desc.name, i);
    if (t.rest != D3D12_RESOURCE_STATE_RENDER_TARGET) barriers[nb++] = transition(t.res.Get(), t.rest, D3D12_RESOURCE_STATE_RENDER_TARGET);
    rtvs[i] = dev_.rtvs_.cpu(t.rtv);
    attachments_[attachmentCount_++] = desc.color[i].texture;
    passWidth_ = t.desc.width;
    passHeight_ = t.desc.height;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
  if (desc.depth) {
    Texture& t = dev_.textures_.get(desc.depth.id);
    dsv = dev_.dsvs_.cpu(t.dsv); // depth rests in DEPTH_WRITE: no barrier
    passWidth_ = t.desc.width;
    passHeight_ = t.desc.height;
  }
  if (nb) cl->ResourceBarrier(nb, barriers);
  cl->OMSetRenderTargets(desc.colorCount, rtvs, FALSE, desc.depth ? &dsv : nullptr);
  for (u32 i = 0; i < desc.colorCount; i++)
    if (desc.color[i].load == LoadOp::Clear) cl->ClearRenderTargetView(rtvs[i], desc.color[i].clear, 0, nullptr);
  if (desc.depth && desc.depthLoad == LoadOp::Clear) cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, desc.depthClear, 0, 0, nullptr);
  // D3D12 starts with an empty scissor; default to the whole target like Metal and Vulkan.
  setViewport(0, 0, (f32)passWidth_, (f32)passHeight_);
  setScissor(0, 0, passWidth_, passHeight_);
}

void D3D12CommandList::endRenderPass() {
  D3D12_RESOURCE_BARRIER barriers[kMaxColorTargets];
  u32 nb = 0;
  for (u32 i = 0; i < attachmentCount_; i++) {
    Texture& t = dev_.textures_.get(attachments_[i].id);
    if (t.rest != D3D12_RESOURCE_STATE_RENDER_TARGET) barriers[nb++] = transition(t.res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, t.rest);
  }
  if (nb) cl->ResourceBarrier(nb, barriers);
  attachmentCount_ = 0;
}

void D3D12CommandList::setViewport(f32 x, f32 y, f32 w, f32 h) {
  D3D12_VIEWPORT v{x, y, w, h, 0.0f, 1.0f};
  cl->RSSetViewports(1, &v);
}

void D3D12CommandList::setScissor(i32 x, i32 y, u32 w, u32 h) {
  i32 x0 = std::clamp(x, 0, (i32)passWidth_), y0 = std::clamp(y, 0, (i32)passHeight_);
  i32 x1 = std::clamp(x + (i32)w, x0, (i32)passWidth_), y1 = std::clamp(y + (i32)h, y0, (i32)passHeight_);
  D3D12_RECT r{x0, y0, x1, y1};
  cl->RSSetScissorRects(1, &r);
}

void D3D12CommandList::setPipeline(PipelineHandle pipeline) {
  pipeline_ = &dev_.pipelines_.get(pipeline.id);
  cl->SetPipelineState(pipeline_->pso.Get());
  if (root_ != pipeline_->root.Get()) {
    root_ = pipeline_->root.Get();
    cl->SetGraphicsRootSignature(root_);
    dirtyGroups_ = (1u << kMaxBindGroups) - 1; // a new root signature forgets every argument
  }
  vertexDirty_ = vertexBuffer_.id != 0; // the stride belongs to the pipeline
}

void D3D12CommandList::setBindGroup(u32 index, BindGroupHandle group, std::span<const u32> dynamicOffsets) {
  ACE_ASSERT(index < kMaxBindGroups && dynamicOffsets.size() <= kMaxBindings, "setBindGroup: bad arguments");
  Bound& b = bound_[index];
  b.group = group;
  b.offsetCount = (u32)dynamicOffsets.size();
  std::copy(dynamicOffsets.begin(), dynamicOffsets.end(), b.offsets);
  dirtyGroups_ |= 1u << index;
}

void D3D12CommandList::setVertexBuffer(BufferHandle buffer, u64 offset) {
  vertexBuffer_ = buffer;
  vertexOffset_ = offset;
  vertexDirty_ = true;
}

void D3D12CommandList::setIndexBuffer(BufferHandle buffer, bool index32, u64 offset) {
  Buffer& b = dev_.buffers_.get(buffer.id);
  D3D12_INDEX_BUFFER_VIEW v{};
  v.BufferLocation = b.res->GetGPUVirtualAddress() + offset;
  v.SizeInBytes = (UINT)(b.size - offset);
  v.Format = index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
  cl->IASetIndexBuffer(&v);
}

void D3D12CommandList::flush() {
  ACE_ASSERT(pipeline_, "draw without a pipeline");
  for (u32 g = 0; g < kMaxBindGroups; g++) {
    if (!(dirtyGroups_ & (1u << g)) || !bound_[g].group) continue;
    BindGroup& group = dev_.groups_.get(bound_[g].group.id);
    u32 dynamic = 0;
    for (u32 b = 0; b < kMaxBindings; b++) {
      BindingType type = group.layout.bindings[b];
      if (type != BindingType::UniformBuffer && type != BindingType::StorageBuffer) continue;
      const BindGroupEntry& e = group.desc.entries[b];
      u64 offset = e.offset;
      if (type == BindingType::UniformBuffer && dynamic < bound_[g].offsetCount) offset += bound_[g].offsets[dynamic++];
      i8 param = pipeline_->rootParam[g][b];
      if (param < 0) continue; // the pipeline does not use this binding
      D3D12_GPU_VIRTUAL_ADDRESS address = dev_.buffers_.get(e.buffer.id).res->GetGPUVirtualAddress() + offset;
      ACE_ASSERT(type != BindingType::UniformBuffer || address % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0,
                 "uniform buffer offset %llu is not 256-byte aligned", (unsigned long long)offset);
      if (type == BindingType::UniformBuffer) cl->SetGraphicsRootConstantBufferView((UINT)param, address);
      else cl->SetGraphicsRootShaderResourceView((UINT)param, address);
    }
    if (pipeline_->srvTable[g] >= 0 && group.srvCount)
      cl->SetGraphicsRootDescriptorTable((UINT)pipeline_->srvTable[g], dev_.views_.gpu(group.srvStart));
    if (pipeline_->samplerTable[g] >= 0 && group.samplerCount)
      cl->SetGraphicsRootDescriptorTable((UINT)pipeline_->samplerTable[g], dev_.samplerHeap_.gpu(group.samplerStart));
  }
  dirtyGroups_ = 0;
  if (vertexDirty_) {
    Buffer& b = dev_.buffers_.get(vertexBuffer_.id);
    D3D12_VERTEX_BUFFER_VIEW v{};
    v.BufferLocation = b.res->GetGPUVirtualAddress() + vertexOffset_;
    v.SizeInBytes = (UINT)(b.size - vertexOffset_);
    v.StrideInBytes = pipeline_->vertexStride;
    cl->IASetVertexBuffers(0, 1, &v);
    vertexDirty_ = false;
  }
}

void D3D12CommandList::draw(u32 vertexCount, u32 firstVertex) {
  flush();
  cl->DrawInstanced(vertexCount, 1, firstVertex, 0);
}

void D3D12CommandList::drawIndexed(u32 indexCount, u32 firstIndex, i32 baseVertex) {
  flush();
  cl->DrawIndexedInstanced(indexCount, 1, firstIndex, baseVertex, 0);
}

} // namespace

std::unique_ptr<Device> createD3D12Device(const DeviceDesc& desc) { return std::make_unique<D3D12Device>(desc); }

} // namespace ace::rhi
