# `String`: An Owned, Growable Byte Buffer - and the First Value/Reference Boundary

**Status:** Implemented
**Document:** `0042-string.md`

---

# Motivation

`docs/std/strings/0002-string.md` specs `String` as "owned, growable UTF-8
string," with `String("Axea")` construction and `.append(...)`. This phase
implements exactly that surface - construction, `.append`, `.length` - and
nothing from the wider `docs/std/strings/` design set beyond it:
`Buffer` (`0004-buffer.md`) followed in its own later phase
(`docs/language/0043-buffer.md`); interpolation-lowering, slicing
(`0005-slicing.md`), Unicode-aware operations (`0006-unicode.md`),
FFI/`cstr` (`0007-ffi.md`), and `.parse<T>()` (`0008-parsing-formatting.md`)
remain out of scope. Every one of those needs a language feature that
doesn't exist yet (range-slicing syntax, generic method calls, `extern`
declarations, a `?` operator) - this phase's own scope is deliberately just
the byte-buffer type itself, the same "scope down to what's actually
implementable, defer the rest explicitly" call every collection this
session has made.

```ax
name = String("Axea")
name.append(" Language")

n = name.length
```

**`String` is not generic** - unlike every collection built this session
(`List<T>`, `Map<K,V>`, ...), `String` takes no `<...>` type parameter.
Its constructor also takes a *runtime value* argument (`text`), not a type
name - `String("Axea")` copies `"Axea"`'s own bytes at the moment of the
call, the same way `List<T>()`'s own construction allocates fresh empty
storage, but with actual initial content instead of starting empty. This
makes `String` the first "constructor with a real argument" of any
built-in type here, and the first non-generic heap type.

**The real discovery this phase**: `str` (`docs/std/strings/0001-str.md`)
is a plain, **immutable value** - passed by copy, compared by content,
never mutated - while `String` is an **owned, mutable, reference-semantics**
heap value, exactly like every collection here. `docs/std/strings/0001-str.md`
states "Passing `String` automatically lends a `str`," and implementing
that literally - a String standing in for a str argument at a call
boundary - surfaces a real correctness question no earlier collection ever
raised: **does "lending" alias, or copy?** It must copy. `str` is a value
type; if the "lent" `str` aliased the original `String`'s own buffer, a
later `.append()` on the source `String` would retroactively change an
already-passed, already-returned `str` - which is impossible for the LLVM
backend to produce anyway, since `.append()` always reallocates rather
than mutating the existing buffer in place (see Design below). Getting the
interpreter to agree with that took a real, caught bug (see Region
Checking and Interpreter sections).

**Scope, deliberately**: `String(text)`, `.append(other)`, `.length`. `text`/
`other` accept anything str-coercible - a `str`, or another `String` (the
identical "wider owned type lends the narrower borrowed view" rule
`arrayToSliceCoercion` already established for arrays/slices, applied to a
different pair of types). No `[i]`/slicing, no iteration, not hashable, not
a struct field type - mirrors every other collection's own first-phase
restrictions.

---

# Design: `List<i8>`'s Own Header, an Owned Buffer With a Real Runtime Length

A `String` value is a stable pointer to the *exact same* 2-field header
`List<T>`/`Stack<T>` use - `{i32 length, i8* data}*` - deliberately the
literal same LLVM type `List<i8>` would have (`i8` isn't nameable in Axea
source, so this can never actually collide with a real `List<T>`
instantiation). `data` is always kept **null-terminated** - every
construction/append allocates `length + 1` bytes and writes a trailing
`\0` - which is what makes "String lends a str" nearly free: `str` is
already `i8*` in this backend, so a String's own `data` pointer, read
straight out of its header, *is* a valid `str` with no conversion at all.

**The first collection here needing a runtime-computed copy length.**
Every earlier collection's push/append copies a single scalar (`T` by
value) or a compile-time-known count; `String`'s own `text`/`other`
argument is a `str` (`i8*`) with no length field of its own anywhere -
unlike every element type every other collection here copies, raw bytes
have no self-describing size. This phase declares `@strlen` as a third
libc extern alongside the existing `@malloc`/`@printf`
(`docs/language/0022-llvm-backend.md`'s own established pattern of
declaring exactly the libc functions actually needed, not hand-rolling a
byte-scanning loop that libc already provides correctly and fast).

**Construction** (`emitStringNew`): resolve `text` to a bare `i8*` (see
`resolveStrPtr` below), `@strlen` it, malloc a `{i32, i8*}` header and a
`length + 1`-byte buffer, copy `length` bytes across in a hand-rolled loop
(the same alloca/load/store counter, no-`phi` idiom `emitListPush`'s own
copy loop established), write the trailing `\0`, store `length`/`data`
into the header.

**Append** (`emitStringAppend`): `@strlen` the incoming operand, compute
`newLength = oldLength + otherLength`, malloc a fresh `newLength + 1`-byte
buffer (no amortized growth - the same "reallocate every time" honesty
`List<T>.push`'s own docs already established), run **two** copy loops
(the string's own existing content, then the newly appended bytes,
generalizing `emitListPush`'s own "copy old, then append new" shape from a
single scalar store to a variable-length byte range each), write the
trailing `\0`, then store the new `length`/`data` back into the header's
own fields in place - the header pointer itself never changes, so every
existing alias sees the update, the same "stable pointer, mutated in
place" model every push/set/add here already uses.

**`resolveStrPtr`**: a small shared helper (mirroring `ref()`/`typeOf()`
themselves, not this codebase's usual "separate over shared" rule for
whole *operations*) that resolves an operand register to a bare `i8*`,
extracting the data pointer from a `String` header first if the operand
isn't already one. Used by `emitStringNew`/`emitStringAppend` for their
own `text`/`other` operands, *and* by the regular function-call site
(`IrCall`'s own argument-resolution loop) for the general "String lends a
str" coercion at any call boundary - mirroring `needsSliceConversion`'s
own existing shape there for a different pair of types.

**Printing needs its own dedicated branch, `.length` doesn't.** `String`'s
header is deliberately `List<i8>`-shaped, so `.length`'s field-get rides
`isListType`'s own existing structural check for free - the identical
"free ride" `Stack<T>` got from `List<T>`'s own shape
(`docs/language/0035-stacks.md`). Top-level printing can *not* ride that
same free ride: `List<T>`'s own generic print loop reads each element's
LLVM type from the header (`i8` here, not `i8*`) and has no case for a
bare `i8` - it would try to print each byte as if it were a nested struct
pointer. `isStringType` is therefore a real, new structural predicate
(exact match on `"{i32, i8*}*"`, checked *before* `isListType` in the
top-level print dispatch only), giving `String` a direct `%s`-of-the-data-
pointer print - simpler and faster than the generic loop would have been
even if it worked, since the buffer is already null-terminated.

---

# Parsing: A Call-Shaped Constructor, Not a Generic One

`String` in type position is a bare identifier - unlike every collection
above, it needs no special-casing in `parseTypeName()` at all; the
existing final `return name.text;` fallthrough already produces the
canonical `"String"` string. In expression position, `String(text)` is
special-cased on the literal identifier `"String"` followed by `(` (the
same trick every collection constructor uses to sidestep the general
identifier-then-`(` call parsing), but unlike `List<elem>()`'s own
zero-argument, type-parameterized shape, this parses a real single-
argument expression via the existing `parseArgumentList()` machinery
(shared with ordinary function calls), validating the exactly-one-argument
arity at parse time - matching every collection constructor's own
arity-checked-here convention.

```text
$ ax ast examples/string.ax
Function(build)
  Block
    Assignment(name)
      StringNew
        String(Axea)
    ...
```

---

# Type Checking: `isStrCoercible`, Reused Three Ways

`TypeKind::OwnedString` is new - note the confusing-but-deliberate naming:
this enum already has a `TypeKind::String` case, which is `str`, from
`docs/language/0005-type-system.md`'s original design. `String(text)`'s
own `checkExpr` case (unlike every collection's `resolveType`-delegating
constructor) type-checks `text` directly against a new `isStrCoercible`
helper (`type == kStr || type.kind == TypeKind::OwnedString`) - the single
source of truth for "str, or String" reused in three places:
`String(text)`'s own argument, `.append(other)`'s own argument, and a new
`stringToStrCoercion` branch alongside the existing `arrayToSliceCoercion`
in ordinary function-call argument checking (see `docs/language/0032-slices.md`
for that check's own original shape). `.length` gets its own standalone
`checkFieldType` case, mirroring every non-indexable collection's own
identical pattern - `String` is deliberately not added to `isIndexable`
(slicing is out of scope this phase).

```text
$ ax capabilities bad.ax   # x = String(5)
error: String(...) expects str, got i32
```

---

# Capability Checking: Zero Changes, Again

`"append"` joins `CapabilityChecker`'s flat, method-name-only write-raising
list (the same list `"push"`/`"set"`/`"add"`/... already share) - the
identical free ride every mutating method on every collection here has
gotten since `List<T>.push` first established the list back in `0033`.

---

# Region Checking: The Zero-Exception Story, Confirmed by What's Absent

`isStringTypeString` is an exact match (not a `starts_with` prefix check -
`String` isn't generic), mirroring `Queue<T>`/`SortedSet<T>`'s own
"no `elementTypeName` extraction needed" story: `.append()` returns `unit`,
never the buffer's own content, so there's no `MethodCallExpr` aliasing
case to wire up. `String(text)` gets the usual "brand-new, always `Owned`"
constructor treatment - and here it matters more than it looks: `String(text)`
*copies* `text`'s own bytes into a fresh buffer regardless of `text`'s own
region, so the result is always `Owned` even when `text` itself was
`Borrowed` - the same reasoning that makes the value/reference distinction
below actually sound.

---

# `IrGenerator`: Unambiguous - `append` Joins `enqueue`/`dequeue`

New `IrStringNew`/`IrStringAppend` instructions. `"append"` is a brand-new
method name nothing else in the language uses, so - like `"enqueue"`/
`"dequeue"` before it - no disambiguation resolver is needed at all, the
simplest wiring of any collection method added this session.

```text
$ ax ir examples/string.ax
Function(build)
  Params:
  region.enter
  %0 = const.str "Axea"
  %1 = string.new %0
  %2 = const.str " Language"
  %3 = string.append %1, %2
  ...
```

---

# Interpreter: A Bug Caught by Taking "str Is a Value" Seriously

```cpp
struct StringInstance { std::string data; };
```

Every earlier collection's interpreter struct stood in reference-semantics
company - `ListInstance`, `MapInstance`, all `shared_ptr`-wrapped, all
mutated in place. `StringInstance` is the same shape, for the same reason
(`.append()` must be visible through every alias). But `str` itself is a
**bare `std::string` in the `Value` variant**, not `shared_ptr`-wrapped -
copied by value everywhere, exactly matching the LLVM backend's own `i8*`
snapshot semantics. `asStrContent` is the shared resolver (mirrors
`resolveStrPtr`'s own role at the LLVM layer) used by `String(text)`'s
construction and `.append`'s own argument - both already correct by
construction, since `asStrContent` returns a fresh `std::string` copy,
never the original `shared_ptr<StringInstance>`.

**The bug**: the *general* function-call argument-passing loop had no
equivalent coercion. Passing a `String` where a `str` parameter was
declared left the argument as the original `shared_ptr<StringInstance>` -
unconverted, because nothing checked the declared parameter type against
the runtime `Value`'s own shape. The callee's `str`-typed parameter ended
up holding the *same* `StringInstance` the caller still held - so a later
`.append()` on the caller's own `String` retroactively mutated a value
already "returned" as an immutable `str`, something the compiled backend
provably cannot do (its own `.append()` reallocates rather than mutating
in place, so an already-extracted `i8*` snapshot is stable by construction
no matter what happens to the source `String` afterward). Caught by
writing the aliasing scenario as a test and diffing interpreter output
against compiled `-O0`/`-O1` output - they disagreed until the fix (mirror
`arrayToSliceCoercion`'s own call-site pattern, but *copying* `.data`
into a bare `std::string` rather than re-wrapping a pointer, since `str`'s
own value semantics demand a real severance, not just a re-typed alias).
This is the clearest example yet in this codebase of why every collection
here has been diffed against real compiled output during development, not
just checked against the interpreter alone.

---

# Worked Example

`examples/string.ax`:

```ax
build() -> String
{
    name = String("Axea")
    name.append(" Language")
    return name
}

appendOne(name: String)
{
    name.append("!")
}

useStr(s: str) -> i32
{
    return 1
}

name = build()
lengthAfterBuild = name.length
called = appendOne(name)
lengthAfterAppend = name.length
viaStr = useStr(name)
other = String(name)
otherLength = other.length
```

```text
$ ax run examples/string.ax
name = Axea Language!
lengthAfterBuild = 13
called = ()
lengthAfterAppend = 14
viaStr = 1
other = Axea Language!
otherLength = 14
$ ax llvm-ir examples/string.ax | clang -x ir -O1 - -o out && ./out
# byte-for-byte identical (also re-verified at -O0)
```

`lengthAfterBuild = 13` confirms `"Axea"` (4) + `" Language"` (9) appended
correctly; `appendOne` writes through the caller's own `String` via
`write`-capability `append`, growing it to `14`; `other = String(name)`
constructs a fresh, independent copy *after* the append, so its own
`14`-byte content reflects the post-append state - unlike the
aliasing-vs-copying scenario in the Interpreter section above, which
specifically snapshots *before* a later append to prove the two states
diverge.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **No `[i]`/slicing.** `docs/std/strings/0005-slicing.md`'s own
  `date[..4]` syntax needs a range-slicing operator that exists nowhere in
  this language yet.
- **No interpolation-lowering.** `Buffer`/`.finish()` itself is now
  implemented (`docs/language/0043-buffer.md`), but the compiler-driven
  lowering of string interpolation into it is still purely aspirational -
  string interpolation itself (`"Hello {name}"`) has no lexer/parser
  support at all yet.
- **No Unicode-aware operations.** Every byte is treated as, well, a byte -
  `.length` is a byte count, not a codepoint or grapheme count
  (`docs/std/strings/0006-unicode.md`'s own `.bytes`-vs-default distinction
  has no counterpart here: there's only the one, byte-level view).
- **No FFI/`cstr` type.** `docs/std/strings/0007-ffi.md`'s own `extern c`/
  `to_cstr()` design needs `extern` declarations, which don't exist in this
  language yet - though `String`'s own null-terminated buffer already
  makes an eventual `to_cstr()` a trivial "return the data pointer"
  operation once `extern` exists.
- **No `.parse<T>()`.** `docs/std/strings/0008-parsing-formatting.md`'s own
  sketch needs both generic method-call syntax and the `?` operator,
  neither of which exist yet.
- **`append` is not amortized O(1).** Every call reallocates the entire
  buffer - the same complexity shortfall `List<T>.push`'s own docs already
  accept and document.
- **`String` is not a valid `Map`/`Set` key type, and cannot be a struct
  field type.** Mirrors every other collection's own first-phase
  restrictions.
- **`String(...)`/`.append` are compiler intrinsics, not real methods**,
  same as every other collection here.

---

# Guiding Rule

> Every collection built this session so far shared one property: every
> value flowing through it had the *same* ownership story throughout -
> always owned-and-mutable (`List<T>`, `Map<K,V>`, ...) or always a bare,
> freely-copyable primitive (`i32`, `bool`). `String` is the first type
> that sits *between* two such stories on purpose - an owned, mutable
> buffer that can *also* stand in for an immutable, freely-copyable view.
> The design doc's own one-line claim ("Passing `String` automatically
> lends a `str`") looked like a small ergonomic convenience right up until
> implementing it honestly required asking what "lends" *means* across a
> value/reference boundary - and the fact that the interpreter and the
> compiled backend disagreed about the answer, silently, until a targeted
> aliasing test caught it, is exactly the kind of bug that stays invisible
> without the "diff against real compiled output" discipline every prior
> collection's own worked example already modeled.
