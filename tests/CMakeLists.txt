add_executable(ace_tests tests.cpp)
target_link_libraries(ace_tests PRIVATE ace_engine)
add_test(NAME ace_tests COMMAND ace_tests)
