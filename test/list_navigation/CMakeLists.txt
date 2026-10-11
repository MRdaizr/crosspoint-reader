# Test the production input and viewport methods with a simulated slow render
# lock, without pulling in the device, filesystem, and font dependencies.
function(extract_navigation_method output source start_marker end_marker)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
  file(READ "${source}" source_text)
  string(FIND "${source_text}" "${start_marker}" start)
  string(FIND "${source_text}" "${end_marker}" end)
  if(start LESS 0 OR end LESS start)
    message(FATAL_ERROR "Navigation method boundaries changed in ${source}")
  endif()
  math(EXPR length "${end} - ${start}")
  string(SUBSTRING "${source_text}" ${start} ${length} method)
  set(${output} "${method}" PARENT_SCOPE)
endfunction()

extract_navigation_method(MOVE_SELECTION "${REPO_ROOT}/src/activities/UiListActivity.cpp"
  "void UiListActivity::moveSelectionTo(" "int UiListActivity::prewarmVisibleListRowsIfNeeded(")
extract_navigation_method(NAVIGATE_BUTTONS "${REPO_ROOT}/src/activities/UiListActivity.cpp"
  "void UiListActivity::navigateButtons(" "void UiListActivity::syncListViewport(")
extract_navigation_method(SYNC_TAB_VIEWPORT "${REPO_ROOT}/src/activities/UiTabListActivity.cpp"
  "void UiTabListActivity::syncTabListViewport(" "void UiTabListActivity::buildTabBar(")
extract_navigation_method(SYNC_LIST_VIEWPORT "${REPO_ROOT}/src/activities/UiListActivity.cpp"
  "void UiListActivity::syncListViewport(" "void UiListActivity::drawChrome(")
extract_navigation_method(NAVIGATE_RING "${REPO_ROOT}/src/activities/UiTabListActivity.cpp"
  "void UiTabListActivity::moveRingTo(" "void UiTabListActivity::syncTabListViewport(")
extract_navigation_method(FILE_CUSTOM_INPUT "${REPO_ROOT}/src/activities/home/FileBrowserActivity.cpp"
  "bool FileBrowserActivity::handleCustomInput(" "bool FileBrowserActivity::handleButtons(")
extract_navigation_method(LIBRARY_BUTTONS "${REPO_ROOT}/src/activities/library/LibraryListActivity.cpp"
  "void LibraryListActivity::navigateButtons(" "bool LibraryListActivity::handleButtons(")
configure_file(ListNavigationTest.cpp.in ListNavigationTest.cpp @ONLY)

add_executable(ListNavigationTest "${CMAKE_CURRENT_BINARY_DIR}/ListNavigationTest.cpp"
  ${REPO_ROOT}/src/util/ButtonNavigator.cpp)
target_include_directories(ListNavigationTest PRIVATE
  ${REPO_ROOT}/test/button_navigation/stubs ${REPO_ROOT}/src/util
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include)
target_link_libraries(ListNavigationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ListNavigationTest)
