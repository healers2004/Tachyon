cmake_minimum_required(VERSION 3.20)
project(Tachyon LANGUAGES CXX)

# Plain C++20 everywhere (-std=c++20, /std:c++20); fail if the compiler can't do it.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# GoogleTest is downloaded at configure time (needs internet once) and built with the project.
include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  GIT_SHALLOW TRUE)
# MSVC: build GoogleTest against the same (DLL) C runtime as our code, or linking fails.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)  # we only use gtest, not the mocking library
FetchContent_MakeAvailable(googletest)

# Lets `ctest` run every registered test in the repo.
enable_testing()
include(GoogleTest)

add_subdirectory(horizon)
add_subdirectory(tachyon)

# libFuzzer targets for the RESP parser and the log reader. Clang only; see tools/fuzz.sh.
option(TACHYON_FUZZ "Build the libFuzzer targets in tools/fuzz" OFF)
if(TACHYON_FUZZ)
  add_subdirectory(tools/fuzz)
endif()
