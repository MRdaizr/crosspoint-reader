if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.16)
  project(crosspoint_txt_tests C CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
  include(FetchContent)
  FetchContent_Declare(googletest GIT_REPOSITORY https://github.com/google/googletest.git GIT_TAG v1.17.0)
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
if(NOT TARGET txt_test_runtime)
  add_library(txt_test_runtime STATIC
    ${REPO_ROOT}/lib/Txt/TxtToHtml.cpp
    ${REPO_ROOT}/lib/Txt/TxtSourceMap.cpp
    ${REPO_ROOT}/src/util/TxtProgressBridge.cpp)
  target_include_directories(txt_test_runtime PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/stubs
    ${REPO_ROOT}/test/koreader_xpath_resolver/stubs
    ${REPO_ROOT}/test/css_parser/stubs
    ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/Txt ${REPO_ROOT}/src)
  target_link_libraries(txt_test_runtime PUBLIC crosspoint_test_common)
  if(NOT MSVC)
    target_compile_options(txt_test_runtime PRIVATE -fno-exceptions -fno-rtti)
  endif()
endif()
if(NOT TARGET txt_test_xml)
  add_library(txt_test_xml STATIC
    ${REPO_ROOT}/lib/expat/xmlparse.c ${REPO_ROOT}/lib/expat/xmlrole.c ${REPO_ROOT}/lib/expat/xmltok.c)
  target_include_directories(txt_test_xml PUBLIC ${REPO_ROOT}/lib/expat)
  target_compile_definitions(txt_test_xml PUBLIC XML_CONTEXT_BYTES=1024 XML_GE=0)
endif()
