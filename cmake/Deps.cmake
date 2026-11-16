# Third-party dependencies. Everything is pinned and fetched into the build tree;
# nothing is vendored in the repo.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# Declares a dependency whose own CMakeLists we do not want to run (header-only
# libraries, or ones we compile ourselves with a few lines below).
macro(ace_fetch_sources name repo tag)
  FetchContent_Declare(${name} GIT_REPOSITORY ${repo} GIT_TAG ${tag} GIT_SHALLOW ON
    SOURCE_SUBDIR _ace_no_cmake_)
  FetchContent_MakeAvailable(${name})
endmacro()

# --- SDL3: window, input, audio, native surface handles ----------------------
option(ACE_FETCH_SDL "Build SDL3 from source instead of using an installed one" OFF)
if(NOT ACE_FETCH_SDL AND NOT IOS AND NOT ANDROID)
  find_package(SDL3 CONFIG QUIET)
endif()
if(NOT SDL3_FOUND)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.20 GIT_SHALLOW ON)
  FetchContent_MakeAvailable(SDL3)
endif()

# --- Header-only ------------------------------------------------------------
ace_fetch_sources(glm https://github.com/g-truc/glm.git 1.0.1)
add_library(glm INTERFACE)
target_include_directories(glm SYSTEM INTERFACE ${glm_SOURCE_DIR})
target_compile_definitions(glm INTERFACE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_ENABLE_EXPERIMENTAL)

ace_fetch_sources(entt https://github.com/skypjack/entt.git v3.13.2)
add_library(entt INTERFACE)
target_include_directories(entt SYSTEM INTERFACE ${entt_SOURCE_DIR}/single_include)

ace_fetch_sources(json https://github.com/nlohmann/json.git v3.11.3)
add_library(json INTERFACE)
target_include_directories(json SYSTEM INTERFACE ${json_SOURCE_DIR}/single_include)

ace_fetch_sources(stb https://github.com/nothings/stb.git master)
ace_fetch_sources(cgltf https://github.com/jkuhlmann/cgltf.git v1.14)
add_library(stb_cgltf INTERFACE)
target_include_directories(stb_cgltf SYSTEM INTERFACE ${stb_SOURCE_DIR} ${cgltf_SOURCE_DIR})

# --- Lua --------------------------------------------------------------------
ace_fetch_sources(lua https://github.com/lua/lua.git v5.4.7)
file(GLOB LUA_SOURCES ${lua_SOURCE_DIR}/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|onelua|ltests)\\.c$")
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua SYSTEM PUBLIC ${lua_SOURCE_DIR})
if(IOS)
  target_compile_definitions(lua PRIVATE "system(s)=(-1)") # os.execute is unavailable on iOS
endif()

# --- SPIRV-Cross: SPIR-V -> Metal Shading Language at load time (Apple only) ---
if(ACE_RHI_METAL)
  set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(spirv_cross GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
    GIT_TAG vulkan-sdk-1.4.363.0 GIT_SHALLOW ON)
  FetchContent_MakeAvailable(spirv_cross)
endif()

# --- Editor UI ----------------------------------------------------------------
if(ACE_BUILD_EDITOR)
  ace_fetch_sources(imgui https://github.com/ocornut/imgui.git v1.91.9b-docking)
  ace_fetch_sources(imguizmo https://github.com/CedricGuillemet/ImGuizmo.git master) # sources live in src/
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp)
  target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp ${imguizmo_SOURCE_DIR}/src)
  target_link_libraries(imgui PUBLIC SDL3::SDL3)
endif()
