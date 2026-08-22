# `SortedSet<T>`: `SortedMap<K,V>`'s Own Tree, Minus the Value Field

**Status:** Implemented
**Document:** `0041-sorted-sets.md`

---

# Motivation

`docs/language/0029-collections.md` specs `SortedSet<T>` as "a unique-value
collection that remains sorted," with `Set<T>`'s own `add`/`contains`/
`remove` vocabulary. With `SortedMap<K,V>` (`0040-sorted-maps.md`) just
finished as a real AVL tree, `SortedSet<T>` is the natural next step -
`SortedMap<K,V>` was already this same relationship once removed
(`Set<T>` is to `Map<K,V>` as `SortedSet<T>` is to `SortedMap<K,V>`), and
this phase makes that parallel exact: the same AVL algorithm, the same node
shape minus one field, `Set<T>`'s own API rather than a new one.

```ax
ids = SortedSet<i32>()
ids.add(93)
ids.add(87)

has = ids.contains(87)   // true
ids.remove(93)
```

**Scope restriction, and why: `T` is `i32`, `i64`, `f64`, `char`, or `str`
only this phase.** Identical reasoning to `PriorityQueue<T>`'s element type
and `SortedMap<K,V>`'s key (`docs/language/0039-priority-queues.md`,
`docs/language/0040-sorted-maps.md`) - `<`/`<=`/`>`/`>=` only ever
typecheck for those five kinds, so they're the only types a tree can order
today (`str` compares via a real lexicographic byte-walk,
`registerOrderRuntime`/`@axea.less.str` - see docs/language/0042-string.md;
`i64`/`f64` are `docs/language/0051-numeric-widening.md`'s own addition).
The *owned* `String` type stays excluded
even though it's str-coercible everywhere else - ordering only ever
considers the bare value type.

**API**: `add(x)`, `contains(x) -> bool`, `remove(x)`, `.length` -
byte-for-byte `Set<T>`'s own signatures, exactly the way `SortedMap<K,V>`'s
own API was byte-for-byte `Map<K,V>`'s. No `for`-in iteration this phase -
same deferred piece `SortedMap<K,V>` has (see that document's own Known
Imprecision section): the entire reason a *sorted* set exists over a plain
one is iteration order, and that still has no syntax to hang off in this
language. Not hashable, not a struct field type, not a `Map`/`Set` key
type - mirrors `SortedMap<K,V>`'s own identical choices.

**The wrinkle, compounding**: `add` collides with `Set<T>`'s own `add` (a
new two-way collision), and `contains`/`remove` now have *four* candidates
sharing one receiver type - `Map<K,V>`, `Set<T>`, `SortedMap<K,V>`, and
`SortedSet<T>` - the widest collision this codebase has resolved yet.
Resolved the identical way every prior one was: a new `isSortedSetExpr`
resolver, checked *before* `isSortedMapExpr`/`isSetExpr` in the dispatch
chain (mirrors `SortedMap<K,V>`'s own `sortedMapKind`-before-`setKind`
ordering, which itself mirrored `PriorityQueue<T>`'s
`priorityQueueKind`-before-`stackKind` ordering).

---

# Design: `SortedMap<K,V>`'s Node, Minus One Field

A `SortedSet<T>` value is a stable pointer to the identical 2-field
`{i32 count, Node* root}*` header `SortedMap<K,V>` uses, pointing at its own
named node type `%axea.SortedSetNode.<id>` - `{ key, height, left, right }`
(field indices 0-3, one field narrower than `SortedMap<K,V>`'s own
`{ key, value, height, left, right }` since there's no value to store).
Every algorithmic piece - `height`, the two rotation primitives,
`insertNode`/`removeNode`'s recursive rebalancing shape, `minValueNode`'s
iterative descent, the successor-splicing two-children removal, the
`i1*`-out-parameter pattern for "did this actually insert/remove," no
`phi`, no `free` - is `SortedMap<K,V>`'s own design carried over unchanged
(see `docs/language/0040-sorted-maps.md`'s own Design section for the full
explanation of each). The one genuine difference: `insertNode`'s
"already present" base case has nothing to update (no value field), so it
simply reports `isNewOut = 0` and returns the node unchanged - `SortedMap<K,V>`'s
equivalent case has to `store` the new value first.

**Distinguishing `SortedSet<T>`'s header from `SortedMap<K,V>`'s own** needs
the same care `Map<K,V>`/`Set<T>` already established for their own shared
3-field shape: both are `{i32, NodePtr}*`-shaped, but the node pointer
itself names a different type (`%axea.SortedSetNode.<id>*` vs.
`%axea.SortedMapNode.<id>*`), so `isSortedSetType` checks that substring
directly - checked *before* `isListType` for the same "explicit, not
accidental" reason every other named-node collection here already is.

---

# Parsing: The Single-Type-Argument Shape, Copied from Set<T>

`SortedSet<elem>` in type position and `SortedSet<elem>()` in expression
position both mirror `Set<T>`'s own single-argument branches exactly. New
`SortedSetNewExpr` AST node, fielded identically to `SetNewExpr`. Method
calls need no parser changes.

```text
$ ax ast examples/sorted_set.ax
Function(build)
  Block
    Assignment(ids)
      SortedSetNew(i32)
    ExprStmt
      MethodCall(add)
        Name(ids)
        Integer(93)
    ...
```

---

# Type Checking: `Set<T>`'s Case, Copied Byte-for-Byte

`TypeKind::SortedSet` is new, reusing `Set<T>`'s own string-based
`elementTypeName` representation (not `List`/`Stack`'s flat `elementKind`
tag) - `SortedSet<T>` is "`Set<T>`, but ordered," so it mirrors `Set<T>`'s
own representation choice, not the array-like collections'.
`resolveType`'s `"SortedSet<elem>"` branch requires
`isOrderableKind(elementType.kind)` directly, the identical orderability
check `SortedMap<K,V>`'s own key uses - not `Set<T>`'s `isHashable` call,
since the real constraint here is orderability, not hashability.
`isOrderableKind` accepts `i32`/`i64` (numeric order), `f64` (numeric
order via `fcmp`'s *ordered* predicates - see
docs/language/0051-numeric-widening.md), `char` (codepoint order - see
docs/language/0044-char.md), and `str` (real lexicographic byte order via
`registerOrderRuntime`/`@axea.less.str` - see docs/language/0042-string.md);
the *owned* `String` type is deliberately excluded even though it's
str-coercible everywhere else, since ordering only ever considers the bare
value type.
`MethodCallExpr` gets its own `TypeKind::SortedSet` case, copied
byte-for-byte from `Set<T>`'s own: `add`/`contains`/`remove` (1 argument,
`contains` returns `bool`, the others return `unit`).

```text
$ ax capabilities bad.ax   # s = SortedSet<bool>()
error: SortedSet<T> requires an orderable element type (i32, i64, f64,
char, or str only in this phase - no other type is comparable yet), found
SortedSet<bool>
```

---

# Capability Checking: Zero Changes, Again

`"add"`/`"remove"` are already in `CapabilityChecker`'s flat, method-name-
only write-raising list (added for `Set<T>` back in `0034`) - so
`SortedSet<T>.add`/`.remove` are covered automatically, the same free ride
every prior sorted/priority collection got from its own unsorted namesake.

---

# Region Checking: `Set<T>`'s Own Zero-Exception Story

`isSortedSetTypeString` mirrors `isSetTypeString` - no `elementTypeName`
extraction needed at all, the identical reasoning `Set<T>` itself already
established: none of `add`/`contains`/`remove` ever return the stored
element, so there's no `MethodCallExpr` aliasing case to wire up (unlike
`SortedMap<K,V>.get()`, which needed `Map<K,V>.get()`'s own exception).
`SortedSetNewExpr` gets the usual "brand-new, always `Owned`" constructor
case, and the borrowed-parameter/return-leak machinery grows a
`SortedSet` branch identical to `Set<T>`'s own.

---

# `IrGenerator`: The Widest Collision Yet - Four Candidates, One Receiver

New `IrSortedSetNew`/`Add`/`Contains`/`Remove` instructions, fielded
identically to their `IrSet*` counterparts. `isSortedSetExpr` is a new
resolver with `isSortedMapExpr`'s exact shape (literal `SortedSetNewExpr`
vs. `SetNewExpr`/`MapNewExpr`/`SortedMapNewExpr`, a parameter type prefix
check against all three, a call's return-type prefix check, or a name
recorded in `IrScope`'s own new parallel `isSortedSet` map).

`lowerExpr`'s `MethodCallExpr` case now checks `sortedSetKind` *before*
`sortedMapKind`/`setKind` at both remaining call sites: `"add"` (previously
unambiguous - only `Set<T>` had it - now two-way against `SortedSet<T>`)
and `"contains"`/`"remove"` (previously three-way among `Map`/`Set`/
`SortedMap`, now four-way).

```text
$ ax ir examples/sorted_set.ax
Function(build)
  Params:
  region.enter
  %0 = sortedset.new i32
  %1 = const.i32 93
  %2 = sortedset.add %0, %1
  ...
```

---

# `LlvmIrEmitter`: `SortedMap<K,V>`'s Ten Functions, Minus One Field Everywhere

`llvmType("SortedSet<T>")` returns `"{i32, " + node + "*}*"` where `node` is
the instantiation's own `%axea.SortedSetNode.<id>` - genuinely distinct
from both `Set<T>`'s 3-field header and `SortedMap<K,V>`'s differently-named
node, so `isSortedSetType` is its own real structural predicate, not a free
ride off an existing one.

`registerSortedSetInstantiation` mirrors `registerSortedMapInstantiation`'s
own lazy-registration pattern, declaring the 4-field node type and nine
runtime functions (one fewer than `SortedMap<K,V>`'s ten - `get` doesn't
exist for a set) per distinct element type actually used: `height`,
`rotateLeft`/`rotateRight`, `insertNode`/`removeNode` (recursive),
`minValueNode` (iterative), and `add`/`contains`/`remove` (header-level
wrappers). Every template is `SortedMap<K,V>`'s own text with field indices
shifted down by one (no value field at index 1) and every value-touching
line removed.

```text
$ ax llvm-ir useSS.ax   # useSS(s: SortedSet<i32>) -> i32 { return s.length }
define i32 @useSS({i32, %axea.SortedSetNode.0*}* %0) {
```

`.length`'s field-get and the top-level print loop both grow a `SortedSet`
branch (checked before `isListType`, alongside `SortedMap<K,V>`'s own) -
printing falls back to `"SortedSet(N entries)"`, the same count-only choice
every unordered-print collection here already makes, for the identical
reason: no `for`-in desugaring exists yet.

---

# Interpreter: `std::set`, Not a Hand-Rolled Tree

```cpp
struct SortedSetInstance { std::set<std::int64_t> elements; };
```

The identical principle `SortedMapInstance`'s own `std::map` already
established, one level simpler: `std::set` is *already* a real balanced
tree, so `add`/`contains`/`remove` are its own `insert`/`contains`/`erase`
directly, with zero hand-rolled AVL logic. `toString` prints count-only -
`"SortedSet(N entries)"` - matching the LLVM backend's own identical
choice, verified directly against compiled `-O0`/`-O1` output during
development (including the same 17-add/4-remove stress test
`SortedMap<K,V>`'s own development used, adapted to element-only content).

---

# Worked Example

`examples/sorted_set.ax`:

```ax
build() -> SortedSet<i32>
{
    ids = SortedSet<i32>()
    ids.add(93)
    ids.add(87)
    ids.add(98)
    return ids
}

addOne(ids: SortedSet<i32>)
{
    ids.add(87)
}

drain(ids: SortedSet<i32>) -> i32
{
    total = 0
    total = total + 87
    ids.remove(87)
    total = total + 93
    ids.remove(93)
    total = total + 98
    ids.remove(98)
    return total
}

ids = build()
lengthAfterBuild = ids.length
hasEighty = ids.contains(87)
called = addOne(ids)
lengthAfterDuplicateAdd = ids.length
total = drain(ids)
lengthAfterDrain = ids.length
```

```text
$ ax run examples/sorted_set.ax
ids = SortedSet(0 entries)
lengthAfterBuild = 3
hasEighty = true
called = ()
lengthAfterDuplicateAdd = 3
total = 278
lengthAfterDrain = 0
$ ax llvm-ir examples/sorted_set.ax | clang -x ir -O1 - -o out && ./out
# byte-for-byte identical (also re-verified at -O0)
```

`addOne` writes through the caller's own tree via `write`-capability `add`,
and `lengthAfterDuplicateAdd = 3` (not `4`) confirms the duplicate `add(87)`
was correctly a no-op - `87` was already present. `drain` sums
`87 + 93 + 98 = 278`, removing each as it goes; `ids` itself prints as
`SortedSet(0 entries)` throughout, including the very first line - same
"top-level bindings print only once the whole program has finished"
precedent every collection's own worked example here already shows.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **`T` is `i32` only.** Same gap `PriorityQueue<T>`/`SortedMap<K,V>`'s own
  restrictions already document.
- **No `for`-in iteration**, and therefore no way to observe sorted order
  from Axea code directly this phase - the identical deferred piece
  `SortedMap<K,V>` has, for the identical reason (blocked on iteration
  syntax that doesn't exist, not on anything tree-specific).
- **No `Optional`-based safe operations** - matches every other collection
  here's identical precedent (nothing in `SortedSet<T>`'s API needed one
  anyway - `add`/`remove`/`contains` all already handle "not present"
  gracefully, the same way `Set<T>` does).
- **`SortedSet<T>` is not a valid `Map`/`Set` key type, and cannot be a
  struct field type.** Mirrors `SortedMap<K,V>`'s own deliberate choices.
- **`add`/`contains`/`remove` are compiler intrinsics, not real methods**,
  same as every other collection here.

---

# Guiding Rule

> `SortedMap<K,V>` was the harder problem - working out that AVL rotations
> map naturally onto genuine recursive LLVM functions, and building the
> full ten-function template set for the first time. `SortedSet<T>` being
> cheap afterward isn't a coincidence or a shortcut - it's the payoff for
> having already found the right *shape* for the algorithm once. The same
> pattern that made `Queue<T>` cheap after `Deque<T>` and `PriorityQueue<T>`'s
> `peek` cheap after its own `pop` applies one level up here: once a hard
> problem's solution is factored correctly (a node type, a rebalance
> routine, an out-parameter convention), the "same tree, minus one field"
> sequel isn't new design work, just careful mechanical adaptation - and
> knowing which parts of a template are load-bearing (the rebalancing
> logic) versus which parts are specific to the wider case (every line that
> touches a value) is what makes that adaptation fast and correct rather
> than another from-scratch derivation.
