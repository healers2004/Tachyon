# Static library: an archive of compiled .cpp files that programs link against.
add_library(horizon
  src/crc32.cpp
  src/engine.cpp
  src/log.cpp)
# PUBLIC: Horizon and everything that links it can #include "horizon/...".
# PRIVATE: only Horizon's own files see src/, so platform/file.h stays internal.
target_include_directories(horizon PUBLIC include PRIVATE src)

# One file implementation per OS, behind the same platform/file.h.
if(WIN32)
  target_sources(horizon PRIVATE src/platform/win32/file.cpp)
else()
  target_sources(horizon PRIVATE src/platform/posix/file.cpp)
endif()

# Test program: our tests + Horizon + GoogleTest's ready-made main().
add_executable(horizon_tests
  tests/crc32_test.cpp
  tests/engine_test.cpp
  tests/log_test.cpp)
target_link_libraries(horizon_tests PRIVATE horizon GTest::gtest_main)
# Registers each TEST(...) with ctest.
gtest_discover_tests(horizon_tests)
