cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

FetchContent_Declare(
  crosspoint_simulator
  GIT_REPOSITORY https://github.com/0x1abin/crosspoint-simulator.git
  GIT_TAG 6058c3da013fbe1579d41c7c5cc77cd466d37f12
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest crosspoint_simulator)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
)
if(MSVC)
  target_compile_options(crosspoint_test_common INTERFACE /W4 /permissive- /utf-8)
else()
  target_compile_options(crosspoint_test_common INTERFACE
    -Wall
    -Wextra
    -pedantic
  )
endif()

enable_testing()
include(GoogleTest)

# Build-time font sharing is pure Python and must not introduce runtime RAM use.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME BuildScriptTests
    COMMAND ${Python3_EXECUTABLE} -B -m unittest discover
      -s ${CMAKE_CURRENT_SOURCE_DIR}/build_scripts -v)
endif()

add_subdirectory(streaming_json_parser)
add_subdirectory(release_json_parser)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(utf8_compose)
add_subdirectory(weread_webapi)
add_subdirectory(airpage)
add_subdirectory(opds_filename)
add_subdirectory(html_to_plain_text)
add_subdirectory(minibidi_arabic)
add_subdirectory(combining_marks)
add_subdirectory(credential_integrity)
add_subdirectory(xml_parser_utils)
add_subdirectory(token_boundary)
add_subdirectory(font_cache_manager)
add_subdirectory(css_parser)
add_subdirectory(inflate_stream)
add_subdirectory(chapter_position)
add_subdirectory(protected_paths)
add_subdirectory(reader_resume_gate)
add_subdirectory(koreader_xpath_resolver)
add_subdirectory(sd_card_font)
add_subdirectory(parsed_text)
add_subdirectory(word_store)
add_subdirectory(visible_offset_lut)
add_subdirectory(button_navigation)
add_subdirectory(recent_books)
add_subdirectory(fs_helpers)
add_subdirectory(airpage_wallpaper)
add_subdirectory(text_spacing)
add_subdirectory(content_opf_parser)
add_subdirectory(koreader_sync_profile)
add_subdirectory(library_builder)
add_subdirectory(library_format)
add_subdirectory(library_index_file)
add_subdirectory(library_text)
add_subdirectory(page_link)
add_subdirectory(txt_to_html)
add_subdirectory(txt_source_map)
add_subdirectory(txt_progress_bridge)
add_subdirectory(plugins)
add_subdirectory(reader_session)
add_subdirectory(plugin_manifest_classification)
add_subdirectory(progress_file)
add_subdirectory(library_web_api)
add_subdirectory(book_cache_utils)
if(NOT MSVC AND NOT APPLE)
  add_subdirectory(deadline_dns)
endif()
if(PLUGIN_CRYPTO_WOLF_ROOT OR EXISTS "${REPO_ROOT}/.pio/libdeps/default/Arduino-wolfSSL/src/wolfcrypt/src/sha.c")
  add_subdirectory(plugin_crypto)
else()
  message(STATUS "PluginCryptoPolicyTest requires PlatformIO dependencies or PLUGIN_CRYPTO_WOLF_ROOT")
endif()
