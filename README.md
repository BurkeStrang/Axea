# Axea

Early C++23 compiler prototype for the Axea programming language.

## Repository Layout

-   `compiler/` - Axea compiler
-   `runtime/` - Runtime support
-   `std/` - Standard library
-   `docs/` - Specifications, compiler docs, book, and RFCs
-   `examples/` - Example programs
-   `tests/` - Compiler and language tests

## Current milestone

The compiler can:

- tokenize basic Axea syntax
- parse a sequence of top-level statements, function declarations, and struct declarations
- parse arithmetic expressions with precedence, booleans, strings, comparison operators, `if`/`else if`/`else` expressions, blocks, function calls, field access, struct literals, field assignment, and `++`/`--`
- type-check programs (symbol binding + a `TypeChecker` pass) before running them
- infer `read`/`write`/`take` capabilities for function parameters and verify explicit capability declarations (`CapabilityChecker`)
- check that a `take`-consumed parameter isn't used again afterward within the same block (ownership / use-after-move)
- dump tokens or the AST
- interpret whole programs: functions (including recursion and early `return`), struct construction, field access, and field mutation (visible through shared struct references)

Example:

```ax
struct User
{
    name: str
    age: i32
}

birthday(user: User) -> i32
{
    user.age++
    user.age
}

u = User { name: "Burke"  age: 35 }
newAge = birthday(u)
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
./build/ax run examples/function.ax
./build/ax run examples/recursion.ax
./build/ax run examples/struct.ax
./build/ax run examples/capabilities.ax
./build/ax capabilities examples/capabilities.ax
```

Expected AST (`examples/hello.ax`):

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

```text
result = 120
```

```text
p = Point { x: 3, y: 4 }
sum = 7
```

Expected `capabilities` output (`examples/capabilities.ax`):

```text
Function(display)
  Param(user: read)
Function(birthday)
  Param(user: write)
Function(celebrate)
  Param(user: write)
Function(archive)
  Param(user: take)
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
4. ~~Add function declarations and blocks.~~ Done.
5. ~~Add structs.~~ Done.
6. ~~Add symbol binding and type checking.~~ Done.
7. ~~Add Axea's `read` / `write` / `take` capability analysis.~~ Done.
8. ~~Add ownership analysis.~~ Done.
9. Add regions and escape analysis.
10. Design Axea IR.
11. Add LLVM as a backend only after the frontend semantics are stable.
12. Add standard library, tooling, and IDE support.

See `docs/language/` for the full syntax, grammar, and type-system specifications this implementation follows.
