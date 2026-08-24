# `Result<T, E>`

**Status:** Implemented
**Document:** `0063-result.md`

---

# Motivation

`docs/language/0014-error-handling.md` - a two-line proposal predating almost everything else in
this codebase - sketches `T!` ("value or error") alongside `T?` ("optional"), the latter made real
by `docs/language/0052-optional.md`. This phase builds the other half, `Result<T, E>`: a genuine
two-variant sum type, `Ok(value)` or `Err(error)`, with a real payload on *both* sides (unlike
`Optional<T>`'s own `None`, which carries nothing) - so a failure can say *why*, not just *that* it
failed.

```ax
divide(a: i32, b: i32) -> Result<i32, str>
{
    if b == 0
    {
        return Err("division by zero")
    }
    return Ok(a / b)
}
```

`?` (`docs/language/0052-optional.md`'s own postfix operator) is generalized to work on either
`Optional<T>` or `Result<T,E>`, matching Rust's own `?` operator working across both `Option<T>`
and `Result<T,E>`. `.unwrap_or(default)` is shared between the two by name (see Design below for
why that's not a coincidence); `.is_ok()`/`.is_err()` are `Result`'s own distinctly-named
non-propagating check, mirroring `Optional`'s `.is_some()`/`.is_none()` without conflating the two
APIs.

---

# Design: `Map<K,V>`'s Representation, `Optional<T>`'s Control Flow - and a Real Simplification
Neither Fully Anticipated

**Type representation borrows from `Map<K,V>`, not `Optional<T>`.** `Optional<T>` has exactly one
type parameter, stored in `Type::elementTypeName`. `Result<T,E>` has two, so it reuses `Map<K,V>`'s
existing two-field shape directly: `elementTypeName` is `T` (the Ok payload - the same role it
already plays for `Optional<T>`), `valueTypeName` is `E` (the Err payload - the same field
`Map<K,V>`'s own value type already occupies). No new `Type` fields, no new `TypeKind` field shape
- `TypeKind::Result` is the only new enumerator, added rather than repurposing the pre-existing
(and, before this phase, completely inert - zero references anywhere in `TypeChecker.cpp`)
`TypeKind::Error`, since that enumerator's own original intent is undocumented and unrelated.
`Parser::parseTypeName`'s own `"Result<T,E>"` branch, and `TypeChecker`/`LlvmIrEmitter`'s own
`resolveType`/`llvmType` branches, all mirror `Map<K,V>`'s existing bracket-depth-aware comma split
(`findTopLevelComma`) exactly - `Result<i32, Map<i32,i32>>` splits correctly, the identical concern
`Map<K,V>`'s own split already had to solve.

**LLVM representation extends `Optional<T>`'s by one field.** `%axea.Result.<id> = type { i1 isOk,
T, E }` - the direct 3-field extension of `Optional<T>`'s own named `{i1 hasValue, T}` (see
`docs/language/0052-optional.md`'s own Design section for why it has to be a *named*, not
anonymous, struct - the identical `isSliceType` collision risk applies here too). Both payload
slots are always physically present (mirroring `Optional<T>`'s own `undef`-for-the-unused-slot
convention for `None`): constructing `Ok(x)` fills the Ok slot with `x` and the Err slot with
`undef`; `Err(e)` does the reverse. Neither slot is ever read without checking `isOk` first, so an
`undef` bit pattern in the unused slot is never actually observed.

**`Ok(x)`/`Err(e)` need context for the *other* parameter - a strictly harder version of
`None`'s own problem, solved identically.** `None` carries no expression at all, so it always needs
external context (a declared type, or the enclosing function's own return type) to resolve
`Optional<T>`'s `T`. `Ok(x)`/`Err(e)` are less starved - each carries a real expression, giving one
of the two type parameters for free - but the *other* one is exactly as unknowable as `None`'s own
`T` was, so both still need the identical "borrowed from context" treatment `TypeChecker::checkStmt`'s
`AssignmentStmt`/`ReturnStmt` cases already give `None`, just narrowed to the one missing parameter
instead of the whole type. Anywhere else (a function-call argument, nested inside another
expression), `checkExpr(OkExpr)`/`checkExpr(ErrExpr)` always throw "cannot infer" - the same
standalone-construction restriction `None` already has, extended.

**The real simplification, found only once the LLVM layer was reached: `Result<T,E>`'s "positive
case" occupies the *exact same struct positions* `Optional<T>` already uses.** Field 0 is the
tag (`hasValue`/`isOk`) in both layouts; field 1 is the positive payload (`Optional<T>`'s only
payload; `Result<T,E>`'s Ok payload) in both. This wasn't planned in advance - it fell out of
choosing the field order `{i1, T, E}` rather than, say, `{i1, E, T}` - but once noticed, it meant
`IrOptionalIsSome`/`IrOptionalUnwrap` (the two instructions `.is_some()`/`.is_none()`/`?`/
`.unwrap_or` already lowered to) needed **no new sibling instructions at all** for `Result<T,E>`'s
own `.is_ok()`/`.is_err()`/`?`/`.unwrap_or` - they're reused *verbatim*, field-position semantics
generalizing for free. `IrOptionalUnwrap` gained one new field, `field` (defaulting to `1`,
Optional's own and Result's Ok position), used only by `?`'s Result-flavored Err-propagation path
(`field = 2`, extracting the one position `Optional<T>`'s own 2-field layout has no equivalent of)
- the *only* genuinely new IR surface this phase needed for propagation/unwrapping was that one
field, plus a new `IrResultNew` for 3-field construction. `LlvmIrEmitter`'s own emission functions
for `IrOptionalIsSome`/`IrOptionalUnwrap` needed **zero changes** (their `extractvalue`/`xor` text
never hardcoded "Optional" at all, only reading whatever aggregate type `object`'s own register
already resolved to) - only the *type-inference* step (`inferTypesInList`, deciding what LLVM type
`IrOptionalUnwrap`'s own destination register should be considered) needed a branch consulting the
right registration table (`isResultType(objectType)` → `resultOkPayloadType`/`resultErrPayloadType`,
else `optionalPayloadType`).

---

# `?`'s Own Generalization: Same Branch Shape, Decided by the Enclosing Function's Return Type

`TryExpr` (`?`) was already `Optional`-only. Generalizing it needed care in exactly one place:
`IrGenerator` keeps no real type table (repeated precedent throughout this codebase - see
`optionalPayloadTypeName`'s own comment), so it can't inspect the operand's own inferred type to
decide "is this Optional or Result." Instead, the *enclosing function's own declared return type*
(a plain string already sitting in the AST, `ctx.function->returnType`) decides which shape to
build for the failure path - `starts_with("Result<")` versus everything else - exactly mirroring
how `TypeChecker`'s own `checkExpr(TryExpr)` already requires the operand's kind to *match* the
function's own declared return kind (never mismatched - see Known Imprecision), so reading the
decision off the function's own type string is always correct, never a guess.

The condition check and success-side unwrap are shared verbatim with `Optional<T>`'s own `?` (see
Design above). Only the failure side differs: `None`'s own branch builds a fresh, payload-less
`IrOptionalNew{value: -1, payloadTypeName: T}`; `Result<T,E>`'s own branch first extracts the
operand's own Err payload (`IrOptionalUnwrap{field: 2}` - preserving the *real* error value, unlike
`None`, which has nothing to preserve), then builds a fresh `IrResultNew{isOk: false, value:
<extracted>, otherPayloadTypeName: <the enclosing function's own Ok/T type>}`.

---

# Interpreter: A Dynamically-Typed `?`, and One Real Lifetime Bug Avoided by Precedent

`ResultInstance { bool isOk; Value okValue; Value errValue; }` mirrors `OptionalInstance` exactly
(reference semantics, shared_ptr-wrapped, an unread field left default-constructed). Unlike
`IrGenerator`, the interpreter's `Value` is dynamically typed, so `TryExpr`'s own generalization
needs no return-type inspection at all - it just checks which shape `operandValue` *actually is* at
runtime (`std::get_if<shared_ptr<OptionalInstance>>` vs `<ResultInstance>`) and propagates the
matching kind, reusing the exact same `ReturnSignal` mechanism `return` itself already throws.
`toString`'s new `"Ok(<payload>)"`/`"Err(<payload>)"` case was hand-verified byte-for-byte against
`LlvmIrEmitter`'s own `@axea.result.<id>.to_str`, the same discipline every prior printing phase in
this codebase already established.

**One lifetime concern actively avoided by an already-established convention, not newly
discovered here:** `docs/language/0062-display-trait.md`'s own file-local `g_activeInterpreter`
pointer (used by `toString` to reach a live `Interpreter` for Display-trait dispatch) is reset in
`~Interpreter()`, not at the end of `run()` - a design that phase had to fix once, after its first
version got the top-level-auto-print timing window wrong. `Result<T,E>`'s own `toString` case adds
no *new* interpreter-lifetime dependency at all (it's a pure, self-contained `Value` inspection,
same as `Optional<T>`'s own case beside it), so this phase inherits that already-corrected design
for free rather than needing to rediscover the same bug.

---

# Worked Example

```ax
divide(a: i32, b: i32) -> Result<i32, i32>
{
    if b == 0
    {
        return Err(0 - 1)
    }
    return Ok(a / b)
}

sumTwo(a: i32, b: i32, c: i32, d: i32) -> Result<i32, i32>
{
    x = divide(a, b)?
    y = divide(c, d)?
    return Ok(x + y)
}

good = sumTwo(10, 2, 20, 4)
bad = sumTwo(10, 2, 20, 0)

goodVal = good.unwrap_or(0 - 1)
badVal = bad.unwrap_or(0 - 1)
goodIsOk = good.is_ok()
badIsErr = bad.is_err()

results = [good, bad]

struct Wrapper { r: Result<i32, i32> }
w = Wrapper { r: good }
```

```text
good = Ok(10)
bad = Err(-1)
goodVal = 10
badVal = -1
goodIsOk = true
badIsErr = true
results = [Ok(10), Err(-1)]
w = Wrapper { r: Ok(10) }
```

`sumTwo(10, 2, 20, 0)`'s own two `?`s are the interesting part, mirroring `Optional<T>`'s own
worked example precedent exactly: the first `divide(10, 2)?` succeeds (`x = 5`), but the second
`divide(20, 0)?` hits `Err(-1)` and returns *that exact value* immediately from `sumTwo` - `y` is
never bound, `Ok(x + y)` never runs. Hand-verified byte-for-byte identical across the interpreter,
`ax llvm-ir | clang -x ir -O0`, and `-O1`, including `Result<T,E>` nested inside an `Array` and
inside a struct field (both routing through the same default element/field printer `Optional<T>`
and every struct/collection already share).

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **`?`'s Err type must match the enclosing function's own Err type exactly - no automatic
  error-type conversion.** Rust's own `?` runs an `Into`/`From` conversion as it propagates, letting
  a low-level error bubble up as a broader error type automatically; this phase's `?` requires
  `operandType.valueTypeName == expectedReturnType->valueTypeName` exactly, throwing a clear error
  otherwise. A real conversion mechanism would need a trait-like "how do I turn an `E1` into an
  `E2`" lookup this language has no equivalent of yet (`docs/language/0062-display-trait.md`'s own
  `Display` is the only trait that exists, and it isn't that).
- **Printing `Result<T,E>` is restricted to `T,E ∈ {i32, i64, f64, bool}`, independently on each
  side** - the identical restriction `Optional<T>`'s own printing already has (see
  `docs/language/0052-optional.md`'s own Known Imprecision), for the identical reason:
  `Ok(x)`/`Err(e)` themselves place no such restriction on construction, only *rendering* one to
  text is this narrow. `Result<i32, str>` type-checks, constructs, and propagates through `?`
  completely normally - only interpolating/printing one directly throws a clear compile-time error
  in the LLVM backend (the interpreter has no such restriction at all, an asymmetry `Optional<T>`
  already established and this phase inherits rather than resolves).
- **No `.ok()`/`.err()` conversion to `Optional<T>`, no `.map`/`.and_then` combinators** - Rust's
  own `Result` has a wide combinator API; this phase builds exactly the surface the source doc's
  own two-line sketch and `Optional<T>`'s own established four-method precedent
  (`Some`/`None`/`?`/`.unwrap_or`/`.is_some`/`.is_none`) call for for `Result<T,E>`
  (`Ok`/`Err`/`?`/`.unwrap_or`/`.is_ok`/`.is_err`), not a full port of Rust's own API surface.
- **No user-facing distinction between "this instruction happens to also work for Result" and
  "this is Result's own instruction"** - `IrOptionalIsSome`/`IrOptionalUnwrap` keep their
  `Optional`-flavored names despite now serving both types (see Design above for why duplicating
  them would only add dead-identical codegen, not real independence) - a deliberate naming
  compromise, not an oversight; their own doc comments in `Ir.hpp` explain the sharing explicitly
  for exactly this reason.

---

# Guiding Rule

> A second sum type doesn't automatically mean twice the runtime machinery - checking whether an
> existing instruction's own codegen ever actually depended on which *concrete* type it was
> reading (here: it didn't, since `extractvalue`/`xor` only ever cared about the register's own
> already-resolved LLVM type, never a hardcoded name) is worth doing before writing a parallel
> sibling instruction that would only ever produce byte-identical output. And borrowing a
> representation from the closest structural analogue already in the codebase (`Map<K,V>`'s own
> two-type-parameter string fields, not `Optional<T>`'s one-parameter shape) is often more honest
> than inventing a third pattern - `Result<T,E>` isn't really "`Optional<T>` with an error message
> bolted on," it's "`Map<K,V>`'s own representation, controlled by `Optional<T>`'s own control
> flow" - and naming that plainly, in both directions, made several design decisions fall out
> automatically rather than needing to be argued for individually.
