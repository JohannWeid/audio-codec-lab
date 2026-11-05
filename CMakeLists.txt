cmake_minimum_required(VERSION 3.25)
project(audio_codec_lab VERSION 0.1.0 LANGUAGES CXX)

option(CODEC_WERROR "Treat compiler warnings as errors" OFF)
option(CODEC_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT is_multi_config AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Set before GoogleTest is added, so that the test framework is instrumented as well.
if(CODEC_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "CODEC_SANITIZE is only supported with GCC and Clang.")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Settings for our own targets only. The floating-point flags are part of the determinism rules in
# spec/codec-spec.md: compilers must not fuse a * b + c, because that changes results between
# platforms. -ffast-math must never be added.
add_library(codec_options INTERFACE)
if(MSVC)
  target_compile_options(codec_options INTERFACE
    /W4 /permissive- /fp:precise
    $<$<BOOL:${CODEC_WERROR}>:/WX>)
else()
  target_compile_options(codec_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -ffp-contract=off
    $<$<BOOL:${CODEC_WERROR}>:-Werror>)
endif()

add_library(codec
  src/codec/version.cpp
  src/codec/wav.cpp
)
target_include_directories(codec PUBLIC src)
target_link_libraries(codec PRIVATE codec_options)
target_compile_definitions(codec PRIVATE CODEC_VERSION="${PROJECT_VERSION}")

foreach(tool IN ITEMS encode decode)
  add_executable(codec-${tool} src/tools/codec_${tool}.cpp)
  target_link_libraries(codec-${tool} PRIVATE codec codec_options)
endforeach()

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
  URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
  SYSTEM
)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)
add_executable(codec_tests
  tests/test_floating_point.cpp
  tests/test_version.cpp
  tests/test_wav.cpp
)
target_link_libraries(codec_tests PRIVATE codec codec_options GTest::gtest_main)
gtest_discover_tests(codec_tests)
