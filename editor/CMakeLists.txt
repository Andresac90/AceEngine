add_executable(ace_editor main.cpp editor.cpp imgui_rhi.cpp projects.cpp)
target_link_libraries(ace_editor PRIVATE ace_engine imgui)
# The editor is a development tool run from the source tree; it finds tools/ through this.
target_compile_definitions(ace_editor PRIVATE ACE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
