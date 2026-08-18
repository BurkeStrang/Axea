# `Queue<T>`: `Stack<T>`'s Own Pattern, Backed by `Deque<T>` Instead of `List<T>`

**Status:** Implemented
**Document:** `0038-queues.md`

---

# Motivation

`docs/language/0029-collections.md` specs `Queue<T>` explicitly: "A FIFO collection backed internally by `Deque<T>`," with `enqueue(x)`/`dequeue() -> T`. With `Deque<T>` just finished, `Queue<T>` is the natural next step in the "Recommended Hierarchy" and, like `Stack<T>` was for `List<T>`, the cheapest remaining collection: it needs no new representation at all, just two new method names mapped onto operations `Deque<T>` already has.

```ax
jobs = Queue<Job>()
jobs.enqueue(job1)
jobs.enqueue(job2)
job = jobs.dequeue()   // job1 - classic FIFO: add at the back, remove from the front
```

**This phase is `Stack<T>`'s own phase, replayed with `Deque<T>` as the backing type instead of `List<T>`** — and it's simpler in one real way: `Stack<T>`'s `push`/`pop` collided with `List<T>`'s own method names, needing the `isStackExpr` disambiguation resolver in `IrGenerator`. `Queue<T>`'s `enqueue`/`dequeue` are **brand-new names nothing else in the language uses** — no collision, no resolver needed anywhere.

**The key realization, mirroring `Stack<T>`'s own**: `llvmType("Queue<T>")` produces the *exact same text* `llvmType("Deque<T>")` does — `"{i32, i32, " + llvmType(T) + "*}*"` — since `Queue<T>` is genuinely backed by `Deque<T>`'s own representation, not just similarly shaped. Consequence: **no `isQueueType` predicate exists anywhere** — `isDequeType`'s existing structural check already matches a `Queue<T>` header by construction, so `.length`'s field-get and top-level printing both handle `Queue<T>` correctly with zero new `LlvmIrEmitter` branches — not a coincidence, a direct consequence of the two types being LLVM-identical, exactly like `Stack<T>`/`List<T>` before them.

**Deliberate scope restriction, mirroring `Stack<T>`'s own choice**: even though `Deque<T>`'s indexing machinery is right there and would be "free" to expose, `Queue<T>` deliberately does **not** get `[i]`/`for`-in. `0029`'s own Guiding Principle says `Stack<T>` and `Queue<T>` exist specifically "to communicate intent" — `Stack<T>` already turned down indexing for exactly this reason despite being LLVM-identical to indexable `List<T>`; `Queue<T>` makes the same call relative to indexable `Deque<T>`, for the same reason.

**Scope**: `enqueue(x)`, `dequeue() -> T`, `.length`. No `peek`/safe-dequeue-with-`Optional` (`0029`'s own sketch shows `if job = queue.dequeue()`, but `Optional`/`T?` still has zero support anywhere in this codebase — `dequeue()` throws on empty in the interpreter, no bounds check in compiled code, matching `List<T>.pop()`/`Deque<T>.pop_front()`'s own established precedent). Not hashable (mirrors `LinkedList<T>`/`Deque<T>`'s own deliberate choice, not `Stack<T>`'s earlier one — nothing in scope calls for it).

---

# Design: The Simplest Region-Checking Story of Any Collection This Session

`Queue<T>` is a stable pointer to the same anonymous 3-field header `Deque<T>` already has — `{i32 count, i32 start, T* data}*` — so it inherits every representation decision `0037-deques.md` already made and verified: no ring-buffer wraparound, `push`-side reallocation, `pop`-side pure arithmetic. `enqueue` maps onto `Deque<T>.push_back` (add at the back); `dequeue` maps onto `Deque<T>.pop_front` (remove from the front) — the composition that makes it genuinely FIFO rather than `Deque<T>`'s own LIFO-or-FIFO-your-choice flexibility.

Because `dequeue()` always removes and there's no `[i]`/peek at all, the "does this alias the container" question that `Map<K,V>.get()`/`Stack<T>.peek()`/`Deque<T>[i]` each needed dedicated `RegionChecker` handling for **never arises here** — the simplest region-checking story of any collection built this session.

---

# Parsing: A Direct Copy of the Established Two Forms

`Queue<elem>` in type position and `Queue<elem>()` in expression position mirror every prior collection's own branches exactly. New `QueueNewExpr` AST node, fielded identically to `DequeNewExpr`.

```text
$ ax ast examples/queue.ax
Function(build)
  Block
    Assignment(jobs)
      QueueNew(i32)
    ExprStmt
      MethodCall(enqueue)
        Name(jobs)
        Integer(10)
    ...
```

---

# Type Checking: A Standalone `.length` Case, Not `isIndexable`

`TypeKind::Queue` is new, flat `elementKind`/`elementStructName`. `resolveType`'s `"Queue<elem>"` branch mirrors the established one-level nesting restriction. `Queue<T>` is **not** added to `isIndexable` — the deliberate "communicate intent" restriction — so `.length` gets its own standalone `checkFieldType` case, mirroring `Stack<T>`/`LinkedList<T>`'s own (not `Deque<T>`'s, which folds into the shared indexable path).

```text
$ ax capabilities bad.ax   # f() -> i32 { q = Queue<i32>()  q.enqueue(1)  return q[0] }
error: indexing into non-array/slice type Queue<i32>
```

`MethodCallExpr` gets its own `TypeKind::Queue` branch: `enqueue` (1 arg matching `elementType`, returns unit), `dequeue` (0 args, returns `elementType`).

---

# Capability Checking: Two New Names, Zero New Logic

`"enqueue"`, `"dequeue"` were simply added to the existing write-raising method-name list, alongside `push_front`/`push_back`/`pop_front`/`pop_back`/etc. — method-name-only, not object-type-aware:

```text
$ ax capabilities examples/queue.ax
Function(enqueueOne)
  Param(jobs: write)
Function(drain)
  Param(jobs: write)
```

---

# Region Checking: No `MethodCallExpr` Exception, No `elementStructType`

`isQueueTypeString` (mirrors `isStackTypeString`) is needed even though the LLVM type is shared with `Deque<T>`, because `RegionChecker` reasons about *Axea* type strings ("Queue<T>" vs "Deque<T>"), not LLVM shapes. `checkFunction`'s per-parameter heap-type detection and the return-type leak check both grow a `Queue` branch identical to `Stack`'s own — a borrowed `Queue<T>` parameter can't be returned whole without `take`:

```text
$ ax regions leak.ax   # leak(q: Queue<Job>) -> Queue<Job> { return q }
error: function 'leak' cannot return 'q': parameter 'q' is borrowed and does not outlive the call
- declare 'take' if ownership should transfer
```

Unlike `Deque<T>`, `Queue<T>` needs **no `elementStructType` extraction at all** — there's no `[i]`/peek whose result could alias the container, so `dequeue()`'s struct-typed result is always safely `Owned` under the default rule:

```ax
take_first(q: Queue<Point>) -> Point { return q.dequeue() }   # type-checks without `take`
```

---

# `IrGenerator`: No Disambiguation Resolver Needed At All

New `IrQueueNew`, `IrQueueEnqueue`, `IrQueueDequeue`. Unlike `Deque<T>`'s own `push_front`/`push_back`/`pop_front`/`pop_back` (which collide with `LinkedList<T>`'s identical method names, needing `isDequeExpr`), `enqueue`/`dequeue` are unique names nothing else in the language uses — so `lowerExpr`'s `MethodCallExpr` case dispatches directly by method name, exactly like `LinkedList<T>`'s own names did before `Deque<T>` introduced its own collision.

```text
$ ax ir examples/queue.ax
Function(build)
  Params:
  region.enter
  %0 = queue.new i32
  %1 = const.i32 10
  %2 = queue.enqueue %0, %1
  ...
```

---

# `Interpreter`: `std::deque`, Not `std::vector` — the Opposite Choice From `DequeInstance`

```cpp
struct QueueInstance { std::deque<Value> elements; };
```

`DequeInstance` deliberately uses `std::vector<Value>` so it can plug into `asIndexable`. `Queue<T>` has no indexing to support at all, so there's no reason to pay that cost — `std::deque` gives real O(1) `enqueue` (`push_back`)/`dequeue` (`pop_front`) directly, the same choice `LinkedListInstance` already made for the identical reason. `dequeue()` throws on empty, matching precedent. `toString` uses the same bracket format `List`/`Stack`/`Deque` all use — a queue's FIFO order is exactly as well-defined as theirs.

---

# `LlvmIrEmitter`: Zero New Predicates, Three New Emit Functions

`llvmType("Queue<T>")` returns `"{i32, i32, " + llvmType(T) + "*}*"` directly — the literal same text `llvmType("Deque<T>")` produces. `emitQueueNew`/`emitQueueEnqueue`/`emitQueueDequeue` are structurally identical to `emitDequeNew`/`emitDequePushBack`/`emitDequePopFront` (byte-for-byte copies of the malloc+GEP shapes, `emitQueueEnqueue` reusing the exact same hand-rolled copy-loop-offset-by-`start` helper `emitDequePushBack` uses), kept as separate functions per this codebase's consistent "separate over shared" convention — exactly like `emitStackPush`/`emitStackPop` were kept separate from `emitListPush`/`emitListPop` despite being structurally identical operations on the identical underlying type.

```text
$ ax llvm-ir useQueue.ax   # useQueue(q: Queue<i32>) -> i32 { return q.length }
define i32 @useQueue({i32, i32, i32*}* %0) {
```

No `IndexGet`/`IndexSet`/print-loop changes were needed anywhere: `Queue<T>` never reaches those code paths (not indexable, per `TypeChecker`'s own restriction), and top-level printing "just works" via the existing `isDequeType`-gated branch since the header text is identical to `Deque<T>`'s own.

---

# Worked Example

`examples/queue.ax`:

```ax
build() -> Queue<i32>
{
    jobs = Queue<i32>()
    jobs.enqueue(10)
    jobs.enqueue(20)
    jobs.enqueue(30)
    return jobs
}

enqueueOne(jobs: Queue<i32>)
{
    jobs.enqueue(99)
}

drain(jobs: Queue<i32>) -> i32
{
    total = 0
    while jobs.length > 0
    {
        total = total + jobs.dequeue()
    }
    return total
}

jobs = build()
lengthAfterBuild = jobs.length
first = jobs.dequeue()
lengthAfterDequeue = jobs.length
called = enqueueOne(jobs)
lengthAfterEnqueue = jobs.length
total = drain(jobs)
lengthAfterDrain = jobs.length
```

```text
$ ax run examples/queue.ax
jobs = []
lengthAfterBuild = 3
first = 10
lengthAfterDequeue = 2
called = ()
lengthAfterEnqueue = 3
total = 149
lengthAfterDrain = 0
$ ax llvm-ir examples/queue.ax | clang -x ir -O1 - -o out && ./out
# byte-for-byte identical (also re-verified at -O0)
```

`first = 10` confirms `dequeue()` removes the *first* enqueued element (`10`, not `30`) — genuine FIFO order, not `Deque<T>`'s own LIFO-capable flexibility. `enqueueOne` writes through the caller's own queue via `write`-capability `enqueue`, growing the queue back to length `3` (`lengthAfterEnqueue`). `drain`'s `while jobs.length > 0` loop dequeues every remaining element via `write`-capability `dequeue`, summing `20 + 30 + 99 = 149`, leaving the queue empty.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **`enqueue` is not actually O(1)**, inheriting `Deque<T>.push_back`'s own honest complexity shortfall (reallocates on every call) — the same gap between `0029`'s documented complexity and this codebase's deliberately simplified reality that `List<T>.push` and `Deque<T>`'s own pushes already have.
- **No `peek`/safe-dequeue-with-`Optional`.** `0029`'s own sketch shows `if job = queue.dequeue()`, but `Optional`/`T?` has zero support anywhere in this codebase.
- **No `[i]`/`for`-in**, deliberately — "communicate intent," the same restriction `Stack<T>` already accepts relative to indexable `List<T>`.
- **`Queue<T>` is not a valid `Map`/`Set` key type.** Mirrors `LinkedList<T>`/`Deque<T>`'s own deliberate choice.
- **`Queue<T>` cannot be a struct field type.** The same restriction every collection in this language has.
- **`enqueue`/`dequeue` are compiler intrinsics, not real methods**, same as every other collection here.

---

# Guiding Rule

> When a new collection is genuinely "the same representation, different vocabulary," the fastest way to scope it is to name which *earlier* phase it's replaying, and then look for where the replay diverges. `Queue<T>` replays `Stack<T>`'s own phase almost exactly - same "no isXType predicate needed" realization, same "communicate intent" indexing restriction, same "separate over shared" emit functions - but naming that parallel up front is what made the one real divergence obvious immediately, instead of requiring rediscovery: `enqueue`/`dequeue` don't collide with anything, so the disambiguation-resolver machinery `Stack<T>`'s `push`/`pop` needed simply doesn't apply here. Recognizing a pattern is only useful if you also verify where it stops holding, not just where it does.
