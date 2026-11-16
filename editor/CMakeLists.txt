add_executable(ace_editor main.cpp editor.cpp imgui_rhi.cpp)
target_link_libraries(ace_editor PRIVATE ace_engine imgui)
