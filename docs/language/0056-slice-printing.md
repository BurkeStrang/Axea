# `print`/`write`/Interpolation/`.join()` of `slice<T>`

**Status:** Implemented
**Document:** `0056-slice-printing.md`

---

# Motivation

`docs/language/0054-collection-printing.md` gave every collection kind a real "stringify to a
heap string" runtime function, with one deliberate exception, named explicitly in its own Known
Imprecision section: `slice<T>` - "still can't be printed/interpolated/joined directly... real
further work, not built out this phase." The reason was structural, not an oversight: every other
collection this codebase supports (`Array`, `List<T>`, `Map<K,V>`, ...) is a *pointer* to a small
heap record, so `registerCollectionToStrRuntime`'s existing dispatch chain uniformly GEPs and
loads a length field and a data-pointer field out of that record. `slice<T>` is the one exception
- a `{T*, i32}` "fat pointer" passed **by value** (`docs/language/0032-slices.md`'s own design,
hand-verified against real clang codegen since nothing else in this codebase had passed a struct
by value before) - so neither field is behind a pointer to GEP through at all; both already sit
directly in the value itself. This phase closes that one remaining gap: a dedicated by-value
branch in the two places that needed one, nothing else.

---

# What Changed

**`TypeChecker::isTextRepresentable`** gained `TypeKind::Slice` to its switch - the only
TypeChecker-level change needed for `print`/`write`/interpolation, since every runtime backend
piece downstream of that check (the interpreter's `toString`, `stringifyValueOfType`'s own
`type.front() == '{'` dispatch) already routed a slice's LLVM/Value shape into the right place;
this was purely a permission gate, not a missing capability. **`.join()`'s own receiver-kind
check** (`docs/language/0050-collection-join-and-slicing.md`) gained `TypeKind::Slice` alongside
`Array`/`List`, for the same reason - the runtime side (`asIndexable` in the interpreter,
`resolveIndexableView` in the LLVM backend) needed one addition each, covered below.

**Interpreter: no changes needed to `toString` at all.** `SliceInstance`'s own branch has existed,
correct, since `docs/language/0032-slices.md` first introduced the type - previously commented
"provably unreachable in a well-typed program" (since `isTextRepresentable` rejected the type
before this branch could ever run), now genuinely reachable. `asIndexable` (the shared helper
`.join()`/indexing/`.length` all already use) has handled `SliceInstance` since it was written
too. Both pieces existed purely because they were written generically alongside `ArrayInstance`'s
own handling at the time - closing this gap in the interpreter turned out to be zero lines of new
code, only a TypeChecker-level permission change.

**LLVM backend: two new by-value branches**, each placed directly alongside the pointer-based
List<T> branch it otherwise mirrors exactly (same bracket-and-comma loop shape, same single-index
GEP element access `emitIndexGet`'s own `isSliceType` branch already established):

- **`registerCollectionToStrRuntime`** (print/write/interpolation) - `%data = extractvalue
  {T*, i32} %v, 0` / `%len = extractvalue {T*, i32} %v, 1` in place of List's own
  GEP-into-header-then-load pair, then the identical loop body (including
  `emitElementToStrCall` for the element itself, so struct/nested-collection slice elements work
  automatically, same as every other collection kind).
- **`resolveIndexableView`** (`.join()`'s own shared helper, also used by `array[a..b]` slicing -
  though a bare `slice<T>` is never actually the *object* of a `[a..b]` re-slice, since
  re-slicing a slice remains out of scope per `docs/language/0050-collection-join-and-slicing.md`'s
  own Known Imprecision) - the same `extractvalue`-instead-of-`GEP`+`load` swap, returning an
  `IndexableView` identical in shape to every other branch so `emitJoin` itself needed zero
  changes.

Neither branch needed to be reachable for a *nested* slice element (inside a struct field, inside
another collection) - `slice<T>` remains forbidden everywhere except as a function parameter via
the pre-existing `rejectSliceOutsideParameter`, untouched by this phase - so the only real trigger
is a `slice<T>`-typed parameter used directly inside the function it was passed to.

---

# Worked Example

```ax
struct Point { x: i32 }

describe(s: slice<i32>) -> i32 {
    print(s)
    print("interp: {s}")
    print(s.join(","))
    return s.length
}

describePoints(pts: slice<Point>) -> i32 {
    print(pts)
    print(pts.join(", "))
    return pts.length
}

numbers = [1, 2, 3, 4]
n = describe(numbers)

points = [Point { x: 1 }, Point { x: 2 }]
m = describePoints(points)
```

```text
[1, 2, 3, 4]
interp: [1, 2, 3, 4]
1,2,3,4
[Point { x: 1 }, Point { x: 2 }]
Point { x: 1 }, Point { x: 2 }
```

Hand-verified byte-for-byte identical across the interpreter, `-O0`, and `-O1`, including a slice
of struct elements (stringified via the same `@axea.tostring.<Name>` helper every other collection
of structs already reuses) and a slice forwarded through a second function parameter unchanged
(`outer(s: slice<i32>) -> i32 { return inner(s) }` - the existing pass-through-unchanged coercion
rule `docs/language/0032-slices.md` established, untouched by this phase).

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **Re-slicing a `slice<T>` (`mySlice[a..b]`) remains unsupported** - a pre-existing, separate
  restriction from `docs/language/0050-collection-join-and-slicing.md`'s own Known Imprecision,
  untouched by this phase. This phase is about stringifying an *existing* slice value, not about
  producing a new one from it.
- **A `slice<T>` still cannot appear as a struct field, collection element, Map/Set key or
  value, or local variable's declared type** - `rejectSliceOutsideParameter` is entirely
  untouched; the only way to obtain a `slice<T>` value remains a function parameter.

---

# Guiding Rule

A "deliberately scoped out" gap is often smaller than it first looked once the actual blocker is
identified precisely: this one turned out to be exactly two `extractvalue`-based branches (one
per already-generic dispatch point), because every *other* piece of the machinery - the
interpreter's own `toString`/`asIndexable`, the LLVM backend's element-stringification and
join-loop bodies - was already written type-agnostically and needed no change at all. Naming the
real structural reason for a scope cut precisely (here: "by-value, not by-pointer" - not just
"slice<T> is different") is what let the follow-up land as a small, targeted phase instead of a
speculative rewrite.
