# Closures: `fn(params) -> R { body }`, Move-Only Capture

**Status:** Implemented
**Document:** `0067-closures.md`

---

# Motivation

Every function before this phase was a named, top-level `FunctionDecl` - there was no way to
write an anonymous function value, no way for a function to "close over" a variable from its own
enclosing scope, and no way to pass behavior around as data. This phase adds real closures:

```ax
makeAdder(base: i32) -> fn(i32) -> i32
{
    return fn(x: i32) -> i32 { return x + base }
}

add5 = makeAdder(5)
add10 = makeAdder(10)
y = add5(1) + add10(1)   // 6 + 11 = 17
```

Two decisions were settled before implementation began:

- **Move-only capture, never by reference.** A closure literal takes ownership of every
  enclosing-scope value its own body references, copied in at the moment the literal is
  evaluated - there's no way to mutate a capture and have it observed back in the enclosing
  scope, and no explicit capture list syntax. This was chosen with an explicit eye toward a
  future async phase: a closure that can be stored and polled later is exactly the situation
  where a *borrowed* capture can dangle (the whole reason a fully-fledged borrow-checked
  language needs `Pin` for its own async story) - starting move-only sidesteps that class of
  problem entirely rather than deferring it.
- **`fn(params) -> R { body }` syntax**, not Rust's `|params| body`. Axea already uses `|` for
  union types (`i32 | str` - see `docs/language/0065-unions.md`), so pipe-delimited closures
  would have been genuinely ambiguous with the parser already committed to that token. A leading
  keyword avoids the collision and reads naturally as "this is a value of function type."

---

# Design

## Grammar: an anonymous `FunctionDecl`, syntactically and as a type

`Parser::parseClosureExpr` parses the exact same `(params) -> ReturnType { body }` shape
`parseFunctionDecl` already does (including `=>` single-expression sugar), just as an
*expression* rather than a top-level declaration - `ClosureExpr` lives in `Stmt.hpp` next to
`BlockExpr`, for the identical reason: it needs `Param`, declared there. As a *type*,
`fn(T1,T2)->R` is parsed by `Parser::parseTypeNameAtom` into the same no-spaces canonical form
every other type produces, and resolved by `TypeChecker::resolveType` into `Type{kind: Closure,
structName: "fn(T1,T2)->R"}` - reusing `structName` for a closure's own identity, the same role
Struct/Enum's own name already has there (a closure's signature *is* its identity, unlike
Optional<T>'s own "structName unused, elementTypeName is my payload" shape). The structured
(param types, return type) form is re-split from that one canonical string on demand, via
`TypeChecker::closureParamAndReturnTypes` (each pass keeps its own copy, per this whole
codebase's "separate over shared" convention) - mirroring `Result<T,E>`'s own "store the
canonical string, re-resolve on demand" pattern.

## Calling a closure value reuses ordinary call syntax - zero new grammar

`callback(x)` parses as an *ordinary* `CallExpr` (`Identifier(args)`), exactly like calling a
real function - the parser doesn't know or care whether `callback` names a top-level function or
a local holding a closure value. Each pass adds one check, ahead of its own generic
function-lookup: *is `callee` a name bound in the current scope to a closure-typed value?* If so,
resolve it as a closure call instead - a local always shadows a same-named top-level function
(the ordinary "inner scope wins" rule, first actually observable here since no earlier feature
let a local be *called*). TypeChecker/Interpreter/IrGenerator each answer this differently,
matching what each already tracks: TypeChecker checks its own `TypeEnv` (gaining a `contains`
method for this); IrGenerator reuses `IrScope::findSimpleType` (built for
`docs/language/0065-unions.md`'s own implicit-wrap resolution, and generic enough to answer "is
this name's own type text `fn(...)`" for free); the interpreter just pattern-matches the
already-evaluated runtime `Value`.

## Move-only capture: an over-approximating free-variable scan, refined by subtraction

Determining *what* a closure captures needs a free-variable scan of its own body. Rather than
building a fully scope-aware walker (tracking every nested block's own local shadowing
precisely), each pass that needs this (`CapabilityChecker`, `Interpreter`, `IrGenerator` - three
separate copies of `collectReferencedNames`, again per "separate over shared") uses a *dumb*,
unconditionally-recursive collector: gather every bare `NameExpr` text appearing anywhere in the
closure body, then subtract the closure's own top-level param names. This deliberately
over-approximates (a name shadowed by a *nested* local *inside* the closure body, rather than one
of its own top-level params, is still collected) - safe for how it's actually used: `Interpreter`/
`IrGenerator` only ever *capture* a collected name that's actually bound in the enclosing
scope (an over-collected top-level function name, also picked up by the same dumb scan, is
simply never bound anywhere and silently dropped); `CapabilityChecker`'s own move-tracking only
ever *rejects* a program for capturing an already-moved name, so over-collecting can only be
overly cautious, never silently wrong. This is a real, narrow imprecision - see Known Imprecision.

Move semantics fall out of feeding this same capture set into the existing
`CapabilityChecker::checkMovesInExpr` moved-set (marking each captured name moved in the
*enclosing* function, exactly like a `take`-consuming call argument already is) and into
`inferExpr`'s own capability inference: a captured struct-typed param of the enclosing function
unconditionally needs `Capability::Take` - not read/write/take depending on what the closure body
does with it, the way an ordinary nested call's own inference works, since the enclosing function
is done with the value the instant it's captured, regardless of what the closure does with its
own copy afterward.

## A genuinely new IR capability: an indirect call through a runtime function pointer

Every call before this phase targeted a statically-known name (`IrCall`). A closure value is
called *through* a runtime pointer, so this phase adds real function-pointer values and an
indirect call:

- **`IrClosureNew`** builds the closure value itself: a heap-allocated "fat pointer" -
  `{ fn ptr, i8* opaque captures }` - reusing `IrStructNew`/`IrFieldGet` completely unchanged for
  everything except the fn-ptr field itself (which has no Axea-level type, so it can't go through
  the ordinary `irProgram.structs`-driven machinery the way a captures struct's own fields do).
  `LlvmIrEmitter::registerClosureInstantiation` structurally keys `%axea.Closure.<id>` by
  signature alone (mirrors `Optional<T>`/`Result<T,E>`'s own memoized-by-content instantiation) -
  captures are hidden behind the opaque `i8*`, so two closures sharing one signature collapse
  onto the same LLVM type regardless of what each one actually captures. The captures themselves
  are an ordinary, *per-literal* named struct (`closure.captures.<N>`, structurally distinct
  per closure literal, registered into `irProgram.structs` the normal way) - genuinely different
  scopes of sharing for the two halves of the same value.
- **A real top-level function per closure literal** ("the trampoline",
  `IrGenerator::generateClosureTrampoline`) compiles the closure's own body, taking the captures
  struct as an always-Borrowed/Read hidden first parameter (the closure *value* owns it, reused
  across every call of that value, never dropped by any one invocation) and its own declared
  params normally, with each captured name bound inside via an ordinary `IrFieldGet` off that
  first param.
- **`IrClosureCall`** loads the fn-ptr and captures fields back out of a closure value and emits
  a genuine indirect call - `call RetType (ParamTypes...) %fnPtrReg(%capturesReg, args...)`,
  LLVM's own standard syntax for calling through a register rather than a symbol (already
  familiar to this codebase from variadic extern calls like `printf`).

One real bug found building this: the trampoline's own *declared* LLVM parameter type for the
captures struct is its own specific type (`%closure.captures.N*`), never the generic `i8*` the
fat pointer's own field 0 needs - so storing the trampoline's address into that field always
needs an explicit `bitcast`, not a direct store. Missing this produces a real LLVM type mismatch,
caught before it ever reached `clang` by re-deriving the trampoline's own "real" function-pointer
type from `capturesObject`'s own already-inferred type at `IrClosureNew` emission time.

---

# Worked Example

```ax
apply(f: fn(i32) -> i32, x: i32) -> i32
{
    return f(x)
}

makeAdder(base: i32) -> fn(i32) -> i32
{
    return fn(x: i32) -> i32 { return x + base }
}

double(x: i32) -> i32
{
    return x * 2
}

run() -> i32
{
    add5 = makeAdder(5)
    doubler: fn(i32) -> i32 = fn(x: i32) -> i32 { return x * 2 }
    // `double` is a bare top-level function name, not a closure literal - the implicit
    // function-reference-to-closure coercion wraps it into a real closure value here.
    fact: fn(i32) -> i32 = fn(n: i32) -> i32 {
        // Self-referential closure - `fact` calling itself needs no new syntax at all.
        if n <= 1 { return 1 }
        return n * fact(n - 1)
    }
    return apply(add5, 10) + apply(doubler, 10) + apply(double, 10) + fact(5)
    // 15 + 20 + 20 + 120 = 175
}

y = run()
```

Verified byte-for-byte identical across the interpreter, `-O0`, and `-O1`, including a closure
capturing a struct-typed local by move, one closure passed as another function's own parameter (a
higher-order function), a bare top-level function name used the same way, a struct-typed closure
*parameter*, and a self-referential (recursive) closure.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- ~~No bare-name function values~~ - **fixed**: `apply(double, 5)` - passing a *named top-level
  function* where a closure value is expected - now works, at all three boundaries that need it
  (call argument, declared-local assignment, and `return`). `TypeChecker::isFunctionRefAssignableToClosure`
  validates the referenced function's own real signature matches the target closure type exactly
  (checking `TypeEnv` first, so a same-named local always shadows the top-level function - the
  ordinary "inner scope wins" rule this whole feature already established for closure *calls*
  applies here too); each backend then wraps it into a real closure value at the same boundary,
  checked *before* the generic evaluation path (which would otherwise throw "undefined variable"
  trying to resolve a bare function name that's never actually bound anywhere). The interpreter's
  `ClosureInstance::wrappedFunction` is a trivial forward with no captures of its own.
  `IrGenerator::generateFunctionRefTrampoline` gives the LLVM backend the same thing: since
  `IrClosureCall`'s own ABI always calls through a captures-struct-taking function pointer, and a
  named function takes no such hidden first param, a bare function reference still needs a real
  trampoline - one memoized per distinct function name (not per reference site), taking a single
  shared, always-empty captures struct (`closure.captures.fnref`) it never reads, since a bare
  function reference captures nothing.
- ~~Struct-typed closure *parameters* aren't supported~~ - **fixed**: `CapabilityChecker::registerClosure`
  mints each closure literal its own synthetic `FunctionDecl` (name/params only - never read via
  `.body`, since the closure's own body is walked directly wherever `inferExpr`/`checkMovesInExpr`
  already reach the `ClosureExpr` node) and registers it into the *same* `functions_`/`inferred_`
  maps a real top-level function already uses - letting `raise`, `effectiveOrInferred`, the
  fixpoint loop's own per-function walk, the final declared-vs-inferred merge, and
  `checkMovesInExpr`'s own driver loop all work completely unchanged, with zero closure-specific
  logic of their own (this doc's own guiding rule: "reuse the struct machinery until the
  representation genuinely can't be a struct"). The fixpoint loop's own live
  `for (name, function) : functions_` walk had to become a snapshot-then-iterate instead, since
  discovering a closure now inserts a *new* key into that same map mid-walk (undefined behavior
  otherwise for a live `std::unordered_map` iteration) - closures found during one pass simply
  join the very next full pass, same fixpoint-convergence guarantee any single function's own
  capabilities already had. `effectiveCapabilities()` gained a closure-keyed sibling,
  `closureEffectiveCapabilities()` (keyed by `const ClosureExpr*`, since a closure has no name of
  its own), threaded through `RegionChecker::check`/`IrGenerator::generate` as new optional
  parameters (default empty, so every caller not wired through - most test helpers - keeps
  compiling unchanged and gets this checker's own original, always-safe Owned/Read fallback).
  `RegionChecker`'s own `ClosureExpr` case now computes each param's real `Owned`/`Borrowed`
  region from that data (the identical `borrowed = struct-typed && capability != Take` formula
  `checkFunction` already uses for a real function's own params), so a closure with a borrowed
  struct param returning it directly is correctly rejected exactly like a real function would be
  - the same fixture the fix's own end-to-end test needed a **struct-returning** enclosing
  function to actually exercise: `RegionChecker::checkFunction` only ever walks a function's body
  at all when *that function's own* return type could itself carry an aliasing risk, an
  optimization that transitively also gates whether any closure nested inside it ever gets
  region-checked. `IrGenerator::generateClosureTrampoline` consumes the same two pieces of data to
  choose a real per-param `IrMove`/`IrBorrowRead`/`IrBorrowWrite` (plus a trailing `IrDrop` for an
  `Owned` struct param), mirroring `generateFunction`'s own identical param loop, instead of the
  phase's original unconditional `IrMove` for every param. A *captured* struct-typed local is
  unaffected by any of this (always move/`Take`, needing no per-param inference at all).
- ~~The free-variable scan over-approximates (shadowing)~~ - **corrected on closer inspection**:
  this isn't actually a gap. This language's own assignment rule - `x = v` mutates the *nearest
  existing* binding for `x` anywhere up the scope chain, and only defines a genuinely fresh local
  when `x` doesn't already exist anywhere - means a closure body can never create a local that
  merely *shadows* a captured name; `x = 20` inside a closure that captured `x` always mutates
  the closure's own captured copy (confirmed directly: `x = 10; f: fn()->i32 = fn()->i32 { x =
  20  return x }; return f()` returns 20, identically across the interpreter and both LLVM
  optimization levels). Combined with the existing "only actually capture a name that's really
  bound in the enclosing scope" filter (handling a genuinely new closure-local name) and the
  existing per-closure param-name subtraction (handling param shadowing, recursively for nested
  closures), the dumb collector's own over-approximation was already sound.
- ~~`RegionChecker` treats a closure literal as opaque~~ - **fixed**: `RegionChecker::regionOfExpr`
  now has a real `ClosureExpr` case - it binds the closure's own params into a fresh child
  `RegionEnv` and recurses into the closure's own body with `regionOfExpr`, exactly like a real
  function body, rather than falling through to the generic "always Owned" fallback every other
  not-specially-handled expression kind still uses.
- ~~Closures are never freed~~ - **corrected**: this turned out not to be a closure-specific gap
  at all. `IrDrop`/`IrMove`/`IrBorrowRead`/`IrBorrowWrite`/`IrRegionEnter`/`IrRegionExit` are
  emitted by `IrGenerator` but never consumed by *either* backend (`grep` for each one across
  `LlvmIrEmitter.cpp`/`Interpreter.cpp` turns up nothing) - they're current, region-*checking*-
  only markers with no runtime effect yet, for every heap type this compiler has, not just
  closures. Nothing is freed by the compiled backend today; the interpreter relies on C++
  `shared_ptr` refcounting instead of needing to. Actually implementing real memory reclamation
  is a real, but wholly separate, project-wide undertaking - out of scope for a closures-specific
  pass.
- ~~No recursive/self-referential closures~~ - **fixed, no new syntax**: `f: fn(i32)->i32 =
  fn(n: i32)->i32 { ... f(n-1) ... }` (or the undeclared-type `f = fn(...){...}` spelling) already
  parsed; the fix is entirely in each pass's own `AssignmentStmt` handling, pre-binding `f`
  *before* checking/evaluating the closure literal's own body, rather than any new grammar.
  TypeChecker's `closureSignatureType` computes `f`'s own `fn(...)->...` type directly from the
  literal's own param/return type text (independent of its own body, which is why this needed to
  be its own helper - also now reused by `checkExpr`'s own `ClosureExpr` case) and pre-defines it
  in `env` before checking the body, so a self-call resolves through the exact same
  "closure-typed local" call path every other closure call already goes through - no new
  call-resolution logic anywhere. The interpreter builds the `ClosureInstance` completely
  unchanged, then *unconditionally* also sets `instance->captures[name] = Value{instance}` - a
  real, deliberate reference cycle (`ClosureInstance` is `shared_ptr`-owned), added *after*
  construction rather than taught to the ordinary free-variable scan, because that scan
  structurally can never discover a self-*call* on its own (`f(x)`'s own `f` is a `CallExpr`'s
  plain-string `callee` field, never a `NameExpr` node the scan would ever see - it only ever
  finds a self-*reference used as a value*, e.g. `return f`, which the ordinary scan already
  handles once `f` is pre-bound the same way TypeChecker's `env` is). The cycle means a
  self-referential closure's own instance is never reclaimed by the interpreter's usual
  `shared_ptr` refcounting - an accepted, documented trade-off (see "Closures are never freed"
  above: real memory reclamation is already out of scope project-wide, not just for this case).
  `IrGenerator` needed a genuinely different mechanism, since no closure *value* exists yet at the
  point its own body is still being compiled to hand an `IrClosureCall` an object to indirect
  through: a self-call instead compiles to a direct, ordinary `IrCall` back to the trampoline's
  own name, forwarding the trampoline's own `__captures` register straight through as the hidden
  first argument (the *same* captures object every call of this closure value already shares, so
  no new one is ever built for the recursive call). `Context` gained
  `selfRecursiveName`/`selfRecursiveTrampolineName`/`selfRecursiveCapturesRegister`, set once
  `generateClosureTrampoline` knows its own captures register - naturally inherited into every
  nested block *within that same trampoline* (a block's own `Context blockCtx = ctx` copy) and
  just as naturally absent from any other function/trampoline (each always builds a fresh
  `Context` from scratch). `lowerStmt`'s own `AssignmentStmt` case detects "is `value` directly a
  closure literal" and stashes the name into a narrow, ambient
  `pendingSelfReferenceName_` member (mirroring `TypeChecker::currentFunctionModule_`'s identical
  convention) for `lowerExpr`'s own `ClosureExpr` case to consume-and-clear via `std::exchange` a
  few frames later - clearing it immediately is what stops a *nested* closure literal (a genuinely
  different, unrelated one) from ever inheriting a stale value.
  **Narrower than the interpreter in one specific way**: only a self-*call* (`f(x)`) is supported
  by the LLVM backend: a closure using its *own name as a plain value* inside its own body (e.g.
  `return f` without calling it) still throws a clear `"undefined variable"` at IR-generation time,
  since no placeholder-then-patch mechanism was built for tying that particular knot at the LLVM
  level (the interpreter *does* support this narrower pattern too, for free, since its own fix is
  entirely value-level rather than needing any new IR capability). A genuinely rare pattern next to
  ordinary recursive calls, and one that fails loudly rather than silently miscompiling, so left as
  a documented, narrower imprecision rather than built out this phase.
  **A second, unrelated bug found while verifying this**: `IrGenerator::simpleTypeOfExpr` had no
  `ClosureExpr` case at all, so an *undeclared*-type closure-literal assignment
  (`f = fn(x: i32)->i32 {...}`, as opposed to `f: fn(i32)->i32 = fn(...){...}`) was never recorded
  via `scope.defineSimpleType` - a *later* call to `f` (recursive or not) couldn't recognize it as
  a closure-typed local at all, silently falling through to an ordinary `IrCall` targeting a
  nonexistent top-level function named `f`, caught only much later as an opaque
  `unordered_map::at` deep inside `LlvmIrEmitter`. This was a real, pre-existing bug (not one of
  this doc's own original six), affecting *any* undeclared-type closure call, not just a
  recursive one - fixed alongside this phase since recursion testing is what surfaced it.

---

# Guiding Rule

*Reuse the struct machinery until the representation genuinely can't be a struct.* A captures
struct is an ordinary named struct, needing nothing new. The closure value itself almost is too -
the "fat pointer" is two `IrStructNew` fields, not a bespoke instruction - except that one of
those two fields (a function pointer) has no Axea-level type to hang the existing
`irProgram.structs` machinery off of. That one field, and the genuinely new capability of calling
through a register instead of a name, are the entire new surface this phase actually needed.
