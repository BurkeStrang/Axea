# Explicit `return` Required for Value-Producing Functions

**Status:** Implemented
**Document:** `0027-explicit-return.md`

---

# Motivation

Axea functions originally worked like Rust: a function's body is a block, and the block's trailing (non-`;`-terminated) expression was implicitly the function's return value, alongside optional early explicit `return` statements. This document changes that for functions specifically: **a function that produces a value must always use an explicit `return` statement.** There is no more "the last expression in the body is the return value" fallback.

Nested block-expressions are untouched — `if`/`else` used as an expression (`x = if cond { 1 } else { 2 }`) still produces a value via its trailing expression exactly as before, since there's no other mechanism for an `if`-expression to produce a value. The only thing that changed: a **function's own top-level body** no longer treats its trailing expression as the thing being returned.

**Unit-returning functions are unaffected.** A function with no return value can still fall off the end of its block with no `return` at all; only value-producing functions must always `return` explicitly.

---

# The Rule: `definitelyReturns`

`TypeChecker::checkFunction` no longer infers a function's "actual return type" from its body's expression type (the old mechanism, appropriate for implicit return). Instead it runs a real control-flow analysis, `TypeChecker::definitelyReturns`, that asks a different question: *does every path through this block hit an explicit `return`?*

A block definitely returns if:
- any top-level statement is a `return`, or
- any top-level statement is an `if`/`else` where **both** branches (recursively) definitely return, or
- the block's own trailing expression is such a fully-returning `if`/`else`.

This is the same shape as `parseBlock`'s own "is this immediately followed by `}`" rule for deciding `ExprStmt` vs. `result` — a top-level `if`/`else` can appear either as a mid-block statement or as the block's own trailing value, and `definitelyReturns` checks both positions.

If the function's declared return type isn't `unit` and its body doesn't definitely return, that's a compile error: *"function 'X' does not return a value of type T on all paths (did you forget 'return'?)"*.

Every individual `return`'s value type is still checked by the existing, unchanged `checkBlock`/`checkStmt`/`checkExpr` machinery — `definitelyReturns` only adds the "did you cover every path" question on top.

---

# The Bug This Was Blocked On

This surfaced (and fixed) a latent TypeChecker gap noticed during Phase 6 (LLVM Backend): `if cond { return a } else { return b }` as a function's *entire* body used to fail to type-check. Neither branch produces a block-result *value* — both just `return` — so the `if`-expression's own inferred type was `unit`, which mismatched any non-unit declared return type. Under implicit-return semantics this pattern was simply unreachable; there was no valid way to write it.

```ax
sign(x: i32) -> i32
{
    if x < 0 { return 0 - 1 } else { return 1 }
}
```

Under explicit-required return, this is now the single most common way to write a value-producing function with a conditional — so `definitelyReturns` treating "both branches return" as itself a form of definitely-returning isn't an edge case, it's the point.

---

# Fat-Arrow Sugar Desugars to `return`

`f(x) => expr` used to desugar to `BlockExpr({}, expr)` — an implicit-return shape that would now always fail `definitelyReturns` for any non-unit function. Rather than special-case fat-arrow bodies anywhere else in the pipeline, `Parser::parseFunctionDecl` now desugars `=>` directly to `{ return expr }` (`BlockExpr({ ReturnStmt(expr) }, nullptr)`). `f(x) => expr` and `f(x) { return expr }` are now literally the same AST shape — sugar stays sugar for the explicit form, and every downstream pass (type checking, capability/region checking, IR generation, interpretation) needs zero awareness that fat-arrow syntax exists.

---

# Two More Places That Assumed Implicit Return

Three passes besides the parser and type checker had baked in the assumption that a function's body's trailing value *was* its return value. Each needed a small, targeted fix — described here because each is a small "why," not obvious from the diff alone.

**`Interpreter::callFunction`** used to delegate to the same generic `BlockExpr` evaluation used everywhere (which always returns the trailing `result` value) and catch `ReturnSignal` around it. That generic mechanism has to stay untouched for general block/if-expression evaluation, but the function's own top-level body now needs different handling: execute its statements and, if present, evaluate its trailing `result` expression *for side effects only*, discarding the value — since (per `definitelyReturns`) that can only legally happen for a unit-returning function now, and a stray discarded expression must never leak out as "the" return value (`x = someUnitFn()` must bind `x` to unit, not to whatever a leftover expression happened to compute).

**`RegionChecker::checkFunction`** used to wrap its whole-body walk in `requireOwned(regionOfExpr(*function.body, ...), function)` — treating the body's own trailing value as if it were an implicit return needing an ownership check. Every individual `return` site was *already* independently protected (`regionOfStmt`'s `ReturnStmt` case, reached via the same recursive walk, however deeply nested in `if`/`else`) — the outer `requireOwned` wrapper was specifically for the implicit-return path, which no longer exists. Fixed by still calling `regionOfExpr(*function.body, ...)` for its recursive side effect, just no longer wrapping the (now largely meaningless) return value in `requireOwned`.

**`IrGenerator::generateFunction`** used to unconditionally append a synthetic `IrReturn{value=result}` after lowering the body, where `result` was whatever the trailing expression lowered to. Two problems once explicit return is required:
1. For a function whose body already explicitly returns on every path (the new common case), this became a *second* terminator appended right after an already-fully-terminated top-level `Branch` — invalid, since a basic block can have only one terminator.
2. For a unit-returning function with a stray discarded trailing expression, `result`'s register must not leak out as "the" return value.

Fixed with a new local `alwaysTerminates` helper (structurally identical to `definitelyReturns`, just walking lowered `IrInst` lists instead of AST nodes — kept as a separate, pure implementation per this codebase's convention of each pass owning its own walk, the same way `RegionChecker::regionOfExpr` and `CapabilityChecker::inferExpr` are independent). The synthetic `Return` append is now guarded by `!alwaysTerminates(irFunction.body)`, and when it *does* fire (only ever for a unit-returning function in a well-typed program now) its value is hardcoded to `-1` (bare/unit) rather than `result`.

A near-identical bug existed one layer further down: `LlvmIrEmitter::emitFunction` has its own defensive "should not happen" fallback (`if (!terminated) append a trailing ret`), and `emitInstructions`'s own "terminated" tracking only recognized a literal top-level `IrReturn` — not a `Branch` whose both sides already terminate. `LlvmIrEmitter` gained the same `alwaysTerminates` helper (independently, not shared with `IrGenerator`'s), and `emitInstructions`'s `Branch` case now returns `true` when both sides always terminate, so `emitFunction`'s defensive fallback correctly stays silent instead of appending a second `ret` after the merge block's `unreachable`.

`CapabilityChecker` needed no changes — its whole-body `inferExpr`/`checkMovesInExpr` walks are conservative dataflow passes independent of "is this value actually returned"; walking a now-discarded trailing expression is harmless, the same category of acceptable imprecision as before.

---

# Worked Example

```ax
sign(x: i32) -> i32
{
    if x < 0 { return 0 - 1 } else { return 1 }
}
```

```text
$ ax ir sign.ax
Function(sign)
  Params: %0=x
  region.enter
  move %0
  %1 = const.i32 0
  %2 = binop Less %0, %1
  %7 = br %2 {
    region.enter
    %3 = const.i32 0
    %4 = const.i32 1
    %5 = binop Minus %3, %4
    return %5
    region.exit
  } (-> %-1) else {
    region.enter
    %6 = const.i32 1
    return %6
    region.exit
  } (-> %-1)
  region.exit
```

Both branches' `-> %-1` confirms neither contributes a block-result value — they return instead. (`%7`'s own Branch register is never actually used by anything, since nothing after it consumes it; a future cleanup could avoid allocating it at all when both sides always return, but it's harmless as-is.)

```text
$ ax llvm-ir sign.ax
define i32 @sign(i32 %0) {
entry:
  %1 = add i32 0, 0
  %2 = icmp slt i32 %0, %1
  br i1 %2, label %if.then0, label %if.else0
if.then0:
  %3 = add i32 0, 0
  %4 = add i32 0, 1
  %5 = sub i32 %3, %4
  ret i32 %5
if.else0:
  %6 = add i32 0, 1
  ret i32 %6
if.merge0:
  unreachable
}
```

No phi node at the merge block — neither branch falls through to it, so it has no predecessors and is simply `unreachable`, exactly as it would be for a hand-written C function with an `if`/`else` that returns on both sides.

---

# Not Yet Implemented / Open Questions

- No "unreachable code after an unconditional return" warning. `f() -> i32 { return 1  return 2 }` type-checks fine today (the second `return` is simply dead code); a real unreachability lint is future work.
- `definitelyReturns` only understands `if`/`else` — there's no `match`/`loop` yet for it to reason about. Whichever phase adds those will need to extend this analysis (e.g. a `match` with an exhaustive set of returning arms should count, mirroring how "both branches return" does here).
- The parser still happily parses a value-producing function whose body ends in a bare, un-returned expression — rejection happens at type-check time, not parse time. This matches how every other semantic (as opposed to syntactic) error in this compiler is reported, and keeps the parser from needing to know a function's declared return type.

---

# Guiding Rule

> A function's signature is a promise, and the only thing allowed to keep that promise is the word `return` — not "whatever expression happened to be written last." Where a value could still legitimately flow out through an unreturned trailing expression (unit functions, nested `if`/`else` used as a value), that stays exactly as expressive as before; the change is narrowly about function-body top levels, not about expressions in general.
