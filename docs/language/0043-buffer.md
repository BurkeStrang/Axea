# `Buffer`: Genuine Amortized Growth, and a Zero-Copy Handoff to `String`

**Status:** Implemented
**Document:** `0043-buffer.md`

---

# Motivation

`docs/std/strings/0004-buffer.md` specs `Buffer` as Axea's mutable
text-construction type: `Buffer()`, `.append(text)`, `.append_line(text)`,
`.clear()`, `.len`/`.capacity`, `.reserve(n)`, `.finish()` returning an
immutable `String`. This phase implements exactly that surface (minus one
deliberate naming deviation - see Type Checking below) - no string
interpolation lowering (`docs/std/strings/0004-buffer.md`'s own "Compiler
Optimizations" section is still purely aspirational; string interpolation
itself has no lexer/parser support yet).

```ax
b = Buffer()
b.append("Hello ")
b.append(name)
b.append("!")
message = b.finish()
```

**`Buffer` is `String`'s own mirror image, not its twin.** Both are
non-generic, owned, mutable, reference-semantics heap types
(`docs/language/0042-string.md`), sharing the method name `"append"` (see
`IrGenerator` below). But `String` intentionally has no `.capacity` at all -
every `.append()` reallocates unconditionally, "honest" about being
O(n) per call. `Buffer` exists specifically *because* that's wrong for
the "many small appends in a loop" use case `docs/std/strings/0004-buffer.md`
names as its whole reason to exist - which makes `Buffer` the **first
collection in this codebase with genuine conditional growth**: a real
`br i1` "grow or don't" branch, not "reallocate every single call" like
every `List<T>.push`/`Stack<T>.push`/`Map<K,V>.set`/`String.append`/... that
came before it.

---

# Design: A 3-Field Header, and the Bug Its Own Shape Causes

A `Buffer` value is a stable pointer to a 3-field header -
`{i32 length, i32 capacity, i8* data}*` - one field more than `String`'s
own 2-field `{i32, i8*}*`. `data` is always kept **null-terminated**,
same invariant as `String`, so `Buffer`'s own top-level print (and any
future FFI use) is always a direct `%s` of the data pointer with no extra
work.

**The bug this shape causes, caught before any build was attempted.**
`Deque<T>`/`Queue<T>`'s own header is `"{i32, i32, " + llvmType(T) + "*}*"`
(`docs/language/0037-deques.md`/`0038-queues.md`) - for `T = i8`, that's
*textually identical* to `Buffer`'s own header. `i8` isn't nameable in real
Axea source, so this can never actually collide with a real `Deque<T>`
instantiation - but `isDequeType`'s own predicate is a *structural* text
check, not a "was this actually constructed as a Deque" check, and it does
match `Buffer`'s header. Two call sites needed a dedicated `isBufferType`
branch, checked *before* the shared `isDequeType` (and, in the field-get
case, the wider `isListType(...)||...` chain) it would otherwise silently
fall into:

- **`emitFieldGet`** - the pre-existing shared branch for every
  `List`/`Map`/`Set`/`Deque`/`SortedMap`/`SortedSet` field-get assumes any
  match is `.length` at field index 0. `Buffer`'s own new `.capacity`
  field lives at index 1 - without a dedicated branch, reading
  `.capacity` would have silently returned `.length`'s own value instead.
- **The top-level print dispatch** - `isDequeType`'s own print branch
  reads the data pointer from field index 2 assuming a `Deque`'s layout,
  which *happens* to also be `Buffer`'s own data field index - so this one
  specific collision would have accidentally printed correctly by luck.
  The dedicated `isBufferType` branch was still added (checked before
  `isDequeType`, mirroring `isStringType`'s own dedicated branch just
  below it) for the same reason `String` got one: correctness shouldn't
  depend on an accidental field-index coincidence holding forever.

Both fixes are the same shape as `isStringType`'s own "checked before
`isListType`" precedent - the general lesson repeats: whenever a new
collection's header text structurally overlaps an existing predicate for a
type it isn't, a dedicated, earlier-checked predicate is required, not
optional.

**Real amortized growth (`ensureBufferCapacity`).** Shared by
`emitBufferAppend`/`emitBufferAppendLine`/`emitBufferReserve`: load the
current `capacity`, compare against the needed size via `icmp sgt`, and
branch. If growth is needed, `doubled = capacity * 2`; `newCapacity` is
`doubled` unless the needed size still exceeds it, in which case
`newCapacity` is the needed size directly (`select`, not a second branch -
`icmp sgt` + `select` composes both "double" and "at least what's needed"
into one value with no extra control flow). Malloc a buffer of
`newCapacity` bytes, copy the buffer's own existing `length` bytes across
(the same hand-rolled alloca/load/store counter loop, no `phi`, every copy
loop in this backend already uses), store the new `capacity`/`data` back
into the header's own fields in place, then fall through to a shared done
label the no-growth-needed path branches to directly. Every caller
re-reads `capacity`/`data` **fresh from the header** after calling this,
never trusting a register value computed before the call - the same
"reload from memory instead of `phi`" trick every other multi-predecessor
merge in this backend already uses, applied here to a helper-function
boundary instead of a loop/branch merge.

**`.clear()` is the one operation that touches neither `capacity` nor
`data`** - it exists specifically so a cleared `Buffer` can be refilled
without reallocating, the entire reason `Buffer` tracks capacity
separately from length in the first place (unlike `String`, which has no
`capacity` field to preserve).

**`.finish()` is a genuine ownership transfer, not a copy.** It reads the
buffer's own current `length`/`data` field *values* (not bytes) and stores
them directly into a freshly malloc'd 2-field `String` header - zero byte
copying, the cheapest possible correct implementation of "hand this
content over." The *original* buffer is then reset to the exact same
fresh, minimal state `emitBufferNew` produces (`length = 0`, `capacity = 1`,
a freshly malloc'd 1-byte `data` holding `'\0'`) - never left null or
dangling, so a finished `Buffer` remains safely reusable, exercised
directly in the worked example below.

---

# Parsing: Identical to `String`'s Own Empty-Constructor Shape

`Buffer` in type position is a bare identifier, needing no
`parseTypeName()` special-casing, same as `String`. In expression
position, `Buffer()` is special-cased on the literal identifier `"Buffer"`
followed by `(`, but - unlike `String(text)`'s single-argument
constructor, and *unlike* every generic collection's `List<elem>()` shape -
it parses **zero** type parameters and **zero** arguments: `BufferNewExpr`
is an empty struct, mirroring `ContinueStmt`'s own empty-struct precedent.

```text
$ ax ast examples/buffer.ax
Function(build)
  Block
    Assignment(b)
      BufferNew
    ExprStmt
      MethodCall(append)
        Name(b)
        String(Axea)
```

---

# Type Checking: One Deliberate Naming Deviation

`TypeKind::Buffer` resolves via the same bare exact-name match `String`
uses (not generic - `resolveType` compares `name == "Buffer"` directly, no
`starts_with` prefix scan). `.append`/`.append_line` type-check their
argument against the existing `isStrCoercible` helper - the same "str, or
String" rule `String.append` already uses, now shared a third way.
`.reserve(n)` requires an `i32` argument. `.finish()` takes no arguments
and returns `TypeKind::OwnedString`, tying `Buffer` and `String` together
at the type level for the first time.

**Deviation from spec:** `docs/std/strings/0004-buffer.md` names the
length field `.len`. This implementation uses **`.length`** instead, for
consistency with every other collection in this codebase (`List`, `Map`,
`Set`, `Deque`, `Queue`, `SortedMap`, `SortedSet`, `String` all use
`.length`) - a deliberate deviation from the design doc in favor of
codebase-wide consistency, not an oversight.

```text
$ ax capabilities bad.ax   # x = Buffer().reserve("oops")
error: reserve(...) expects i32, got str
```

---

# Capability Checking: Zero Changes, Again

`"append_line"`, `"clear"`, `"reserve"`, `"finish"` join `"append"` on
`CapabilityChecker`'s flat, method-name-only write-raising list - the same
free ride every mutating method on every collection here has gotten since
`0033`.

---

# Region Checking: No Aliasing Exception Needed, Same Reason as `String`

`isBufferTypeString` is an exact match, mirroring `isStringTypeString`.
None of `Buffer`'s five methods return the buffer's own content by
reference - `.finish()` returns a brand-new, independently-owned `String`
(the interpreter and LLVM backend both implement this as a genuine content
handoff, not a borrow), so there is no `MethodCallExpr` aliasing case to
wire up, same as every other collection method here that returns `unit`
or a freshly-constructed value. `BufferNewExpr` gets the usual "brand-new,
always `Owned`" constructor treatment.

```text
$ ax run bad.ax   # leaky(b: Buffer) -> Buffer { return b }
error: function 'leaky' cannot return 'b': parameter 'b' is borrowed and
does not outlive the call - declare 'take' if ownership should transfer
```

---

# `IrGenerator`: The First Real Two-Way Method-Name Collision Since `String`

Unlike `String`'s own `"append"` (which needed no disambiguation when it
was the only type using that name), `Buffer` and `String` **both** have
`.append(...)`. `isBufferExpr` mirrors `isSortedSetExpr`'s own resolver
shape (literal `NewExpr` check, parameter declared-type check, an
`IrScope`-tracked assignment map, call return-type check) but with exact
`==` type-string comparisons rather than `starts_with`, since neither type
is generic. `"append"` dispatch checks `bufferKind` first, falling back to
`IrStringAppend` otherwise; `"append_line"`/`"clear"`/`"reserve"`/`"finish"`
are unconditional (no other type defines those names, so no resolver is
needed for them specifically - only the shared `"append"` name needs one).

```text
$ ax ir examples/buffer.ax
Function(build)
  Params:
  region.enter
  %0 = buffer.new
  %1 = const.str "Axea"
  %2 = buffer.append %0, %1
  %3 = const.str " Language"
  %4 = buffer.append_line %0, %3
  %5 = const.str "second line"
  %6 = buffer.append %0, %5
  %7 = buffer.finish %0
  return %7
```

---

# Interpreter: `std::string`'s Own Growth, for Free

```cpp
struct BufferInstance { std::string data; };
```

Same reference-semantics shape as `StringInstance`, for the same reason.
But unlike the LLVM backend, the interpreter writes **no** growth logic at
all: `std::string` already tracks its own capacity and grows it
amortized internally, so `.append`/`.append_line` are just `+=`,
`.clear()` is `std::string::clear()`, `.reserve(n)` is
`std::string::reserve(n)`, and `.length`/`.capacity` are
`.size()`/`.capacity()` directly. `.finish()` `std::move`s `data` into a
fresh `StringInstance`, then immediately resets the buffer's own `data` to
a fresh `std::string()` - moved-from state is valid-but-unspecified by the
standard, and this interpreter never leaves it that way, matching
`emitBufferFinish`'s own "always reset to a known-good empty state"
guarantee exactly.

**Documented, expected divergence:** `.capacity`'s *exact* numeric value
is not expected to match between the interpreter (`std::string`'s own
internal growth heuristic) and the compiled backend (the explicit
doubling formula in `ensureBufferCapacity`) - confirmed directly:

```text
interpreter:  c0=15  c1=15  c2=15  c3=15  c4=61
compiled:     c0=1   c1=2   c2=4   c3=12  c4=62
```

This is treated as an acceptable, implementation-defined difference, the
same way two different real-world `std::string` implementations would
also disagree with each other. `.length`, content, and `.finish()`'s
result are *not* expected to diverge, and were verified not to.

---

# Worked Example

`examples/buffer.ax`:

```ax
build() -> String
{
    b = Buffer()
    b.append("Axea")
    b.append_line(" Language")
    b.append("second line")
    return b.finish()
}

appendBang(b: Buffer)
{
    b.append("!")
}

report = build()
reportLength = report.length

notes = Buffer()
reserved = notes.reserve(64)
appended = notes.append("draft")
called = appendBang(notes)
notesLength = notes.length
notesCapacity = notes.capacity

cleared = notes.clear()
lengthAfterClear = notes.length
reused = notes.append("reused")
lengthAfterReuse = notes.length
```

```text
$ ax run examples/buffer.ax
report = Axea Language
second line
reportLength = 25
notes = reused
reserved = ()
appended = ()
called = ()
notesLength = 6
notesCapacity = 64
cleared = ()
lengthAfterClear = 0
reused = ()
lengthAfterReuse = 6
$ ax llvm-ir examples/buffer.ax | clang -x ir -O1 - -o out && ./out
# byte-for-byte identical (also re-verified at -O0), except notesCapacity
# (documented divergence above)
```

`reportLength = 25` confirms `build()`'s three appends (`"Axea"` + `"
Language"` + `\n` + `"second line"`) landed in `.finish()`'s own result
correctly. `notesCapacity = 64` confirms `.reserve(64)` took effect and
was preserved across the later `.append("draft")` and the borrowed
`appendBang` call. `lengthAfterClear = 0` followed by
`reused = ()`/`lengthAfterReuse = 6` confirms `notes` is genuinely
reusable after `.clear()` - no reallocation was needed for the following
`.append("reused")` to succeed correctly.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **`.len` renamed to `.length`.** A deliberate deviation from
  `docs/std/strings/0004-buffer.md`'s own spec, for codebase-wide
  consistency (see Type Checking above).
- **No string-interpolation lowering.** `docs/std/strings/0004-buffer.md`'s
  own "Compiler Optimizations" section (lowering `"Hello {name}"` into a
  `Buffer` automatically) needs lexer/parser support for interpolation
  syntax that doesn't exist yet.
- **`.capacity`'s exact value is implementation-defined**, expected (and
  confirmed) to diverge between the interpreter and the compiled backend -
  see Interpreter above.
- **No `[i]`/slicing, no iteration, not hashable, not a struct field
  type.** Mirrors `String`'s own first-phase restrictions exactly.
- **`Buffer()`/its five methods are compiler intrinsics, not real
  methods**, same as every other collection here.

---

# Guiding Rule

> `Buffer` didn't need a new kind of correctness question the way
> `String` did (no value/reference boundary to cross) - every piece of its
> *design* was already settled by `String`'s own precedent. What it needed
> instead was **discipline about a header shape colliding with an
> unrelated existing predicate**, caught purely by reasoning through every
> `isXType` structural check *before* attempting a build, not by a test
> failure after the fact. The first real amortized-growth branch in this
> codebase also didn't need new *ideas* - `icmp sgt` + `select` composes
> two conditions from primitives every earlier collection already used -
> it needed the discipline of reloading every value from memory after the
> branch rather than trusting a pre-branch register, the same "no `phi`
> anywhere" rule this codebase has followed since its very first loop.
