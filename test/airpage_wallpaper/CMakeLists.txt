add_executable(AirPageWallpaperTest AirPageWallpaperTest.cpp
  ${REPO_ROOT}/src/activities/network/airpage/AirPageWallpaper.cpp)
target_include_directories(AirPageWallpaperTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${REPO_ROOT}/src/activities/network/airpage)
target_link_libraries(AirPageWallpaperTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(AirPageWallpaperTest)
