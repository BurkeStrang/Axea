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

run() -> i32
{
    add5 = makeAdder(5)
    doubler: fn(i32) -> i32 = fn(x: i32) -> i32 { return x * 2 }
    return apply(add5, 10) + apply(doubler, 10)   // 15 + 20 = 35
}

y = run()
```

Verified byte-for-byte identical across the interpreter, `-O0`, and `-O1`, including a closure
capturing a struct-typed local by move and one closure passed as another function's own
parameter (a higher-order function).

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **No bare-name function values.** `apply(double, 5)` - passing a *named top-level function* as
  if it were a closure value - isn't supported; only a `fn(...) { ... }` literal (or a variable
  already holding one) produces a real closure value. Wrapping a bare function reference into a
  closure value implicitly is a plausible, deliberately deferred follow-up.
- **Struct-typed closure *parameters* aren't supported.** `CapabilityChecker`'s own read/write/
  take inference is keyed per top-level `FunctionDecl` name; a closure is anonymous, and
  extending that machinery to cover it - a real, if bounded, amount of new plumbing - was
  deliberately deferred rather than half-built this phase. A *captured* struct-typed local is
  unaffected by this restriction (always move/`Take`, needing no per-param inference at all).
- **The free-variable scan over-approximates.** A name shadowed by a local declared *inside* the
  closure body (not one of its own top-level params) is still collected as if captured - safe
  for how it's used (see Design), but a real, narrow gap from a fully scope-aware capture
  analysis.
- **`RegionChecker` treats a closure literal as an opaque, always-Owned value** via its own
  generic fallback, without recursing into the closure's own body - safe (the fallback can't
  accept an unsound program), but not maximally precise about region issues *inside* a closure
  body specifically.
- **Closures are never freed.** Neither the closure value's own fat-pointer struct nor its
  captures struct is ever `IrDrop`-tracked - `isObviouslyStructTyped` (the mechanism that decides
  what gets dropped at scope exit) doesn't recognize a closure-typed local. A real, if bounded
  (this compiler has no other form of automatic memory reclamation for heap values either),
  memory-management gap.
- **No recursive/self-referential closures** - a closure literal has no name of its own to call
  itself by.

---

# Guiding Rule

*Reuse the struct machinery until the representation genuinely can't be a struct.* A captures
struct is an ordinary named struct, needing nothing new. The closure value itself almost is too -
the "fat pointer" is two `IrStructNew` fields, not a bespoke instruction - except that one of
those two fields (a function pointer) has no Axea-level type to hang the existing
`irProgram.structs` machinery off of. That one field, and the genuinely new capability of calling
through a register instead of a name, are the entire new surface this phase actually needed.
