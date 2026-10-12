add_executable(ReleaseJsonParserTest
  ReleaseJsonParserTest.cpp
  ${REPO_ROOT}/lib/JsonParser/ReleaseJsonParser.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(ReleaseJsonParserTest PRIVATE
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(ReleaseJsonParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReleaseJsonParserTest)

set(OTA_SOURCE "${REPO_ROOT}/src/network/OtaUpdater.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${OTA_SOURCE}")
file(READ "${OTA_SOURCE}" OTA_TEXT)
string(FIND "${OTA_TEXT}" "bool OtaUpdater::isUpdateNewer()" VERSION_START)
string(FIND "${OTA_TEXT}" "const std::string& OtaUpdater::getLatestVersion()" VERSION_END)
if(VERSION_START LESS 0 OR VERSION_END LESS VERSION_START)
  message(FATAL_ERROR "OTA version method boundaries changed")
endif()
math(EXPR VERSION_LENGTH "${VERSION_END} - ${VERSION_START}")
string(SUBSTRING "${OTA_TEXT}" ${VERSION_START} ${VERSION_LENGTH} VERSION_METHOD)
configure_file(OtaVersionTest.cpp.in OtaVersionTest.cpp @ONLY)
add_executable(OtaVersionTest "${CMAKE_CURRENT_BINARY_DIR}/OtaVersionTest.cpp")
target_include_directories(OtaVersionTest PRIVATE ${REPO_ROOT}/test/css_parser/stubs)
target_link_libraries(OtaVersionTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(OtaVersionTest)
