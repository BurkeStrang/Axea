# Loops: `while`, `loop`, `break`, `continue`

**Status:** Implemented
**Document:** `0028-loops.md`

---

# Motivation

The language had no loop construct through every phase so far — `0021-axea-ir.md` and `0022-llvm-backend.md` both explicitly justify parts of their own scope *because* "the language has no loops yet." This document adds `while cond { }`, infinite `loop { }` (value-producing via `break value`, per `0001-syntax.md`'s own sketch), and `break`/`continue` (unlabeled — always target the innermost enclosing loop). `for`-in is deliberately not part of this: it needs ranges/iterators as a feature of its own.

Loops forced two problems the architecture had never had to solve:

1. **A mutation that must persist across iterations and after the loop.** An `if`/`else` branch's own scope is a deliberate *barrier* (`IrScope`'s `isBarrier`, `Environment`'s equivalent) — a mutation inside one branch never escapes it, since Phase 5 chose not to build phi-based merging for branches. A loop is the opposite: `while n < 10 { n = n + 1 }` is pointless unless the mutation is visible to the next iteration's condition check and to code after the loop.
2. **A value that can escape via any number of `break` sites**, not just the two branches an `if`/`else` merge already handles.

---

# `while` Is a Statement, `loop` Is an Expression

Resolves `0002-grammar.md`'s own open question ("is `while` an expression?"). `loop` is typed by whatever its `break value`s agree on — every exit is a `break`, so there's always a well-defined value. `while` can *also* exit normally when its condition goes false, which has no natural value to produce, so it never produces one at all: it's a statement (`WhileStmt`), like `return`, not an expression. `break`/`continue` are statements too (`BreakStmt`/`ContinueStmt`).

`TypeChecker::checkExpr`'s new `LoopExpr` case collects every `break value`'s type reachable within that loop (not crossing into a nested loop — a break there targets the inner loop) via a threaded `currentLoopBreakTypes` parameter, the same pattern already used for `expectedReturnType` (null ⇒ not inside a loop, rejecting `break`/`continue` outside one, mirroring the existing "'return' used outside a function" check). Every collected type must agree — same style as `if`/`else`'s existing "branches have incompatible types" check. A loop with **no** `break` anywhere is typed `unit`: a genuinely infinite loop is really `never`, but `TypeKind::Never` has no checking logic wired up anywhere in this codebase (same category as `Drop`'s known imprecisions in `0021-axea-ir.md`) — `unit` is the pragmatic, harmless stand-in. `while` reuses the identical collection machinery purely to validate `break`/`continue` are used correctly inside it, then rejects any `break` that carried a value — there's no expression position for `while` to hand it to.

`RegionChecker` mirrors this with its own threaded `currentLoopBreakRegions` (`RegionInfo` instead of `Type`), extending `IfExpr`'s existing "Borrowed if either branch could be borrowed" rule to "Borrowed if *any* reachable break could be."

---

# Bug Caught: Plain Assignment Has Always Silently Shadowed

The very first `while` loop written to test this (`while n < limit { n = n + 1 }`) hung forever. The cause predates loops entirely: `Environment`/`IrScope` have always had a `define()` (shadow, local-only) vs. `assign()` (mutate an existing binding, walk up) split, but `AssignmentStmt` (plain `x = expr`) had *always* called `define()` unconditionally — including inside `if`/`else` branches — only `++`/`--` ever called `assign()`. This was invisible before loops because nothing previously needed a plain-assignment mutation to escape a nested scope (the existing "mutation doesn't escape a branch" tests all use `n++`, never `n = n + 1`); a loop body gets its own scope per iteration, so `n = n + 1` inside it just shadowed a throwaway local the `while` condition (evaluated against the outer scope) never saw.

Fixed everywhere, not just in loops (confirmed as the right scope — no existing test pinned down the old if/else-with-plain-assignment behavior, and this is how most block-scoped languages without an explicit `let`/`mut` distinction already treat bare `x = ...`): `Environment::contains()`/`IrScope::contains()` (new — walks the chain without throwing) lets `AssignmentStmt`'s handling, in both `Interpreter::execute` and `IrGenerator::lowerStmt`, choose `assign()` when the name already exists anywhere in scope and `define()` only for a genuinely new one. `assign()` already respected barriers before this fix, so `if`/`else`'s own "mutation doesn't escape" behavior is unaffected — this only actually changes anything reachable through a *non-barrier* chain, which today means loop bodies.

---

# Axea IR: `IrLoop`, `IrBreak`, `IrContinue`

Structured like `IrBranch` (a nested `conditionBlock`/`body`, not labeled basic blocks) — Axea IR stays non-CFG for the same reason `0021-axea-ir.md` already gives for `if`/`else`; turning it into a real CFG is the LLVM backend's job. `conditionBlock` is empty for infinite `loop` (re-evaluated at the top of every iteration for `while`, unlike `if`'s one-time condition). `IrInst::dest` is the loop's own produced value, mirroring `IrBranch::dest`.

**Loop-carried variables.** `IrGenerator::lowerLoop` (shared by `WhileStmt` and `LoopExpr`) snapshots `IrScope` before and after lowering the body (`IrScope::snapshot()`, new — every name visible, walking the whole chain) and diffs the two: any name whose register changed becomes a `(preLoopReg, bodyEndReg)` pair in `IrLoop::carried`. The body itself is lowered through a **non-barrier** scope — the one deliberate divergence from `if`/`else`, since a loop's entire point is that mutations persist.

**`break`/`continue` mid-body.** A `break`/`continue` can fire *before* the body's natural end, at which point `IrLoop::carried`'s `bodyEndReg` doesn't exist yet — the register that would hold it might not even be defined at that point in control flow. So `IrBreak`/`IrContinue` each carry their *own* `carried` snapshot (`IrGenerator::currentLoopCarriedDiff`, diffing against a stack of pre-loop snapshots — top of stack is the innermost loop, pushed/popped by `lowerLoop`), capturing exactly which carried variables changed, and to what, by that specific point.

```ax
sumOdds(limit: i32) -> i32
{
    n = 0
    total = 0
    while n < limit
    {
        n = n + 1
        if n / 2 * 2 == n { continue }
        total = total + n
    }
    return total
}
```

```text
$ ax ir sumOdds.ax
  %13 = loop while {
    %3 = binop Less %1, %0
  } (-> %3) {
    ...
    %11 = br %10 {
      region.enter
      continue (%1 -> %5)
      region.exit
    } (-> %-1) else {
      region.enter
      region.exit
    } (-> %-1)
    %12 = binop Plus %2, %5
  } carried: (%1 -> %5) (%2 -> %12)
```

At the `continue`, `n` has already advanced (`%1 -> %5`) but `total` hasn't (correctly absent) — the loop's own `carried` list, computed at the body's natural end, has both.

---

# LLVM Backend: `alloca`/`load`/`store`, Not Phi, for Carried Variables

A phi-based approach needs exact predecessor tracking for every `continue` site and the natural fallthrough, multiplying with how many `continue`s exist. Sidestepped entirely: for each carried pair, `alloca` a slot before the loop and `store` the pre-loop value into it; at the top of the header (re-executed every iteration), `load` the slot and **rebind `fctx.llvmRegisterOf[preLoopReg]`** to the load's result, so every reference within `conditionBlock`/`body` transparently sees "whatever's currently in the slot" via the existing `ref()` machinery from `0022-llvm-backend.md`. Every `IrContinue`, every `IrBreak`, and the natural fallthrough each `store` their own current value before jumping. At the exit block, each slot is `load`ed **once more** and bound to `bodyEndReg`, so code *after* the loop sees the correct final value regardless of which of those paths actually got there — the slot is always current by the time control reaches exit. This is the standard "unoptimized codegen" pattern real compilers use for mutable locals, consistent with this backend's own stated philosophy ("LLVM is responsible for optimization," `0022-llvm-backend.md`'s Motivation) — `-O1`+ cleans it up into real SSA/phi form on its own.

The loop's own produced value (`loop { ... break x }`, when consumed) is unrelated to any of this — it's a phi at the exit block collecting every `(value, exiting-label)` pair from a value-carrying `break`, generalizing `emitBranch`'s existing merge-phi from at most two predecessors to however many break sites exist. An infinite loop with zero reachable `break`s gets an `unreachable` exit block instead, mirroring `emitBranch`'s "both sides terminate" case exactly.

## Bug Caught: `alwaysTerminates` Didn't Know About `break`/`continue`

`emitInstructions` treats a bare `IrBreak`/`IrContinue` as a terminator (returns `true`, stopping further processing in that list) exactly like `IrReturn` — but the *static* `alwaysTerminates` predicate (used to decide whether more code after a construct is reachable) only recognized `IrReturn` and a fully-covering `IrBranch`. For `if flag { break 1 } else { break 2 }` as an entire loop body, `emitBranch` correctly determined *dynamically* that both sides terminate (emitting the merge block as `unreachable`) — but `emitInstructions`'s *static* check (`alwaysTerminates(thenBlock) && alwaysTerminates(elseBlock)`) disagreed, didn't return `true`, and `emitLoop`'s "did the body fall through naturally" fallback fired anyway — appending a `store` and `br` **after** the merge block's `unreachable`, two terminators in one block. Caught immediately by real `clang` (confirms `0022-llvm-backend.md`'s own point about what a real toolchain catches that hand-review can't): *"instruction expected to be numbered '%N' or greater."* Fixed by adding `IrBreak`/`IrContinue` to `alwaysTerminates` alongside `IrReturn`.

## Bug Caught: An LLVM `-O0` Codegen Crash on Dead Loads

Compiling `n = 0  return loop { n = n + 1  break n }` through `clang -x ir -` (default `-O0`) reliably **segfaults** inside `SelectionDAGBuilder::CopyToExportRegsIfNeeded`, deep in x86 instruction selection — not a verifier rejection (the IR passes `clang -S -emit-llvm`'s own verification cleanly), a genuine crash in unoptimized codegen. Isolated by hand-editing the emitted `.ll` down to a minimal repro: the crash disappears the moment a **dead** `load` (the exit block's carried-slot reload, emitted unconditionally even when nothing after the loop actually references that variable) is removed, and disappears just as reliably by compiling with `-O1` instead of `-O0` (which runs dead-code elimination before instruction selection). This is a known-class limitation of LLVM 18.1.3's unoptimized (`-O0`) instruction selection path, not a defect in the emitted IR. Rather than adding a risky, invasive "lazily materialize the reload only if referenced" mechanism to sidestep a narrow backend bug, the documented, low-risk fix is: **compile Axea's LLVM output with `-O1` or higher** — consistent with the "LLVM owns optimization" stance already established, and no real use of a compiler's output happens at `-O0` anyway.

---

# Worked Example

```ax
findFirstOver(limit: i32) -> i32
{
    n = 0
    return loop
    {
        n = n + 1
        if n > limit { break n }
    }
}
```

```text
$ ax llvm-ir findFirstOver.ax
define i32 @findFirstOver(i32 %0) {
entry:
  %1 = add i32 0, 0
  %2 = alloca i32
  store i32 %1, i32* %2
  br label %loop.header0
loop.header0:
  %3 = load i32, i32* %2
  br label %loop.body0
loop.body0:
  %4 = add i32 0, 1
  %5 = add i32 %3, %4
  %6 = icmp sgt i32 %5, %0
  br i1 %6, label %if.then1, label %if.else1
if.then1:
  store i32 %5, i32* %2
  br label %loop.exit0
if.else1:
  br label %if.merge1
if.merge1:
  store i32 %5, i32* %2
  br label %loop.header0
loop.exit0:
  %7 = load i32, i32* %2
  %8 = phi i32 [ %5, %if.then1 ]
  ret i32 %8
}
```

```
$ ax llvm-ir findFirstOver.ax | clang -x ir -O1 - -o out && ./out
```

Compiles and runs correctly end to end — verified for every test in `tests/LlvmIrEmitterTests.cpp` and `examples/loops.ax` by actually invoking `clang -O1` and diffing against `ax run`'s output, the same discipline established in `0022-llvm-backend.md`.

---

# Known Imprecision (By Design, Not Oversight)

- **Move-tracking resets each loop iteration**, extending the exact same already-documented per-block limitation `0009-ownership.md` describes for `if`/`else`: `CapabilityChecker::checkMovesInExpr` gives a loop body a fresh, empty moved-set, so a value moved on one iteration isn't tracked as moved on the next. Not a new gap — the same one, one level further.
- **A loop with no reachable `break` types as `unit`, not `never`.** See above.
- **No labeled loops.** `break`/`continue` always target the innermost enclosing loop; there's no way to target an outer one from a nested loop.
- **`for`-in is not implemented.** Needs ranges/iterators as a feature of its own — `0001-syntax.md`'s own `for` sketch stays aspirational.

---

# Open Questions

- Once `for`-in exists, does it lower to the same `IrLoop` shape (condition = "has next", body = bind + user code), or does an iterator protocol want its own IR instruction?
- Should labeled loops (`'outer: loop { break 'outer }`) be added before or after `for`-in, given both are natural "next" loop features?
- Does per-iteration move-tracking ever need to become real dataflow, or does the `if`/`else` precedent mean this stays accepted indefinitely?

---

# Guiding Rule

> A loop's carried state must mean the same thing whether you asked for it by name, mutated it with `++`, `continue`d past it, or `break`ed out from under it — the value at any point where control can actually be observed (the next iteration's condition, code after the loop, a break's own value) has to be *the* value, not one of several plausible ones. Where the structured, non-CFG representation of `if`/`else` could get away with two predecessors, a loop's `alloca`/`load`/`store` model exists specifically because loops don't get to.
