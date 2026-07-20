add_executable(ShushanGameTest
  ShushanGameTest.cpp
  ${REPO_ROOT}/src/games/shushan/ShushanGame.cpp
  ${REPO_ROOT}/src/games/shushan/ShushanGameSaveCodec.cpp
)

target_include_directories(ShushanGameTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/games/shushan
)

target_link_libraries(ShushanGameTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  ArduinoJson
)

gtest_discover_tests(ShushanGameTest)
