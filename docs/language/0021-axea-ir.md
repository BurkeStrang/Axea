# Axea IR

**Status:** Implemented (Phase 5)
**Document:** `0021-axea-ir.md`

---

# Motivation

The original spec for this document was two lines:

> Purpose: Represent ownership, capabilities, regions, and control flow independently of LLVM.
> Example operations: `borrow.read`, `borrow.write`, `move`, `drop`, `region.enter`, `region.exit`.

`docs/language/0020-compiler-architecture.md`'s pipeline places it as its own stage, after everything built in Phases 2–4: `... -> Capability Analysis -> Escape Analysis -> Region Analysis -> Axea IR -> LLVM IR -> Native Code`. This document is the actual instruction set and lowering pass that fills that stage in.

**Scope, stated up front**: this phase *generates* Axea IR from the fully checked AST and makes it observable (`ax ir`, mirroring `ast`/`capabilities`/`regions`). It does not change how `ax run` executes — the tree-walking `Interpreter` still interprets the AST directly. Executing this IR, and lowering it further into real LLVM IR, are Phase 6's job; the roadmap treats "Axea IR" and "LLVM Backend" as separate phases precisely so this one could stay scoped to "represent the program," not "run it a second way."

---

# Structured, Not a Flattened CFG

Axea IR is a per-function list of instructions using virtual registers (`%0`, `%1`, ...), where `if`/`else` stays a single `Branch` instruction holding two *nested* instruction lists rather than being flattened into separate labeled basic blocks joined by phi nodes:

```ax
pick(flag: bool) -> i32
{
    if flag { 1 } else { 2 }
}
```

```text
Function(pick)
  Params: %0=flag
  region.enter
  borrow.read %0
  %2 = br %0 {
    region.enter
    %1 = const.i32 1
    region.exit
  } (-> %1) else {
    region.enter
    region.exit
  } (-> %-1)
  ...
```

(Note the `else` block above is empty because this example never actually reaches it — it's for the `-> %-1` illustration; a real `if`/`else` here would show `%2 = const.i32 2` in the `else` block instead.)

This mirrors the AST's own `IfExpr` shape, just with expressions flattened into three-address-code-style instructions instead of a tree. The language has no loops yet — the only control flow is `if`/`else` and early `return` — so there's no back-edge, no loop-carried value, nothing that actually *needs* a real CFG with block merging and phi nodes. Building that machinery now, for nothing downstream to consume, would be premature; turning this structured form into LLVM's actual SSA/CFG form is exactly what Phase 6 ("LLVM Backend") is for.

---

# Instruction Set

| Instruction | Produces a register? | Purpose |
|---|---|---|
| `ConstInt` / `ConstBool` / `ConstString` | yes | literal |
| `BinOp` | yes | arithmetic/comparison (operator is a `TokenKind`, reused directly rather than a parallel enum) |
| `Call` | yes | function call |
| `StructNew` | yes | struct literal construction |
| `FieldGet` | yes | read a field |
| `FieldSet` | no | write a field |
| `Branch` | yes (the merge register) | `if`/`else`; holds `thenBlock`/`elseBlock` plus `thenValue`/`elseValue` naming which register in each list holds that branch's result |
| `Return` | no | terminator; register `-1` means bare/unit return |
| `BorrowRead` / `BorrowWrite` / `Move` | no | one per parameter, at function entry |
| `RegionEnter` / `RegionExit` | no | bracket the function body and each nested `if`/`else` branch |
| `Drop` | no | a struct-typed local at its block's end, or an owned (`take`) struct parameter at function exit |

The set is deliberately small and canonical — surface sugar is desugared *by lowering*, not represented in IR at all. `++`/`--` becomes an explicit `Get`/`Const`/`BinOp`/`Set` sequence (or `Const`/`BinOp` plus a register rebind, for a plain name); the fat-arrow function-body shorthand was already normalized into a block by the parser back in Phase 2, so IR generation never even sees it.

---

# Ownership/Capability/Region Are In The Instruction Stream

This is what makes it *Axea* IR rather than generic three-address code — every one of the doc's six original example ops is load-bearing, not decorative:

- **`BorrowRead`/`BorrowWrite`/`Move`**: one per parameter, emitted at function entry, chosen directly from `CapabilityChecker::effectiveCapabilities()` and `RegionChecker::regions()` (Phases 3–4) — `Move` for an owned (`take`) parameter, `BorrowWrite`/`BorrowRead` otherwise depending on the resolved capability. Nothing is recomputed here; IR generation trusts the earlier passes completely.
- **`RegionEnter`/`RegionExit`**: bracket the function body, and additionally each `if`/`else` branch — one pair per scope `RegionChecker`'s own `RegionEnv` would create, so the IR's bracketing structure matches the region checker's own scoping one-for-one.
- **`Drop`**: a struct-typed local at the end of its own block, and an owned struct parameter at function exit. See "Known Imprecision" below for what this deliberately doesn't attempt.

```ax
archive(take user: User) -> str { user.name }
```

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

---

# A Bug Worth Naming (Two, Actually)

Every phase so far caught something during implementation rather than after. This one caught two, both in how register bindings are scoped during lowering — mirroring `Environment`'s own `define()`/`assign()` split (Phase 3), but with a wrinkle specific to *static* lowering that the interpreter never has to deal with.

**Bug 1 (caught during design): shadowing must not leak.** A plain `AssignmentStmt` inside an `if`-branch shadows within its own block — exactly like `Environment::define()`. A flat, unscoped `name -> register` map would let that shadow permanently overwrite the outer binding once the branch ends. Fixed by giving the lowering context its own real scope chain, `IrScope`, mirroring `Environment`/`TypeEnv`/`RegionEnv`.

**Bug 2 (caught while writing the test for Bug 1's fix): `assign()` mutation must not cross a branch boundary either.** `++`/`--` on a bare name uses `Environment::assign()` semantics — walk up and mutate the *existing* binding in place, not shadow it. That's correct for the *interpreter*, because only one branch of an `if` ever actually executes at runtime. But IR generation lowers **both** branches structurally, against the same shared parent scope, one after the other. Without a fix, `n++` inside the `then`-branch would call `assign()`, which would walk up and permanently mutate the *outer* scope's register for `n` — so by the time the `else`-branch was lowered next, it would incorrectly see the `then`-branch's already-incremented value, even though at runtime that branch might never run.

The fix: `IrScope` gained an `isBarrier` flag. An `if`/`else` branch's own scope is a barrier — `assign()` still updates the binding locally (so later code within that *same* branch sees the mutation, verified by a dedicated test), but refuses to walk past itself into the shared parent. This is not full dataflow merging (no phi nodes, per the "Structured, Not a Flattened CFG" section above) — it's the minimum needed for two sibling branches, and the code after them, to never see each other's hypothetical mutations. `tests/IrGeneratorTests.cpp` pins down both the "does not leak out" and "does persist within the same branch" halves of this permanently, and `./build/ax ir` on a small repro shows it directly: a name incremented inside one branch and referenced right after the `if` correctly still resolves to the original parameter register, exactly because nothing merges the two possible outcomes.

---

# Known Imprecision (By Design, Not Oversight)

Consistent with every prior phase's own documented simplifications:

- **No move-aware drop elision.** A value that was itself taken/moved elsewhere still gets a `Drop` marker at its scope's end. Harmless today (nothing executes this IR), but imprecise.
- **No "don't drop a returned value" precision.** A struct-typed local that happens to be the block's own trailing/returned value is still dropped.
- **Local `Drop` recognition is heuristic, not a real type inference pass.** `IrGenerator::isObviouslyStructTyped` only recognizes a *direct* struct literal, or a bare reference to an already-struct-typed parameter, as worth a `Drop` — not the result of a call or a field access. A full type-carrying lowering (propagating struct-ness through every expression kind, the way `RegionChecker::regionOfExpr` already does for its own purposes) would be more precise, at the cost of threading type information through the entire lowering pass for a marker that has no runtime effect yet. Deferred until something actually consumes `Drop` and precision starts to matter.
- **No basic-block CFG, no phi nodes, nothing executes this IR** — all stated above, repeated here because they're the load-bearing scope boundary for this whole phase.

---

# Compiler Implementation

`IrGenerator::generate(const Program&, capabilities, regions) -> IrProgram` (`compiler/ir/IrGenerator.hpp/.cpp`) takes Phase 3/4's results directly, the same pattern `RegionChecker` already established for consuming `CapabilityChecker`'s output. `IrScope` mirrors `Environment`'s scope-chain shape exactly, plus the barrier flag described above. A small `emit`/`emitVoid` helper pair (allocate a register, set it as the instruction's `dest`, append, return the register) keeps each lowering call site to a few lines instead of repeating that boilerplate throughout. `ax ir` runs the full pipeline (type-check → capability-check → region-check → generate) and prints every function, then any top-level script instructions; `ax run` is unchanged — IR generation isn't part of execution in this phase.

---

# Open Questions

- Once loops exist, does the structured (non-CFG) representation still hold up, or does that become the natural point to introduce real basic blocks?
- Should `Drop` become move-aware before or after LLVM lowering starts consuming it — i.e., is elision precision a Phase 5 follow-up or naturally part of Phase 6?
- Does `RegionEnter`/`RegionExit` bracketing at the granularity of "every `if`/`else` branch" hold up once there are constructs with many more nested scopes (`match` arms, comprehensions), or does it need a coarser or finer-grained placement rule?

---

# Guiding Rule

> The IR should tell the truth about what the source program does, without accidentally telling a truth about a program that could never actually run. Every branch it lowers, it lowers fully; every mutation it represents, it scopes to exactly the code that mutation could actually reach — nothing more, on the strength of a runtime guarantee the IR itself doesn't have.
