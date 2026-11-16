#include "rhi/rhi.h"
#include <SDL3/SDL.h>

namespace ace::rhi {

#if ACE_RHI_METAL
std::unique_ptr<Device> createMetalDevice(const DeviceDesc& desc);
#endif
#if ACE_RHI_VULKAN
std::unique_ptr<Device> createVulkanDevice(const DeviceDesc& desc);
#endif
#if ACE_RHI_D3D12
std::unique_ptr<Device> createD3D12Device(const DeviceDesc& desc);
#endif

const char* backendName(Backend b) {
  switch (b) {
    case Backend::Metal: return "Metal";
    case Backend::Vulkan: return "Vulkan";
    case Backend::D3D12: return "D3D12";
  }
  return "?";
}

bool isDepthFormat(Format f) { return f == Format::D32F; }

u32 bytesPerPixel(Format f) {
  switch (f) {
    case Format::R8: return 1;
    case Format::RGBA16F: return 8;
    case Format::RGBA32F: return 16;
    default: return 4;
  }
}

bool backendAvailable(Backend b) {
  switch (b) {
    case Backend::Metal: return ACE_RHI_METAL;
    case Backend::Vulkan: return ACE_RHI_VULKAN;
    case Backend::D3D12: return ACE_RHI_D3D12;
  }
  return false;
}

Backend defaultBackend() {
  if (ACE_RHI_METAL) return Backend::Metal;
  if (ACE_RHI_D3D12) return Backend::D3D12;
  return Backend::Vulkan;
}

u64 windowFlagsFor(Backend b) {
  if (b == Backend::Metal) return SDL_WINDOW_METAL;
  if (b == Backend::Vulkan) return SDL_WINDOW_VULKAN;
  return 0;
}

std::unique_ptr<Device> createDevice(const DeviceDesc& desc) {
  switch (desc.backend) {
#if ACE_RHI_METAL
    case Backend::Metal: return createMetalDevice(desc);
#endif
#if ACE_RHI_VULKAN
    case Backend::Vulkan: return createVulkanDevice(desc);
#endif
#if ACE_RHI_D3D12
    case Backend::D3D12: return createD3D12Device(desc);
#endif
    default: break;
  }
  ACE_ERROR("RHI backend %s is not compiled into this build", backendName(desc.backend));
  return nullptr;
}

} // namespace ace::rhi
