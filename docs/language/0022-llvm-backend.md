# LLVM Backend

**Status:** Implemented (Phase 6)
**Document:** `0022-llvm-backend.md`

---

# Motivation

The original spec for this document was two lines:

> LLVM is responsible for: Optimization, Register allocation, Code generation, Object file generation.
> Axea remains responsible for language semantics and safety before lowering to LLVM IR.

Phase 5 (Axea IR) deliberately stopped short of anything LLVM-shaped: no basic-block CFG, no phi nodes, nothing executed. This phase is the actual lowering pass that turns Phase 5's structured `IrProgram` into textual LLVM IR (`.ll`) — the last stage Axea itself is responsible for. Everything after this (optimization, register allocation, native code generation, object files) is real LLVM's job, and real LLVM never runs during this phase: there's no `llc`/`opt`/`clang` on this system, and linking against libLLVM's C++ API would be this project's first external dependency after five phases of staying self-contained. So the emitter is plain text generation — `std::string emit(const IrProgram&)` — and correctness this phase means "hand-reviewed, structurally plausible, valid-by-inspection LLVM IR text," not "assembled and run."

---

# Why This Needed Its Own Design Pass

Axea IR (Phase 5) was never built to satisfy LLVM's actual requirements, and printing it in a different syntax would have produced text that *reads* plausible while being subtly broken in at least three ways:

1. **Real basic blocks with phi nodes, not a structured `Branch`.** Phase 5's `if`/`else` is one `Branch` instruction holding two nested instruction lists — exactly the CFG-flattening problem Phase 5 explicitly deferred. LLVM has no such nested form; `if`/`else` has to become labeled blocks joined by a `phi` at the merge point.
2. **A dangling-pointer bug.** The obvious way to represent a struct instance is `alloca` (stack allocation). But a function that constructs and returns a struct (`make(x: i32, y: i32) -> Point { Point { x: x  y: y } }`, from `examples/regions.ax`) would then return a pointer to memory that's invalid the instant the function returns — `alloca` is scoped to the current call frame. This was caught during design, before any code was written (see "Bugs Caught" below).
3. **Instructions after a terminator.** Axea IR always appends a `RegionExit` marker after a body's final `Return`; LLVM requires exactly one terminator per basic block and rejects anything emitted after it.

---

# Type Mapping

Axea IR carries no per-register type (Phase 5 never needed one — `ax ir` just prints register numbers). The emitter re-derives each register's LLVM type in one forward pass per function (`inferTypesInList`), mirroring how `RegionChecker` derives region info from usage rather than storing it: `ConstInt → i32`, `ConstBool → i1`, `ConstString → i8*`, `BinOp → i32` or `i1` depending on the operator, `Call → callee's declared return type`, `StructNew → pointer to that struct type`, `FieldGet → the field's declared type`, `Branch → the type its reachable side(s) produce`.

| Axea | LLVM |
|---|---|
| `i32` | `i32` |
| `bool` | `i1` |
| `str` | `i8*` (pointer to a global constant) |
| `unit` | `void` (function return only) |
| struct `T` | `%T*` (always by pointer) |

Struct type declarations (`%T = type { ... }`) come from a new `IrProgram::structs` registry (name → ordered field name/type pairs), and `IrFunction` gained `paramTypes` alongside its existing `paramNames` — both are Phase 5 extensions this phase needed but Phase 5 itself never did, populated in `IrGenerator::generate`/`generateFunction` from `FunctionDecl`/`StructDecl` info that was already in scope.

Structs pass **by pointer** everywhere — matching the language's existing reference semantics (the interpreter already uses `shared_ptr<StructInstance>`, and `CapabilityChecker`/`RegionChecker` already treat struct arguments as shared, mutable-through-write-capability references). Primitives pass and return by value.

---

# Every Struct Is Heap-Allocated, Never Freed

This is the fix for the dangling-pointer bug above: every `StructNew` lowers to a `malloc` call, not `alloca`, so the resulting pointer stays valid regardless of which function returns it or how far it travels. Struct size comes from the standard LLVM textual-IR "null-pointer GEP" idiom (`getelementptr %T, %T* null, i32 1` → `ptrtoint ... to i64`) rather than hand-computed byte sizes, so padding/alignment stay LLVM's problem, never Axea's.

```llvm
%2 = getelementptr %Point, %Point* null, i32 1
%3 = ptrtoint %Point* %2 to i64
%4 = call i8* @malloc(i64 %3)
%5 = bitcast i8* %4 to %Point*
```

Nothing is ever `free`d this phase. That's a deliberate, documented simplification in the same spirit as Phase 5's "`Drop` markers exist but don't free anything yet" — making `Drop` actually call `free` (or building a real allocator) is meaningful follow-on work, not this phase's job; see "Not Yet Implemented."

---

# `if`/`else` Lowers to Real Blocks Plus a Phi

`Branch`'s existing `thenBlock`/`elseBlock`/`thenValue`/`elseValue` (computed by Phase 5) already say everything needed — no new "which value merges" analysis, just correct LLVM block/label bookkeeping:

```ax
pick(flag: bool) -> i32 { return if flag { 1 } else { 2 } }
```

```llvm
define i32 @pick(i1 %0) {
entry:
  br i1 %0, label %if.then0, label %if.else0
if.then0:
  %1 = add i32 0, 1
  br label %if.merge0
if.else0:
  %2 = add i32 0, 2
  br label %if.merge0
if.merge0:
  %3 = phi i32 [ %1, %if.then0 ], [ %2, %if.else0 ]
  ret i32 %3
}
```

Each side is lowered recursively into its own labeled block; a side that falls through gets an unconditional branch to the merge block, and a side that ends in an early `Return` contributes no predecessor. The merge block's `phi` lists only the predecessors that actually reached it — 0, 1, or 2. If *neither* side falls through (both branches `return`), there's no merge value at all: the merge block is unreachable, defined only with `unreachable` so every referenced label still exists (`examples/recursion.ax`-style early return exercises exactly this path — see "Bugs Caught").

A `phi` with a single incoming edge (an `if` with no `else`, mutating and falling through only one side) is still emitted as a real `phi` rather than special-cased away — unlike `add`, `phi` works uniformly for pointer-typed registers too, so one code path covers "both sides reach the merge" and "only one side does."

---

# Bugs Caught

Every phase so far has caught at least one real bug during implementation rather than after. This phase caught three — the dangling-pointer issue was design-time (above); the other two were caught by hand-reviewing the *first* real emitter output against what valid LLVM IR actually requires, since there's no `llc` on this system to catch them automatically.

**Bug 1: register numbers were not strictly increasing.** LLVM's parser requires unnamed (numbered) SSA values within a function to be *defined* in strictly increasing textual order — not merely "before their uses," but literally `%1` cannot appear before `%2` anywhere earlier in the same function. The first working version allocated "extra" temporaries (GEP pointers, `malloc` size calculations — values that don't exist in Axea IR at all) starting from `function.registerCount`, i.e. *after* all of Axea IR's own register numbers. That put them numerically ahead of Axea-numbered registers that were textually emitted later in the same block (e.g., in `display(user: User) -> str { return user.name }`, the field's GEP pointer got `%2` while the `load` that consumes it — and is Axea IR's own register `1` — got `%1`, printed *after* `%2`). Caught by manually re-deriving LLVM's actual numbering rule and tracing through the first emitted function rather than trusting that "every def precedes its use" was sufficient.

  Fixed by allocating LLVM register numbers purely in emission order, from one per-function counter (`FunctionContext::nextLlvmRegister`), with a `llvmRegisterOf: AxeaRegister -> LlvmRegister` map built up as each instruction is actually emitted (`defineRegister`) rather than reused from Phase 5's own numbering. Parameters are registered first, in declaration order, matching how LLVM itself numbers them. A dedicated regression test (`tests/LlvmIrEmitterTests.cpp`) scans every emitted function's `%N = ` definitions and asserts the numbers never go backwards, accounting for each function's own parameter count.

**Bug 2: the "no value" sentinel was treated as a real register.** Axea IR uses register `-1` to mean "this branch produced no value" — e.g. an `if` with no `else` gets an implicit unit else-branch whose `elseValue` is `-1`. The phi-construction code originally checked only whether a side had *terminated* before referencing its value; a non-terminated side with `value == -1` (exactly the implicit-unit-else case, exercised by `examples/recursion.ax`'s bare `if n <= 1 { return 1 }`) crashed with an `unordered_map::at` out-of-range lookup, since `-1` was never a real defined register. Fixed by also requiring `value != -1` before treating a side as a phi predecessor — consistent with `inferTypesInList`'s own handling of the same sentinel when it derives a branch's overall type.

---

# String Literals

Every string literal becomes a module-level global constant, hoisted once per unique literal across the whole program (functions and top-level statements alike), and referenced at each use site via a `getelementptr` into an `i8*`:

```llvm
@.str.0 = private unnamed_addr constant [6 x i8] c"hello\00"
...
%1 = getelementptr [6 x i8], [6 x i8]* @.str.0, i64 0, i64 0
```

This is the standard, simplest representation for string literals in LLVM IR — there are no string operations in the language beyond construction, pass-through, and field storage, so nothing more elaborate is needed.

---

# Compiler Implementation

`compiler/llvmir/LlvmIrEmitter.hpp/.cpp` — named `llvmir`, not `llvm`, since nothing here links against real LLVM; it emits text. Public surface is `std::string emit(const IrProgram&)`; it consumes only Phase 5's `IrProgram`, not the AST or the checkers directly — the entire point of having an IR is that the backend only needs to understand IR concepts.

`FunctionContext` is the per-function emission state, threaded through the recursive walk (the same role `IrGenerator::Context` played in Phase 5): a type map (`registerTypes`), the register-numbering map and counter described above, a label counter for fresh `if.then`/`if.else`/`if.merge` names, the current basic block's label (updated recursively so a nested branch's own merge label becomes the correct phi predecessor one level up), and the output stream.

`ax llvm-ir <file.ax>` prints the emitted `.ll` text to stdout, running the full existing pipeline (type-check → capability-check → region-check → `IrGenerator::generate`) then the new emitter — consistent with every other command's convention (no `-o` flag; redirect if you want a file). `ax run`/`ax ir` are unchanged; LLVM IR emission isn't part of execution or Phase 5's own printer in this phase.

---

# Not Yet Implemented

Consistent with every prior phase's own stated scope cuts:

- **Top-level script statements aren't emitted as a callable entry point.** `IrProgram::topLevel` (a bare script's assignments/expressions, outside any function) has no established `main`-equivalent convention yet — Phase 2 deliberately kept the language script-style with no `main` requirement, and wiring up an entry point is a separate decision, not implied by "emit LLVM IR for what's already there." String literals from `topLevel` are still collected into globals (harmless and needed if a future entry point references them), but no function is emitted for `topLevel` itself.
- **No `free`, no real allocator.** Every struct leaks; see "Every Struct Is Heap-Allocated, Never Freed" above.
- **No division-by-zero trap.** The interpreter checks and throws at runtime (Phase 1); native `sdiv` doesn't, matching how C/C++ itself treats it — undefined behavior, not a checked error.
- **`Drop` still has no teeth.** Axea IR's `Drop` markers are informational only (Phase 5) and stay that way here — they emit no LLVM instruction at all, same as `BorrowRead`/`BorrowWrite`/`Move`/`RegionEnter`/`RegionExit`.
- **Nothing actually assembles or runs this output.** No `llc`, no `clang`, no object files, no native binary — verification this phase is hand-review of the emitted text against known-good LLVM IR shapes, backed by `tests/LlvmIrEmitterTests.cpp`'s structural/substring assertions (the same style LLVM's own test suite uses at a larger scale). Installing a real LLVM toolchain and exercising this output through `llc` is follow-on work, not required for this phase.

---

# Open Questions

- Once loops exist, `if`/`else`'s phi-merge approach extends naturally, but a loop's back-edge will need its own phi at the loop header — does the current per-`Branch` label/phi bookkeeping generalize cleanly, or does loop lowering want its own dedicated pass?
- Should `topLevel` gain a real `main`-equivalent convention as part of eventually running emitted IR through `llc`, or does that decision belong to whatever phase first makes running native output a goal?
- Once `free`/a real allocator exists, does region/capability information already computed by Phases 3–4 (and threaded through Axea IR's `BorrowRead`/`BorrowWrite`/`Move`/`Drop` markers) turn out to be sufficient to place `free` calls safely, or does it need to get more precise first (per Phase 5's own "Known Imprecision" notes on `Drop`)?

---

# Guiding Rule

> The backend's only job is to tell LLVM the truth in LLVM's own required form — every register numbered exactly the way LLVM's parser demands, every branch a real block, every struct a pointer that outlives its constructor. Where Axea IR was deliberately imprecise (leaked allocations, informational-only `Drop`), the emitter stays imprecise too rather than inventing new semantics LLVM was never asked to check.
