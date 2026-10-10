cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(plugin_crypto_tests C CXX)
  enable_testing()
  set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
endif()
# Reuse local, already-installed wolfSSL SHA primitives only. No FetchContent,
# package installation, SDK changes or dependency/TLS settings modifications.
set(PLUGIN_CRYPTO_WOLF_ROOT "${REPO_ROOT}/.pio/libdeps/default/Arduino-wolfSSL/src"
    CACHE PATH "Local wolfSSL source containing wolfcrypt/src/sha.c")
if(NOT EXISTS "${PLUGIN_CRYPTO_WOLF_ROOT}/wolfcrypt/src/sha.c")
  message(FATAL_ERROR "Set PLUGIN_CRYPTO_WOLF_ROOT to the installed local wolfSSL src directory")
endif()
add_library(plugin_crypto_test_sha STATIC
  ${PLUGIN_CRYPTO_WOLF_ROOT}/wolfcrypt/src/sha.c
  ${PLUGIN_CRYPTO_WOLF_ROOT}/wolfcrypt/src/sha256.c
  ${PLUGIN_CRYPTO_WOLF_ROOT}/wolfcrypt/src/des3.c
  ${PLUGIN_CRYPTO_WOLF_ROOT}/wolfcrypt/src/rc2.c
  ${PLUGIN_CRYPTO_WOLF_ROOT}/wolfcrypt/src/memory.c)
target_include_directories(plugin_crypto_test_sha PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/host_config ${PLUGIN_CRYPTO_WOLF_ROOT})
target_compile_definitions(plugin_crypto_test_sha PUBLIC WOLFSSL_USER_SETTINGS WOLFSSL_OPTIONS_H)
add_executable(PluginCryptoPolicyTest PluginCryptoPolicyTest.cpp)
target_compile_features(PluginCryptoPolicyTest PRIVATE cxx_std_20)
target_include_directories(PluginCryptoPolicyTest PRIVATE ${REPO_ROOT}/src)
target_link_libraries(PluginCryptoPolicyTest PRIVATE plugin_crypto_test_sha)
add_test(NAME PluginCryptoPolicyTest COMMAND PluginCryptoPolicyTest)
