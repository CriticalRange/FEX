find_package(Clang REQUIRED CONFIG)

# The official Windows LLVM package ships clang as separate static libraries.
if (NOT TARGET clang-cpp)
  add_library(clang-cpp INTERFACE)
  target_link_libraries(clang-cpp INTERFACE
    clangTooling
    clangFrontend
    clangParse
    clangSema
    clangAnalysis
    clangEdit
    clangASTMatchers
    clangRewrite
    clangSerialization
    clangDriver
    clangLex
    clangBasic)
endif()

if (NOT TARGET LLVM)
  add_library(LLVM INTERFACE)
endif()

# Query clang's global resource directory for system include directories
if (NOT CLANG_RESOURCE_DIR)
  find_program(CLANG_EXEC_PATH clang REQUIRED)

  execute_process(COMMAND ${CLANG_EXEC_PATH} -print-resource-dir
    OUTPUT_VARIABLE CLANG_RESOURCE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
file(TO_CMAKE_PATH "${CLANG_RESOURCE_DIR}" CLANG_RESOURCE_DIR)

add_library(thunkgenlib STATIC analysis.cpp data_layout.cpp gen.cpp)
target_include_directories(thunkgenlib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(thunkgenlib SYSTEM PUBLIC ${CLANG_INCLUDE_DIRS})
target_link_libraries(thunkgenlib PUBLIC clang-cpp LLVM)
target_link_libraries(thunkgenlib PRIVATE fmt::fmt)
target_compile_definitions(thunkgenlib INTERFACE -DCLANG_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")

# Clang's libtooling won't compile with libstdc++'s debug mode.
# clang-cl rejects the GNU -U form.
if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(thunkgenlib PUBLIC "-U_GLIBCXX_DEBUG")
endif()

add_executable(thunkgen main.cpp)
target_link_libraries(thunkgen PRIVATE thunkgenlib)
