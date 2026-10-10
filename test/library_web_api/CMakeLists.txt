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

add_executable(LibraryWebApiTest
  LibraryWebApiTest.cpp
  ${REPO_ROOT}/src/network/LibraryWebApi.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryBuilder.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryIndexFile.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryFormat.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryText.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryQuery.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(LibraryWebApiTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/network
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(LibraryWebApiTest PRIVATE crosspoint_test_common GTest::gtest_main)
if(CROSSPOINT_REGRESSION_SANITIZERS AND NOT MSVC)
  target_compile_options(LibraryWebApiTest PRIVATE -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(LibraryWebApiTest PRIVATE -fsanitize=address,undefined)
  # Avoid ASAN shadow collisions with randomized PIE placement on WSL/Linux.
  if(UNIX AND NOT APPLE)
    target_compile_options(LibraryWebApiTest PRIVATE -fno-pie)
    target_link_options(LibraryWebApiTest PRIVATE -no-pie)
  endif()
endif()
gtest_discover_tests(LibraryWebApiTest DISCOVERY_TIMEOUT 60)
