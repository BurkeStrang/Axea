# Compiler Architecture

Pipeline

Source → Lexer → Parser → AST → Name Resolution → Type Checker →
Capability Analysis → Escape Analysis → Region Analysis → Axea IR → LLVM
IR → Native Code

Implementation language: C++23 Build system: CMake
