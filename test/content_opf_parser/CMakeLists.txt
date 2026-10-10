# This subtree can run in isolation while other agents work on the firmware.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.16)
  project(crosspoint_sync_metadata_tests C CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  add_library(crosspoint_test_common INTERFACE)
  if(NOT MSVC)
    target_compile_options(crosspoint_test_common INTERFACE -Wall -Wextra -pedantic)
  endif()
  enable_testing()
  include(GoogleTest)
endif()

add_executable(ContentOpfParserTest
  ContentOpfParserTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContentOpfParser.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c)
target_include_directories(ContentOpfParserTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/koreader_xpath_resolver/stubs
  ${REPO_ROOT}/test/css_parser/stubs
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/expat)
target_compile_definitions(ContentOpfParserTest PRIVATE XML_CONTEXT_BYTES=1024 XML_GE=0)
target_link_libraries(ContentOpfParserTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ContentOpfParserTest)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_subdirectory(../koreader_sync_profile koreader_sync_profile)
endif()
