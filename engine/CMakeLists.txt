set(ACE_SOURCES
  core/base.cpp
  rhi/rhi.cpp
  platform/app.cpp
  scene/scene.cpp
  assets/assets.cpp
  render/renderer.cpp
  render/path_tracer.cpp
  script/script.cpp
  runtime/project.cpp)

if(ACE_RHI_METAL)
  list(APPEND ACE_SOURCES rhi/metal/metal_device.mm)
  set_source_files_properties(rhi/metal/metal_device.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
if(ACE_RHI_VULKAN)
  list(APPEND ACE_SOURCES rhi/vulkan/vulkan_device.cpp)
endif()
if(ACE_RHI_D3D12)
  list(APPEND ACE_SOURCES rhi/d3d12/d3d12_device.cpp)
endif()

add_library(ace_engine STATIC ${ACE_SOURCES})
target_include_directories(ace_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ace_engine PUBLIC SDL3::SDL3 glm entt json PRIVATE stb_cgltf lua)
target_compile_options(ace_engine PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers>)

foreach(backend METAL VULKAN D3D12)
  if(ACE_RHI_${backend})
    target_compile_definitions(ace_engine PUBLIC ACE_RHI_${backend}=1)
  else()
    target_compile_definitions(ace_engine PUBLIC ACE_RHI_${backend}=0)
  endif()
endforeach()

if(ACE_RHI_METAL)
  target_link_libraries(ace_engine PRIVATE spirv-cross-msl "-framework Metal" "-framework QuartzCore" "-framework Foundation")
endif()
if(ACE_RHI_VULKAN)
  target_link_libraries(ace_engine PRIVATE Vulkan::Vulkan)
endif()
if(ACE_RHI_D3D12)
  target_link_libraries(ace_engine PRIVATE d3d12 dxgi dxguid)
endif()

# Compiled shaders live in ace_data/shaders next to the executables.
file(GLOB ACE_SHADERS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.slang)
ace_compile_shaders(ace_shaders ${ACE_DATA_DIR}/shaders ${ACE_SHADERS})
add_dependencies(ace_engine ace_shaders)
