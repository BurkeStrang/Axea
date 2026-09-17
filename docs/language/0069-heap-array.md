# `HeapArray<T>`: A Safe, Dynamically-Sized Heap Allocation Primitive

**Status:** Implemented (Tier 1 of a two-tier plan - see "Scope" below)
**Document:** `0069-heap-array.md`

---

# Motivation

`std/collections.ax`'s own growable types (`List<T>`, and everything built on it - `Stack<T>`,
`Deque<T>`, `Queue<T>`, `PriorityQueue<T>`) needed a variable-length heap buffer to hold their
elements, and the language had no safe way to get one: `docs/language/0019-unsafe.md` says outright
that "the only way any Axea program allocates heap memory is implicitly, through compiler-intrinsic
constructs... there is no user-reachable allocation primitive at all." So these five types were
built on raw `*T`, `extern c malloc`/`free`, and `unsafe` blocks - the only tool available - even
though the language's own single-owner move/drop system (see `docs/language/0009-ownership.md`) was
already real and working for *fixed*-shape data: every plain struct literal already compiles to a
genuine `malloc` (`LlvmIrEmitter::emitStructNew`) and is already freed automatically, with zero
runtime refcount, by a compile-time-proven drop pass (`IrGenerator::emitReturn` walking
`Context::liveScopeStack`). The one thing missing was a way to say "this struct owns a buffer of N
heap elements where N is a runtime value" - `HeapArray<T>` is exactly that primitive, plugged
directly into the existing drop machinery rather than inventing a new safety story.

```ax
i32 run()
{
    a = HeapArray<i32>(5)
    a[0] = 10
    a[4] = 99
    return a[0] + a[4]
}
x = run()
print(x)
```

(Index-*assignment* - `a[0] = 10` - needs to be inside a function body; like a bare method-call
statement, it isn't one of the handful of shapes this language's top-level script scope accepts
directly - a pre-existing limitation, unrelated to `HeapArray<T>` itself.)

---

# Design

## `HeapArray<T>(n)` - construction

A builtin, not a real callable function - recognized by literal text, the same convention
`sizeof<T>()`/`hash<T>(value)`/`keyEq<T>(a, b)` already use (see
`docs/language/0006-generics.md`). `n` is a runtime `i32`, unlike `[T;N]`'s compile-time-known `N`.
Allocates `n` `T`-typed elements, default-initialized (zero/false/null, per element type), and
returns a single-owned handle.

Represented as a real 2-field struct, `{ i32 length, T* data }`, synthesized lazily the first time
a given `T` is referenced (`IrGenerator::registerHeapArrayType`, mirroring
`registerSharedType`'s own lazy-registration shape for `Shared<T>`) and mangled as `HeapArray.<T>`
(dot, not `<>` - the same "must be a legal LLVM identifier" reasoning `Shared.<T>` already
established). This is deliberately the *only* place in this whole backend `T*`'s own raw pointer
appears without `unsafe` anywhere nearby - it's an implementation detail of the primitive itself,
never exposed to the Axea program that constructed the `HeapArray<T>`.

## `arr[i]` / `arr[i] = v` - the one runtime-checked indexing shape in this backend

Every other indexable shape in this compiler (`[T;N]`, `slice<T>`, `List<T>`/`Deque<T>` internals) is
**not** bounds-checked in compiled code - `docs/language/0031-arrays.md` documents this as
accepted, pre-existing UB, and the interpreter's own bounds check there is a convenience, not a
safety guarantee the compiled backend shares. `HeapArray<T>` is the first exception: since its size
is a genuine runtime value (unlike `[T;N]`'s statically-known `N`), a real runtime check was
unavoidable to make it actually safe rather than just differently unsafe.

```llvm
%inBounds = icmp ult i32 %index, %length
br i1 %inBounds, label %ok, label %panic
panic:
  call void @axea.panic()
  unreachable
ok:
  ; ordinary GEP + load/store
```

`icmp ult` (unsigned) rather than two separate signed checks deliberately catches a negative index
and an over-length index with the same single comparison: a negative `i32`, reinterpreted unsigned,
is a huge positive number (`-1` -> `4294967295`), which is never `< length` for any real length.

`@axea.panic()` (`LlvmIrEmitter::registerPanicRuntime`) is this backend's first controlled-failure
path at all - prints a fixed message and calls libc `exit(1)`. Before this, there was no
`declare void @exit(i32)` anywhere in the generated IR; every other runtime violation in compiled
code is silent, documented UB. The interpreter's own bounds check (already existing, shared via
`asIndexable` - see below) throws a catchable error instead, matching every other collection's
existing interpreted-vs-compiled asymmetry.

## Drop integration - reusing, not replacing, the existing move/drop system

`HeapArray<T>` participates in the *same* single-owner move-tracking every plain struct/`Shared<T>`
already has (`RegionChecker`/`CapabilityChecker` need no changes at all - both are purely name-based,
type-agnostic, per `docs/language/0009-ownership.md`). The only real engineering here was making
sure every place that already special-cases a raw declared-type string (`structs_.contains(...)` for
drop-tracking, "is this value fresh" checks, etc.) also recognizes `HeapArray<T>`'s canonical
angle-bracket text and translates it to the mangled `HeapArray.<T>` key - mirroring the identical
translation `Shared<T>` already needed (`mangleSharedTypeName`/`mangleHeapArrayTypeName` in both
`IrGenerator.cpp` and `LlvmIrEmitter.cpp`).

`@axea.drop.HeapArray.<T>` is the one drop function in this backend that frees *two* things: the
`data` buffer (a raw `*T` field the generic "recurse into struct/enum fields" loop can never see by
construction - explicitly special-cased, `name.starts_with("HeapArray.")`) and then the wrapper
struct's own allocation, exactly like every other plain struct's drop function already does.

**Three real, previously-latent bugs found and fixed while wiring this up** (all in the same family
- a raw declared-type string reaching a `structs_.contains(...)` check without the translation a
`NameExpr`'s own already-mangled form gets for free):
- `emitStructRefcountHelpers`'s own struct-field-drop loop (`LlvmIrEmitter.cpp`) never mangled a
  field's declared type before the check - a `HeapArray<T>` (or `Shared<T>`) struct field would
  silently never get recursively dropped at all (a leak, not a corruption, but a real one).
- `retainFieldValueIfNeeded` (`IrGenerator.cpp`) had the identical gap on the "untrack this value
  from its own local scope, it's now owned by this struct field" side - missing it meant a value
  moved into a struct-literal field would be dropped *twice*: once by its own scope's exit, once
  more by the new struct's own later drop.
- `FieldAssignStmt`'s own lowering (`self.data = newData`, the exact shape `List<T>.push`'s own
  growth needs) had a second, separate un-mangled check gating the same "retain new, release old"
  logic entirely - this one produced a real, reproduced double-free crash (a segfault) in testing,
  fixed the same way.

## Why the interpreter needed almost no changes at all

`HeapArray<T>`'s runtime shape (a length-bounded, shared, mutable sequence of `Value`) is *exactly*
what `ArrayInstance` (a `std::shared_ptr<std::vector<Value>>`, used for fixed `[T;N]` arrays)
already is - so `HeapArrayNewExpr` just constructs one directly, and indexing needed **zero**
interpreter changes at all: `asIndexable`, the helper `IndexExpr`/`IndexAssignStmt`/`.length` already
share, already recognizes `shared_ptr<ArrayInstance>` and already bounds-checks it.

## `GenericMonomorphizer` and `isBuiltinGenericName`

`HeapArray` is a *builtin* generic (added to `isBuiltinGenericName`'s list, alongside `Optional`/
`Shared`/`Result`/`slice`) - `GenericMonomorphizer` never tries to look it up as a user-declared
struct template the way it does `Box<T>`, but its `cloneExpr`/`collectTypeRefsInExpr` walks still
needed a `HeapArrayNewExpr` case (mirroring `HashOfExpr`'s own identical `typeName + one value arg`
shape) so a generic struct's own embedded method bodies (`List<T>.push`'s own
`HeapArray<T>(newCapacity)`) correctly substitute `T` per concrete instantiation.

---

# Scope: Tier 1 of Two

This closes the gap for `List<T>`/`Stack<T>`/`Deque<T>`/`Queue<T>`/`PriorityQueue<T>` -
`std/collections.ax` now builds every one of these five with zero `unsafe`, zero raw pointers, zero
manual `malloc`/`free` (verified: `grep -c "unsafe\|malloc\|free("` across that section of the file
is zero, excluding comments).

`LinkedList<T>`, `Map<K,V>`, `Set<T>`, `SortedMap<K,V>`, and `SortedSet<T>` are **not** covered by
this - each needs a heap node reachable from more than one place at once (`next`+`prev`,
parent+children, a bucket chain), which the single-owner move model can never express directly, no
matter what allocator sits underneath it. Closing that gap needs a second, separate primitive (an
arena + integer-index pattern, restructuring those five around one owned `Array<Node<T>>` instead of
pointer-linked nodes - see the follow-up scoping discussion) - genuinely separate design work (a
free-list for reused slots, in particular) not bundled into this document.

---

# Known Limitations (This Phase)

- **No negative-size validation.** `HeapArray<T>(n)` doesn't check `n >= 0` - a negative `n` in
  compiled code becomes a huge unsigned byte count passed to `malloc`, likely failing outright; the
  interpreter would throw from `std::vector`'s own allocation failure. Matches this codebase's
  existing "raw allocation size isn't validated" precedent (`collections.ax`'s own prior raw
  `malloc` calls had the identical gap) - not a new regression, just not newly closed either.
- **Uninitialized reads within bounds.** A `HeapArray<T>(n)`'s elements are default-initialized
  (zero/false/null), not left truly uninitialized the way a raw `malloc`'d buffer is - so reading an
  in-bounds, never-explicitly-written element returns a default value, not garbage. This is *safer*
  than the old raw-pointer behavior, not a gap, but is worth naming: a bug that relies on "I forgot
  to initialize this slot" being visibly wrong will instead see a quiet default.
- **A nested-generic element type isn't fully re-mangled.** `HeapArray<Box<T>>` (a `HeapArray` whose
  own element is itself a further generic instantiation) type-checks and constructs, but
  `LlvmIrEmitter::llvmType`'s own `HeapArray<...>` branch doesn't recursively re-mangle the element
  text the way a fully general implementation would - untested, likely fine for the common case
  (`HeapArray<i32>`, `HeapArray<SomeStruct>`), not verified for a doubly-nested one.
