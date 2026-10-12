add_executable(ReaderLinkStackTest ReaderLinkStackTest.cpp)
target_link_libraries(ReaderLinkStackTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ReaderLinkStackTest)

# Exercise the production file methods with the existing failure-injection storage.
set(LINK_SOURCE "${REPO_ROOT}/src/activities/reader/EpubReaderActivity.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LINK_SOURCE}")
file(READ "${LINK_SOURCE}" LINK_TEXT)
string(FIND "${LINK_TEXT}" "bool EpubReaderActivity::saveLinkStack()" LINK_START)
string(FIND "${LINK_TEXT}" "void EpubReaderActivity::restoreSavedPosition()" LINK_END)
if(LINK_START LESS 0 OR LINK_END LESS LINK_START)
  message(FATAL_ERROR "Reader link-stack method boundaries changed")
endif()
math(EXPR LINK_LENGTH "${LINK_END} - ${LINK_START}")
string(SUBSTRING "${LINK_TEXT}" ${LINK_START} ${LINK_LENGTH} LINK_METHODS)
configure_file(ReaderLinkStackFileTest.cpp.in ReaderLinkStackFileTest.cpp @ONLY)
add_executable(ReaderLinkStackFileTest "${CMAKE_CURRENT_BINARY_DIR}/ReaderLinkStackFileTest.cpp")
target_include_directories(ReaderLinkStackFileTest PRIVATE
  ${REPO_ROOT}/test/txt_to_html/stubs
  ${REPO_ROOT}/test/koreader_xpath_resolver/stubs
  ${REPO_ROOT}/test/css_parser/stubs)
target_link_libraries(ReaderLinkStackFileTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ReaderLinkStackFileTest)
