# Static library: an archive of compiled .cpp files that programs link against.
add_library(horizon src/version.cpp)
# PUBLIC: Horizon and everything that links it can #include "horizon/...".
target_include_directories(horizon PUBLIC include)

# Test program: our tests + Horizon + GoogleTest's ready-made main().
add_executable(horizon_tests tests/version_test.cpp)
target_link_libraries(horizon_tests PRIVATE horizon GTest::gtest_main)
# Registers each TEST(...) with ctest.
gtest_discover_tests(horizon_tests)
