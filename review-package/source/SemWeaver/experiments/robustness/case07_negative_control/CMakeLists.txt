cmake_minimum_required(VERSION 3.20)
project(SemWeaverCase07NegativeControl LANGUAGES CXX)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

if(NOT DEFINED CHECKER_SOURCE)
  message(FATAL_ERROR "Pass -DCHECKER_SOURCE=/absolute/path/to/SAGenTestChecker.cpp")
endif()

set(PLUGIN_SOURCES "${CHECKER_SOURCE}")
if(DEFINED UTILITY_SOURCE AND DEFINED UTILITY_HEADER)
  list(APPEND PLUGIN_SOURCES "${UTILITY_SOURCE}")
  set(GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-include")
  file(MAKE_DIRECTORY "${GENERATED_INCLUDE_DIR}/clang/StaticAnalyzer/Checkers")
  configure_file(
    "${UTILITY_HEADER}"
    "${GENERATED_INCLUDE_DIR}/clang/StaticAnalyzer/Checkers/utility.h"
    COPYONLY
  )
  configure_file(
    "${UTILITY_HEADER}"
    "${GENERATED_INCLUDE_DIR}/clang/StaticAnalyzer/Checkers/Utility.h"
    COPYONLY
  )
endif()

add_library(SAGenTestPlugin MODULE ${PLUGIN_SOURCES})
target_compile_features(SAGenTestPlugin PRIVATE cxx_std_17)
target_include_directories(SAGenTestPlugin PRIVATE
  ${LLVM_INCLUDE_DIRS}
  ${CLANG_INCLUDE_DIRS}
  ${GENERATED_INCLUDE_DIR}
)
target_compile_definitions(SAGenTestPlugin PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(SAGenTestPlugin PRIVATE
  clangAnalysis
  clangAST
  clangBasic
  clangLex
  clangStaticAnalyzerCore
  clangStaticAnalyzerFrontend
)
set_target_properties(SAGenTestPlugin PROPERTIES PREFIX "")
