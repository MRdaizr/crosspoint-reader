cmake_minimum_required(VERSION 3.16)

# May be added by the parent, or configured alone without the simulator/firmware.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(crosspoint_quality_regression LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  enable_testing()
  include(GoogleTest)
  set(GTEST_SOURCE_DIR "" CACHE PATH "Existing googletest source checkout (no fetch)")
  if(GTEST_SOURCE_DIR)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    add_subdirectory("${GTEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest")
  else()
    find_package(GTest REQUIRED)
  endif()
  add_library(crosspoint_test_common INTERFACE)
  if(MSVC)
    target_compile_options(crosspoint_test_common INTERFACE /W4 /permissive- /utf-8)
  else()
    target_compile_options(crosspoint_test_common INTERFACE -Wall -Wextra -pedantic)
  endif()
endif()

option(CROSSPOINT_REGRESSION_SANITIZERS "ASAN + UBSAN on the two quality regression targets" OFF)

add_executable(BookCacheUtilsTest
  BookCacheUtilsTest.cpp
  ${REPO_ROOT}/src/util/BookCacheUtils.cpp
)
target_include_directories(BookCacheUtilsTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/util
)

target_link_libraries(BookCacheUtilsTest PRIVATE crosspoint_test_common GTest::gtest_main)
if(CROSSPOINT_REGRESSION_SANITIZERS AND NOT MSVC)
  target_compile_options(BookCacheUtilsTest PRIVATE -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(BookCacheUtilsTest PRIVATE -fsanitize=address,undefined)
  # Avoid ASAN shadow collisions with randomized PIE placement on WSL/Linux.
  if(UNIX AND NOT APPLE)
    target_compile_options(BookCacheUtilsTest PRIVATE -fno-pie)
    target_link_options(BookCacheUtilsTest PRIVATE -no-pie)
  endif()
endif()
gtest_discover_tests(BookCacheUtilsTest DISCOVERY_TIMEOUT 60)
