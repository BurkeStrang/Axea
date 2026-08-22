# `SortedMap<K,V>`: A Real AVL Tree, the First Self-Balancing Structure

**Status:** Implemented
**Document:** `0040-sorted-maps.md`

---

# Motivation

`docs/language/0029-collections.md` specs `SortedMap<K,V>` as "a key/value
collection that keeps keys ordered," sketched with `[key] = value` indexed
assignment, `for name, score in scores` two-variable iteration, and
"Expected implementation: balanced search tree." None of that surface syntax
exists anywhere in this codebase yet - no `[key] =` indexed-assignment
desugar for a non-array-shaped receiver, no two-variable `for`-in
destructuring. This phase deliberately keeps `SortedMap<K,V>`'s *API*
identical to `Map<K,V>`'s own (`set`/`get`/`contains`/`remove`/`.length`),
so the only genuinely new work is *underneath*: a real self-balancing
binary search tree, not a renamed hash table.

```ax
scores = SortedMap<i32,i32>()
scores.set(93, 1)
scores.set(87, 2)

top = scores.get(87)         // 2
has = scores.contains(93)    // true
scores.remove(93)
```

**Scope restriction, and why: `K` is `i32`, `i64`, `f64`, `char`, or `str`
only this phase.** Exactly the restriction `PriorityQueue<T>` already
established for its own element type
(`docs/language/0039-priority-queues.md`) - `<`/`<=`/`>`/`>=`
(`TypeChecker::requireOrdered`, backed by `isOrderableKind`) only ever
typecheck for those five kinds, so they're the only types a tree can
meaningfully *order* today - `str` compares via a real lexicographic
byte-walk (`registerOrderRuntime`/`@axea.less.str`, see
docs/language/0042-string.md), not a bare pointer comparison; `i64`/`f64`
are `docs/language/0051-numeric-widening.md`'s own addition. The *owned*
`String` type stays excluded
even though it's str-coercible everywhere else - ordering, like
`Set<T>`/`Map<K,V>`'s own hashability, only ever considers the bare value
type. `V` has no such restriction - never compared, only stored, exactly
like `Map<K,V>`'s own `V` - so `SortedMap<i32,Point>`,
`SortedMap<i32,List<i32>>`, even `SortedMap<i32,Map<i32,i32>>` all work,
mirroring `Map<K,V>`'s own already-generic `V`.

**Algorithm choice: AVL, not red-black.** Both are legitimate "real balanced
search tree" answers to `0029`'s own spec. AVL's rebalancing decision is
purely arithmetic (compare left/right subtree heights, four rotation
cases) - red-black's own fixup needs to track node color and walks several
uncle-color/parent-color cases per rebalance. AVL was the more tractable
choice to hand-emit correctly as raw LLVM IR text, the same kind of "which
option is actually tractable to hand-roll in this codegen style" judgment
call `Deque<T>`'s own "growable array with a `start` offset, not a real
ring buffer" choice made (`docs/language/0037-deques.md`).

**API**: `set(key, value)`, `get(key) -> V`, `contains(key) -> bool`,
`remove(key)`, `.length` - byte-for-byte `Map<K,V>`'s own signatures; the
ordering lives entirely in the implementation, not the interface. No
`[key]`/`[key] =` syntax and no `for`-in iteration this phase (see Known
Imprecision below) - `SortedMap<K,V>` isn't indexable, mirroring
`Map<K,V>`/`Set<T>`'s own identical restriction. Not hashable, not a struct
field type, not a `Map`/`Set` key type - mirrors `Queue<T>`/
`PriorityQueue<T>`'s own identical choices; nothing in scope calls for any
of them.

**The wrinkle**: `set`/`get` are the same two method names `Map<K,V>` already
claims, and `contains`/`remove` are shared with *both* `Map<K,V>` and
`Set<T>` - the first *two-different-width* collision in this codebase
(`set`/`get` is two-way, `contains`/`remove` is three-way, on the exact same
receiver). Resolved the same way every prior collision here was:
`IrGenerator` gets a new `isSortedMapExpr` resolver (a sibling of
`isSetExpr`, not a generalization of it), consulted *before* falling back
to the existing `Map`-vs-`Set` `setKind` check - mirrors
`PriorityQueue<T>`'s own `priorityQueueKind`-before-`stackKind` ordering
for `push`/`pop`/`peek` exactly.

---

# Design: A Named, Self-Referential Node - the First Node Header This Backend Balances

A `SortedMap<K,V>` value is a stable pointer to a small anonymous 2-field
heap header - `{i32 count, Node* root}*` - mirroring `List<T>`'s own
"stable pointer, mutated in place" header shape, but (like `Map<K,V>`/
`Set<T>`/`LinkedList<T>` before it) its second field is a *named*,
self-referential node pointer (`%axea.SortedMapNode.<id>`, declared by
`registerSortedMapInstantiation`), not a plain `T*`. Each node is
`{ key, value, height, left, right }` (field indices 0-4) - the classic AVL
node shape, `height` cached rather than recomputed on every rebalance
check.

**What makes this different from every earlier collection here**: `Map<K,V>`'s
own hash-table insert/remove never needs to *reshape* the structure based
on content - a bucket's chain just grows or shrinks. An AVL insert/delete
genuinely restructures the tree via rotations, and - unlike `PriorityQueue<T>`'s
heap sift, which is a single linear walk toward the root or a leaf - has to
recurse *down* to the affected node and then rebalance on the way back
*up*, at every level the recursion passes through. That shape (recurse,
mutate, examine what the recursive call reported, possibly rotate, return
upward) maps naturally onto genuine recursive LLVM functions - the first
time this backend's own runtime-function templates (see `Map<K,V>`'s
`kMapSetTemplate` and friends, `docs/language/0034-maps-and-sets.md`) call
*themselves*, not just each other. `insertNode`/`removeNode` are true
self-recursive `@axea.sortedmap.<id>.insertNode`/`.removeNode` functions;
`get`/`contains` need no recursion at all (a plain iterative descent, same
shape as `Map<K,V>.get()`'s own bucket-chain walk).

**Still no `phi`, even in recursive functions.** Every runtime-function
template this backend has ever hand-written (`Map<K,V>`'s `set`/`get`,
`LinkedList<T>`'s push/pop) already uses named (not numbered) registers,
which don't carry the strict-increasing-order constraint the main
instruction-lowering path needs `alloca` to route around - but they *still*
use `alloca`/load/store for every loop-carried or multi-path value, not
`phi`, matching this backend's single established convention throughout.
`SortedMap<K,V>`'s own two out-parameters (`insertNode`'s `i1* isNewOut`,
`removeNode`'s `i1* isRemovedOut` - "did this call actually insert/remove,
as opposed to update-in-place or find nothing") are exactly this pattern:
written once, at whichever base case is reached, and read back by the
top-level `set`/`remove` wrapper to decide whether `.length`'s count needs
to change.

**Rebalancing, concretely.** After `insertNode` recurses into a child and
splices the (possibly rotated) result back in, it recomputes `height` and
`balance = height(left) - height(right)`, then picks among four rotation
cases by comparing the just-inserted `key` against the relevant child's own
key (the standard "which side did this insertion actually land on"
simplification - correct because only one root-to-leaf path changed this
call). `removeNode`'s own rebalance is the textbook sequel: no
just-inserted key to compare against, so it inspects the *child's own*
balance factor instead to choose LL-vs-LR / RR-vs-RL. Two-children removal
splices out the in-order successor (`minValueNode` - the smallest key in
the right subtree) by copying its key/value up and recursively removing
*it* instead, via an internal `i1* %dummy` out-parameter the top-level
`.remove()` call never sees. Matches every other removal in this codebase:
the unlinked node is never `free`d (see `Map<K,V>.remove()`'s own identical
"leak, don't free" choice) - this backend has never called `free` anywhere.

---

# Parsing: The Two-Type-Argument Shape, Copied from Map<K,V>

`SortedMap<key,value>` in type position and `SortedMap<key,value>()` in
expression position both mirror `Map<K,V>`'s own two-argument branches
exactly (comma-separated, `parseTypeName()`-recursive on each side). New
`SortedMapNewExpr` AST node, fielded identically to `MapNewExpr`. Method
calls need no parser changes - `object.method(args)` already parses
generically.

```text
$ ax ast examples/sorted_map.ax
Function(build)
  Block
    Assignment(scores)
      SortedMapNew(i32, i32)
    ExprStmt
      MethodCall(set)
        Name(scores)
        Integer(93)
        Integer(1)
    ...
```

---

# Type Checking: Orderability, Not Hashability

`TypeKind::SortedMap` is new, reusing `Map<K,V>`'s own `elementTypeName`/
`valueTypeName` string-storage fields (K/V can each be arbitrarily nested,
so - like `Map`/`Set` - a single flat `elementKind` tag isn't enough).
`resolveType`'s `"SortedMap<key,value>"` branch reuses `Map<K,V>`'s own
bracket-depth-aware comma split, but requires
`isOrderableKind(keyType.kind)` directly rather than calling `isHashable` -
the real constraint here is orderability, not hashability, and (exactly
like `PriorityQueue<T>`'s own check) that single condition already
excludes every non-orderable type there is. `isOrderableKind` accepts
`i32`/`i64` (numeric order), `f64` (numeric order via `fcmp`'s *ordered*
predicates - see docs/language/0051-numeric-widening.md), `char`
(codepoint order - see docs/language/0044-char.md), and `str` (real
lexicographic byte order via `registerOrderRuntime`/`@axea.less.str` - see
docs/language/0042-string.md); the *owned* `String` type is deliberately
excluded even though it's str-coercible everywhere else, since ordering
only ever considers the bare value type:

```text
$ ax capabilities bad.ax   # m = SortedMap<bool,i32>()
error: SortedMap<K,V> requires an orderable key type (i32, i64, f64, char,
or str only in this phase - no other type is comparable yet), found
SortedMap<bool,i32>
```

`SortedMapNewExpr`'s own `checkExpr` case delegates straight to
`resolveType("SortedMap<" + ... + ">")`, the same "delegate to resolveType
for real semantic validation" choice `MapNewExpr`/`SetNewExpr`/
`PriorityQueueNewExpr` already make. `MethodCallExpr` gets its own
`TypeKind::SortedMap` case - `set`/`get`/`contains`/`remove`, copied
byte-for-byte from `Map<K,V>`'s own case (same argument-count/type checks,
same return types). `.length` gets its own standalone `checkFieldType`
case, mirroring `Map<K,V>`/`Set<T>`'s own (not folded into `isIndexable` -
no `[key]` this phase).

---

# Capability Checking: Zero Changes, Again

`"set"`/`"remove"` are already in `CapabilityChecker`'s flat, method-name-
only write-raising list (added for `Map<K,V>` back in `0034`) - so
`SortedMap<K,V>.set`/`.remove` are covered automatically, the same free ride
`PriorityQueue<T>.push`/`.pop` already got from `List<T>`'s own list.
`"get"`/`"contains"` need no addition either - read-only by default.

---

# Region Checking: `Map<K,V>`'s Own `.get()` Exception, For Free

`isSortedMapTypeString`/`sortedMapValueTypeName` mirror `isMapTypeString`/
`mapValueTypeName` exactly (only `V`, not `K`, matters - `.get()` is the
only `SortedMap` operation that can hand back a value aliasing the tree's
own stored instance). `SortedMapNewExpr` gets the same "brand-new, always
`Owned`" `regionOfExpr` case every collection's constructor does.

The existing `.get()`/`.peek()` aliasing exception (added for
`Map<K,V>.get()`, then covering `Stack<T>.peek()` too) already covers
`SortedMap<K,V>.get()` for free - it's a literal string match on the method
name `"get"`, not an object-type check:

```text
$ ax regions leak.ax   # leak(m: SortedMap<i32,Point>) -> Point { return m.get(1) }
error: function 'leak' cannot return 'm': parameter 'm' is borrowed and does
not outlive the call - declare 'take' if ownership should transfer
```

---

# `IrGenerator`: The First Two-Different-Width Collision on One Receiver

New `IrSortedMapNew`/`Set`/`Get`/`Contains`/`Remove` instructions, fielded
identically to their `IrMap*` counterparts. `isSortedMapExpr` is a new
resolver with `isSetExpr`'s exact shape (literal `SortedMapNewExpr` vs.
`MapNewExpr`/`SetNewExpr`, a parameter type prefix check, a call's
return-type prefix check, or a name recorded in `IrScope`'s own new
parallel `isSortedMap` map).

`lowerExpr`'s `MethodCallExpr` case now checks `sortedMapKind` *before*
`setKind` at all four call sites: `"set"`/`"get"` (previously unambiguous -
only `Map<K,V>` had them - now two-way against `SortedMap<K,V>`) and
`"contains"`/`"remove"` (previously two-way between `Map`/`Set`, now
three-way).

```text
$ ax ir examples/sorted_map.ax
Function(build)
  Params:
  region.enter
  %0 = sortedmap.new i32, i32
  %1 = const.i32 93
  %2 = const.i32 1
  %3 = sortedmap.set %0, %1, %2
  ...
```

---

# `LlvmIrEmitter`: A Named Node Header, Ten New Runtime Functions

`llvmType("SortedMap<K,V>")` returns `"{i32, " + node + "*}*"` where `node`
is the instantiation's own `%axea.SortedMapNode.<id>` - a genuinely
different LLVM type from `Map<K,V>`'s own 3-field, bucket-array header, so
(unlike `Stack<T>`/`Queue<T>`/`PriorityQueue<T>` before it) `SortedMap<K,V>`
*does* need its own structural predicate, `isSortedMapType` - checked
*before* `isListType` for the same "explicit, not accidental" reason every
other named-node collection here already is.

`registerSortedMapInstantiation` mirrors `registerMapInstantiation`'s own
lazy-registration-by-canonical-string pattern, declaring the node's 5-field
named type and ten runtime functions per distinct `(K,V)` shape actually
used: `height` (0 for null, else the cached field), `rotateLeft`/
`rotateRight` (the two single-rotation primitives, straight-line - no loop,
no recursion, just direct pointer surgery + two height recomputations via
`select`), `insertNode`/`removeNode` (the genuinely recursive pair - see
Design above), `minValueNode` (iterative descent to a subtree's leftmost
node), and `set`/`get`/`contains`/`remove` (thin header-level wrappers,
exactly `Map<K,V>`'s own shape). `K` needs no `registerKeyRuntime` call the
way `Map<K,V>`'s own arbitrary, hashable `K` does - it's always `i32`, so
every comparison is a direct `icmp slt`/`icmp sgt i32`.

```text
$ ax llvm-ir useSM.ax   # useSM(m: SortedMap<i32,i32>) -> i32 { return m.length }
define i32 @useSM({i32, %axea.SortedMapNode.0*}* %0) {
```

`.length`'s field-get and the top-level print loop both grow a
`SortedMap` branch (checked before `isListType`, same position
`isLinkedListType` already occupies) - printing falls back to
`"SortedMap(N entries)"`, the same count-only choice `Map<K,V>`/`Set<T>`/
`LinkedList<T>` already made, for the same reason: no `for`-in desugaring
exists yet to hang a real traversal off.

---

# Interpreter: `std::map`, Not a Hand-Rolled Tree

```cpp
struct SortedMapInstance { std::map<std::int64_t, Value> entries; };
```

Every earlier collection's interpreter representation stood in for
whatever the LLVM backend hand-rolls, without replicating its actual
algorithm (`MapInstance`'s `std::unordered_map` doesn't reimplement
chaining; `LinkedListInstance`'s `std::deque` doesn't reimplement node
links). `SortedMapInstance` follows the identical principle one step
further: `std::map` is *already* a real balanced tree (red-black, in every
standard library) - reusing it gives the exact "keys stay ordered, O(log n)
operations" behavior for free, with zero hand-rolled AVL logic in the
interpreter at all. `set`/`get`/`contains`/`remove` are `std::map`'s own
`operator[]`/`find`/`contains`/`erase`; `get` on a missing key throws,
matching `Map<K,V>.get()`'s own identical precedent. `toString` prints
count-only - `"SortedMap(N entries)"` - deliberately matching the LLVM
backend's own choice even though `std::map` could trivially print sorted
contents; the two backends must agree byte-for-byte (verified directly
against compiled `-O0`/`-O1` output during development, including an
exhaustive 17-insert/4-remove/6-get stress test exercising every AVL
rotation case).

---

# Worked Example

`examples/sorted_map.ax`:

```ax
build() -> SortedMap<i32,i32>
{
    scores = SortedMap<i32,i32>()
    scores.set(93, 1)
    scores.set(87, 2)
    scores.set(98, 3)
    return scores
}

updateOne(scores: SortedMap<i32,i32>)
{
    scores.set(87, 99)
}

drainSmallest(scores: SortedMap<i32,i32>) -> i32
{
    total = 0
    total = total + scores.get(87)
    scores.remove(87)
    total = total + scores.get(93)
    scores.remove(93)
    total = total + scores.get(98)
    scores.remove(98)
    return total
}

scores = build()
lengthAfterBuild = scores.length
hasEighty = scores.contains(87)
called = updateOne(scores)
valueAfterUpdate = scores.get(87)
total = drainSmallest(scores)
lengthAfterDrain = scores.length
```

```text
$ ax run examples/sorted_map.ax
scores = SortedMap(0 entries)
lengthAfterBuild = 3
hasEighty = true
called = ()
valueAfterUpdate = 99
total = 103
lengthAfterDrain = 0
$ ax llvm-ir examples/sorted_map.ax | clang -x ir -O1 - -o out && ./out
# byte-for-byte identical (also re-verified at -O0)
```

`updateOne` writes through the caller's own tree via `write`-capability
`set`, and `valueAfterUpdate = 99` confirms the update replaced `87`'s
value in place rather than inserting a duplicate (`lengthAfterBuild` stays
`3`, not `4`, right before it). `drainSmallest` sums `99 + 1 + 3 = 103`,
removing each key as it goes; `scores` itself prints as `SortedMap(0
entries)` throughout (including the very first line) - top-level bindings
print only once the whole program has finished, exactly like every other
collection's own identical final-state printing here, so this particular
worked example only ever observes `scores` after `drainSmallest` has
already emptied it.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **`K` is `i32` only.** No `by:`/`order:` selector, no `Ordered` trait -
  same gap `PriorityQueue<T>`'s own element-type restriction already
  documents (see `docs/language/0039-priority-queues.md`).
- **No `[key]`/`[key] =` syntax.** `0029`'s own sketch shows it, but no
  indexed-assignment desugar exists for any non-array-shaped receiver in
  this codebase - `set(key, value)` is the only mutation path, matching
  `Map<K,V>`'s own identical choice.
- **No `for`-in iteration**, and therefore no way to observe sorted order
  from Axea code directly this phase - only internally (verified by tests
  spanning many inserts/removes/updates). The entire reason `SortedMap<K,V>`
  exists over plain `Map<K,V>` is iteration order, so this is the single
  biggest deferred piece - blocked on two-variable `for`-in destructuring
  syntax that doesn't exist anywhere in this language yet, not on anything
  tree-specific.
- **No `Optional`-based safe `get`.** Throws on a missing key in the
  interpreter, returns an unspecified sentinel in compiled code - matches
  `Map<K,V>.get()`'s own exact precedent.
- **`SortedMap<K,V>` is not a valid `Map`/`Set` key type, and cannot be a
  struct field type.** Mirrors `Queue<T>`/`PriorityQueue<T>`'s own
  deliberate choices.
- **`set`/`get`/`contains`/`remove` are compiler intrinsics, not real
  methods**, same as every other collection here.

---

# Guiding Rule

> `PriorityQueue<T>` was this session's first collection needing a genuine
> algorithm instead of a renamed operation - but its sift-up/sift-down was
> still a single linear walk. `SortedMap<K,V>` is the first one where the
> natural shape of the algorithm (recurse down, rebalance on the way back
> up) doesn't fit that "one hand-rolled loop" mold at all - it fits the
> shape of a function calling itself. The right response wasn't to force a
> recursive algorithm into an iterative loop for uniformity's sake; it was
> to recognize that this backend's existing runtime-function machinery
> (named registers, real `define`d LLVM functions, already used for every
> `Map`/`Set`/`LinkedList` operation) already supports genuine recursion for
> free, and to reach for it deliberately instead of fighting the algorithm's
> own natural shape.
