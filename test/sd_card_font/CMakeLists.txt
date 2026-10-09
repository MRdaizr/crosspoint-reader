add_executable(SdCardFontTest
  SdCardFontTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(SdCardFontTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
)
target_link_libraries(SdCardFontTest PRIVATE crosspoint_test_common GTest::gtest_main)
if(MSVC)
  # The firmware header is included before HalStorage in SdCardFont.cpp.
  target_compile_options(SdCardFontTest PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/stubs/HalStorage.h)
else()
  target_compile_options(SdCardFontTest PRIVATE -fno-exceptions)
endif()
gtest_discover_tests(SdCardFontTest)
