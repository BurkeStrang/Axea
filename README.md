# Axea

Early C++23 compiler prototype for the Axea programming language.

## Current milestone

The compiler can:

- tokenize basic Axea syntax
- parse a single assignment statement
- parse arithmetic expressions with precedence
- dump tokens or the AST

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

## Suggested next steps

1. Add function declarations and blocks.
2. Make `if` an expression.
3. Add booleans, strings, and comparison operators.
4. Add symbol binding and type checking.
5. Add an interpreter.
6. Add structs.
7. Add Axea's `read` / `write` / `take` capability analysis.
8. Design Axea IR.
9. Add LLVM as a backend only after the frontend semantics are stable.
