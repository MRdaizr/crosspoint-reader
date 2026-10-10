cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(plugin_core_tests C CXX)
  enable_testing()
  set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
endif()
add_executable(PluginCoreTest PluginCoreTest.cpp
  ${REPO_ROOT}/lib/ReaderSession/ReaderSession.cpp)
target_compile_features(PluginCoreTest PRIVATE cxx_std_20)
target_include_directories(PluginCoreTest PRIVATE ${REPO_ROOT}/src ${REPO_ROOT}/lib/ReaderSession)
add_test(NAME PluginCoreTest COMMAND PluginCoreTest)
add_executable(PluginTemplateTest PluginTemplateTest.cpp)
target_compile_features(PluginTemplateTest PRIVATE cxx_std_20)
target_include_directories(PluginTemplateTest PRIVATE ${REPO_ROOT}/src)
add_test(NAME PluginTemplateTest COMMAND PluginTemplateTest)

# Compile the same bounded adapters and XML entity-disabled engine as firmware.
add_library(plugin_test_expat STATIC ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c ${REPO_ROOT}/lib/expat/xmltok.c)
target_include_directories(plugin_test_expat PUBLIC ${REPO_ROOT}/lib/expat)
target_compile_definitions(plugin_test_expat PRIVATE XML_GE=0 XML_CONTEXT_BYTES=1024 BYTEORDER=1234)
add_executable(PluginParserTest PluginParserTest.cpp
  ${REPO_ROOT}/src/activities/plugins/JsonListParser.cpp
  ${REPO_ROOT}/src/activities/plugins/XmlListParser.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp)
target_compile_features(PluginParserTest PRIVATE cxx_std_20)
target_include_directories(PluginParserTest PRIVATE stubs ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/JsonParser)
target_link_libraries(PluginParserTest PRIVATE plugin_test_expat)
add_test(NAME PluginParserTest COMMAND PluginParserTest)
find_program(PLUGIN_NODE NAMES node nodejs)
if(PLUGIN_NODE)
  add_test(NAME PluginHostTest COMMAND ${PLUGIN_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/PluginHostTest.cjs)
  add_test(NAME LibraryBrowserTest COMMAND ${PLUGIN_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/LibraryBrowserTest.cjs)
endif()
