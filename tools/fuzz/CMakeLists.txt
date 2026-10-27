# libFuzzer targets (Clang only). Built when the project is configured with -DTACHYON_FUZZ=ON;
# tools/fuzz.sh sets the compiler and sanitizer flags and runs them.
add_executable(fuzz_resp_parser fuzz_resp_parser.cpp)
target_link_libraries(fuzz_resp_parser PRIVATE tachyon_core)

add_executable(fuzz_log_reader fuzz_log_reader.cpp)
target_link_libraries(fuzz_log_reader PRIVATE horizon)

# -fsanitize=fuzzer links libFuzzer's main(), which calls LLVMFuzzerTestOneInput in a loop.
foreach(target fuzz_resp_parser fuzz_log_reader)
  target_link_options(${target} PRIVATE -fsanitize=fuzzer)
endforeach()
