# Everything except main(), so tests can link it without starting the server.
add_library(tachyon_core
  src/commands/commands.cpp
  src/commands/store.cpp
  src/protocol/resp_parser.cpp
  src/protocol/resp_writer.cpp
  src/server/connection.cpp
  src/server/server.cpp)
# Lets code write #include "protocol/resp_parser.h".
target_include_directories(tachyon_core PUBLIC src)

# One socket implementation per OS, behind the same platform/socket.h.
if(WIN32)
  target_sources(tachyon_core PRIVATE src/platform/win32/socket.cpp)
  target_link_libraries(tachyon_core PUBLIC ws2_32)  # the Winsock library
else()
  target_sources(tachyon_core PRIVATE src/platform/posix/socket.cpp)
endif()

# The server executable. Doesn't link Horizon yet; storage arrives in Phase 3.
add_executable(tachyon src/main.cpp)
target_link_libraries(tachyon PRIVATE tachyon_core)

add_executable(tachyon_tests
  tests/commands_test.cpp
  tests/connection_test.cpp
  tests/resp_parser_test.cpp
  tests/resp_writer_test.cpp
  tests/socket_test.cpp
  tests/store_test.cpp)
target_link_libraries(tachyon_tests PRIVATE tachyon_core GTest::gtest_main)
gtest_discover_tests(tachyon_tests)
