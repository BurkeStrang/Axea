# `print`/`write`/Interpolation of Struct and Collection Values

---

# Motivation

`docs/language/0049-printing-formatting.md`'s own Known Imprecision section
(written while closing a *different* gap - nested string literals inside an
interpolation span) scoped this out explicitly: `print(arrayValue)` wasn't
supported, and neither was interpolating a struct or collection value
directly. At the time, struct support turned out to be nearly free (every
struct already has its own `@axea.print.<Name>` helper for the top-level
binding printer, so `print(p)` just calls it directly), but Array/List/
Map/Set/etc. were scoped out as genuinely larger: unlike struct's one
reusable named function per shape, the top-level binding printer's own
array/List printing is a large, hand-rolled runtime loop that only ever
existed inlined at its one call site - and, separately, interpolation needs
something `print`/`write` don't: a real *string* to concatenate with
surrounding text, not just permission to print directly, which struct's own
`@axea.print.<Name>` (a void-returning, print-direct function) can't
provide either. This phase closes both gaps: every struct and every
collection kind now has a real "stringify to a heap string" function, used
uniformly by `print`/`write`/interpolation alike.

---

# Design: A Reusable Growable-String-Buffer Trio, Not a Second Copy of Buffer

**Why not just reuse `Buffer`'s own append/growth logic?** `Buffer`'s own
`emitBufferNew`/`emitBufferAppend`/`ensureBufferCapacity` (see
`docs/language/0043-buffer.md`) are inline-only codegen: every one of them
takes a live `FunctionContext&` and calls `allocateRegister`/`ref`, tied to
the *specific* Axea function currently being emitted. A struct or
collection's own stringifier, though, is a **standalone, hand-written LLVM
function** (the same convention `registerOptionalToStrRuntime`/
`registerI32ToStrRuntime` already established for scalar types - see
`docs/language/0052-optional.md`), with no live register-numbering context
of its own and callable from anywhere. So this phase builds three small,
self-contained, standalone functions once - `@axea.strbuf.new() ->
{i32,i32,i8*}*`, `@axea.strbuf.append(buf, i8*) -> void`, `@axea.strbuf.
finish(buf) -> i8*` - reusing `Buffer`'s own `{len, cap, data}` header
*shape* for familiarity, but a fully independent implementation (hand-
transcribed doubling-growth logic, not a call into `Buffer`'s own inline
codegen). Every subsequent stringifier is then just a short sequence of
calls into these three, so the growable-buffer complexity is written
exactly once rather than once per struct/collection shape.

**Named registers throughout, deliberately.** Every hand-written function
in this phase (`@axea.strbuf.*`, `@axea.char.to_str`, `@axea.tostring.
<StructName>`, `@axea.tostring.collection.<id>`) uses named registers
(`%buf`, `%idx`, `%fptr0`, ...), never `allocateRegister`'s numbered
scheme - sidestepping the "unnamed registers must be defined in strictly
increasing textual order" constraint this codebase has hit (and fixed)
repeatedly elsewhere, rather than hand-tracking a number counter across
branches/loops by hand. A small local `nextTmp` counter (scoped to the one
function currently being built, not shared - see `emitElementToStrCall`)
still generates fresh, unique names within each function, for whichever
piece needs one.

**`@axea.char.to_str`** is a standalone transcription of
`encodeCharUtf8`'s own UTF-8 encoding (see `docs/language/0044-char.md`) -
same reasoning as `@axea.strbuf.*` above: `encodeCharUtf8` is also
live-fctx-only, so a char value being stringified from within a
standalone function needs its own callable copy. Hand-verified bit-for-bit
against `encodeCharUtf8`'s own exact `shift`/`mask`/`tag` values per
byte-length branch.

**`emitElementToStrCall`** is the one shared dispatch point both
`emitStructToStringHelpers` (struct fields) and
`registerCollectionToStrRuntime` (collection elements) call for "stringify
one value of this type, in standalone-function-text context": i32/i64/f64/
bool/char each call their own existing scalar `to_str` function directly by
name; `Optional<T>` calls `registerOptionalToStrRuntime`; a named struct
pointer calls `@axea.tostring.<Name>` (pre-registered - see below); `str`
is already an `i8*`, returned unchanged; `String` has its own data pointer
extracted inline (the one live-fctx-only piece, `resolveStrPtrOfType`,
duplicated here in its two-line standalone form); anything else recurses
into `registerCollectionToStrRuntime` for a nested collection element.

**Structs are registered unconditionally and upfront** -
`emitStructToStringHelpers`, called once (mirrors
`emitStructPrintHelpers`'s own identical "build every shape regardless of
actual use" choice), since a struct's own field layout is only available
via `program.structs`, not derivable from an LLVM type string the way a
collection's element type is. **Collections are registered lazily**, from
within `stringifyValueOfType` itself the first time one is actually
stringified, memoized by the collection's own full LLVM type text (mirrors
`registerOptionalInstantiationForLlvmPayload`'s identical "no Axea-level
name available at this layer" reasoning) - `registerCollectionToStrRuntime`
dispatches on structural shape, in the exact same order the top-level
binding printer's own dispatch already established (Map/Set/LinkedList/
SortedMap/SortedSet - whose headers are all `"{i32, ...}*"`-shaped too -
checked before the looser Deque/List tests): count-only for Map/Set/
LinkedList/SortedMap/SortedSet (no iteration support this phase, matching
the top-level printer's own identical fallback), a real runtime loop for
Deque/Queue (shared LLVM shape) and List/Stack/PriorityQueue (shared LLVM
shape), and a compile-time-unrolled loop for a fixed array.

**A real, hand-verified ordering bug found along the way.** Collection
stringifiers are the *first* thing in this backend that can register a
brand-new punctuation string (`"["`, `", "`, `"Map("`, ...) *lazily, from
deep inside a function body's own codegen* (`.join()`, `print`,
interpolation - all reachable well after `emitStringGlobals`' own
snapshot point, near the very end of `emit()`). The existing
`stringPtrConstant`/`hoistString` mechanism only accumulates into
`stringGlobals_`; `emitStringGlobals` writes out whatever's accumulated
*at that one point in time* and is never called again - so a string
hoisted afterward gets referenced (`getelementptr ... @.str.N ...`) but
never actually declared anywhere in the output, an "undefined value"
error at `clang` compile time, not a runtime bug. Confirmed directly:
`.join()` on an `Array<Point>` (a fresh collection shape, registered from
inside `run()`'s own body) reproduced it exactly. Fixed the same way
`registerOptionalToStrRuntime` already sidesteps the identical class of
problem for `"Some(%s)"`/`"None"`: `registerCollectionToStrRuntime`'s own
punctuation is now a handful of fixed, self-contained globals
(`@axea.str.openbracket`, `@axea.str.map_open`, ...), declared directly
in `toStrRuntimeText_` (snapshotted only once, at the very end of `emit()`,
regardless of when any individual piece was registered) rather than
routed through the shared, timing-sensitive `stringGlobals_` mechanism.

---

# Type Checking: One Wider Allowlist, Shared by `print`/`write` and Interpolation

`isTextRepresentable` (previously `i32`/`i64`/`f64`/`bool`/`char`/
str-coercible only) now also accepts `Optional<T>` (already fully
supported by `stringifyValueOfType` since `docs/language/0052-optional.md`,
but never actually reachable through `print`/interpolation before this -
a small, adjacent gap closed here too), every struct, and every collection
kind **except** `slice<T>` - a by-value fat pointer (`{T*, i32}`),
structurally unlike every other collection's own heap-header convention,
and (unlike them) already forbidden as a collection element type almost
everywhere via `rejectSliceOutsideParameter`, so stringifying a bare slice
specifically remains real further work. `print`/`write`'s own argument
check, previously a separate `isTextRepresentable(argType) ||
argType.kind == TypeKind::Struct` carve-out (from the prior phase's own
struct-only support), collapses back to a plain `isTextRepresentable(argType)`
call - identical to interpolation's own check now, since the allowlist
itself grew to cover what used to need a special case.

**Two other checks share `isTextRepresentable` too, and both benefited as
a side effect, verified rather than assumed:** array/List slicing
(`array[a..b]`) and `.join(separator)` both gate their own element-type
restriction through the same helper (`docs/language/0050-collection-join-
and-slicing.md`). Slicing a struct-element array turns out to already work
correctly - it's a generic element-wise value copy (`arrslice.copy`'s own
loop shape), never actually needed text-representability for correctness,
just happened to reuse this check for its own scope restriction.
`.join()` on struct/collection elements now genuinely works too, since its
own codegen (`emitJoin`) already called `stringifyValueOfType` per element
- it inherits struct/collection support automatically, the same way
`print`/`write`/interpolation do. Both confirmed by hand (not just "type-
checks now"): real output, byte-for-byte identical between the interpreter
and both `-O0`/`-O1` compiled paths.

---

# Worked Example

```ax
struct Point { x: i32  y: i32 }

arr = [1, 2, 3]
p = Point { x: 3, y: 4 }
print("arr:", arr, "p:", p)
print("interp: {arr} and {p}")

m: Map<i32, i32> = Map<i32, i32>()
a = m.set(1, 10)
print("map:", m)

pq: PriorityQueue<i32> = PriorityQueue<i32>()
b = pq.push(3)
c = pq.push(1)
print("pq:", pq)

structs = [Point { x: 1, y: 1 }, Point { x: 2, y: 2 }]
print("structs:", structs)
joined = structs.join(", ")
print(joined)
```

```text
arr: [1, 2, 3] p: Point { x: 3, y: 4 }
interp: [1, 2, 3] and Point { x: 3, y: 4 }
map: Map(1 entries)
pq: [1, 3]
structs: [Point { x: 1, y: 1 }, Point { x: 2, y: 2 }]
Point { x: 1, y: 1 }, Point { x: 2, y: 2 }
```

Hand-verified byte-for-byte identical across the interpreter, `-O0`, and
`-O1`, including every collection kind (`Map`/`Set`/`LinkedList`/`Deque`/
`Queue`/`PriorityQueue`/`SortedMap`/`SortedSet`/`Stack`), a struct with a
nested collection field of struct elements, and three levels of nested
collections (`List<List<i32>>`).

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **`slice<T>` still can't be printed/interpolated/joined directly.** A
  by-value fat pointer, structurally unlike every other collection here -
  real further work, not built out this phase (see Type Checking above).
- **A struct can't have a collection-typed field at all**, independent of
  this phase entirely (a pre-existing restriction -
  `"List<T> is not supported as a struct field type in this phase"`, and
  the same for every other collection kind) - so a struct's own
  `@axea.tostring.<Name>` never actually needs to recurse into a
  collection-typed field in practice, even though `emitElementToStrCall`
  would handle it correctly if it ever could.
- **Map/Set/LinkedList/SortedMap/SortedSet still print count-only**
  (`"Map(3 entries)"`), not their actual contents - matching the
  top-level binding printer's own pre-existing identical limitation (no
  iteration support for these collections this phase - see
  `docs/language/0034-maps-and-sets.md`/`0036-linked-lists.md`/
  `0040-sorted-maps.md`/`0041-sorted-sets.md`). Extending this would need
  real iteration first, unrelated to stringification itself.
- **Every stringify call heap-allocates and never frees**, matching every
  other `to_str`/`print` helper in this backend's own existing "no
  `free`, values may leak" convention - not a new imprecision, just worth
  restating since collection stringification can now be triggered
  recursively (an array of structs, say), multiplying the allocation
  count per call more than a single scalar `to_str` call would.

---

# Guiding Rule

A capability that looks like "just wire up an existing print path to a new
call site" can turn out to need a structurally different mechanism
entirely once you look closely - `print`/`write` only ever needed
*permission* to print directly (an existing, reusable per-struct
function), but interpolation needed a genuine *string*, which nothing in
this backend had a way to produce for anything past a handful of scalar
types. Building the small, general primitive once (a growable string
buffer, standalone and callable from anywhere) rather than duplicating
each collection's own print loop a second time in string-building form
kept the actual per-shape stringifiers short - and, as with
`docs/language/0052-optional.md`'s own discovery-pass fix, a genuinely new
kind of *timing* bug (a lazily-registered global, reachable from deep
inside ordinary user code, past the point the shared string-hoisting
mechanism stops looking for new entries) was only found by actually
running the newly-legal programs through the compiled backend, not by
reasoning about the type checker in isolation.
