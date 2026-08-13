# `for`-in Loops Over Integer Ranges

**Status:** Implemented
**Document:** `0030-for-loops.md`

---

# Motivation

`0028-loops.md` shipped `while`/`loop`/`break`/`continue` and explicitly deferred `for`-in, reasoning that it "needs ranges/iterators as a feature of its own." With no collection or array type in the language yet, a full iterator protocol still isn't feasible — but a narrower, immediately useful slice is: `for i in start..end { }` iterating an `i32` range with an exclusive upper bound, per `0001-syntax.md`'s own original sketch.

Confirmed scope before implementation: `start..end` is **pure syntactic sugar**, recognized only directly after `for x in`. It is not a first-class `Range` value — there's no `Range` type, no way to bind `r = 0..5` and use `r` later, no ranges as function arguments. That scoping choice is what makes the rest of this document possible: with nothing to type-check, store, or lower as a standalone value, `for`-in can desugar *entirely* at parse time into constructs `0028-loops.md` already built and already fully wired through every stage.

---

# `for i in a..b { body }` Desugars at Parse Time — No `ForStmt`, No `ForExpr`

This matches the project's own established precedent for pure sugar: `f(x) => expr` already desugars in `Parser::parseFunctionDecl` into `{ return expr }` with no dedicated "fat-arrow" AST node — `ax ast` shows the desugared form, not a preserved "this was originally `=>`" marker. `for`-in follows the identical convention. `Parser::parseFor` (`compiler/parser/Parser.cpp`) builds the desugared tree directly out of `AssignmentStmt`, `WhileStmt`, `IncDecStmt`, `BlockExpr`, `ExprStmt`, `IfExpr`, `BinaryExpr`, `BreakStmt`, `NameExpr`, `IntegerExpr`, and `BoolExpr` — every one of them already fully supported by `TypeChecker`, `CapabilityChecker`, `RegionChecker`, `Interpreter`, `IrGenerator`, and `LlvmIrEmitter` before this document's work began. The only production-code change outside the lexer and parser is a two-line tweak to how `AssignmentStmt` is lowered (see "Hygiene" below) — nothing was added to any of those six later stages.

**Lexer** (`TokenKind.hpp`, `Lexer.cpp`): new `For`, `In` keywords and a `DotDot` token for `..`, recognized via the same two-character lookahead pattern already used for `+`/`++`:

```text
$ ax tokens for.ax          # source: for i in 0..3 { }
1:1  For         for
1:5  Identifier  i
1:7  In          in
1:10 Integer     0
1:11 DotDot      ..
1:13 Integer     3
1:15 LeftBrace   {
1:17 RightBrace  }
```

**Parser** (`Parser::parseFor`): `expect(For)`, `expect(Identifier)` (the loop variable name), `expect(In)`, `start = parseExpression(...)`, `expect(DotDot)`, `end = parseExpression(...)`, then `parseBlock()` for the body — the same struct-literal-ambiguity guard `parseIfExpr`/`parseWhile` already use for their condition expressions.

The actual desugared shape, verified via `ax ast` against `examples/loops.ax`'s `sumRange`:

```ax
sumRange(start: i32, end: i32) -> i32
{
    total = 0
    for n in start..end
    {
        total = total + n
    }
    return total
}
```

```text
$ ax ast sumRange.ax
Function(sumRange)
  Param(start: i32)
  Param(end: i32)
  Block
    Assignment(total)
      Integer(0)
    ExprStmt
      Block
        Assignment(__for0_end)
          Name(end)
        Assignment(__for0_i)
          Binary(Minus)
            Name(start)
            Integer(1)
        While
          Bool(true)
          Block
            Increment
              Name(__for0_i)
            ExprStmt
              If
                Binary(GreaterEqual)
                  Name(__for0_i)
                  Name(__for0_end)
              Then
                Block
                  Break
              Else
                Block
            Assignment(n)
              Name(__for0_i)
            Assignment(total)
              Binary(Plus)
                Name(total)
                Name(n)
    Return
      Name(total)
```

In words: an internal counter is pre-decremented once before an infinite `while true`, whose body increments the counter first, then checks the bound and `break`s if it's been reached, then binds the user's loop variable to the counter's current value, then runs the user's body. Two design choices below explain why it's shaped exactly this way rather than the more obvious `{ i = a  while i < b { body  i++ } }`.

---

# Bug Caught: A Trailing Increment Lets `continue` Skip It Forever

The first version of `parseFor` desugared to the "obvious" shape — assign `i = a`, then `while i < b { body; i++ }`, with the increment appended after the user's statements. It passed every straight-line test and hung on the very first one using `continue`:

```ax
for n in 0..6
{
    if n / 2 * 2 == n { continue }
    total = total + n
}
```

A `continue` lowers as a jump straight back to the enclosing `while`'s condition re-check — it doesn't run anything still queued after it in the same body. With the increment placed *after* the user's statements, any `continue` fired from within those statements skips the increment on every single iteration where it fires, and the counter never advances: infinite loop the moment `continue` is reachable at all.

The fix is the restructuring shown in the `ast` dump above: put the increment **first**, at the very top of an infinite `while true`, followed immediately by an explicit `if counter >= end { break }` bound check, then the loop-variable binding, then the user's body. Now `continue`'s jump back to the trivially-true `while true` header always re-enters right at the increment on the very next pass — whether it got there via an explicit `continue` or by running off the natural end of the body, both paths land in exactly the same place. No special-casing of `continue` was needed anywhere; restructuring the desugared shape once made the two cases identical by construction.

---

# Bug Caught: The Induction Variable Mutated a Same-Named Outer Variable

`0028-loops.md`'s own "Bug Caught" section fixed plain assignment (`x = expr`) to *mutate* an existing binding anywhere in the enclosing scope chain rather than always shadowing — confirmed at the time as the correct behavior everywhere, since that's what makes `n = n + 1` inside a `while` body work at all. That same fix broke `for`-in's hygiene the moment it was tested:

```ax
i = 99
for i in 0..3 { }
return i   // gave 3, not 99
```

The desugared loop variable binding (`i = __for0_i`, once per iteration) is a plain `AssignmentStmt`, and `Environment::contains`/`IrScope::contains` walks the *entire* scope chain — including past the loop body's own per-iteration scope, straight out to the enclosing function's `i = 99`. A fresh nested scope alone doesn't create a boundary against ancestors further up the chain; it never did, and `0028-loops.md`'s fix was correct to make it not.

Rather than re-litigating that "assignment mutates everywhere" rule (still correct for real user code — that's exactly what loop-carried variables need), the fix is narrower: `AssignmentStmt` gained an optional `forceDefine` field (`compiler/ast/Stmt.hpp`), defaulted `false` and set only by `Parser::parseFor`'s own desugaring — never producible by anything parsed directly from user syntax. Both `Interpreter::execute` and `IrGenerator::lowerStmt` check it before deciding `assign()` vs. `define()`:

```cpp
if (!assignment->forceDefine && env.contains(assignment->name))
{
    env.assign(assignment->name, value);
}
else
{
    env.define(assignment->name, value);
}
```

The `for` loop's own induction-variable binding always forces a fresh `define()`, so it can never accidentally reach out and mutate a same-named variable in an enclosing scope — `for i in 0..3 { }` after `i = 99` now correctly leaves the outer `i` at `99`.

---

# Hygiene: Mangled Internal Names Per Loop

The counter and bound live under compiler-generated names, `__for<N>_i` and `__for<N>_end`, where `N` comes from a `Parser::forCounter_` member incremented once per `for` parsed — guaranteeing every `for` loop's internal machinery has a unique identity, so nested loops reusing the same user-facing variable name never collide:

```ax
for i in 0..3
{
    for i in 0..2 { total = total + 1 }
}
```

lowers the outer loop's internals as `__for0_i`/`__for0_end` and the inner's as `__for1_i`/`__for1_end` — verified via `ax ast` and via an interpreter test asserting the two loops' internal counter names differ. `__for<N>_*` is not an identifier any user source can produce (identifiers can't start with `_` followed by a token the lexer would otherwise split), so there's no risk of a user variable colliding with the mangled names either.

---

# Everything Below the Parser: Unchanged

`ax capabilities`, `ax regions`, `ax ir`, `ax llvm-ir`, and `ax run` all already handled every node type in the desugared shape, having been fully exercised by `0028-loops.md`'s own `while`/`break`/`continue` work — nothing new needed to be taught to any of them. `ax ir` on the worked example below shows the desugared `while` lowering exactly like any other, `break`/`continue` targeting it exactly like any other, with the pre-decrement/increment-first structure visible directly in the carried-variable diff:

```ax
sumOddsUnder(limit: i32) -> i32
{
    total = 0
    for n in 0..limit
    {
        if n / 2 * 2 == n { continue }
        total = total + n
    }
    return total
}
```

```text
$ ax ir sumOddsUnder.ax
Function(sumOddsUnder)
  Params: %0=limit
  region.enter
  move %0
  %1 = const.i32 0
  %2 = const.i32 0
  %3 = const.i32 1
  %4 = binop Minus %2, %3
  %17 = loop while {
    %5 = const.bool true
  } (-> %5) {
    %6 = const.i32 1
    %7 = binop Plus %4, %6
    %8 = binop GreaterEqual %7, %0
    %9 = br %8 {
      region.enter
      break (%4 -> %7)
      region.exit
    } (-> %-1) else {
      region.enter
      region.exit
    } (-> %-1)
    %10 = const.i32 2
    %11 = binop Slash %7, %10
    %12 = const.i32 2
    %13 = binop Star %11, %12
    %14 = binop EqualEqual %13, %7
    %15 = br %14 {
      region.enter
      continue (%4 -> %7)
      region.exit
    } (-> %-1) else {
      region.enter
      region.exit
    } (-> %-1)
    %16 = binop Plus %1, %7
  } carried: (%4 -> %7) (%1 -> %16)
  return %16
  region.exit
```

```text
$ ax llvm-ir sumOddsUnder.ax | clang -x ir -O1 - -o out && ./out
x = 9
```

Matches `ax run`'s own `x = 9` exactly (`0 + 6` summed as odds under `6` is `1 + 3 + 5 = 9`) — the round-trip already established in `0028-loops.md` for `while`/`loop`, re-verified here with no changes needed to the LLVM backend at all.

---

# Worked Example

`examples/loops.ax`'s `sumRange`:

```ax
sumRange(start: i32, end: i32) -> i32
{
    total = 0
    for n in start..end
    {
        total = total + n
    }
    return total
}

rangeSum = sumRange(1, 5)
```

```text
$ ax run examples/loops.ax
sum = 55
firstOver = 21
rangeSum = 10
```

`sumRange(1, 5)` sums `1 + 2 + 3 + 4 = 10` — the upper bound is exclusive, matching `0..limit` idioms elsewhere in the language (e.g. array-style indexing conventions, though Axea has no arrays yet). Re-run through `ax llvm-ir | clang -x ir -O1 - | ./a.out` and diffed against the above: identical output, all three lines.

---

# Known Imprecision (By Design, Not Oversight)

- **No first-class `Range` value.** `start..end` is recognized only directly after `for x in` — it cannot be bound to a variable, passed as an argument, or used in any other expression position. Extending it to a real `Range` type would need its own type-checking, storage representation, and (for `IrGenerator`/`LlvmIrEmitter`) lowering story; deliberately out of scope here.
- **`i32` ranges only.** `start`/`end` are ordinary `i32` expressions type-checked the same as any other arithmetic operand — no other integer width, and no floating-point ranges.
- **Ascending, unit-step only.** No `start..end step k`, no descending ranges (`5..0` produces zero iterations rather than counting down, since the desugared bound check is a plain `>=`).
- **No iteration over collections.** There's no array/collection type in the language yet for `for`-in to iterate; this remains purely integer-range sugar until one exists.
- **Move-tracking resets each loop iteration**, same as `0028-loops.md` documents for `while`/`loop` — a value moved on one iteration of the desugared `while` isn't tracked as moved on the next, since the desugared body is a loop body like any other.

---

# Open Questions

- If/when a collection or iterator type is added, does `for`-in grow a second desugaring path (e.g. `.next()`-based), or does it stay integer-range-only with a separate construct for iteration?
- Should `start..end step k` or descending ranges be added before a first-class `Range` type, given both are natural "next" extensions to this same purely-syntactic approach?
- Does `forceDefine` need to generalize into a broader "compiler-generated binding" concept, or is `for`-in's induction variable the only place this pattern will ever be needed?

---

# Guiding Rule

> Sugar that desugars into already-correct pieces is only actually correct if the desugared shape reuses those pieces' *real* semantics rather than an idealized version of them — the first attempt at `for`-in reused `while` and `continue` from `0028-loops.md` for free, but reused the *whole* truth of "`continue` jumps to the header, skipping whatever's still queued after it," and reused the *whole* truth of "plain assignment mutates any enclosing binding it finds," not just the parts that happened to match the naive mental model. Both bugs disappeared once the desugared shape was made honest about both.
