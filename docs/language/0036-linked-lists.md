# `LinkedList<T>`: The First Genuinely Node-Based Collection

**Status:** Implemented
**Document:** `0036-linked-lists.md`

---

# Motivation

`docs/language/0029-collections.md`'s "Recommended Hierarchy" lists `LinkedList<T>` right after `List<T>`/`slice<T>`, in its own `Linked` category (distinct from `Contiguous` and `Queues`). With `List<T>`, `slice<T>`, `Stack<T>`, and generic `Map<K,V>`/`Set<T>` all done, `LinkedList<T>` is the next collection in hierarchy order — and, unlike `Queue<T>` (spec'd as "backed internally by `Deque<T>`"), it doesn't depend on anything else being built first.

```ax
s = LinkedList<i32>()
s.push_front(1)
s.push_back(2)
front = s.pop_front()   // 1
back = s.pop_back()     // 2
count = s.length
```

This is a genuinely different kind of collection from everything built so far. `List<T>`/`Stack<T>` are contiguous arrays (an anonymous 2-field heap header). `Map<K,V>`/`Set<T>` are hash tables (a *named*, self-referential entry type walked via a chain). `LinkedList<T>` needs its own self-referential named node type too (`{value, prev, next}`) — but unlike a hash table's single-direction chain walk, `push_front`/`push_back`/`pop_front`/`pop_back` each need a real conditional branch (is the list currently empty?) to keep `head`/`tail` consistent. That branching need is exactly why `Map`/`Set`'s `set`/`get`/`contains`/`remove` are emitted as callable **template-text runtime functions** (`@axea.map.<id>.set`, etc. — real LLVM `define`s using named `%tokens`, filled in by string substitution) rather than inlined straight-line/loop-only code the way `List<T>.push`/`.pop` are: named LLVM registers don't have this backend's own "strictly increasing anonymous register" constraint, so real `br`-based control flow is far easier to hand-write as template text than as a hand-rolled `fctx.nextLabel++` sequence. `LinkedList<T>` reuses that exact same approach, for the exact same reason.

**Scope, deliberately kept tight**, matching every prior phase's "minimal necessary API" discipline: `push_front(x)`, `push_back(x)`, `pop_front() -> T`, `pop_back() -> T`, `.length`. No `peek_front`/`peek_back` — `0029`'s sketch doesn't show them, and skipping them sidesteps the whole "does this alias the container" `RegionChecker` question entirely: every operation either adds or removes, never peeks. No arbitrary-position insert/remove — `LinkedList`'s traditional strength, but real scope growth, deferred. No iteration: `for x in expr` (`Parser::parseFor`) desugars into `.length` + `expr[i]` indexing, a mechanism that assumes O(1) random access — fundamentally the wrong operation for a linked list, not just "not implemented yet" the way `Map`/`Set`'s iteration gap is — so top-level printing prints `LinkedList(N entries)` (count only), matching `Map`/`Set`'s own precedent, but for a stronger underlying reason.

---

# Design: A Named Node Type, Callable Runtime Functions, and Nothing Else New

A `LinkedList<T>` value is a stable pointer to a small anonymous 3-field heap header — `{i32 length, Node* head, Node* tail}*` — mirroring `List<T>`'s own "stable pointer, mutated in place" header, just with two link fields instead of one data pointer. `push_front`/`push_back`/`pop_front`/`pop_back` mutate the header's own fields (and one node's own `prev`/`next`) in place, so every existing alias sees updates for free — the same guarantee every other heap-allocated collection in this language already gives.

**The node type must be named**, exactly like `Map`/`Set`'s entry type: a node's own self-reference (`prev`/`next` pointing at another node of the same shape) can only be expressed in LLVM through a name, never an anonymous struct. Each distinct element type actually used gets its own numbered instantiation, registered lazily the first time `LlvmIrEmitter::llvmType` resolves that canonical `"LinkedList<T>"` string — mirroring `registerMapInstantiation`/`registerSetInstantiation`'s own lazy-registration-by-canonical-string pattern exactly:

```llvm
%axea.LLNode.0 = type { i32, %axea.LLNode.0*, %axea.LLNode.0* }
```

**Push/pop are template-text runtime functions, not inlined code** — the one place this phase's design genuinely had to choose between two established precedents. `List<T>.push`/`.pop` inline directly at each call site (`emitListPush`/`emitListPop`, straight-line code plus one loop, no conditional branching beyond the loop's own header check). `Map<K,V>.set`/`.get` are callable, pre-generated runtime functions using named `%registers` (`@axea.map.<id>.set`, filled in via `fillTemplate`), because their chain-walk logic needs real conditional branching this backend's numbered-anonymous-register convention can't express without a lot of hand-rolled label bookkeeping. `LinkedList<T>`'s push/pop need exactly the same thing: deciding whether the *opposite* end pointer also needs updating (is the list currently empty, for push; does it become empty, for pop) is a genuine `br i1`, not a loop — so `LinkedList<T>` follows `Map`/`Set`'s callable-runtime-function precedent, not `List<T>`'s inline one. `registerLinkedListInstantiation` emits `@axea.linkedlist.<id>.push_front/push_back/pop_front/pop_back` once per distinct element type, called via a single `call` instruction at every actual call site (`emitLinkedListPushFront` etc. — no logic of their own, exactly like `emitMapSet`/`emitMapGet`).

Every one of these four templates was hand-verified against real `clang` (`-O0` and `-O1`) in an isolated `.ll` file — a self-referential `%Node = type { i32, %Node*, %Node* }` header, all four operations, checking length before/after and the exact popped values — *before* being written into the real backend, matching this codebase's established rule for genuinely new LLVM-level ground (the same discipline `0034-maps-and-sets.md`'s byte-walk hashing and `List<T>`-key runtime loop were each held to).

`pop_front`/`pop_back` read the popped value before unlinking, matching `List<T>.pop()`/`Stack<T>.pop()`'s "no bounds check, interpreter checks instead" convention for the *empty* case — a null-head dereference crashes, exactly like every other out-of-bounds case in this backend. The one `br i1` each op contains exists purely to maintain the head/tail invariant on the empty↔non-empty transition, not as a defensive empty-check.

---

# Parsing: A Direct Copy of `List<T>`/`Stack<T>`'s Own Two Forms

`LinkedList<elem>` in type position and `LinkedList<elem>()` in expression position both mirror `List`/`Stack`'s own already-established branches exactly, including the `parseTypeName()`-recursive construction form. New `LinkedListNewExpr` AST node, fielded identically to `ListNewExpr`/`StackNewExpr`.

```text
$ ax ast examples/linked_list.ax
Function(build)
  Block
    Assignment(numbers)
      LinkedListNew(i32)
    ExprStmt
      MethodCall(push_back)
        Name(numbers)
        Integer(10)
    ...
```

---

# Type Checking: A New `TypeKind`, No New `Type` Fields

`TypeKind::LinkedList` is new; `Type` reuses the existing flat `elementKind`/`elementStructName` fields exactly like `List`/`Stack` already do — one type parameter, no K/V-style nesting to worry about, unlike `Map`/`Set`'s own `elementTypeName`/`valueTypeName` addition. `resolveType`'s `"LinkedList<elem>"` branch mirrors `List`/`Stack`'s own one-level nesting restriction (`Array`/`Slice`/`List`/`Stack`/`LinkedList` are all rejected as the element type).

`MethodCallExpr` gets its own `TypeKind::LinkedList` case: `push_front`/`push_back` take 1 argument matching `elementType` and return unit; `pop_front`/`pop_back` take 0 arguments and return `elementType` — the identical shape `List<T>.push`/`.pop` already have.

```text
$ ax capabilities field.ax   # struct Wrapper { items: LinkedList<i32> }
error: LinkedList<T> is not supported as a struct field type in this phase
```

`LinkedList<T>` is *not* hashable (unlike `List<T>`/`Stack<T>`, which fall into `isHashable`'s `Array`/`List` group) — left out deliberately this phase, since nothing in `0029`'s own sketch calls for it and adding it would mean writing a genuine `List<T>`-key-style runtime hash/equality loop for a type whose main use case (mutable, node-based) makes it a poor key candidate regardless.

---

# Capability Checking: Four New Method Names, Zero New Logic

`CapabilityChecker`'s write-raising is purely method-name-based — `push_front`, `push_back`, `pop_front`, `pop_back` were simply added to the existing list (alongside `push`, `pop`, `set`, `remove`, `add`), with no object-type awareness needed at all:

```text
$ ax capabilities examples/linked_list.ax
Function(pushBoth)
  Param(numbers: write)
Function(drain)
  Param(numbers: write)
```

---

# Region Checking: The First Collection Needing *No* Aliasing Exception

`RegionChecker`'s heap-type detection (`isLinkedListTypeString`, mirroring `isListTypeString`/`isStackTypeString`) and the return-type leak check both grow a `LinkedList` branch identical to `List`'s/`Stack`'s own — a borrowed `LinkedList<T>` parameter can't be returned whole without `take`:

```text
$ ax regions leak.ax   # leak(s: LinkedList<Point>) -> LinkedList<Point> { return s }
error: function 'leak' cannot return 's': parameter 's' is borrowed and does not outlive the
call - declare 'take' if ownership should transfer
```

But `regionOfExpr`'s `MethodCallExpr` case needed **zero changes** for `LinkedList<T>` — the first collection this session where that's true. `Map<K,V>.get()` and `Stack<T>.peek()` both needed a dedicated exception because they read a stored element *without* removing it, so a struct-typed result can alias the container. `LinkedList<T>` has no such operation this phase: `push_front`/`push_back` return nothing, and `pop_front`/`pop_back` always remove, so their result is always safely `Owned` under the *default* rule every method already gets, with no exception required. This is exactly why `peek_front`/`peek_back` were left out of scope — keeping them out means this whole phase needed no aliasing-exception work at all.

```ax
take_front(s: LinkedList<Point>) -> Point { return s.pop_front() }   # type-checks without `take`
```

---

# `IrGenerator`: The Simplest Method Dispatch of Any Collection This Session

New `IrLinkedListNew` (carries `elementTypeName`, mirrors `IrListNew`/`IrStackNew`), `IrLinkedListPushFront`, `IrLinkedListPushBack`, `IrLinkedListPopFront`, `IrLinkedListPopBack`. Unlike `Stack<T>`'s `push`/`pop` (which collide with `List<T>`'s own method names, needing a best-effort `isStackExpr` resolver), `push_front`/`push_back`/`pop_front`/`pop_back` are unique names nothing else in this language uses — so `lowerExpr`'s `MethodCallExpr` case dispatches directly by method name, no disambiguation machinery needed at all.

```text
$ ax ir examples/linked_list.ax
Function(build)
  Params:
  region.enter
  %0 = linkedlist.new i32
  %1 = const.i32 10
  %2 = linkedlist.push_back %0, %1
  ...
```

---

# `Interpreter`: `std::deque`, Not a Hand-Rolled Node Chain

```cpp
struct LinkedListInstance { std::deque<Value> elements; };
```

The interpreter's own representation doesn't need to mirror the LLVM backend's node-based layout — exactly like `Map`/`Set`'s interpreter uses `std::unordered_map`/`std::unordered_set` even though the compiled backend hand-rolls a chained hash table. `std::deque` gives O(1) `push_front`/`push_back`/`pop_front`/`pop_back` for free; `pop_front`/`pop_back` throw on empty, matching `List<T>.pop()`/`Stack<T>.pop()`'s own precedent.

`toString` deliberately does **not** use `List`/`Stack`'s bracket format, even though `std::deque` could trivially be walked to produce one — it must match `LlvmIrEmitter`'s own top-level printer byte-for-byte (this whole session's round-trip verification depends on it), and that printer stays count-only for the reasons above.

---

# `LlvmIrEmitter`: `Map`/`Set`'s Monomorphization Pattern, Not `List`/`Stack`'s Inline One

Already covered in detail under Design above. In brief: `llvmType("LinkedList<T>")` calls `registerLinkedListInstantiation`, which declares the named node type and the four `@axea.linkedlist.<id>.*` runtime functions (via the same `fillTemplate` substitution helper `Map`/`Set` already use — new tokens `<<NODE>>`/`<<NODEPTR>>`/`<<NODEPTRPTR>>` in place of `<<ENTRY>>`/`<<ENTRYPTR>>`/`<<ENTRYPTRPTR>>`). `isLinkedListType`/`linkedListInstantiationId` mirror `isMapType`/`isSetType`/`mapSetInstantiationId`, checked *before* `isListType` everywhere a `LinkedList<T>` header could otherwise spuriously match `isListType`'s own looser `"{...}*"` test (`.length`'s field-get, and top-level printing — the latter matters more here than it did for `Stack<T>`: unlike `Stack<T>`, whose header is the *literal same type* as `List<T>`'s, a `LinkedList<T>` header is genuinely differently shaped, so falling through to `List`'s own print loop would misread the head/tail node pointers as a data pointer and produce garbage, not just a cosmetic difference).

`emitLinkedListNew` is direct inline C++ emission (not template text) — construction is straight-line, no branching, so it needs none of the named-register machinery push/pop do. It mirrors `emitListNew`/`emitMapNew`'s malloc + null-GEP-sizeof idiom exactly, just with three fields to zero-initialize instead of two.

```text
$ ax llvm-ir f.ax   # f() { s = LinkedList<i32>()  s.push_front(1) }
%axea.LLNode.0 = type { i32, %axea.LLNode.0*, %axea.LLNode.0* }

define void @axea.linkedlist.0.push_front({i32, %axea.LLNode.0*, %axea.LLNode.0*}* %h, i32 %value) {
entry:
  %sizePtr = getelementptr %axea.LLNode.0, %axea.LLNode.0* null, i32 1
  %sizeInt = ptrtoint %axea.LLNode.0* %sizePtr to i64
  %raw = call i8* @malloc(i64 %sizeInt)
  %newNode = bitcast i8* %raw to %axea.LLNode.0*
  %vp = getelementptr %axea.LLNode.0, %axea.LLNode.0* %newNode, i32 0, i32 0
  store i32 %value, i32* %vp
  %hp = getelementptr {i32, %axea.LLNode.0*, %axea.LLNode.0*}, {i32, %axea.LLNode.0*, %axea.LLNode.0*}* %h, i32 0, i32 1
  %oldHead = load %axea.LLNode.0*, %axea.LLNode.0** %hp
  %pp = getelementptr %axea.LLNode.0, %axea.LLNode.0* %newNode, i32 0, i32 1
  store %axea.LLNode.0* null, %axea.LLNode.0** %pp
  %np = getelementptr %axea.LLNode.0, %axea.LLNode.0* %newNode, i32 0, i32 2
  store %axea.LLNode.0* %oldHead, %axea.LLNode.0** %np
  %isEmpty = icmp eq %axea.LLNode.0* %oldHead, null
  br i1 %isEmpty, label %emptycase, label %nonemptycase
emptycase:
  %tp = getelementptr {i32, %axea.LLNode.0*, %axea.LLNode.0*}, {i32, %axea.LLNode.0*, %axea.LLNode.0*}* %h, i32 0, i32 2
  store %axea.LLNode.0* %newNode, %axea.LLNode.0** %tp
  br label %merge
nonemptycase:
  %ohpp = getelementptr %axea.LLNode.0, %axea.LLNode.0* %oldHead, i32 0, i32 1
  store %axea.LLNode.0* %newNode, %axea.LLNode.0** %ohpp
  br label %merge
merge:
  store %axea.LLNode.0* %newNode, %axea.LLNode.0** %hp
  %lp = getelementptr {i32, %axea.LLNode.0*, %axea.LLNode.0*}, {i32, %axea.LLNode.0*, %axea.LLNode.0*}* %h, i32 0, i32 0
  %oldLen = load i32, i32* %lp
  %newLen = add i32 %oldLen, 1
  store i32 %newLen, i32* %lp
  ret void
}
```

---

# Worked Example

`examples/linked_list.ax`:

```ax
build() -> LinkedList<i32>
{
    numbers = LinkedList<i32>()
    numbers.push_back(10)
    numbers.push_back(20)
    numbers.push_front(5)
    return numbers
}

pushBoth(numbers: LinkedList<i32>)
{
    numbers.push_front(1)
    numbers.push_back(99)
}

drain(numbers: LinkedList<i32>) -> i32
{
    total = 0
    while numbers.length > 0
    {
        total = total + numbers.pop_front()
    }
    return total
}

numbers = build()
lengthAfterBuild = numbers.length
front = numbers.pop_front()
back = numbers.pop_back()
lengthAfterPops = numbers.length
called = pushBoth(numbers)
lengthAfterPushBoth = numbers.length
total = drain(numbers)
lengthAfterDrain = numbers.length
```

```text
$ ax run examples/linked_list.ax
numbers = LinkedList(0 entries)
lengthAfterBuild = 3
front = 5
back = 20
lengthAfterPops = 1
called = ()
lengthAfterPushBoth = 3
total = 110
lengthAfterDrain = 0
$ ax llvm-ir examples/linked_list.ax | clang -x ir -O1 - -o out && ./out
# byte-for-byte identical (also re-verified at -O0)
```

`lengthAfterBuild = 3` confirms `build()`'s `push_back(10)`, `push_back(20)`, `push_front(5)` produce `[5, 10, 20]` (front to back). `front = 5`/`back = 20` confirm `pop_front`/`pop_back` remove from opposite ends, leaving `[10]` (`lengthAfterPops = 1`). `pushBoth` writes through the caller's own list via `write`-capability `push_front`/`push_back`, producing `[1, 10, 99]` (`lengthAfterPushBoth = 3`). `drain`'s `while numbers.length > 0` loop pops every element via `write`-capability `pop_front`, summing to `1 + 10 + 99 = 110`, leaving the list empty (`lengthAfterDrain = 0`).

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **No `peek_front`/`peek_back`.** Every operation this phase either adds or removes, never aliases — keeping it that way is what let `RegionChecker` skip an aliasing exception entirely this phase.
- **No arbitrary-position insert/remove.** `LinkedList`'s traditional strength over `List<T>`, but real additional scope — deferred.
- **No `for`-in iteration**, and top-level printing falls back to `LinkedList(N entries)` rather than full contents — not merely "not implemented yet" the way `Map`/`Set`'s iteration gap is: `for`-in desugars into `[i]` indexing, which assumes O(1) random access, the one thing a linked list is specifically not for.
- **`LinkedList<T>` is not a valid `Map`/`Set` key type.** Left out deliberately (see Type Checking above) — nothing in scope calls for it, and a mutable node-based collection is a poor key candidate regardless.
- **`LinkedList<T>` cannot be a struct field type.** The same restriction every collection in this language has, kept out purely to bound scope, not a hard limit.
- **No amortized-anything concerns apply here** (unlike `List<T>`'s "no amortized growth" or `Map`/`Set`'s "no amortized shrink") — every `push`/`pop` is genuinely O(1), the one operation-complexity table entry in `0029` this phase satisfies exactly as specified.
- **`push_front`/`push_back`/`pop_front`/`pop_back` are compiler intrinsics, not real methods.** Same as every other collection here — there's no user-definable method/`impl` system in this language.

---

# Guiding Rule

> A collection's *representation* constraints, not its author's familiarity with linked lists, decide which existing precedent to reuse. `LinkedList<T>` looks nothing like `Map<K,V>`/`Set<T>` on the surface - no hashing, no buckets, no key/value pairs - but it shares the one property that actually matters for codegen: a self-referential node type that needs real conditional branching to maintain its own invariants. Recognizing that shared constraint - not the shared vocabulary of "collection" - is what pointed at `Map`/`Set`'s callable-runtime-function pattern instead of `List`/`Stack`'s inline one, and it's also what revealed the flip side: no aliasing exception was needed anywhere, not because it was overlooked, but because staying disciplined about scope (no peek, no iteration) meant the one situation that would have required it - reading a stored element without removing it - never arose in the first place.
