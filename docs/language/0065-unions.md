# Anonymous Union Types (`T1 | T2 | ...`), Lowered onto `enum`'s Own Machinery

**Status:** Implemented
**Document:** `0065-unions.md`

---

# Motivation

`docs/language/0064-enums.md` gave Axea general, user-*declared*, nominal tagged unions. But a
lot of real "this value is one of a few types" code doesn't want to declare and name a whole
`enum` for a single call site - TypeScript's `string | number` is the obvious comparison, and the
one that prompted this phase. The goal: a *structural*, auto-instantiated union type, spelled
with `|` directly in a type position, requiring no declaration at all:

```ax
f(x: i32 | str) -> str
{
    return match x
    {
        i32(n) => "a number"
        str(s) => "a string"
    }
}

y = f(5)      // "a number" - no wrapper syntax
z = f("hi")   // "a string"
```

`f(5)` and `f("hi")` both just work - a plain `i32`/`str` value is implicitly tagged into the
union at the call boundary, the actual ergonomic point of the feature (confirmed as the intended
design over an explicit-wrapper alternative before this phase began).

---

# Design: A Union *Is* a Synthetic `enum`

Axea already has two kinds of type registration: *nominal* (`struct`/`enum` - the user writes a
name, declares it once, and refers to it by that name) and *structural* (`Map<K,V>`/
`Optional<T>`/`Result<T,E>` - auto-instantiated on demand, memoized by a canonical string). A
union type is structural in exactly `Map<K,V>`'s sense - `i32 | str` is a type the moment it's
*written*, with no separate declaration - but it needs genuine tagged-union *behavior*
(construction, `match` dispatch, printing), which `docs/language/0064-enums.md` already built in
full for `enum`. So a union doesn't get its own parallel machinery: it lowers, from
`TypeChecker` down, onto the exact same "enum is a flattened struct" representation a real
`enum` already uses. The first time a given canonical union string is seen, each layer
auto-registers a *synthetic* `EnumDecl` for it - one variant per alternative, named after that
alternative's own canonical type name, with that one type as its single field - into the same
registry (`enums_`) a real, user-declared enum lives in. From that point on, `match`,
`@axea.tostring.<Name>`, `@axea.print.<Name>`, and every LLVM-level struct/GEP/malloc mechanism
treat it as an ordinary enum, with zero new dispatch logic anywhere in any of those.

A union type's alternatives are restricted to **simple types**: a primitive (`i32`, `str`, ...)
or a plain `struct`/`enum` name - not `List<T>`, `Map<K,V>`, an array, or another union. The
reason isn't arbitrary: a `match` arm's own variant pattern (`i32(n) => ...`) is always a single
`Identifier` token (see `Parser::parseMatchExpr`), and `"List<i32>"` can never be spelled as one.
Allowing a compound alternative would produce a union whose own variants are permanently
unmatchable. `TypeChecker::resolveUnionType` rejects one with a clear error at the point the
union type itself is resolved.

## Canonicalization

`Parser::parseTypeName` parses one or more `|`-separated type atoms, then - only if there's more
than one - sorts and deduplicates them and joins the result with `|`. `i32 | str` and
`str | i32` both parse to the identical string `"i32|str"`, so they're the same type by
`Type::operator==`'s ordinary structural equality, with no special-casing needed anywhere else.
A single, `|`-free type atom returns unchanged - this is a pure superset of the existing grammar,
never affecting any type that doesn't use `|`.

## Implicit Wrapping

The user-facing point of the feature: a plain value is auto-tagged wherever a union type is
expected, at every boundary this phase covers - a call argument, a declared local's initializer,
and a `return`. This is a real assignability-rule change, not just parsing: `TypeChecker` gains
`isUnionMember(valueType, targetType)`, OR'd into the existing strict-equality check at each of
those three sites (the exact same shape `arrayToSliceCoercion`/`stringToStrCoercion` already use
for array→slice and `String`→`str`). Struct literal fields are **not** covered this phase (see
Known Imprecision below) - implicit wrapping is deliberately scoped to the three boundaries the
motivating example actually needs.

Critically, this isn't only a type-checking relaxation - a bare `i32` register and a tagged
`%i32.str*` struct pointer are genuinely different runtime representations, so the wrap has to
produce *real* instructions somewhere. Each of `Interpreter`/`IrGenerator` independently
re-derives, at the same three boundaries, whether wrapping is needed and to which alternative
(this codebase's usual "each pass owns its own logic, no shared resolved-type state" convention -
see `docs/language/0064-enums.md`'s own `enumNameOfExpr` for the precedent):

- **Interpreter** (`Interpreter::wrapForUnion`) needs no registry at all: given the declared
  type string and the already-evaluated runtime `Value`, it just checks which alternative the
  value's own `std::variant` shape matches (`std::holds_alternative<std::int64_t>` for
  `i32`/`i64`, a `StructInstance`'s own `typeName` for a struct alternative, ...). This does mean
  a union can't combine two alternatives that share the same underlying runtime representation -
  `i32 | i64` is representationally ambiguous, since the interpreter never distinguishes their
  width at runtime at all (see `docs/language/0005-type-system.md`). `TypeChecker` doesn't
  special-case this restriction explicitly this phase; it's a known gap (see below), not one hit
  by the worked example.
- **IrGenerator** (`IrGenerator::wrapForUnion`) has no runtime value to inspect - it needs to
  decide, at compile time, which alternative a value expression corresponds to. A new, narrow
  `simpleTypeOfExpr` resolves a literal's own primitive type, a struct literal's own type name, a
  parameter's or scope-tracked local's own declared/inferred simple type
  (`IrScope::defineSimpleType`/`findSimpleType`, mirroring the existing `defineIsBuffer`-style
  trackers), or a called function's own declared return type - deliberately no more general than
  that (an arithmetic expression, an `if`, or a `match` result isn't resolved; `docs/language/
  0064-enums.md`'s own "IrGenerator keeps no real type table" stance, scoped down to exactly what
  wrapping needs). Once the alternative is known, wrapping emits an ordinary `IrStructNew` -
  tag first, then the value - identical in shape to a real `EnumName.Variant(x)` construction.
  A value that's already exactly the target union (forwarding an existing union value through
  another union-typed boundary, resolved via the same `enumNameOfExpr` a real enum's own `match`
  scrutinee resolution already uses) passes through unchanged rather than being re-wrapped.

## The LLVM-Safety Wrinkle: `'|'` Isn't a Legal Identifier Character

A union's canonical name (`"i32|str"`) is exactly the right *Axea-level* identity - readable,
and what `enums_`/`IrScope::findEnumName` key on - but `'|'` isn't a legal unquoted LLVM
identifier character (only `[a-zA-Z$._][a-zA-Z$._0-9]*` is). This surfaced as a real, if
predictable, "invalid IR" bug during worked-example verification: `%i32|str = type {...}` and
`define %i32|str* @f(...)` both fail to parse. The fix is a single, narrow rename at the exact
points a union's name turns into actual LLVM text - `IrStructNew::typeName` (set by
`wrapForUnion`), `IrFunction::paramTypes`/`returnType` (set by `generateFunction`), and
`irProgram.structs`/`irProgram.enums`'s own keys (set by `generate`'s enum-flattening loop) -
via `llvmSafeTypeName`, which substitutes `'.'` for `'|'` (a character no existing Axea type name
ever contains, so the substitution is deterministic and collision-free, and a no-op for every
real, `'|'`-free struct/enum name). `TypeChecker`, `Interpreter`, and IrGenerator's *own* internal
`enums_` registry all keep using the readable `'|'` form throughout - only the LLVM-text boundary
needed to change.

One further ordering consequence: a real, user-declared `enum`'s flattening into
`irProgram.structs`/`irProgram.enums` happens right after `registerStructs` - every real enum is
known up front, from a single pass over top-level items. A union's own registration is lazy -
discovered while lowering a function body (a local's declared type, an argument being wrapped,
...) - so `enums_` isn't done growing until the *whole* program has been lowered. `generate`'s
enum-flattening loop was moved to run after every function has been generated, not before, to
account for this (harmless for real enums, whose own registration happens in
`registerStructs`, well before that point either way).

---

# Worked Example

```ax
struct Point { x: i32  y: i32 }

describe(v: Point | i32) -> str
{
    return match v
    {
        Point(p) => "a point"
        i32(n) => "a number"
    }
}

run() -> i32
{
    p = Point { x: 1, y: 2 }
    print(describe(p))    // "a point"
    print(describe(5))    // "a number"

    w: i32 | str = 5
    print(w)               // "i32(5)" - a union prints exactly like a real enum

    return 0
}

x = run()
```

Verified byte-for-byte identical across the interpreter, `-O0`, and `-O1`.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **Struct literal fields aren't an implicit-wrap boundary this phase.** `struct Response { v: i32
  | str }` typechecks fine, but `Response { v: 5 }` is not auto-wrapped - only call arguments,
  declared-local initializers, and `return` are. Extending the same `isUnionMember` OR-check to
  struct literal field type-checking (plus the matching `wrapForUnion` call in `Interpreter`/
  `IrGenerator`'s own struct-literal lowering) is a small, mechanical follow-up, deliberately
  deferred rather than speculatively built for a boundary the motivating example never exercises.
- **`i32 | i64` (or any two alternatives sharing the interpreter's own runtime representation) is
  a real representational gap**, not rejected with a dedicated error this phase - the
  interpreter's `wrapForUnion` would tag such a value as whichever alternative it checks first,
  silently, rather than correctly. `TypeChecker` doesn't special-case this; it's caught only by
  its practical absence from every real use case tried so far.
- **Union alternatives are restricted to simple types** (a primitive or a plain struct/enum
  name) - `List<T>`, `Map<K,V>`, an array, or a nested union are rejected outright, because a
  `match` arm's own variant pattern is always a single `Identifier` token (see Design above).
  Not a limitation anyone hit in practice yet, but a genuine, deliberate scope cut.
- **A union used as *another* type's own type parameter** (`Optional<i32 | str>`, a struct field
  typed `i32 | str`, a `List<i32 | str>`) resolves fine at the `TypeChecker` level (unions are
  ordinary `Type`s once resolved), but hasn't been exercised through `IrGenerator`/
  `LlvmIrEmitter` - `llvmSafeTypeName`'s own rename only covers the specific fields this phase's
  three wrapping boundaries populate, not every conceivable place a type string flows into IR.

---

# Guiding Rule

*A structural type family gets one representation, not two.* `enum` already solved "tagged union,
`match`, tag-aware printing" completely; a union type's only genuinely new work is arriving at
that same representation *structurally* instead of nominally - canonicalization, auto-
registration on first use, and the implicit-wrap codegen needed to actually produce a tagged
value at a boundary where none was written by hand. Everything downstream of "here is an
`EnumDecl`" - `match`, printing, LLVM struct/GEP/malloc machinery - is reused completely
unchanged, exactly the reuse this whole feature was proposed to exploit.
