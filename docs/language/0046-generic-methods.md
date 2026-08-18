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
restricted to `T ∈ {i32, bool}` - the two primitive types with an obvious,
unambiguous text encoding. No user-defined generic methods, no multiple
type arguments, no method-resolution-by-type-argument beyond this one
built-in - the same "build the general mechanism, ship the one concrete
use case the roadmap actually named" scoping call this whole session has
made repeatedly.

```ax
n = "42".parse<i32>()
b = "true".parse<bool>()
year = date[..4].parse<i32>()
```

**Deliberately without the `?` operator.** The design doc's own example
uses `.parse<i32>()?`, implying a fallible result (`Optional<i32>` or
similar) that `?` unwraps. Neither `Optional<T>` nor `?` exist in this
language. Rather than block `.parse<T>()` entirely on building an entire
error-handling story, `.parse<T>()` here returns `T` **directly**, with a
defined, harmless fallback on invalid input (`0` for `i32`, `false` for
`bool`) rather than raising an error - the same kind of scope-narrowing
deviation `str[a..b]`'s own "copies instead of viewing" and `Buffer`'s
`.len` rename already made, documented here rather than silently assumed.

---

# Design: Two Hand-Rolled Runtime Functions, Not Inlined Logic

`.parse<T>()`'s object is resolved to a bare `i8*` via `resolveStrPtr`
(shared with `String`/`Buffer`'s own str-coercion), then handed to one of
two **shared, lazily-registered, module-level runtime functions** -
`@axea.parse.i32`/`@axea.parse.bool` - the same "register once, call
everywhere" pattern `registerMapInstantiation` already established for
monomorphized collections, at the smallest possible scale (exactly two
possible registrations, not per-shape).

**`@axea.parse.i32`**: a hand-rolled digit loop - checks for a leading
`-`, then accumulates `acc = acc*10 + digit` over consecutive ASCII
digits (`'0'..'9'`) until a non-digit or the null terminator, negating
the result at the end via `select` if a `-` was seen. Loop-carried state
(`idx`/`acc`/`negFlag`) lives in three `alloca` slots, reloaded fresh
each iteration - the same "no `phi` anywhere" convention every loop in
this backend has followed since its first one. Invalid input (no digits
at all) simply never enters the accumulation branch, yielding `0` -
no separate validation path needed, the loop's own structure already
produces the documented fallback for free.

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
were given, and `typeArgument` is one of the two supported names -
returning `kI32`/`kBool` respectively.

```text
$ ax capabilities bad.ax   # x = 5.parse<i32>()
error: 'parse' requires str, got i32
$ ax capabilities bad.ax   # x = "5".parse<str>()
error: parse<str> is not supported - only parse<i32> and parse<bool> are implemented this phase
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
since the result is always a plain `i32`/`bool` value, "owned" here means
exactly what it already means for every other primitive, needing no new
reasoning at all.

---

# `IrGenerator` / Interpreter: One Instruction, One Mirrored Implementation

`IrParse{object, targetType}` lowers directly from a `MethodCallExpr`
whose `method == "parse"`, checked first in `IrGenerator`'s own dispatch
chain (unambiguous by name - no disambiguation `*Kind` resolver needed,
same as `append_line`/`enqueue`/`dequeue` before it).

The interpreter's own `.parse<T>()` handling is a **second, independent
implementation** of the exact same rules `@axea.parse.i32`/
`@axea.parse.bool` hand-emit as LLVM IR - the same leading-`-` handling,
the same digit loop, the same "invalid input yields the documented
fallback" choice, the same "must be exactly `'true'`" bool rule - written
in plain C++ instead. Not shared with the LLVM backend (per this
codebase's "separate over shared" convention for interpreter-vs-backend
logic - they're different *operations*, one running at compile time
producing IR text, the other running directly), but verified to agree
byte-for-byte via the same diff-against-compiled-output discipline every
prior feature here has used - including the negative-number, invalid-
input, and case-sensitivity edge cases specifically.

```text
$ ax ir examples/generic_methods.ax
%2 = str.slice %0[..%1]
%3 = parse<i32> %2
```

---

# Worked Example

`examples/generic_methods.ax`:

```ax
extractYear(date: str) -> i32
{
    return date[..4].parse<i32>()
}

date = "2026-08-18"
year = extractYear(date)

count = "42".parse<i32>()
negative = "-17".parse<i32>()
invalid = "abc".parse<i32>()

enabled = "true".parse<bool>()
disabled = "false".parse<bool>()

s = String("123")
fromString = s.parse<i32>()
```

```text
$ ax run examples/generic_methods.ax
date = 2026-08-18
year = 2026
count = 42
negative = -17
invalid = 0
enabled = true
disabled = false
s = 123
fromString = 123
$ ax llvm-ir examples/generic_methods.ax | clang -x ir -O1 - -o out && ./out
# byte-for-byte identical (also re-verified at -O0)
```

`year = 2026` confirms `.parse<i32>()` composes directly with `str`
slicing's own result through a borrowed function parameter; `invalid = 0`
confirms the documented invalid-input fallback, not a crash or thrown
error, matching the "no `?` operator yet" scope decision above.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **No `Optional<T>`/`?` operator.** `.parse<T>()` returns `T` directly
  with a silent fallback on invalid input, not a fallible result - the
  design doc's own `.parse<i32>()?` isn't reproducible until both of
  those exist.
- **Only `T ∈ {i32, bool}`.** No `parse<char>`, `parse<f32>` (no float
  type exists yet), or any user-defined target type.
- **No overflow checking for `parse<i32>`.** A very large digit sequence
  wraps silently, matching plain `i32` arithmetic's own existing
  unchecked-overflow behavior everywhere else in this backend.
- **`parse<bool>` accepts only the exact literal `"true"`/anything-else`
  distinction** - no case-insensitivity, no `"1"`/`"0"`, no
  `"yes"`/`"no"`.
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
