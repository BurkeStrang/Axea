cmake_minimum_required(VERSION 3.25)
project(axea LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(axea_compiler
    src/lexer/Lexer.cpp
    src/parser/Parser.cpp
)

target_include_directories(axea_compiler PUBLIC src)

target_compile_options(axea_compiler PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

add_executable(ax src/main.cpp)
target_link_libraries(ax PRIVATE axea_compiler)

enable_testing()
add_executable(axea_tests tests/LexerParserTests.cpp)
target_link_libraries(axea_tests PRIVATE axea_compiler)
add_test(NAME axea_tests COMMAND axea_tests)
