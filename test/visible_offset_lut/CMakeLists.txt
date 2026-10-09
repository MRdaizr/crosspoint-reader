add_executable(VisibleOffsetLutTest VisibleOffsetLutTest.cpp)
target_include_directories(VisibleOffsetLutTest PRIVATE
  ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/test/parsed_text/stubs)
target_link_libraries(VisibleOffsetLutTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(VisibleOffsetLutTest)
