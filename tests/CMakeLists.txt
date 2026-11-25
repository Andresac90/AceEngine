add_executable(ace_tests tests.cpp)
target_link_libraries(ace_tests PRIVATE ace_engine)
target_include_directories(ace_tests PRIVATE ${CMAKE_SOURCE_DIR}/editor) # header-only UI helpers
add_test(NAME ace_tests COMMAND ace_tests)
