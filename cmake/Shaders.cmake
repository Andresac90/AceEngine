# Shader build: every engine/shaders/*.slang is compiled by slangc into SPIR-V,
# one file per entry point (<name>.vs.spv / <name>.fs.spv). Vulkan consumes the
# SPIR-V directly, Metal transpiles it at load time, D3D12 gets DXIL (Windows).
set(ACE_SLANG_VERSION 2026.22)
# slangc is a host tool: when cross-compiling (iOS, Android) do not look in the target sysroot.
find_program(SLANGC slangc HINTS $ENV{HOME}/.cache/ace/slang/bin ${CMAKE_BINARY_DIR}/_slang/bin NO_CMAKE_FIND_ROOT_PATH)
if(NOT SLANGC)
  if(CMAKE_HOST_APPLE)
    set(_slang_pkg macos-aarch64)
  elseif(CMAKE_HOST_WIN32)
    set(_slang_pkg windows-x86_64)
  else()
    set(_slang_pkg linux-x86_64)
  endif()
  message(STATUS "Downloading Slang ${ACE_SLANG_VERSION} (${_slang_pkg})")
  FetchContent_Declare(slang_bin
    URL https://github.com/shader-slang/slang/releases/download/v${ACE_SLANG_VERSION}/slang-${ACE_SLANG_VERSION}-${_slang_pkg}.tar.gz
    SOURCE_DIR ${CMAKE_BINARY_DIR}/_slang)
  FetchContent_MakeAvailable(slang_bin)
  find_program(SLANGC slangc HINTS ${CMAKE_BINARY_DIR}/_slang/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
endif()
message(STATUS "slangc: ${SLANGC}")

# ace_compile_shaders(<target> <out_dir> <shader.slang>...)
function(ace_compile_shaders target out_dir)
  set(outputs)
  file(GLOB _includes ${CMAKE_SOURCE_DIR}/engine/shaders/*.slangh)
  foreach(src ${ARGN})
    get_filename_component(name ${src} NAME_WE)
    foreach(stage vs fs)
      if(stage STREQUAL vs)
        set(entry vsMain)
        set(slang_stage vertex)
      else()
        set(entry fsMain)
        set(slang_stage fragment)
      endif()
      set(out ${out_dir}/${name}.${stage}.spv)
      add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
        COMMAND ${SLANGC} ${src} -target spirv -profile spirv_1_3 -entry ${entry} -stage ${slang_stage}
                -matrix-layout-column-major -O2 -o ${out}
        DEPENDS ${src} ${_includes} VERBATIM
        COMMENT "slangc ${name}.${stage}")
      list(APPEND outputs ${out})
      if(WIN32) # DXIL for the D3D12 backend; slangc ships dxcompiler on Windows.
        set(out_dx ${out_dir}/${name}.${stage}.dxil)
        add_custom_command(OUTPUT ${out_dx}
          COMMAND ${SLANGC} ${src} -target dxil -profile sm_6_0 -entry ${entry} -stage ${slang_stage}
                  -matrix-layout-column-major -O2 -o ${out_dx}
          DEPENDS ${src} ${_includes} VERBATIM)
        list(APPEND outputs ${out_dx})
      endif()
    endforeach()
  endforeach()
  add_custom_target(${target} ALL DEPENDS ${outputs})
endfunction()
