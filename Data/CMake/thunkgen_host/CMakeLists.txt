cmake_minimum_required(VERSION 3.20)
project(thunkgen_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
  # The Windows LLVM package is /MT. fmt and thunkgen must use the same CRT.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

get_filename_component(FEX_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${FEX_ROOT}/External/fmt" "${CMAKE_BINARY_DIR}/fmt" EXCLUDE_FROM_ALL)

add_subdirectory("${FEX_ROOT}/ThunkLibs/Generator" "${CMAKE_BINARY_DIR}/generator")
set_target_properties(thunkgen PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
