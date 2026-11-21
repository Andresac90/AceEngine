add_executable(ace_editor main.cpp editor.cpp imgui_rhi.cpp projects.cpp)
target_link_libraries(ace_editor PRIVATE ace_engine imgui)
# The editor is a development tool run from the source tree; it finds tools/ through this.
target_compile_definitions(ace_editor PRIVATE ACE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(WIN32)
  target_sources(ace_editor PRIVATE resources/editor.rc) # icon
  # No console window when started from Explorer (tools/make_editor_windows.ps1).
  # Off by default: development and test runs want the log in the terminal.
  option(ACE_EDITOR_WINDOWED "Build the editor without a console window" OFF)
  set_target_properties(ace_editor PROPERTIES WIN32_EXECUTABLE ${ACE_EDITOR_WINDOWED})
endif()
