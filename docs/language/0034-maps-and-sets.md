# `Map<K,V>` / `Set<T>`: A Real Hash Table, Generic Over Any Hashable Key

**Status:** Implemented
**Document:** `0034-maps-and-sets.md`

---

# Motivation

`docs/language/0029-collections.md` sketches `Map<K,V>`/`Set<T>` as hash-based collections with expected O(1) operations. This document originally shipped a first cut fixed to `Map<i32,i32>`/`Set<i32>` — deliberately scoped that way because a chained hash table's entry (`{key, value, next}`) is self-referential and needs a *named* LLVM type, and supporting arbitrary key/value types meant solving a real design problem first: what does it mean to hash and compare an *arbitrary* Axea value?

This revision answers that, modeled explicitly on Rust: `Map<K,V>`/`Set<T>` now support **any type** as a value, and any *hashable* type as a key:

```ax
counts = Map<str,i32>()
counts.set("apple", 3)
counts.set("banana", 5)

struct Point { x: i32  y: i32 }
seen = Set<Point>()
seen.add(Point { x: 1  y: 1 })

places = Map<str,Point>()
places.set("home", Point { x: 3  y: 4 })
```

**Why Rust's model specifically.** Asked how other languages handle this, the answer splits into two families: object/reference-based languages (Java, Python, C#) get a single hash-table implementation "for free" because `hashCode()`/`equals()` are virtual method calls — dynamic dispatch does the work. Compiled, non-boxed languages (C++, Rust, Swift) don't have that luxury: they generate a **distinct hash/equality implementation per concrete type actually used** (monomorphization), with built-in implementations for primitives and derived (often automatic) implementations for compound types. Axea has no vtables, no boxing, and no runtime type descriptors — it's a flat, statically-typed, C++/Rust-shaped language, not a Java/Python-shaped one — so monomorphization is the only approach that actually fits the existing architecture.

**Key/value asymmetry, mirroring Rust exactly:** keys need real `Hash + Eq`; values are only ever stored, never searched by, so they need neither.

- **Valid key types (K for `Map`, element type for `Set`)**, recursively: `i32`, `bool`, `str`, a `struct` (if every field is itself a valid key type — recursive, automatic, cycle-safe — see "Type Checking" below), a fixed array `[T;N]` (if `T` is), `List<T>` (if `T` is). **Not valid**: `Map`/`Set` themselves (mirrors Rust — `HashMap`/`HashSet` don't implement `Hash`, since there's no canonical order to hash over), `slice<T>` (already parameter-only everywhere in this language).
- **Valid value types (V for `Map` only — `Set` has no separate value)**: anything resolvable except `slice<T>` — including `struct`, arrays, `List<T>`, and nested `Map`/`Set`. No hashability requirement.

Two genuinely new pieces of LLVM-level ground, each hand-verified in an isolated `.ll` file against real `clang` (`-O0` and `-O1`) before being written into the real backend:

1. **Byte-walk hashing/equality over a null-terminated `i8*`** — the mechanism `str` keys need, and the first byte-level loop in this codebase.
2. **A runtime element-by-element hash/equality walk over a `List<T>`'s buffer** — needed because, unlike a fixed array, a `List<T>`'s length isn't known until runtime.

---

# Design: Monomorphized Entry Types, and a Memoized, Recursive Key-Runtime Generator

**Per-instantiation entry types, assigned sequential IDs on first sight.** Every distinct `Map<K,V>`/`Set<T>` shape actually used in a program gets its own named LLVM type and its own runtime functions, registered lazily the first time `LlvmIrEmitter::llvmType` resolves that canonical type string (mirrors how string literals are already deduplicated by content):

```llvm
%axea.MapEntry.0 = type { i32, i32, %axea.MapEntry.0* }         ; Map<i32,i32>
%axea.MapEntry.1 = type { i8*, %Point*, %axea.MapEntry.1* }     ; Map<str,Point>
%axea.SetEntry.0 = type { %Point*, %axea.SetEntry.0* }          ; Set<Point>
```

`%axea.MapEntry.<id>`/`%axea.SetEntry.<id>` are *named* — unlike every other collection in this language, which uses an anonymous shape — because an entry's own self-reference (`next` pointing at another entry of the same type) can only be expressed in LLVM through a name. The header itself stays anonymous, exactly like `List<T>`'s own header: `Map<K,V>` → `{i32 count, i32 bucketCount, %axea.MapEntry.<id>** buckets}*`.

**Key hash/equality, memoized and recursive.** `registerKeyRuntime(axeaKeyType)` returns the `(hashFnName, eqFnName)` pair to call for a given key type, generating the actual functions into the module the first time that key type is seen:

- `i32`/`bool`: a single `mul`/`icmp` each — `@axea.hash.i32`, `@axea.hash.bool`, `@axea.eq.i32`, `@axea.eq.bool`.
- `str`: a byte-walk hash loop and a byte-walk equality loop over the `i8*`, using this codebase's established `alloca`/`load`/`store` loop-counter idiom (no `phi` — this backend's unnamed, strictly-sequential register numbering can't forward-reference a `phi`'s back-edge value).
- `struct`: `@axea.hash.<StructName>`/`@axea.eq.<StructName>` — **name-based, not numeric**, generated once per distinct struct actually used as a key (reused across every `Map`/`Set` keyed by that struct), recursing into each field's own `registerKeyRuntime` call. Hash combines field hashes with a classic djb2/Java-style accumulator; equality `AND`s every field's own equality call together (no short-circuit branching — simpler, always-correct straight-line code).
- Fixed array `[T;N]`/`List<T>`: synthetic numeric IDs (`@axea.hash.arr.<id>`, `@axea.hash.list.<id>`), recursing into the element type's own `registerKeyRuntime`. The array case unrolls (`N` is compile-time-known, same reasoning as the bucket-array zero-init below); the `List<T>` case needs a genuine runtime loop — compare lengths first (an immediate mismatch needs no element walk at all), then an `alloca`-based element-by-element walk.

Cycle safety for a self-/mutually-recursive struct chain (already possible today, since structs are always heap-allocated by pointer) lives entirely in `TypeChecker::isHashable` (see below) — by the time `registerKeyRuntime` runs, the key has already been proven acyclic, so the LLVM-generation side never needs its own cycle guard.

**Per-instantiation runtime functions.** For each registered `(id, K, V)`, `LlvmIrEmitter` emits `@axea.map.<id>.set/get/contains/remove/resize` (and the `Set` equivalents) — structurally the same chain-walk/insert/resize shape the original `i32`-only phase hand-verified, now parameterized over `K`/`V`'s actual LLVM types and calling into `registerKeyRuntime`'s hash/equality pair instead of a fixed inline `mul`+`icmp`. Every `.set`/`.get`/`.contains`/`.remove`/`.add` call in a compiled program calls that specific instantiation's own function — `emitMapSet`/`emitMapGet`/etc. resolve which instantiation by parsing the `id` straight out of the already-inferred header type string (`"{i32, i32, %axea.MapEntry.7**}*"` → `7`), no separate reverse-lookup table needed.

---

# Type Checking: A Recursive, Cycle-Safe `isHashable`

`Type` gained two new string fields for `Map`/`Set` — `elementTypeName` (doubles as `Set`'s own element type) and `valueTypeName` (`Map`-only). Unlike `Array`/`Slice`/`List`'s flat `elementKind` tag (a single `TypeKind`, sufficient for one level of primitive/struct nesting), `K`/`V` can themselves be arbitrarily nested (`List<i32>`, another `Map<...>`, a struct) — a flat tag can't carry that, so these store the *canonical* `resolveType`-able string instead, re-resolved on demand wherever the full `Type` is actually needed (mirrors the same "store the string, re-resolve later" trick `MapNewExpr`/`SetNewExpr` already used at the AST layer).

`resolveType`'s `Map<key,value>`/`Set<elem>` branches do a bracket-depth-aware comma split (the naive `find(',')` from the original phase breaks the instant a value type can itself contain a comma, e.g. `Map<i32, Map<i32,i32>>`), recursively resolve each part, and validate the key with `isHashable`:

```text
$ ax capabilities bad.ax   # m = Map<Map<i32,i32>,i32>()
error: Map<K,V> requires a hashable key type (i32, bool, str, or a struct/array/List composed
entirely of hashable types), found Map<Map<i32,i32>,i32>
```

`isHashable` recurses: `i32`/`bool`/`str` → true; `Array`/`List` → recurse on the element (reusing their existing flat representation, untouched by this rewrite); `Struct` → true iff every declared field's resolved type is hashable, recursing with the struct's own name added to a `visitedStructs` set first — a self-/mutually-recursive struct chain (already possible today, since structs are always by-pointer) short-circuits to `false` on revisiting a struct still being checked, rather than looping forever; everything else (`Map`, `Set`, `slice`, `unit`, ...) → false.

`MethodCallExpr`'s `.set(k, v)`/`.get(k)`/`.contains(k)`/`.remove(k)` (and `Set`'s `.add`/`.contains`/`.remove`) check arguments against `K`'s/`V`'s *real* resolved types now, not a hardcoded `i32` — and `.get` returns `V`'s real type, which is also where struct-aliasing propagation starts (see `RegionChecker` below).

---

# Region Checking: `.get()` Is the One Method That *Doesn't* Remove — It Has to Alias, Not Own

`List<T>.pop()` removes its element, so its result is always safely `Owned` — nothing else still references it. `Map<K,V>.get()` does **not** remove: if `V` is a struct, the map still holds the exact same instance afterward, so the returned pointer aliases the map's own storage, exactly the way indexing into an array-of-structs already does. Reusing `pop`'s "always `Owned`" rule for `get` would have silently let a borrowed `Map<K, StructV>` parameter's stored struct escape a function's return through `.get()` — caught by writing the regression test for it, not anticipated up front:

```text
$ ax regions leak.ax   # leak(m: Map<i32,Point>) -> Point { return m.get(1) }
error: function 'leak' cannot return 'm': parameter 'm' is borrowed and does not outlive the
call - declare 'take' if ownership should transfer
```

So `RegionChecker`'s `MethodCallExpr` case special-cases `"get"`: when the object's `elementStructType` is populated, it propagates the object's own `Region`/`sourceParam` (mirroring `IndexExpr`'s identical rule for a struct-typed array element) instead of `pop`'s "always `Owned`." `checkFunction`'s existing per-parameter `Map`/`Set` detection grows a `V`-struct-name extraction (its own small bracket-aware comma split, per this codebase's "each pass owns its own walk" convention) feeding that `elementStructType`. `Set` needs no equivalent change: none of its operations (`add`/`contains`/`remove`) ever return the stored element.

---

# `IrGenerator`: `IrMapNew`/`IrSetNew` Carry Their Concrete Type Strings; `isSetExpr` Still Just Needs a Boolean

`IrMapNew`/`IrSetNew` (previously fieldless, since the fixed `i32,i32` shape needed nothing) now carry `keyTypeName`/`valueTypeName`/`elementTypeName`, populated straight from the AST's own `MapNewExpr`/`SetNewExpr` fields — mirroring how `IrListNew::elementTypeName` already works. `isSetExpr` (added last phase specifically because `.contains`/`.remove` are valid on both `Map` and `Set`) needed no changes: it only ever resolved a boolean "is this a `Set`," never `K`/`V` granularity.

---

# `Interpreter`: `Value`-Keyed Containers with a Structural `ValueHash`/`ValueEq`

```cpp
struct ValueHash { std::size_t operator()(const Value&) const; };
struct ValueEq { bool operator()(const Value&, const Value&) const; };
struct MapInstance { std::unordered_map<Value, Value, ValueHash, ValueEq> entries; };
struct SetInstance { std::unordered_set<Value, ValueHash, ValueEq> elements; };
```

`ValueHash`/`ValueEq` dispatch on `Value`'s active `std::variant` alternative: `int64_t`/`bool`/`string` hash/compare directly; `shared_ptr<StructInstance>`/`ArrayInstance`/`ListInstance` hash/compare their **dereferenced contents**, recursively combining each field's/element's own `ValueHash`/`ValueEq` — not pointer identity, so two separately-constructed-but-equal keys collide and compare equal, matching the value semantics the LLVM backend enforces. No cycle protection needed here either, for the identical reason as `TypeChecker::isHashable`: a self-referential struct was already rejected as a key type before any `Value` of that shape could reach a `Map`/`Set` in a well-typed program.

---

# Worked Example

`examples/map_set.ax`'s generic section (the file also keeps the original `i32`-keyed resize-forcing section from the first phase):

```ax
struct Point { x: i32  y: i32 }

wordCounts() -> Map<str,i32>
{
    counts = Map<str,i32>()
    counts.set("apple", 3)
    counts.set("banana", 5)
    counts.set("apple", 4)   # update, not a duplicate
    return counts
}

uniquePoints() -> Set<Point>
{
    seen = Set<Point>()
    seen.add(Point { x: 1  y: 1 })
    seen.add(Point { x: 1  y: 1 })   # structurally equal - deduped
    seen.add(Point { x: 2  y: 2 })
    return seen
}

locations() -> Map<str,Point>
{
    places = Map<str,Point>()
    places.set("origin", Point { x: 0  y: 0 })
    places.set("home", Point { x: 3  y: 4 })
    return places
}

counts = wordCounts()
appleCount = counts.get("apple")
wordTotal = counts.length

points = uniquePoints()
pointCount = points.length
hasOrigin = points.contains(Point { x: 1  y: 1 })

places = locations()
home = places.get("home")
homeX = home.x
homeY = home.y
```

```text
$ ax run examples/map_set.ax   # (generic section)
counts = Map(2 entries)
appleCount = 4
wordTotal = 2
points = Set(2 entries)
pointCount = 2
hasOrigin = true
places = Map(2 entries)
home = Point { x: 3, y: 4 }
homeX = 3
homeY = 4
$ ax llvm-ir examples/map_set.ax | clang -x ir -O1 - -o out && ./out
# byte-for-byte identical (also re-verified at -O0)
```

`appleCount = 4` confirms `.set` on an already-present `str` key updates in place rather than duplicating; `pointCount = 2` (not 3) confirms `Set<Point>` deduplicates two separately-constructed-but-structurally-equal `Point` values; `hasOrigin = true` confirms `.contains` recognizes a freshly-constructed `Point { x: 1, y: 1 }` as equal to the one already stored. A generated hash/equality pair for `Point` (from `ax llvm-ir`):

```llvm
define i32 @axea.hash.Point(%Point* %v) {
entry:
  %fp0 = getelementptr %Point, %Point* %v, i32 0, i32 0
  %fv0 = load i32, i32* %fp0
  %fh0 = call i32 @axea.hash.i32(i32 %fv0)
  %am0 = mul i32 0, 31
  %acc0 = add i32 %am0, %fh0
  %fp1 = getelementptr %Point, %Point* %v, i32 0, i32 1
  %fv1 = load i32, i32* %fp1
  %fh1 = call i32 @axea.hash.i32(i32 %fv1)
  %am1 = mul i32 %acc0, 31
  %acc1 = add i32 %am1, %fh1
  ret i32 %acc1
}
```

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **No iteration (`for`-in) over a `Map`/`Set`.** Printing or walking every entry needs a further bucket-and-chain traversal loop on top of everything else; deferred.
- **No full content printing at the top level.** Without iteration, a top-level `Map`/`Set` binding still prints as `Map(N entries)`/`Set(N entries)` — the O(1)-accessible count field — not its actual contents.
- **No set algebra (`|`, `&`, `-`) or `map { ... }`/`set { ... }` literal block syntax** from `0029-collections.md`'s original sketch.
- **`Map`/`Set` (and `slice<T>`) still can't be a key type.** Mirrors Rust's own `HashMap`/`HashSet` not implementing `Hash`; `slice<T>`'s parameter-only restriction applies uniformly to K and V both.
- **`Map<K,V>`/`Set<T>` still cannot be a struct field type.** Kept out purely to bound this pass's surface area, same restriction `List<T>` already has; not a hard architectural limit.
- **Compiled `.get` on a missing key returns an unspecified sentinel** (`i32`'s own minimum for an `i32` `V`, `false` for `bool`, `null` for every pointer-shaped `V`) rather than a defined value or an `Optional`. The interpreter always throws instead — matches indexing's own out-of-bounds split exactly.
- **No amortized-shrink on `remove`.** The bucket array only ever grows (on load factor), never shrinks — matches `List<T>`'s own "no shrink/realloc on `pop`" choice.
- **`set`/`get`/`contains`/`remove`/`add` are compiler intrinsics, not real methods.** Same as `List<T>`'s `push`/`pop` — there's no user-definable method/`impl` system in this language.

---

# Guiding Rule

> Ask what the *operation* actually does before reusing a shape that merely looks similar. `List<T>.pop()` and `Map<K,V>.get()` read like the same kind of thing — "get a stored element out of a collection" — and it would have been easy to give `.get()` `.pop()`'s exact region-checking treatment on that resemblance alone. But `pop` *removes*; `get` *peeks*. That one-word difference is the entire reason `.get()`'s result must alias its container while `.pop()`'s result safely doesn't — and it only surfaced by writing the test that exercises exactly that distinction, not by reasoning about it in the abstract. The same discipline carried through the rest of this phase: the recursive `isHashable`/`registerKeyRuntime` machinery only had to handle real cases (i32, bool, str, struct, array, `List<T>`) because each new key shape was hand-verified against real `clang` before being trusted, the same way the original chained-hash-table mechanism was proven in isolation before this whole feature existed.
