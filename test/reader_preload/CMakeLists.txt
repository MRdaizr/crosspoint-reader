# Compile the production preload method with deterministic heap/cache fixtures.
# Like recent_books, extract the method so unrelated hardware rendering paths
# do not need to be linked into a host failure-injection test.
set(PRELOAD_SOURCE "${REPO_ROOT}/src/activities/reader/EpubReaderActivity.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PRELOAD_SOURCE}")
file(READ "${PRELOAD_SOURCE}" PRELOAD_TEXT)
string(FIND "${PRELOAD_TEXT}" "void EpubReaderActivity::silentIndexNextChapterIfNeeded(" PRELOAD_START)
string(FIND "${PRELOAD_TEXT}" "void EpubReaderActivity::advanceNextChapterPreload(" PRELOAD_END)
if(PRELOAD_START LESS 0 OR PRELOAD_END LESS PRELOAD_START)
  message(FATAL_ERROR "EpubReaderActivity preload method boundaries changed")
endif()
math(EXPR PRELOAD_LENGTH "${PRELOAD_END} - ${PRELOAD_START}")
string(SUBSTRING "${PRELOAD_TEXT}" ${PRELOAD_START} ${PRELOAD_LENGTH} PRELOAD_METHOD)
configure_file(ReaderPreloadTest.cpp.in ReaderPreloadTest.cpp @ONLY)
add_executable(ReaderPreloadTest "${CMAKE_CURRENT_BINARY_DIR}/ReaderPreloadTest.cpp")
target_include_directories(ReaderPreloadTest PRIVATE ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/test/css_parser/stubs)
target_link_libraries(ReaderPreloadTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ReaderPreloadTest)
