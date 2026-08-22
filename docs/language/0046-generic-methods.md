# `.method<T>()`: Generic Method Calls, and `.parse<T>()` as the First (and Only) One

**Status:** Implemented
**Document:** `0046-generic-methods.md`

---

# Motivation

`docs/std/strings/0008-parsing-formatting.md` sketches `.parse<T>()`:

```ax
year = date[..4].parse<i32>()?
```

named as a blocker in three earlier docs (`docs/language/0032-slices.md`,
`0042-string.md`) as needing "generic method calls" - a syntax this
language had no support for at all. This phase implements the general
`object.method<TypeArg>(args)` **syntax and dispatch mechanism**, and uses
it for exactly one concrete method: `.parse<T>()` on `str`/`String`,
originally restricted to `T ∈ {i32, bool}` - the two primitive types with
an obvious, unambiguous text encoding. `T ∈ {i32, i64, f64, bool}` as of
`docs/language/0051-numeric-widening.md`, once `i64`/`f64` themselves
existed to parse into. No user-defined generic methods, no multiple type
arguments, no method-resolution-by-type-argument beyond this one built-in
- the same "build the general mechanism, ship the one concrete use case
the roadmap actually named" scoping call this whole session has made
repeatedly.

```ax
n = "42".parse<i32>()
b = "true".parse<bool>()
year = date[..4].parse<i32>()
big = "123456789012".parse<i64>()
pi = "3.14159".parse<f64>()
```

**Originally, deliberately without the `?` operator.** The design doc's
own example uses `.parse<i32>()?`, implying a fallible result
(`Optional<i32>` or similar) that `?` unwraps. At the time this phase
first landed, neither `Optional<T>` nor `?` existed in this language.
Rather than block `.parse<T>()` entirely on building an entire error-
handling story, `.parse<T>()` originally returned `T` **directly**, with
a defined, harmless fallback on invalid input (`0` for `i32`/`i64`, `0.0`
for `f64`, `false` for `bool`) rather than raising an error - the same
kind of scope-narrowing deviation `str[a..b]`'s own "copies instead of
viewing" and `Buffer`'s `.len` rename already made, documented here
rather than silently assumed. `Optional<T>`/`?` are both real now (see
`docs/language/0052-optional.md`), and `.parse<T>()` was rebuilt to
return `Optional<T>` with genuine success/failure detection as part of
that same phase - the design doc's own `.parse<i32>()?` example is
reproducible today.

---

# Design: Four Shared Runtime Functions, Not Inlined Logic

`.parse<T>()`'s object is resolved to a bare `i8*` via `resolveStrPtr`
(shared with `String`/`Buffer`'s own str-coercion), then handed to one of
four **shared, lazily-registered, module-level runtime functions** -
`@axea.parse.i32`/`@axea.parse.i64`/`@axea.parse.f64`/`@axea.parse.bool`
- the same "register once, call everywhere" pattern
`registerMapInstantiation` already established for monomorphized
collections, at a small scale (four possible registrations, not
per-shape). `@axea.parse.i64` was added alongside `i64` itself
(`docs/language/0051-numeric-widening.md`) - a mechanical width
translation of `@axea.parse.i32`'s own loop, nothing new. `@axea.parse.f64`
is the one genuinely different addition: unlike the other three, it isn't
hand-rolled - see below.

**`@axea.parse.i32`**: a hand-rolled digit loop - checks for a leading
`-`, then accumulates `acc = acc*10 + digit` over consecutive ASCII
digits (`'0'..'9'`) until a non-digit or the null terminator, negating
the result at the end via `select` if a `-` was seen. Loop-carried state
(`idx`/`acc`/`negFlag`) lives in three `alloca` slots, reloaded fresh
each iteration - the same "no `phi` anywhere" convention every loop in
this backend has followed since its first one. Invalid input (no digits
at all) simply never enters the accumulation branch, yielding `0` -
no separate validation path needed, the loop's own structure already
produces the documented fallback for free. `@axea.parse.i64` is the
identical loop at `i64` width throughout (wider `acc`/negation, same
`i32`-width `idx` since a string index never needs 64 bits) - no new
algorithm.

**`@axea.parse.f64`**: a real `declare double @strtod(i8*, i8**)` libc
call, `endptr` always `null` - a deliberate departure from every other
`@axea.parse.*` function's own hand-rolled style. Decimal-to-binary
floating-point parsing is a real, easy-to-get-subtly-wrong algorithm
(rounding, exponents, edge cases around denormals) - the same "reuse
libc's own well-tested logic rather than risking a hand-written bug"
reasoning `registerI32ToStrRuntime`'s own choice of `sprintf` over
hand-rolled itoa already established, just in the parsing direction
instead of formatting. `strtod` already returns `0.0` for input with no
valid leading number, giving the documented invalid-input fallback for
free, with zero extra logic - the same "the primitive's own natural
behavior already matches what we wanted" luck `@axea.parse.i32`'s own
loop structure has for `0`.

**`@axea.parse.bool`**: a hand-rolled **short-circuit** byte-by-byte
comparison against the fixed literal `"true"` - real branches, not an
unrolled straight-line chain, specifically so a short input like `"t"`
never reads past its own null terminator: each subsequent byte is only
read after confirming the previous one matched a non-null expected
character, which guarantees the buffer actually continues that far. This
is a genuine, non-obvious correctness requirement caught by reasoning
through the memory-safety implications before writing the naive unrolled
version - a fixed 5-byte-offset read chain would silently read
out-of-bounds for any input shorter than 4 characters. Anything other
than exactly `"true"` (including `"TRUE"`, `"false"`, `""`, `"trueish"`)
parses as `false`.

**No `@strcmp` needed.** Rather than declare a fourth libc extern and
hoist a `"true"` string constant (which would have raised a real ordering
question - runtime function *registration* happens lazily during
instruction emission, not during the `inferTypes` pass every earlier
`register*Instantiation` relies on for its own "safe to snapshot" timing
guarantee), the fixed-length comparison is hand-rolled directly against
literal byte values (`116`/`114`/`117`/`101`/`0` = `'t'`/`'r'`/`'u'`/
`'e'`/`\0`) - self-contained, no new extern, no string-hoisting-order
question to answer at all.

---

# Parsing: A 4-Token Lookahead, Not a Backtracking Grammar

The classic ambiguity every C++-like language with both generics and
`<`/`>` comparison operators hits: is `x.field < y` a less-than
comparison, or the start of a generic call `x.field<T>(...)`? This parser
has no backtracking machinery, so the fix is a **fixed 4-token lookahead**
committed to only when it's unambiguous: after `object.method`, if the
very next four tokens are exactly `<` `Identifier` `>` `(`, this is
treated as a generic call; otherwise nothing is consumed and parsing
falls through to the existing method-call-vs-field logic unchanged. This
means `p.field < 10` (comparison) and `s.parse<i32>()` (generic call) are
never confused, verified directly with a dedicated regression test
(`p.field < 10` where `field` is even `i32`-typed, so a naively-permissive
lookahead would have every reason to misfire).

`MethodCallExpr` gained one new field (`typeArgument`, empty for every
ordinary call) rather than becoming a new AST node - every existing pass
that already walks `object`/`arguments` generically (`CapabilityChecker`,
`RegionChecker`) needed **zero** changes to keep working for the new
shape too, since they never look at `typeArgument` at all.

```text
$ ax ast examples/generic_methods.ax
MethodCall(parse<i32>)
  StrSlice
    Name(date)
    Integer(4)
```

---

# Type Checking: One New Branch, Checked Before the Object-Type Dispatch

`.parse<T>()`'s own `checkExpr` case is checked *before* the existing
`objectType.kind == TypeKind::List`/`Stack`/... dispatch chain, since it
applies across two different `TypeKind`s (`str` is `TypeKind::String`;
`String` is confusingly `TypeKind::OwnedString` - see `0042-string.md`'s
own naming note) rather than being tied to one. Validates, in order:
`object` is str-coercible, `typeArgument` is non-empty, zero arguments
were given, and `typeArgument` is one of the four supported names -
returning `Optional<i32>`/`Optional<i64>`/`Optional<f64>`/`Optional<bool>`
respectively (as of `docs/language/0052-optional.md` - originally, before
`Optional<T>` existed, this returned `kI32`/`kI64`/`kF64`/`kBool`
directly).

```text
$ ax capabilities bad.ax   # x = 5.parse<i32>()
error: 'parse' requires str, got i32
$ ax capabilities bad.ax   # x = "5".parse<str>()
error: parse<str> is not supported - only parse<i32>, parse<i64>, parse<f64>, and parse<bool> are implemented this phase
$ ax capabilities bad.ax   # x = "5".parse()
error: 'parse' requires an explicit type argument, e.g. parse<i32>()
```

---

# Capability Checking / Region Checking: Zero New Code

`.parse<T>()` is deliberately **not** added to `CapabilityChecker`'s
write-raising method-name list - it never mutates its receiver, so the
existing generic `MethodCallExpr` walk (recurse into `object`/
`arguments`, infer nothing extra) already gives it the correct default:
`Read`. `RegionChecker`'s own generic `MethodCallExpr` case already
returns `Region::Owned` for any method whose name isn't `"get"`/`"peek"`
(the two aliasing exceptions, neither of which `.parse<T>()` is) - and
since the result is always a plain `i32`/`i64`/`f64`/`bool` value,
"owned" here means exactly what it already means for every other
primitive, needing no new reasoning at all.

---

# `IrGenerator` / Interpreter: One Instruction, One Mirrored Implementation

`IrParse{object, targetType}` lowers directly from a `MethodCallExpr`
whose `method == "parse"`, checked first in `IrGenerator`'s own dispatch
chain (unambiguous by name - no disambiguation `*Kind` resolver needed,
same as `append_line`/`enqueue`/`dequeue` before it).

The interpreter's own `.parse<T>()` handling is a **second, independent
implementation** of the exact same rules `@axea.parse.i32`/
`@axea.parse.i64`/`@axea.parse.bool` hand-emit as LLVM IR - the same
leading-`-` handling, the same digit loop (`i32`/`i64` share it outright
in the interpreter, since both already share the one `std::int64_t`
`Value` alternative - see `docs/language/0051-numeric-widening.md`), the
same success/failure rule, the same three-way `'true'`/`'false'`/invalid
bool rule - written in plain C++ instead. `f64` is the one case that
*isn't* independently reimplemented: both backends call the identical
underlying libc `strtod` (`std::strtod` in the interpreter, `@strtod` in
the compiled backend), so they agree by construction rather than by
parallel-implementation-and-verification. Not shared with the LLVM
backend otherwise (per this codebase's "separate over shared" convention
for interpreter-vs-backend logic - they're different *operations*, one
running at compile time producing IR text, the other running directly),
but verified to agree byte-for-byte via the same diff-against-compiled-
output discipline every prior feature here has used - including the
negative-number, invalid-input, and case-sensitivity edge cases
specifically.

As of `docs/language/0052-optional.md`, both backends wrap their result in
a real `Optional<T>` with genuine success/failure detection (a real
`digitCount`/`endptr`-driven check, not the "invalid input yields a
silently-returned fallback" contract originally described here) - see that
doc's own `LlvmIrEmitter`/Interpreter sections for the exact rework;
`IrParse` itself is unchanged, still the single instruction described
above.

```text
$ ax ir examples/generic_methods.ax
%2 = str.slice %0[..%1]
%3 = parse<i32> %2
```

---

# Worked Example

`examples/generic_methods.ax` (updated for `docs/language/0052-optional.md`
- `extractYear` now returns `Optional<i32>`, unwrapped at the call site):

```ax
extractYear(date: str) -> Optional<i32>
{
    return date[..4].parse<i32>()
}

date = "2026-08-18"
year = extractYear(date).unwrap_or(0)

count = "42".parse<i32>()
negative = "-17".parse<i32>()
invalid = "abc".parse<i32>()

enabled = "true".parse<bool>()
disabled = "false".parse<bool>()

s = String("123")
fromString = s.parse<i32>()

bigCount = "123456789012".parse<i64>()
pi = "3.14159".parse<f64>()
invalidFloat = "abc".parse<f64>()
```

```text
$ ax run examples/generic_methods.ax
date = 2026-08-18
year = 2026
count = Some(42)
negative = Some(-17)
invalid = None
enabled = Some(true)
disabled = Some(false)
s = 123
fromString = Some(123)
bigCount = Some(123456789012)
pi = Some(3.14159)
invalidFloat = None
$ ax llvm-ir examples/generic_methods.ax | clang -x ir -O1 - -o out && ./out
# byte-for-byte identical (also re-verified at -O0)
```

`year = 2026` confirms `.parse<i32>()` composes directly with `str`
slicing's own result through a borrowed function parameter, unwrapped via
`.unwrap_or(0)`; `invalid = None`/`invalidFloat = None` confirm real
failure detection now, not a silently-returned fallback - printed as
`None` via `docs/language/0052-optional.md`'s own top-level `Optional<T>`
stringification. `bigCount = Some(123456789012)` is a value that
genuinely doesn't fit in `i32`, confirming `parse<i64>()` isn't just
`parse<i32>()` truncated.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **`Optional<T>`/`?` now exist** - see `docs/language/0052-optional.md`.
  `.parse<T>()` returns a real `Optional<T>`, not `T` directly with a
  silent fallback; the design doc's own `.parse<i32>()?` is reproducible
  now, inside a function whose own return type is `Optional<U>`.
- **Only `T ∈ {i32, i64, f64, bool}`.** No `parse<char>`, `parse<f32>`/
  `parse<i8>`/... (only `i32`/`i64`/`f64` themselves exist as real
  numeric types - see `docs/language/0051-numeric-widening.md`), or any
  user-defined target type.
- **No overflow checking for `parse<i32>`/`parse<i64>`.** A very large
  digit sequence still wraps silently and returns `Some(<wrapped
  result>)`, not `None` - overflow isn't treated as failure, matching
  plain `i32`/`i64` arithmetic's own existing unchecked-overflow behavior
  everywhere else in this backend. `parse<f64>` doesn't share this gap -
  `strtod` itself already saturates to `+-HUGE_VAL` on overflow rather
  than wrapping, libc's own well-defined behavior, inherited for free.
- **`parse<bool>` accepts only the exact literals `"true"`/`"false"`** -
  `Some(true)`/`Some(false)` respectively, `None` for anything else
  (see `docs/language/0052-optional.md` - originally, before
  `Optional<T>` existed, anything not exactly `"true"` parsed as a
  defined `false`, not a distinguishable failure). No case-insensitivity,
  no `"1"`/`"0"`, no `"yes"`/`"no"`.
- **No general user-defined generic methods.** The `<T>` syntax exists
  only for this one compiler-intrinsic method; there is no mechanism for
  Axea source to define its own generic method.

---

# Guiding Rule

> A blocked feature ("we can't do X until we have generic methods") is
> often really two separable questions: does the *syntax* need to exist
> in general, and does *this one motivating use case* need every piece of
> machinery the syntax could theoretically support? Building the full
> `object.method<T>(args)` grammar - disambiguated correctly against
> ordinary comparison operators via a fixed lookahead, not a hack - while
> deliberately implementing only the single method the roadmap actually
> named, and deliberately dropping the fallible-result half of that
> method's own design doc rather than building `Optional<T>` and `?` as
> unplanned prerequisites, is the same discipline `Buffer`'s `.len`
> rename and `str[a..b]`'s "copies, doesn't view" already practiced: ship
> the real, useful, honestly-scoped slice of the feature, and write down
> exactly which slice that was.
