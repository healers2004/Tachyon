# Everything except main(), so tests can link it without starting the server.
add_library(tachyon_core
  src/commands/command_log.cpp
  src/commands/commands.cpp
  src/commands/hash_commands.cpp
  src/commands/key_commands.cpp
  src/commands/list_commands.cpp
  src/commands/store.cpp
  src/commands/string_commands.cpp
  src/protocol/resp_parser.cpp
  src/protocol/resp_writer.cpp
  src/server/client_commands.cpp
  src/server/connection.cpp
  src/server/pubsub.cpp
  src/server/server.cpp)
# Lets code write #include "protocol/resp_parser.h".
target_include_directories(tachyon_core PUBLIC src)
# The storage engine. PUBLIC: code that includes tachyon headers also sees horizon/log.h.
target_link_libraries(tachyon_core PUBLIC horizon)

# One socket implementation per OS, behind the same platform/socket.h.
if(WIN32)
  target_sources(tachyon_core PRIVATE src/platform/win32/socket.cpp)
  target_link_libraries(tachyon_core PUBLIC ws2_32)  # the Winsock library
else()
  target_sources(tachyon_core PRIVATE src/platform/posix/socket.cpp)
endif()

# The server executable.
add_executable(tachyon src/main.cpp)
target_link_libraries(tachyon PRIVATE tachyon_core)

add_executable(tachyon_tests
  tests/command_log_test.cpp
  tests/commands_test.cpp
  tests/connection_test.cpp
  tests/expiry_test.cpp
  tests/hash_commands_test.cpp
  tests/list_commands_test.cpp
  tests/pubsub_test.cpp
  tests/resp_parser_test.cpp
  tests/resp_writer_test.cpp
  tests/socket_test.cpp
  tests/store_test.cpp)
target_link_libraries(tachyon_tests PRIVATE tachyon_core GTest::gtest_main)
gtest_discover_tests(tachyon_tests)
