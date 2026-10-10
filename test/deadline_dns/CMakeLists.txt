cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(crosspoint_deadline_dns_regression LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  enable_testing()
  include(GoogleTest)
  set(GTEST_SOURCE_DIR "" CACHE PATH "Existing googletest checkout root (no fetch)")
  if(GTEST_SOURCE_DIR)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    add_subdirectory("${GTEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest")
  else()
    find_package(GTest REQUIRED)
  endif()
  add_library(crosspoint_test_common INTERFACE)
  target_compile_options(crosspoint_test_common INTERFACE -Wall -Wextra -pedantic)
endif()

# The test exercises the actual Itanium ABI/link-wrap path, not a reimplemented
# state machine. Run with GCC/Clang on Linux/WSL; MSVC has a different C++ ABI.
if(MSVC OR APPLE)
  message(FATAL_ERROR "DeadlineDnsTest requires a GNU-compatible ELF --wrap linker (Linux/WSL)")
endif()
add_executable(DeadlineDnsTest
  DeadlineDnsTest.cpp
  FakePlatform.cpp
  ${REPO_ROOT}/src/network/DeadlineDns.cpp
)
target_include_directories(DeadlineDnsTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/src/network
)
target_link_libraries(DeadlineDnsTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_link_options(DeadlineDnsTest PRIVATE -Wl,--wrap=_ZN14NetworkManager10hostByNameEPKcR9IPAddress)
option(DEADLINE_DNS_SANITIZERS "ASAN + UBSAN on DeadlineDnsTest" OFF)
if(DEADLINE_DNS_SANITIZERS)
  target_compile_options(DeadlineDnsTest PRIVATE -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie)
  target_link_options(DeadlineDnsTest PRIVATE -fsanitize=address,undefined -no-pie)
endif()
gtest_discover_tests(DeadlineDnsTest DISCOVERY_TIMEOUT 60)
