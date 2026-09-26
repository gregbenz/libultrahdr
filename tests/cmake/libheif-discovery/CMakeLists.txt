cmake_minimum_required(VERSION 3.15)
project(libheif_discovery_regression LANGUAGES CXX)

if(NOT LIBHEIF_MODULE_DIR OR NOT LIBHEIF_TEST_CASE)
  message(FATAL_ERROR "LIBHEIF_MODULE_DIR and LIBHEIF_TEST_CASE are required")
endif()

set(_fixture_root "${CMAKE_CURRENT_BINARY_DIR}/fixture")
set(_package_dir "${_fixture_root}/lib/cmake/libheif")
file(MAKE_DIRECTORY "${_package_dir}")

set(_include_dir "${_fixture_root}/include/${LIBHEIF_TEST_CASE}/libheif")
file(MAKE_DIRECTORY "${_include_dir}")
if(LIBHEIF_TEST_CASE STREQUAL "one-def")
  file(WRITE "${_include_dir}/heif.h" [=[
#ifdef LIBHEIF_TEST_SINGLE
int heif_image_handle_get_gain_map_image_handle();
#endif
]=])
elseif(LIBHEIF_TEST_CASE STREQUAL "multiple-defs")
  file(WRITE "${_include_dir}/heif.h" [=[
#if defined(LIBHEIF_TEST_FIRST) && defined(LIBHEIF_TEST_SECOND)
int heif_image_handle_get_gain_map_image_handle();
#endif
]=])
elseif(LIBHEIF_TEST_CASE STREQUAL "missing-api")
  file(WRITE "${_include_dir}/heif.h" "/* Gain map API intentionally absent. */\n")
else()
  file(WRITE "${_include_dir}/heif.h" "int heif_image_handle_get_gain_map_image_handle();\n")
endif()

file(WRITE "${_package_dir}/libheifConfig.cmake" [=[
add_library(libheif::heif INTERFACE IMPORTED)
set_property(TARGET libheif::heif PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${LIBHEIF_TEST_INCLUDE_DIR}")
if(LIBHEIF_TEST_CASE STREQUAL "empty-defs")
  set_property(TARGET libheif::heif PROPERTY INTERFACE_COMPILE_DEFINITIONS "")
elseif(LIBHEIF_TEST_CASE STREQUAL "one-def")
  set_property(TARGET libheif::heif PROPERTY INTERFACE_COMPILE_DEFINITIONS LIBHEIF_TEST_SINGLE)
elseif(LIBHEIF_TEST_CASE STREQUAL "multiple-defs" OR LIBHEIF_TEST_CASE STREQUAL "missing-api")
  set_property(TARGET libheif::heif PROPERTY INTERFACE_COMPILE_DEFINITIONS
    LIBHEIF_TEST_FIRST LIBHEIF_TEST_SECOND)
endif()
]=])

set(libheif_DIR "${_package_dir}")
set(LIBHEIF_TEST_INCLUDE_DIR "${_fixture_root}/include/${LIBHEIF_TEST_CASE}")
list(APPEND CMAKE_MODULE_PATH "${LIBHEIF_MODULE_DIR}")
unset(LIBHEIF_HAS_GAIN_MAP CACHE)
find_package(libheif REQUIRED MODULE)

if(LIBHEIF_TEST_CASE STREQUAL "missing-api")
  if(LIBHEIF_HAS_GAIN_MAP)
    message(FATAL_ERROR "The feature check accepted a missing gain map API")
  endif()
elseif(NOT LIBHEIF_HAS_GAIN_MAP)
  message(FATAL_ERROR "The feature check rejected the ${LIBHEIF_TEST_CASE} fixture")
endif()
