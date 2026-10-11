set(SERIALIZATION_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/txt_to_html/stubs
  ${REPO_ROOT}/test/koreader_xpath_resolver/stubs
  ${REPO_ROOT}/test/css_parser/stubs
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Epub/Epub)

add_executable(SerializationTest SerializationTest.cpp)
target_include_directories(SerializationTest PRIVATE ${SERIALIZATION_INCLUDES})
target_link_libraries(SerializationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(SerializationTest)

add_executable(BookMetadataCacheTest BookMetadataCacheTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp)
target_include_directories(BookMetadataCacheTest PRIVATE ${SERIALIZATION_INCLUDES})
target_link_libraries(BookMetadataCacheTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BookMetadataCacheTest)

add_executable(TextBlockStorageTest TextBlockStorageTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp)
target_include_directories(TextBlockStorageTest PRIVATE
  ${REPO_ROOT}/test/parsed_text/stubs
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/EpdFont
  ${SERIALIZATION_INCLUDES})
target_link_libraries(TextBlockStorageTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TextBlockStorageTest)

# Exercise the production JSON adapter with the same ArduinoJson version as
# firmware, without requiring a PlatformIO build in the host-test CI job.
if(EXISTS "${REPO_ROOT}/.pio/libdeps/default/ArduinoJson/src/ArduinoJson.h")
  set(JSON_TEST_INCLUDE "${REPO_ROOT}/.pio/libdeps/default/ArduinoJson/src")
else()
  FetchContent_Declare(serialization_arduinojson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG v7.4.2)
  FetchContent_MakeAvailable(serialization_arduinojson)
  set(JSON_TEST_INCLUDE "${serialization_arduinojson_SOURCE_DIR}/src")
endif()
add_executable(PersistableStoreTest PersistableStoreTest.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp)
target_include_directories(PersistableStoreTest PRIVATE ${SERIALIZATION_INCLUDES} ${JSON_TEST_INCLUDE})
target_compile_definitions(PersistableStoreTest PRIVATE ARDUINOJSON_ENABLE_ARDUINO_STRING=0
  ARDUINOJSON_ENABLE_ARDUINO_STREAM=0 ARDUINOJSON_ENABLE_PROGMEM=0)
target_link_libraries(PersistableStoreTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PersistableStoreTest)
