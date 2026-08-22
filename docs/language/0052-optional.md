# `Optional<T>`, `?`, and a Genuinely Fallible `.parse<T>()`

---

# Motivation

`.parse<T>()` (`docs/language/0046-generic-methods.md`) has never actually
been able to fail. Invalid input parsed as a defined, silently-returned
fallback - `"abc".parse<i32>()` was `0`, `"garbage".parse<bool>()` was
`false`, `"nope".parse<f64>()` was `0.0` - indistinguishable from genuinely
valid input that happened to parse to the same value. `TypeKind::Optional`
has sat in the `TypeKind` enum since `docs/language/0005-type-system.md`,
declared "for architectural fidelity" alongside `I8`/`F32`/every unsigned
width, with zero checking logic anywhere - exactly the kind of inert
declaration `docs/language/0051-numeric-widening.md` made `I64`/`F64` real
instead of. This phase does the same for `Optional`: a real sum type,
constructed via `Some(x)`/`None`, consumed via a new `?` postfix operator
or via `.unwrap_or(default)`/`.is_some()`/`.is_none()`, and `.parse<T>()`
reworked to return `Optional<T>` with genuine success/failure detection
instead of a fallback that can never be told apart from real input.

**Why the four-method API, not just `?`.** `?` alone (propagate on `None`,
unwrap on `Some`) is only legal inside a function whose own declared return
type is `Optional<U>` - and this language's dominant style is short,
top-level scripts and ordinary non-`Optional`-returning functions. Scoping
the API to `?` alone would mean `.parse<T>()`'s result could never be
consumed anywhere outside an `Optional`-returning function, which is most
code. `.unwrap_or(default)`/`.is_some()`/`.is_none()` are the non-
propagating half - ordinary method calls, legal anywhere, exactly like
every other method in this language - so `"42".parse<i32>().unwrap_or(0)`
works at top level, inside an `i32`-returning function, anywhere at all.

---

# Design: A Flat `elementKind`, Not a String - and a *Named*, By-Value LLVM Type

**Type representation.** `Optional<T>`'s payload is always a single,
non-recursively-generic type in this phase (`i32`/`i64`/`f64`/`bool` from
`.parse<T>()`, or - since `Some(x)`/`None` place no restriction on the
payload type beyond ordinary type-checking - a `char`/`str`/`String`/
struct). That's exactly the shape `Array`/`Slice`/`List`'s own flat
`elementKind`/`elementStructName` fields were designed for
(`docs/language/0031-arrays.md`), so `Optional<T>` reuses that
representation directly rather than `Map`/`Set`'s own string-based
`elementTypeName` (reserved for genuinely recursively-nested K/V shapes -
see `docs/language/0034-maps-and-sets.md`). `resolveType`'s own
`"Optional<elem>"` branch mirrors `slice<elem>`'s exactly, one level deep,
rejecting a nested `Optional<Optional<T>>` the same way slice rejects a
nested `slice<slice<T>>`.

**LLVM representation - and why it can't be anonymous.** Every existing
by-value type in this backend (`slice<T>`'s own `"{T*, i32}"` fat pointer)
is an *anonymous* struct literal. `Optional<T>` can't be: `isSliceType`'s
own check is `type.front() == '{' && type.back() != '*'` - true of *any*
anonymous by-value struct, not just slices - so an anonymous
`Optional<i32> = "{i1, i32}"` would silently satisfy `isSliceType`'s own
loose test too, and any code path that then called `sliceElementType` on
it would slice garbage out of text that was never a slice's own `"{T*,
i32}"` shape to begin with. So `Optional<T>` is instead a genuinely
*named* struct, `%axea.Optional.<id> = type { i1, <payload> }`, registered
lazily via `registerOptionalInstantiation` - mirroring `Map`/`Set`/
`LinkedList`/`SortedMap`/`SortedSet`'s own named-type-per-instantiation
convention (`registerSetInstantiation` et al.), just far smaller (one type
declaration, no runtime functions to register alongside it - Optional
needs no hash/equality/resize machinery). `isOptionalType` then just
checks the `"%axea.Optional."` prefix - no further disambiguation needed,
since nothing else in this backend ever produces that prefix.

Unlike every other named type here, though, `Optional<T>` is used *by
value* (a function parameter/return type, an `extractvalue`/`insertvalue`
operand) rather than always behind at least one pointer level the way
`%axea.MapEntry.<id>**`/`%axea.LLNode.<id>*` always are. That distinction
turned out to matter: **hand-verified against clang**, `insertvalue %foo
undef, i32 1, 0` fails with "invalid indices for insertvalue" if `%foo =
type { i32 }` appears *later* in the same file - even though the
identical pattern behind a pointer (`%foo*`, exactly how every other named
type here is used) parses fine either order. So `Optional<T>`'s own type
declarations can't wait until after every function is emitted the way
`mapSetTypeDeclsText_`/etc. safely do (see their own comment on why that's
safe for *them*) - see `LlvmIrEmitter` below for the discovery pass this
required.

**Registration keyed by LLVM text, not Axea type name.** `Some(x)`'s own
payload type is read off `x`'s already-inferred LLVM register type (no
Axea-level type string available at that point - `IrGenerator` keeps no
real type table by design), while `.parse<T>()`/a declared `Optional<T>`
return type both start from an Axea type name string. To guarantee these
three paths all resolve to the *same* `%axea.Optional.<id>` for the same
payload shape - not three different, incompatible named types for what's
semantically one type - `registerOptionalInstantiation`(Axea name) is a
thin wrapper that just resolves to LLVM text first, then defers to
`registerOptionalInstantiationForLlvmPayload`(LLVM text), the actual
memoized registration, keyed by that LLVM text directly.

**Constructor syntax.** `Some(x)` mirrors `String(text)`'s own "identifier
+ one parenthesized argument" parse shape exactly (`Parser::parsePrimary`).
`None` mirrors `true`/`false`'s own bare-keyword shape, though - like
`String`/`Buffer`/`Some` - it's a special-cased identifier text check, not
a reserved `TokenKind`. `None` carries no expression to synthesize a
payload type from, so (unlike `Some(x)`, an ordinary bottom-up synthesis)
its type can only be resolved where the surrounding context already
supplies one: `TypeChecker::checkStmt`'s `AssignmentStmt` (a declared
type, `x: Optional<T> = None`) and `ReturnStmt` (the enclosing function's
own declared return type, `return None`) cases both special-case a bare
`NoneExpr` *before* calling the generic `checkExpr`, taking the type from
context directly. Anywhere else, `checkExpr(NoneExpr)` throws a clear
"cannot infer" error - the same "explicit type required" stance every
other under-constrained construct in this language already takes.

---

# Parsing

`?` needed a new token (`TokenKind::Question`, previously falling into the
`Invalid` catch-all) and a new postfix case, added to `parsePostfix`'s
existing `Dot`/`LeftBracket` loop (not the separate trailing `as` loop) so
it chains naturally both directions - `opt?.field` and `foo().bar<T>()?`
both parse. `Optional<T>`'s own type syntax reuses `Less`/`Greater` the
same way `slice<T>`/`List<T>`/... already do - just one more name added to
`parseTypeName`'s existing single-generic-argument dispatch list, no new
lexer/parser machinery needed there at all.

---

# Type Checking

`checkExpr` gained three new cases: `SomeExpr` (bottom-up synthesis,
`Optional<checkExpr(value)>`), `NoneExpr` (throws unless intercepted by
context - see Design above), and `TryExpr` (`?`) - which needs the
*enclosing function's own declared return type* to validate against. That
turned out to already be available everywhere it was needed: `checkExpr`
already threads an `expectedReturnType` parameter to every single
call site (originally for `return`'s own type-checking), so `?`'s
validation - `expectedReturnType` must itself be `Optional<U>` for *some*
U, `?`'s own operand must be `Optional<T>`, result type is `T` - needed no
new context threading at all, just a new case using context that was
already there.

`.parse<T>()` now returns `Optional<T>` (`arrayLikeType(TypeKind::Optional,
...)`) instead of `T` directly. `.unwrap_or(default)`/`.is_some()`/
`.is_none()` are checked in the same "applies across... rather than tied
to one exact TypeKind" placement `.parse`/`.to_cstr`/`.join` already use
(before the per-TypeKind dispatch chain below), even though these three
*are* tied to exactly one TypeKind (`Optional`) - kept there anyway since
Optional has no TypeKind-keyed "new" dispatch section of its own to join
(never constructed via a `.method()` call, only `Some(x)`/`None`/
`.parse<T>()`).

---

# Capability/Region Checking

`RegionChecker`'s `regionOfExpr` has a final fallback (`return
RegionInfo{Region::Owned, "", ""}`) for any unhandled expression shape -
but that fallback doesn't *recurse* into sub-expressions at all, so
leaving `SomeExpr`/`TryExpr` unhandled would silently fail to track a
borrowed value escaping through `Some(...)` or `?`. Both got real cases
instead: `SomeExpr` walks its value (still always `Owned` overall, same as
`CastExpr`'s identical reasoning - the payload is always either a plain
value or, if a struct, a fresh recursive-borrow check via the walk itself);
`TryExpr` propagates its *operand's* region directly, exactly like
`IndexExpr`'s own element-read does, so a borrowed struct payload
extracted through `?` still carries its region correctly. `NoneExpr` needs
no case - `Owned`, no sub-expressions, exactly what the generic fallback
already produces.

`CapabilityChecker`'s own `inferExpr`/`checkMovesInExpr` have an
analogous "unhandled expression = no sub-expressions, no capability
effect" implicit default (a bare trailing comment, not a real fallback
case) - `SomeExpr`/`TryExpr` got explicit one-line recursive cases in both
(mirroring `CastExpr`'s own identical one-liners) so a moved-from name or
a parameter needing a capability raised isn't silently missed just
because it's wrapped in `Some(...)` or unwrapped via `?`.

---

# `IrGenerator`: Three New Instructions, and `?`/`.unwrap_or` as `IrBranch`

Three new IR instructions, all straight-line except where noted:
`IrOptionalNew` (`Some(x)`/`None` - `value` is `-1` for `None`;
`payloadTypeName` is set only for `None`, since `Some(x)`'s own payload
type is inferred from `value`'s register at the LLVM layer instead - see
Design above), `IrOptionalIsSome` (`.is_some()`/`.is_none()` - a shared
instruction with a `negate` flag rather than two near-identical ones), and
`IrOptionalUnwrap` (the unconditional payload extraction both `?`'s
then-branch and `.unwrap_or`'s is-some branch need - safe specifically
because the `IrBranch` each is embedded in already guarantees `hasValue`
whenever it runs).

**`?` is this language's first expression-context early return.**
Lowered as an `IrBranch` - the same instruction `IfExpr` already uses -
exploiting a piece of `emitBranch` that was already there for a different
reason: a side that ends in `IrReturn` sets its own `*Terminated` flag, so
the merge block's `phi` construction only ever considers sides that
actually reach it (this is what already made `if cond { return a } else {
return b }` work with no merge-block phi at all). `?` builds exactly that
shape - `then`: unwrap and continue; `else`: construct `None` (payload
type read from `ctx.function->returnType`, the enclosing function's own
declared return type - directly available via `Context::function`, no new
threading needed there either) and `return` it - with no new branching
machinery required. `.unwrap_or(default)` is the ordinary, *both-sides-
reach-the-merge* case of the same `IrBranch` shape, structurally identical
to `IfExpr` itself.

`x: Optional<T> = None`/`return None` needed their own small special
cases in `lowerStmt`, exactly mirroring `TypeChecker::checkStmt`'s own -
`IrGenerator` keeps no real type table (see `arrayLengthOf`'s own header
comment), so `None`'s payload type has to be read directly out of the
declared/expected type *string* already sitting in the AST
(`assignment->declaredType`/`ctx.function->returnType`), via a small
`optionalPayloadTypeName("Optional<T>") -> "T"` substring helper - the
same substr-based unwrapping every other generic type string in this
codebase already uses.

---

# Interpreter

A new reference-semantics `OptionalInstance { bool hasValue; Value value;
}` (shared_ptr-wrapped, same convention as every other collection Value
alternative). Unlike the LLVM backend, `None` needs no payload-type
context to construct here at all - `Value` is dynamically typed, so
`OptionalInstance{false, monostate{}}` is a complete `None` regardless of
what `T` the type checker inferred for it; `optionalPayloadTypeName` (the
LLVM backend's own workaround) has no interpreter-side equivalent because
nothing here needs it.

`?`'s early return reuses `ReturnSignal` - the exact same
non-`std::exception` type `return` itself throws, caught at the same
function-call boundary (`callFunction`'s own `try`/`catch`). `TypeChecker`
already guarantees `?` only appears inside a function whose own return
type is `Optional<U>`, so this is always caught there, never escaping to
top-level code.

`.parse<T>()`'s rework needed genuine success detection per type, not just
a new wrapper around the old always-succeeds logic:

- **i32/i64**: the existing hand-rolled digit loop now also tracks a
  `digitCount`; success requires both `digitCount > 0` *and* that the
  loop consumed the *entire* string (`idx == content.size()`) -
  `"123abc"` is `None`, not a truncated `Some(123)`.
- **f64**: `strtod`'s own `endptr` output parameter - previously always
  discarded (`nullptr`) since the old contract never needed to know where
  parsing actually stopped - is now read back: success iff `endptr !=
  content.c_str()` (something was consumed) and `endptr` points at the
  string's own null terminator (nothing was left over).
- **bool**: previously "exactly `\"true\"`, else `false`" (an asymmetric
  fallback). Now genuinely three-way: exactly `"true"` is `Some(true)`,
  exactly `"false"` is `Some(false)`, anything else is `None`.

---

# `LlvmIrEmitter`

**The four `@axea.parse.<T>` runtime functions** (`registerParseRuntime`)
now each return `Optional<T>` (`{i1, T}`) instead of bare `T`, built via
two `insertvalue`s exactly like every other by-value struct construction
in this backend. Success detection mirrors the interpreter's own
independently-reimplemented logic exactly (i32/i64: a `digitCount` counter
plus "stopped at the null terminator" check; f64: `strtod`'s own `endptr`,
previously passed `null` and discarded, now a real output slot; bool: a
second branch chain for `"false"`, alongside the existing `"true"` one) -
verified byte-for-byte against the interpreter via the usual
diff-against-compiled-output discipline.

**Six pre-existing print-dispatch sites** - top-level binding printing,
struct-field printing, and four separate collection-element printing loops
- each independently branch on LLVM element type and fall back to
"assume nested struct pointer, call `@axea.print.<name>`" for anything
unrecognized. `%axea.Optional.<id>` matched none of their existing checks
(`i32`/`i1`/`i8*`/`isCharType`), so every one of them would have taken
that struct-pointer fallback and tried to call a nonexistent
`@axea.print.axea.Optional.<id>` - the exact same bug class
`docs/language/0051-numeric-widening.md` found for `i64`/`double` at these
same six sites. Fixed identically: a new `isOptionalType` branch at each,
routing through `stringifyValueOfType` (which gained its own `Optional`
case, calling a new lazily-registered `@axea.optional.<id>.to_str`
runtime function - `"Some(<payload>)"`/`"None"`, via a real `sprintf`,
reusing `@axea.i32/i64/f64/bool.to_str` for the payload half). Payload
printing is restricted to `i32`/`i64`/`f64`/`bool` (throws a clear error
otherwise) - the exact set `.parse<T>()` itself produces; `Some(x)`/`None`
themselves place no such restriction on construction, only *printing* one
is narrower this phase. The interpreter's own `toString` gained the
identical `"Some(...)"`/`"None"` case, hand-verified to match
character-for-character.

**`sentinelFor`** (Map/SortedMap's "key not found" sentinel - see
`docs/language/0051-numeric-widening.md`'s own fix for `i64`/`double`
here) had no case for a named by-value struct type either, falling back
to `"null"` - invalid IR for anything that isn't a pointer. Fixed with an
`Optional`-specific case returning `"zeroinitializer"`: an all-zero-bits
constant is exactly `None` (`hasValue = i1 0`), already the correct
"nothing here" sentinel with no new machinery needed.

**Type-declaration ordering: a real, hand-verified LLVM constraint.**
Every other named type in this backend is only ever used behind a
pointer, so `mapSetTypeDeclsText_`/`linkedListTypeDeclsText_`/etc. are all
safely appended to the output only *after* every function has been
emitted (their own comments explain why: LLVM doesn't require pointer-
target types to be forward-resolved). `Optional<T>` is used *by value*
throughout, and clang's parser rejects a by-value `insertvalue`/
`extractvalue`/function-signature reference to a named type whose body
appears *later* in the file (confirmed directly - see Design above). So
`emit()` now runs a throwaway discovery pass - `inferTypes` over every
`program.functions` entry into a discarded output stream, purely for its
registration side effects - *before* `out` starts being built at all,
letting `optionalTypeDeclsText_` be written immediately after
`emitStructTypeDecls`, at the true top of the file. The pass is safe to
repeat (every `registerOptionalInstantiation` call is memoized), so
`emitFunction`'s own later, real `inferTypes` call for each function
during normal emission just resolves the identical, already-registered
names - no double registration, no duplicated type-declaration text.

---

# Worked Example

```ax
firstDigit(s: str) -> Optional<i32>
{
    return s.parse<i32>()
}

sumTwo(a: str, b: str) -> Optional<i32>
{
    x = a.parse<i32>()?
    y = b.parse<i32>()?
    return Some(x + y)
}

good = sumTwo("3", "4")
bad = sumTwo("3", "oops")

goodVal = good.unwrap_or(999)
badVal = bad.unwrap_or(999)
goodIsSome = good.is_some()
badIsNone = bad.is_none()

n: Optional<i32> = None
nDefault = n.unwrap_or(42)

parsed = "123".parse<i32>()
invalid = "abc".parse<i32>()
trailing = "123abc".parse<i32>()   # None - not a truncated Some(123)

f = "3.14".parse<f64>()
b1 = "true".parse<bool>()
b2 = "garbage".parse<bool>()       # None - not Some(false)
```

Output (`ax run`, and `ax llvm-ir | clang -x ir -O0/-O1 -` - all three
byte-for-byte identical, hand-verified):

```
good = Some(7)
bad = None
goodVal = 7
badVal = 999
goodIsSome = true
badIsNone = true
n = None
nDefault = 42
parsed = Some(123)
invalid = None
trailing = None
f = Some(3.14)
b1 = Some(true)
b2 = None
```

`sumTwo`'s two `?`s are the interesting part: `sumTwo("3", "oops")` builds
`x = 3` from the first `?` (its operand was `Some(3)`), then hits the
second `?` on `Some`-less `"oops".parse<i32>()` and returns `None`
*immediately* - `y` is never bound, the `Some(x + y)` line never runs.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **`Optional<T>` as a collection element type is now fully supported,
  not rejected** - `List<Optional<i32>>`/`Stack<Optional<i32>>`/etc. all
  work correctly (hand-verified, interpreter and compiled backend
  byte-for-byte identical). This section originally documented a real,
  freshly-discovered bug here (`List<T>`'s own flat `elementKind`
  representation silently corrupting `List<Optional<i32>>` into
  `List<Optional<bool>>`) and scoped it out with a clean rejection instead
  of shipping the corruption - `docs/language/0053-nested-generics.md` is
  the follow-up that fixed the actual representation (moving every
  single-type-parameter kind onto Map/Set's own string-based
  `elementTypeName`, supporting genuinely arbitrary nesting depth), rather
  than leaving the rejection in place permanently. See that doc for the
  one remaining real gap this surfaced: printing a *collection* whose
  element is itself a collection (unrelated to `Optional<T>` specifically
  - `Optional<T>`'s own printing, described just below, is unaffected).
- **Printing `Optional<T>` is restricted to `T ∈ {i32, i64, f64, bool}`.**
  `Some(x)`/`None`/`?`/`.unwrap_or`/`.is_some`/`.is_none` place no
  restriction on `T` at all - a `char`/`str`/struct payload works fully
  for every one of those. Only *rendering* an `Optional<T>` to text (top-
  level auto-print, a struct field, a collection element) is narrower,
  throwing a clear error for anything outside `.parse<T>()`'s own four
  supported types, rather than attempting a recursive stringification this
  phase doesn't build out.
- **No overflow checking**, same as `i32`/`i64`'s own pre-existing
  `.parse<T>()` limitation (`docs/language/0051-numeric-widening.md`) -
  a value that overflows still parses as `Some(<wrapped result>)`, not
  `None`.
- **`?` is the only propagation mechanism** - no `try`/`catch`, no
  `Result<T, E>` (`TypeKind::Error` remains exactly as inert as
  `TypeKind::Optional` was before this phase), no `?` on anything but
  `Optional<T>`.

---

# Guiding Rule

A type declared "for architectural fidelity" with zero checking logic is a
liability, not a placeholder - the moment real code needs it (a fallible
`.parse<T>()`, unavoidably), it's cheaper to make the type real than to
keep inventing narrower workarounds around its absence. And when a new
by-value construct is added to a backend where every prior named type was
pointer-only, don't assume "LLVM doesn't care about declaration order"
generalizes - verify the specific new shape against the real toolchain
before committing to an emission order, the same discipline this
project's own str-ordering and numeric-widening phases already established
for SSA register numbering.
