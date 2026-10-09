add_executable(WordStoreTest WordStoreTest.cpp ${REPO_ROOT}/lib/Epub/Epub/WordStore.cpp)
target_include_directories(WordStoreTest PRIVATE
  ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/parsed_text/stubs)
target_link_libraries(WordStoreTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(WordStoreTest)
