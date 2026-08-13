cmake_minimum_required(VERSION 3.25)
project(axea LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(axea_compiler
    compiler/lexer/Lexer.cpp
    compiler/parser/Parser.cpp
    compiler/interpreter/Interpreter.cpp
    compiler/sema/TypeChecker.cpp
    compiler/sema/CapabilityChecker.cpp
    compiler/sema/RegionChecker.cpp
    compiler/ir/IrGenerator.cpp
)

target_include_directories(axea_compiler PUBLIC compiler)

target_compile_options(axea_compiler PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

add_executable(ax compiler/main.cpp)
target_link_libraries(ax PRIVATE axea_compiler)

enable_testing()
add_executable(axea_tests
    tests/TestMain.cpp
    tests/LexerTests.cpp
    tests/ParserTests.cpp
    tests/InterpreterTests.cpp
    tests/TypeCheckerTests.cpp
    tests/CapabilityCheckerTests.cpp
    tests/RegionCheckerTests.cpp
    tests/IrGeneratorTests.cpp
)
target_include_directories(axea_tests PRIVATE tests)
target_link_libraries(axea_tests PRIVATE axea_compiler)
add_test(NAME axea_tests COMMAND axea_tests)

find_program(CLANG_FORMAT_EXE NAMES clang-format-18 clang-format)
if(CLANG_FORMAT_EXE)
    file(GLOB_RECURSE AXEA_FORMAT_SOURCES
        compiler/*.cpp compiler/*.hpp
        tests/*.cpp tests/*.hpp
    )
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXE} -i ${AXEA_FORMAT_SOURCES}
        COMMENT "Formatting source files with clang-format"
    )
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${AXEA_FORMAT_SOURCES}
        COMMENT "Checking source formatting with clang-format"
    )
endif()
