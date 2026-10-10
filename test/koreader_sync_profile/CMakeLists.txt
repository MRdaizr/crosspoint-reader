find_path(KOREADER_TEST_ARDUINOJSON ArduinoJson.h
  PATHS ${REPO_ROOT}/.pio/libdeps/default/ArduinoJson/src
        ${REPO_ROOT}/.pio/libdeps/gh_release/ArduinoJson/src
        ${REPO_ROOT}/.pio/libdeps/slim/ArduinoJson/src
  NO_DEFAULT_PATH)
if(NOT KOREADER_TEST_ARDUINOJSON)
  include(FetchContent)
  FetchContent_Declare(koreader_test_arduinojson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG v7.4.2)
  FetchContent_MakeAvailable(koreader_test_arduinojson)
  set(KOREADER_TEST_ARDUINOJSON "${koreader_test_arduinojson_SOURCE_DIR}/src")
endif()

add_executable(KOReaderSyncProfileTest
  KOReaderSyncProfileTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/KOReaderCredentialStore.cpp
  ${REPO_ROOT}/lib/KOReaderSync/KOReaderSyncClient.cpp
  ${REPO_ROOT}/lib/KOReaderSync/KOReaderSyncPayload.cpp)
target_include_directories(KOReaderSyncProfileTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/content_opf_parser/stubs
  ${REPO_ROOT}/test/koreader_xpath_resolver/stubs
  ${REPO_ROOT}/test/css_parser/stubs
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/Serialization
  ${KOREADER_TEST_ARDUINOJSON})
target_link_libraries(KOReaderSyncProfileTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(KOReaderSyncProfileTest)
