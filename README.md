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
- type-check programs (symbol binding + a `TypeChecker` pass) before running them, including requiring value-producing functions to explicitly `return` on every path (no implicit "last expression" return)
- infer `read`/`write`/`take` capabilities for function parameters and verify explicit capability declarations (`CapabilityChecker`)
- check that a `take`-consumed parameter isn't used again afterward within the same block (ownership / use-after-move)
- check that a borrowed (`read`/`write`) struct parameter never escapes its function via a return value, directly or nested inside a returned struct literal (`RegionChecker`)
- lower a checked program into Axea IR — a structured, per-function instruction sequence with ownership/capability/region markers embedded directly in the instruction stream (`IrGenerator`)
- emit textual LLVM IR (`.ll`) from Axea IR — real basic blocks and phi nodes for `if`/`else`, heap-allocated (`malloc`) struct instances passed by pointer, hoisted string-literal globals, and a generated `main` that prints every top-level binding (`ax run`-compatible output) so the output compiles and runs through a real LLVM toolchain (`LlvmIrEmitter`)
- dump tokens, the AST, the IR, or the emitted LLVM IR
- interpret whole programs: functions (including recursion and early `return`), struct construction, field access, field mutation (visible through shared struct references), and loops (`while`, infinite `loop` with `break`/`continue`, `break value` for loop-as-expression)

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
    return user.age
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
./build/ax run examples/regions.ax
./build/ax regions examples/regions.ax
./build/ax ir examples/capabilities.ax
./build/ax llvm-ir examples/capabilities.ax
./build/ax run examples/loops.ax
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

Expected `regions` output (`examples/regions.ax`):

```text
Function(make)
  Param(x: owned)
  Param(y: owned)
Function(copy_point)
  Param(p: borrowed)
```

Expected `ir` output (`examples/capabilities.ax`, `archive` function):

```text
Function(archive)
  Params: %0=user
  region.enter
  move %0
  %1 = field.get %0.name
  drop %0
  return %1
  region.exit
```

Expected `llvm-ir` output (`examples/capabilities.ax`, `archive` function):

```llvm
define i8* @archive(%User* %0) {
entry:
  %1 = getelementptr %User, %User* %0, i32 0, i32 0
  %2 = load i8*, i8** %1
  ret i8* %2
}
```

## Compile and run through a real LLVM toolchain

`ax llvm-ir` output includes a generated `main` that prints every top-level binding in the same format `ax run` does, so it can be compiled and executed directly with `clang` (install with e.g. `sudo apt install clang`):

```bash
./build/ax llvm-ir examples/capabilities.ax | clang -x ir - -o /tmp/capabilities
/tmp/capabilities
```

This should print the same output as `./build/ax run examples/capabilities.ax`.

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
9. ~~Add regions and escape analysis.~~ Done.
10. ~~Design Axea IR.~~ Done.
11. ~~Add LLVM as a backend only after the frontend semantics are stable.~~ Done — emits textual LLVM IR with a generated `main`, verified end to end by compiling and running it through `clang`.
12. ~~Add loops (`while`, `loop`, `break`, `continue`).~~ Done, across every stage from parsing through LLVM codegen.
13. Add standard library, tooling, and IDE support.

See `docs/language/` for the full syntax, grammar, and type-system specifications this implementation follows.
