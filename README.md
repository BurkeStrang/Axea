# Axea

Early C++23 compiler prototype for the Axea programming language.

## Current milestone

The compiler can:

- tokenize basic Axea syntax
- parse a single assignment statement
- parse arithmetic expressions with precedence
- parse booleans, strings, comparison operators, and `if` expressions
- dump tokens or the AST
- interpret a single assignment statement, including `if` expressions

Example:

```ax
x = 1 + 2 * 3
```

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run

```bash
./build/ax tokens examples/hello.ax
./build/ax ast examples/hello.ax
./build/ax run examples/hello.ax
./build/ax run examples/if.ax
```

Expected AST:

```text
Assignment(x)
  Binary(Plus)
    Integer(1)
    Binary(Star)
      Integer(2)
      Integer(3)
```

Expected `run` output:

```text
x = 7
```

## Formatting

Format all source files with clang-format:

```bash
cmake --build build --target format
```

Check formatting without modifying files (what CI runs):

```bash
cmake --build build --target format-check
```

To auto-format staged files on every commit, point git at the tracked hooks directory once per clone:

```bash
git config core.hooksPath .githooks
```

## Suggested next steps

1. ~~Add a minimal interpreter for assignment and arithmetic expressions.~~ Done.
2. ~~Add booleans, strings, and comparison operators.~~ Done.
3. ~~Make `if` an expression.~~ Done.
4. Add function declarations and blocks.
5. Add structs.
6. Add symbol binding and type checking.
7. Add Axea's `read` / `write` / `take` capability analysis.
8. Add ownership analysis.
9. Add regions and escape analysis.
10. Design Axea IR.
11. Add LLVM as a backend only after the frontend semantics are stable.
12. Add standard library, tooling, and IDE support.
