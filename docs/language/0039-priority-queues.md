# `PriorityQueue<T>`: A Real Binary Heap, the First Collection That Needs an Actual Algorithm

**Status:** Superseded — see "2026 Update: Ported to Real Axea Source" below
**Document:** `0039-priority-queues.md`

---

# 2026 Update: Ported to Real Axea Source

`PriorityQueue<T>` is no longer a compiler intrinsic. Following `List<T>`/`Stack<T>`/`Deque<T>`/
`Queue<T>`'s own ports (`docs/language/0033-lists.md`/`0035-stacks.md`/`0037-deques.md`/
`0038-queues.md`'s own "2026 Update" sections), `PriorityQueue<T>` is now a real, user-declared
generic struct with a generic inherent `impl` block, living in `std/collections.ax`, **composed
directly on top of the real `List<T>`** — the same pattern `Stack<T>` used, since `push`/`pop`/
`peek` were `Stack<T>`'s own method names before this port, and `PriorityQueue<T>` was the sole
remaining user of them once `Stack<T>` itself became real.

```ax
struct PriorityQueue<T>
{
    List<T> items
}
```

Unlike every earlier collection in this series, `push`/`pop` here can't be thin forwarding calls
onto `self.items`'s own methods — a binary heap's sift-up/sift-down is a genuine algorithm, and
this port's one real risk factor (confirmed via research before implementing) was whether `<`
generalizes correctly to a generic `T` post-monomorphization. It does, with no new language
feature needed: `TypeChecker`'s `Less`/`LessEqual`/`Greater`/`GreaterEqual` checking already calls
the same shared `requireOrdered`/`isOrderableKind` path every plain `x < y` in the language uses,
and `GenericMonomorphizer` substitutes `T` to a concrete type *before* `TypeChecker`/
`LlvmIrEmitter` ever see the cloned method body — so an ordinary `if self.items.get(i) <
self.items.get(parent) { ... }` inside `impl<T> PriorityQueue<T>` just works, going through the
exact same `icmp`/`fcmp`/`@axea.less.str` infrastructure `SortedMap<K,V>`/`SortedSet<T>` already
share.

```ax
use collections

q: PriorityQueue<i32> = collections.newPriorityQueue<i32>()
q.push(30)
q.push(10)
smallest = q.peek()   // 10 - doesn't remove
first = q.pop()       // 10 - removes, always the smallest element present
count = q.length()    // was: q.length
```

**What changed and why:**

- **Construction**: `PriorityQueue<i32>()` call-style sugar →
  `collections.newPriorityQueue<i32>()`, an ordinary (generic) function call - identical reasoning
  to every earlier port's own construction change.
- **`.length` is now a method, `.length()`, not a bare field** - mirrors `Stack<T>.length()`'s own
  precedent exactly, for the identical reason: `PriorityQueue<T>` composes over an internal
  `items: List<T>` field, and this language has no computed-property/field-delegation syntax to
  expose `self.items.length` as a bare `q.length`.
- **`push`/`pop`/`peek` need no syntax change** - they were already ordinary method calls, now
  reaching `PriorityQueue<T>`'s own real `impl` block (hand-written sift-up/sift-down, composed
  over `self.items`'s own `push`/`pop`/`get`/`set`) via the general struct method dispatch, rather
  than the retired `IrPriorityQueuePush`/`IrPriorityQueuePop`/`IrPriorityQueuePeek` intrinsic
  instructions. `IrGenerator`'s own `isPriorityQueueExpr` disambiguation resolver (needed because
  `push`/`pop`/`peek` collided with `Stack<T>`'s own method names, back when `Stack<T>` was still
  intrinsic too) is now fully deleted, mirroring exactly what happened to `isStackExpr` during
  `Stack<T>`'s own retirement.
- **Two accepted, honest tradeoffs, not silently swept under the rug:**
  1. **The "`T` must be orderable" restriction below is no longer enforced eagerly** at
     `PriorityQueue<elem>` type-resolution time, with the dedicated error message this document
     originally described. `PriorityQueue<bool>` now parses/resolves fine as an ordinary generic
     struct reference — the failure instead surfaces from within the monomorphized `push`/sift
     body's own `<` comparison (a generic "comparison requires two orderable values" error, not
     the original dedicated message) — but it still surfaces immediately, at compile time, on any
     `PriorityQueue<bool>` reference, since `GenericMonomorphizer` eagerly clones every `impl`
     method the moment a struct instantiation is referenced, not lazily per call. Same tradeoff
     every earlier port in this series already accepted for its own retired dedicated checks.
  2. **`.peek()` on an empty queue no longer throws**, unlike `.push()`/`.pop()` (both still
     correctly inherit `List<T>`'s own established bounds behavior - `pop()` calls
     `self.items.pop()`, which throws on empty via `List<T>`'s own `length - 1` trick, *before*
     touching the popped queue's own root). `Stack<T>.peek()` reads index `length - 1`, which
     naturally lands out of bounds (and throws) on an empty stack; a heap's root is *always* index
     0 regardless of length, so `PriorityQueue<T>.peek()`'s `self.items.get(0)` stays in-bounds
     even when empty, reading whatever value happens to sit in `List<T>`'s own eagerly-allocated
     1-slot buffer instead of throwing. A genuine, narrow behavioral gap from the retired
     intrinsic.
- A real, previously-undiscovered compiler bug was found and fixed while implementing this
  port's sift loops (a mutation inside an `if`/`else` branch was silently lost in compiled code,
  never reconciled back into the enclosing scope - see `docs/language/0021-axea-ir.md`'s own
  "2026 Update" section for the full story), unrelated to `PriorityQueue<T>` specifically but
  directly blocking its design until fixed.
- See `examples/priority_queue.ax` for a worked example (verified both interpreted and compiled,
  `-O0` and `-O1`, for `i32`/`char`/`str` element types); everything below this section documents
  the *original compiler-intrinsic design* (now retired) for historical context.

---

# Motivation

`docs/language/0029-collections.md` lists `PriorityQueue<T>` in the `Queues`
category and gives it a `push(x)`/`pop() -> T`/`peek() -> T` sketch backed by
"a binary heap," with `O(log n)` push/pop and `O(1)` peek. Every collection
built so far this session (`Stack<T>`, `Queue<T>`, and to a lesser extent
`Deque<T>`) turned out to be "the same representation, different vocabulary"
- a thin renaming layer over `List<T>`/`Deque<T>`'s own existing push/pop
machinery, needing zero new algorithmic codegen. `PriorityQueue<T>` breaks
that streak: a binary heap's `push`/`pop` genuinely reorder elements
(sift-up/sift-down), which no earlier collection's LLVM backend needed to
express as a loop with real comparisons and swaps.

```ax
q = PriorityQueue<i32>()
q.push(30)
q.push(10)
q.push(20)

smallest = q.peek()  // 10 - doesn't remove
first = q.pop()      // 10 - removes, always the smallest element present
count = q.length
```

**Scope restriction, and why: `T` is `i32`, `i64`, `f64`, `char`, or `str`
only this phase.** `0029`'s own sketch shows `PriorityQueue<Job>(by:
.priority)` - a property-selector closure choosing what to compare - and a
separate `order: asc`/`desc` argument. Neither exists anywhere in this
codebase: there is no closure/property-selector syntax, no named/keyword
call arguments, and - the more fundamental gap - `<`/`<=`/`>`/`>=`
(`TypeChecker::requireOrdered`, backed by `isOrderableKind`) only ever
typecheck for those five kinds (`char`'s own natural order is by Unicode
codepoint - see `docs/language/0044-char.md`; `str`'s own is real
lexicographic byte order via LlvmIrEmitter's `registerOrderRuntime`/
`@axea.less.str`, a hand-rolled byte-walk compare - not a bare pointer
comparison; `i64`/`f64` are `docs/language/0051-numeric-widening.md`'s own
addition, `f64` via `fcmp`'s *ordered* predicates). No other declared
numeric width is comparable at all yet (`i8`/`i16`/`i128`, every unsigned
width, `f32` - see `docs/language/0005-type-system.md`) - and the *owned*
`String` type is deliberately excluded too, even though it's str-coercible
everywhere else in this language: ordering, like `Set<T>`/`Map<K,V>`'s own
hashability, only ever considers the bare value type (`isOrderableKind`
checks `TypeKind::String`, i.e. `str`, not `TypeKind::OwnedString`). A heap
has no correctness story without a total order on its elements, so rather
than accept an uncomparable `T` and produce nonsense, `PriorityQueue<T>`
requires
`isOrderableKind(elementType.kind)`, enforced in `resolveType` exactly the
way `Set<T>`/`Map<K,V>` already enforce hashability there (not a
nested-type structural rejection like `List`/`Stack`/`Deque`/`Queue`'s own
copy-pasted block - this is a real semantic constraint, so it gets the same
treatment `isHashable` already established for "some types don't qualify,
and the reason is domain-specific, not just nesting depth"). No
`by:`/`order:` constructor arguments this phase - `PriorityQueue<i32>()`
takes no arguments, exactly like every other collection's constructor
here.

**Ordering, and why: an ascending min-heap - `pop()` always returns the
smallest element present.** `0029` doesn't pin down a default (it only shows
`order: asc` as an explicit override example, implying *some* unstated
default exists), so this phase picks one and documents the choice rather
than leaving it ambiguous: smallest-first, matching Python's `heapq` and
most textbook presentations of "priority queue" as a min-heap by default
(C++'s own `std::priority_queue`/Rust's `BinaryHeap` default the other way -
this is a real, deliberate pick, not an obvious one).

**API**: `push(x)`, `pop() -> T` (removes and returns the current minimum),
`peek() -> T` (reads the current minimum without removing), `.length`. Same
"interpreter checks, compiled code doesn't" precedent as every other
collection's `pop`/`peek` here: `pop()`/`peek()` throw on empty in the
interpreter, no bounds check in compiled code. No `[i]`/`for`-in - same
"communicate intent" restriction `Stack<T>`/`Queue<T>` already accept; a
heap's internal array order isn't even a *meaningful* order to expose (it's
neither insertion order nor sorted order), which makes withholding indexing
here an even easier call than it was for them. Not hashable, not a struct
field type, not a `Map`/`Set` key type - mirrors `Queue<T>`/`Deque<T>`'s own
identical choices; nothing in scope calls for any of them.

**The real wrinkle**: `push`/`pop`/`peek` are the *same three method names*
`Stack<T>` already claims (and `push`/`pop` are also `List<T>`'s own). Every
earlier "shared method name" collision in this codebase was two-way
(`List`-vs-`Stack`, `LinkedList`-vs-`Deque`, `Map`-vs-`Set`); this is the
first *three*-way collision. Resolved the same way regardless: `IrGenerator`
gets a new `isPriorityQueueExpr` resolver (a sibling of `isStackExpr`, not a
generalization of it - this codebase's established "each pass re-derives
independently" convention), consulted *before* falling back to
`isStackExpr`'s own two-way check at each of the three call sites.

---

# Design: A List-Identical Header, a Genuinely New Push/Pop Body

A `PriorityQueue<T>` value is a stable pointer to the *exact same* two-field
heap header `List<T>`/`Stack<T>` already use - `{i32 length, T* data}*` -
storing the heap's elements in the classic *implicit* binary-tree layout
(node `i` at index `i`; its children live at `2i+1`/`2i+2`; its parent at
`(i-1)/2`, integer division). `llvmType("PriorityQueue<T>")` produces the
literal same text `llvmType("List<T>")`/`llvmType("Stack<T>")` do for the
same `T` - so, exactly like `Stack<T>` before it, `.length`'s field-get and
the top-level runtime print loop both handle a `PriorityQueue<T>` value
correctly via `isListType`'s own existing structural check, with **zero new
`LlvmIrEmitter` predicate**. Printing shows the heap's raw internal array
order (bracket format, matching every other collection here) - **not**
sorted order; this is documented as a known, deliberate imprecision below,
not a bug.

**What's genuinely new**: `push`/`pop` can't be copied from
`emitListPush`/`emitListPop` the way `emitStackPush`/`emitStackPop` were.
`push` still calls the same shared `ensureListCapacity` helper
`List<T>.push`/`Stack<T>.push` use first (doubling capacity, not
reallocating to exactly `length+1` - see `docs/language/0033-lists.md`),
appends the new element, but then must **sift the newly appended element
up** toward the root until the heap property (`parent <= child`) holds
again:

```text
idx := length - 1        // the just-appended element's index
while idx != 0:
    parent := (idx - 1) / 2
    if data[parent] <= data[idx]: break     // heap property restored
    swap(data[parent], data[idx])
    idx := parent
```

`pop` moves the last element into the vacated root slot, shrinks the length,
then **sifts that element down** until the heap property holds again:

```text
data[0] := data[length - 1]; length -= 1
idx := 0
loop:
    left := 2*idx + 1
    if left >= length: break                // no children left
    smallest := left
    if left+1 < length and data[left+1] < data[smallest]: smallest := left+1
    if data[smallest] >= data[idx]: break    // heap property restored
    swap(data[idx], data[smallest])
    idx := smallest
```

Both loops are emitted as hand-rolled `alloca`/`load`/`store`-counter LLVM
basic blocks (no `phi`) - the same convention every loop in this backend
already uses (see `emitListPush`'s own copy loop and
`docs/language/0028-loops.md`), just with real `icmp slt`/`icmp sle`
comparisons and swaps in the loop body instead of a pure copy. `pop`'s
sift-down needs one more block than `push`'s sift-up (choosing the smaller
of two children before comparing against the parent), but neither loop
needs a `phi` node: every value either lives in a register that dominates
every block that reads it (the loop bound, `length`), or is re-read from an
`alloca` slot (`idx`, `smallest`) at the top of whichever block needs it -
exactly the same trick that let `emitDequePushFront`/`emitLinkedListPushFront`
avoid `phi` for their own multi-predecessor merges.

`emitPriorityQueuePeek` needs no loop at all - the minimum is always at
index `0` by the heap invariant, so it's a direct `GEP`+`load`, simpler even
than `emitStackPeek` (which has to compute `length - 1` first).

---

# Parsing: A Direct Copy of the Established Two Forms

`PriorityQueue<elem>` in type position and `PriorityQueue<elem>()` in
expression position both mirror every prior collection's own branches
exactly. New `PriorityQueueNewExpr` AST node, fielded identically to
`StackNewExpr`. Method calls need no parser changes at all - `object.method(args)`
already parses generically regardless of which collection `object` turns out
to be.

```text
$ ax ast examples/priority_queue.ax
Function(build)
  Block
    Assignment(jobs)
      PriorityQueueNew(i32)
    ExprStmt
      MethodCall(push)
        Name(jobs)
        Integer(30)
    ...
```

---

# Type Checking: A Real Semantic Restriction, Not a Structural One

`TypeKind::PriorityQueue` is new, reusing the flat `elementKind`/
`elementStructName` fields exactly like `List`/`Stack`/`Deque`/`Queue`
already do (a single type parameter needs no `Map`/`Set`-style
`elementTypeName` string). Unlike those four, though, `resolveType`'s
`"PriorityQueue<elem>"` branch does **not** copy their nested-array/slice/
List/Stack/... rejection block - it instead requires
`isOrderableKind(elementType.kind)` directly (mirroring how `Set<T>`/
`Map<K,V>`'s own branches call `isHashable` rather than a structural
nesting check), since that single condition already excludes every
non-orderable type there is, nested or not - a *stronger*, more precise
restriction than the copy-pasted block would have given for free.
`isOrderableKind` accepts `i32`/`i64` (numeric order), `f64` (numeric
order via `fcmp`'s *ordered* predicates - see
docs/language/0051-numeric-widening.md), `char` (codepoint order - see
docs/language/0044-char.md), and `str` (real lexicographic byte order via
`registerOrderRuntime`/`@axea.less.str` - see docs/language/0042-string.md);
the *owned* `String` type is deliberately excluded even though it's
str-coercible everywhere else, since ordering only ever considers the bare
value type (same reasoning `isHashable` already established for
`Set<T>`/`Map<K,V>`'s own key):

```text
$ ax capabilities bad.ax   # f() { q = PriorityQueue<bool>() }
error: PriorityQueue<T> requires an orderable element type (i32, i64, f64,
char, or str only in this phase - no other type is comparable yet), found
PriorityQueue<bool>
```

`PriorityQueueNewExpr`'s own `checkExpr` case delegates straight to
`resolveType("PriorityQueue<" + ... + ">")`, reusing that single check
rather than duplicating it - the same "delegate to resolveType for anything
with real semantic validation" choice `MapNewExpr`/`SetNewExpr` already
made, diverging from `List`/`Stack`/`Deque`/`Queue`'s own construction-site
duplication (which exists only because their restriction is purely
structural and needs no shared helper).

`Queue<T>`'s own precedent - "not `isIndexable`, `.length` gets a standalone
`checkFieldType` case" - carries over unchanged. `MethodCallExpr` gets its
own `TypeKind::PriorityQueue` case: `push` (1 arg matching `elementType`,
returns unit), `pop`/`peek` (0 arguments, return `elementType`).

---

# Capability Checking: Zero Changes, Again

`"push"`/`"pop"` are already in `CapabilityChecker`'s flat, method-name-only
write-raising list (added for `List<T>` back in `0033`) - so
`PriorityQueue<T>.push`/`.pop` are covered automatically, with no code
change at all, the same free ride `Stack<T>.push`/`.pop` already got.
`"peek"` needs no addition either: every prior read-only method already
required none, since a touched-but-never-written parameter defaults to
`Capability::Read`.

---

# Region Checking: The Simplest Aliasing Story Yet, By Construction

`isPriorityQueueTypeString`/`priorityQueueElementTypeName` mirror
`isStackTypeString`/`stackElementTypeName` exactly, and
`PriorityQueueNewExpr` gets the same "brand-new, always `Owned`"
`regionOfExpr` case every other collection's constructor does.

`peek()`'s existing exception (`methodCall->method == "get" || methodCall->method
== "peek"`, added for `Map<K,V>.get()`/`Stack<T>.peek()`) already covers
`PriorityQueue<T>.peek()` for free, purely because it's a literal string
match on the method name `"peek"`, not an object-type check. In practice
this exception is **unreachable** for `PriorityQueue<T>` specifically: since
`T` is restricted to `i32` this phase, `elementStructType` is always empty
for a `PriorityQueue<i32>`, so the exception's own guard
(`!objectInfo.elementStructType.empty()`) never fires and `peek()` always
falls through to the ordinary `Owned` default anyway. Worth stating
precisely rather than leaving implicit: **`PriorityQueue<T>` is, today, the
one collection in this codebase with no aliasing risk through its own
element type at all** - a direct, provable consequence of the `i32`-only
restriction above, not a coincidence.

---

# `IrGenerator`: The First Three-Way Method-Name Collision

New `IrPriorityQueueNew`/`IrPriorityQueuePush`/`IrPriorityQueuePop`/
`IrPriorityQueuePeek` instructions, fielded identically to their `IrStack*`
counterparts. `isPriorityQueueExpr` is a new resolver with `isStackExpr`'s
exact shape (literal `PriorityQueueNewExpr` vs. `StackNewExpr`/`ListNewExpr`,
a parameter type prefix check, a call's return-type prefix check, or a name
recorded in `IrScope`'s own new parallel `isPriorityQueue` map) - a sibling,
not a generalization, per this codebase's established "each pass re-derives
independently" rule (see `docs/language/0035-stacks.md`'s own identical
framing for `isStackExpr` itself).

`lowerExpr`'s `MethodCallExpr` case now checks `priorityQueueKind` *before*
falling back to `stackKind` at all three call sites: `push`/`pop` (already
two-way between `List`/`Stack`, now three-way) and - the one behavioral
change to existing code - `peek`, which used to be dispatched unconditionally
to `IrStackPeek` on the (accurate, at the time) grounds that "only `Stack<T>`
has peek." That comment is no longer true, so `peek` now goes through the
same `priorityQueueKind.value_or(false)` check `push`/`pop` already use.

```text
$ ax ir examples/priority_queue.ax
Function(build)
  Params:
  region.enter
  %0 = priorityqueue.new i32
  %1 = const.i32 30
  %2 = priorityqueue.push %0, %1
  ...
```

---

# `LlvmIrEmitter`: One New Predicate-Free Header, Two New Loops

`llvmType("PriorityQueue<T>")` returns `"{i32, " + llvmType(T) + "*}*"` -
the literal same text `llvmType("List<T>")`/`llvmType("Stack<T>")` produce -
so no `isPriorityQueueType` predicate exists anywhere, exactly like
`Stack<T>` before it: `isListType`'s existing structural check already
matches a `PriorityQueue<T>` header by construction.

`emitPriorityQueueNew` is a direct copy of `emitStackNew`/`emitListNew`.
`emitPriorityQueuePush` copies `emitStackPush`'s own malloc-and-copy-loop
verbatim, then appends a sift-up loop (see Design above) operating on the
freshly grown buffer before the new length/data are stored back into the
header. `emitPriorityQueuePop` reads the root (the value ultimately
returned), moves the last element into the root slot, decrements the
length, then runs a sift-down loop before returning the value read at the
very start. `emitPriorityQueuePeek` is a single `GEP`+`load` at index `0` -
no loop, no bounds check, simpler than `emitStackPeek`.

```text
$ ax llvm-ir useQueue.ax   # useQueue(q: PriorityQueue<i32>) -> i32 { return q.length }
define i32 @useQueue({i32, i32*}* %0) {
```

No `IndexGet`/`IndexSet`/print-loop changes needed anywhere, for the same
reason `Queue<T>` needed none: `PriorityQueue<T>` never reaches those code
paths (`TypeChecker` never treats it as indexable), and top-level printing
already works via the existing `isListType`-gated branch.

---

# Interpreter: A Real `std::push_heap`/`std::pop_heap`, Not a Renamed `vector`

```cpp
struct PriorityQueueInstance { std::vector<Value> elements; };
```

Unlike every earlier collection's interpreter representation (a bare
`vector`/`deque` standing in for whatever the LLVM backend hand-rolls),
`PriorityQueueInstance` is the first one where the interpreter needs to
implement the *same algorithm* the compiled backend does, not just "the
obvious STL container." `push`/`pop` use `std::push_heap`/`std::pop_heap`
with a `std::greater`-style comparator (STL heaps are max-heaps by default;
reversing the comparator turns "largest first" into "smallest first,"
matching this phase's ascending-min-heap choice) over `elements`, comparing
each pair's `std::int64_t` payload directly (`T` is always `i32` this
phase, so no generic `Value` comparator is needed). `pop()`/`peek()` throw
on empty, matching every other collection's identical precedent. `toString`
uses the same bracket format `List`/`Stack`/`Deque`/`Queue` all use, showing
the heap's raw array order - **not** sorted order, matching the LLVM
backend's own identical choice (see Design above).

---

# Worked Example

`examples/priority_queue.ax`:

```ax
PriorityQueue<i32> build()
{
    jobs = PriorityQueue<i32>()
    jobs.push(30)
    jobs.push(10)
    jobs.push(20)
    return jobs
}

void pushOne(PriorityQueue<i32> jobs)
{
    jobs.push(5)
}

i32 drain(PriorityQueue<i32> jobs)
{
    total = 0
    while jobs.length > 0
    {
        total = total + jobs.pop()
    }
    return total
}

jobs = build()
lengthAfterBuild = jobs.length
smallest = jobs.peek()
called = pushOne(jobs)
lengthAfterPush = jobs.length
smallestAfterPush = jobs.peek()
total = drain(jobs)
lengthAfterDrain = jobs.length
```

```text
$ ax run examples/priority_queue.ax
jobs = []
lengthAfterBuild = 3
smallest = 10
called = ()
lengthAfterPush = 4
smallestAfterPush = 5
total = 65
lengthAfterDrain = 0
$ ax llvm-ir examples/priority_queue.ax | clang -x ir -O1 - -o out && ./out
# byte-for-byte identical (also re-verified at -O0)
```

`smallest = 10` confirms `peek()` reads the minimum regardless of push
order (`10` was pushed second, after `30`); `pushOne` writes through the
caller's own heap via `write`-capability `push`, and `smallestAfterPush = 5`
confirms the newly pushed, smaller element correctly sifted all the way to
the root; `drain`'s `while jobs.length > 0` loop pops every element in
ascending order (`5 + 10 + 20 + 30 = 65`), leaving the heap empty. `jobs`
itself prints as `[]`, not `[10, 30, 20]` - top-level bindings print only
once the whole program (including `drain`'s own full pop loop) has
finished, exactly like `examples/stack.ax`/`examples/queue.ax`'s own
identical final-state printing; the heap-order-not-sorted-order distinction
(see Design above) is real, but this particular example only ever observes
`jobs` after it's already empty.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **`T` is `i32`, `i64`, `f64`, `char`, or `str` only.** No `by:`/`order:`
  selector, no `Ordered` trait - neither closures/property-selectors nor
  comparability for any other type exist anywhere in this codebase yet
  (see Motivation above and `docs/language/0051-numeric-widening.md`).
- **`push` is `O(log n)` amortized**, not worst-case: the sift-up itself is
  always `O(log n)`, and `push` now shares `List<T>`/`Stack<T>`'s own
  amortized-doubling growth (`ensureListCapacity` - see
  `docs/language/0033-lists.md`) rather than reallocating on every call, so
  the growth step is `O(1)` amortized too.
- **No `Optional`-based safe pop/peek.** `pop()`/`peek()` throw on empty in
  the interpreter, no bounds check in compiled code - `Optional`/`T?` still
  has no support anywhere in this codebase (same precedent every other
  collection's pop/peek here already follows).
- **No `[i]`/`for`-in**, deliberately - "communicate intent," the same
  restriction `Stack<T>`/`Queue<T>` already accept, made easier here by the
  fact that the heap's internal order isn't a meaningful one to expose in
  the first place.
- **Printing shows heap order, not sorted order.** `[10, 30, 20]`, not
  `[10, 20, 30]` - matches the actual backing array in both the interpreter
  and the compiled binary, not a "nicer" re-sorted view.
- **`PriorityQueue<T>` is not a valid `Map`/`Set` key type, and cannot be a
  struct field type.** Mirrors `Queue<T>`/`Deque<T>`'s own deliberate
  choices.
- **`push`/`pop`/`peek` are compiler intrinsics, not real methods**, same as
  every other collection here.

---

# Guiding Rule

> Every collection built earlier this session was cheap because it was
> genuinely "the same representation, different vocabulary" - the work was
> in *finding* the right earlier phase to replay, not in inventing new
> codegen. `PriorityQueue<T>` is the first one where that pattern only holds
> for half the feature: the *header* really is `List<T>`'s own, for free,
> but `push`/`pop`'s *bodies* are not `emitListPush`/`emitListPop` with a
> new name - they need a real loop with real comparisons, the first of its
> kind in this backend. Recognizing *which half* of a new feature is a
> replay and which half is genuinely new work is what kept this phase
> honest: reusing the header shape saved real effort (free `.length`, free
> printing, free capability inference), while budgeting real design time for
> sift-up/sift-down instead of hoping they'd turn out to be another
> copy-paste job.
