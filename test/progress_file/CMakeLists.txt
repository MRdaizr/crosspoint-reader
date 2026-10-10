add_executable(ProgressFileTest ProgressFileTest.cpp)
target_include_directories(ProgressFileTest PRIVATE
  ${REPO_ROOT}/test/txt_to_html/stubs
  ${REPO_ROOT}/test/koreader_xpath_resolver/stubs
  ${REPO_ROOT}/test/css_parser/stubs
  ${REPO_ROOT}/src/activities/reader)
target_link_libraries(ProgressFileTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ProgressFileTest)
