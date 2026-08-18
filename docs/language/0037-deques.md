# `Deque<T>`: A Growable Array With a Start Offset — Real Indexing, No Ring-Buffer Wraparound

**Status:** Implemented
**Document:** `0037-deques.md`

---

# Motivation

`docs/language/0029-collections.md` puts `Deque<T>` in its own `Queues` category (distinct from `Contiguous` and `Linked`), lists it in the "Recommended Hierarchy," and gives it a `Complexity Summary` row ("O(1) ends" for both add/remove, "insertion order"). Unlike `Stack<T>`/`Queue<T>`/`PriorityQueue<T>`, `0029` has **no dedicated `# Deque<T>` section with a worked code example** — its only textual role is as `Queue<T>`'s own stated backing store ("A FIFO collection backed internally by `Deque<T>`"). So this phase had to make one real design call `Stack<T>`/`LinkedList<T>` didn't need to: what does `Deque<T>` actually look like underneath.

```ax
d = Deque<i32>()
d.push_front(1)
d.push_back(2)
front = d.pop_front()   // 1
back = d.pop_back()     // 2
mid = d[5]               // real O(1) indexing - LinkedList<T> can't do this
count = d.length
```

**The design call, and why.** A real ring buffer (modular/wraparound index arithmetic on every access) is the textbook choice, but it's a substantial new codegen pattern for a marginal payoff. Instead: `Deque<T>` is a **growable array with a `start` offset** — header `{i32 count, i32 start, T* data}*` (anonymous, no named type needed — its third field is a plain `T*`, not a self-referential entry pointer the way `Map`/`Set`/`LinkedList`'s own third field is). `push_front`/`push_back` always reallocate to `count + 1` and copy the existing `count` elements out **unwrapped** — mirrors `List<T>.push`'s own "no amortized growth, reallocate every push" simplification exactly — writing the new element before or after the copied range, and resetting `start = 0` every time. `pop_front`/`pop_back` never reallocate (mirrors `List<T>.pop()`'s own precedent): `pop_front` just increments `start` and decrements `count`; `pop_back` just decrements `count`. Because every push resets `start` back to `0`, `start` only ever *increases* via `pop_front` and never wraps — so indexing is `data[start + i]`, a single `add` before the existing GEP pattern every array-like type already uses. No modular arithmetic, no ring-buffer edge cases, at the honest cost of the same complexity shortfall `List<T>.push` already accepts and documents (not true O(1), reallocates on every push).

**This unlocks real indexing "for free."** Unlike `LinkedList<T>` (deliberately no `[i]` — a node chain genuinely doesn't support O(1) random access), `Deque<T>`'s array-with-offset representation supports `[i]` *and* `[i] =` with only a `+start` tweak on every existing generic array/List code path — `TypeChecker::isIndexable`, `RegionChecker`'s `IndexExpr` aliasing (already fully generic, driven by `elementStructType`), and `IrGenerator`'s `IrIndexGet`/`IrIndexSet` lowering (already fully generic, no per-type dispatch at all) needed either a one-line addition or *zero* changes. That also means `for x in myDeque { ... }` (`Parser::parseFor`'s existing `.length` + `[i]` desugar) works immediately, with no Parser changes at all — genuinely differentiating `Deque<T>` from `LinkedList<T>`, matching `0029`'s own choice to keep them in separate categories.

**Scope, deliberately kept tight**, mirroring every prior phase: `push_front(x)`, `push_back(x)`, `pop_front() -> T`, `pop_back() -> T`, `.length`, `[i]` get/set, and (for free) `for`-in iteration. No `peek_front`/`peek_back` — same reasoning as `LinkedList<T>`: keeps `RegionChecker` free of any new aliasing-*exception* work, since `pop_front`/`pop_back` always remove (safely `Owned` under the default rule) and `[i]`'s aliasing is already handled generically by the existing array/List rule, not a new `MethodCallExpr`-specific case. Not made hashable — mirrors `LinkedList<T>`'s own deliberate choice: nothing in scope calls for it.

**The real wrinkle, caught by precedent**: `push_front`/`push_back`/`pop_front`/`pop_back` are the *same method names* `LinkedList<T>` already uses. `IrGenerator`'s `MethodCallExpr` lowering dispatches by name — this is `Stack`-vs-`List` recurring a second time, now as `Deque`-vs-`LinkedList`, solved the identical way: a best-effort `isDequeExpr` resolver mirroring `isStackExpr`'s exact shape.

---

# Design: An Anonymous Header, No Monomorphization At All

A `Deque<T>` value is a stable pointer to a small anonymous 3-field heap header — `{i32 count, i32 start, T* data}*` — mirroring `List<T>`'s own "stable pointer, mutated in place" header, just with an extra `start` field. This is the **cheapest header of any multi-field collection built this session**: unlike `Map`/`Set`/`LinkedList`, its third field is a plain `T*`, not a self-referential entry pointer, so it needs no named LLVM type and no per-instantiation registration at all — `llvmType("Deque<T>")` just returns `"{i32, i32, " + llvmType(T) + "*}*"` directly.

```text
$ ax llvm-ir useDeque.ax   # useDeque(d: Deque<i32>) -> i32 { return d.length }
define i32 @useDeque({i32, i32, i32*}* %0) {
```

**Distinguishing a `Deque<T>` header from `Map`/`Set`'s own 3-field header** needed one subtlety: a naive "ends with `**}*`" check (double-pointer, matching `Map`/`Set`'s own `EntryPtrPtr` field) would misfire on `Deque<str>` — `str` is itself `"i8*"`, so `Deque<str>`'s own data field is `"i8**"`, the same double-star suffix. `isDequeType` instead excludes the `"%axea."` prefix explicitly (`Map`/`Set`'s own headers always reference a named entry type), which is correct regardless of what `T` is. Checked *before* `isListType` everywhere a `Deque<T>` header could otherwise spuriously match `isListType`'s own looser `"{...}*"` test — and here it matters more than it did for `LinkedList<T>`: falling through to `List`'s own code would misread `Deque`'s `start`/`data` fields as `List`'s own single `data` field, silently corrupting every read.

---

# Parsing: A Direct Copy of `List`/`Stack`/`LinkedList`'s Own Two Forms

`Deque<elem>` in type position and `Deque<elem>()` in expression position both mirror the established branches exactly. New `DequeNewExpr` AST node, fielded identically to `ListNewExpr`/`StackNewExpr`/`LinkedListNewExpr`.

```text
$ ax ast examples/deque.ax
Function(build)
  Block
    Assignment(numbers)
      DequeNew(i32)
    ExprStmt
      MethodCall(push_back)
        Name(numbers)
        Integer(10)
    ...
```

---

# Type Checking: `isIndexable` Gains One New Member

`TypeKind::Deque` is new; `Type` reuses the existing flat `elementKind`/`elementStructName` fields, exactly like `List`/`Stack`/`LinkedList`. `resolveType`'s `"Deque<elem>"` branch mirrors the established one-level nesting restriction.

The one line that matters most: `isIndexable` (the shared predicate `IndexExpr`/`IndexAssignStmt`/`checkFieldType`'s `.length` case all already use for `Array`/`Slice`/`List`) gains `Deque`. Because `IndexExpr`'s own type-checking code is already fully generic — no per-collection-type branching at all — this one addition is the *entire* wiring needed for `[i]`, `[i] =`, and `.length` all to type-check correctly. `MethodCallExpr` still gets its own `TypeKind::Deque` branch for `push_front`/`push_back`/`pop_front`/`pop_back`, identical in shape to `LinkedList`'s own.

```text
$ ax capabilities bad.ax   # f() -> i32 { d = Deque<i32>()  d.push_back(1)  return d[true] }
error: array index must be i32, found bool
```

---

# Capability Checking: Zero Changes

`push_front`/`push_back`/`pop_front`/`pop_back` were already added to the write-raising method list last phase (for `LinkedList<T>`) — method-name-only, not object-type-aware, so they're automatically covered. `IndexAssignStmt`'s write-raising is already fully generic (added for arrays, already proven to extend cleanly to `slice<T>`), so `d[i] = x` raises `write` with no `Deque`-specific code either:

```text
$ ax capabilities examples/deque.ax
Function(setFirst)
  Param(numbers: write)
Function(sumWithForIn)
  Param(numbers: read)
Function(drain)
  Param(numbers: write)
```

---

# Region Checking: `IndexExpr`'s Existing Rule Does All the Work

`RegionChecker`'s heap-type detection (`isDequeTypeString`, mirroring `isListTypeString`) and the return-type leak check both grow a `Deque` branch identical to `List`'s own — a borrowed `Deque<T>` parameter can't be returned whole without `take`. Unlike `LinkedList<T>`, `Deque<T>` genuinely needs `elementStructType` extraction: `[i]` on a struct-typed `Deque` aliases the container, exactly like `List<T>[i]` already does.

The key point: **no new `MethodCallExpr` exception was needed at all**. `IndexExpr`'s own `regionOfExpr` case is already fully generic — it already knows to propagate the object's own `Region`/`sourceParam` when `elementStructType` is populated, since array/List index-reads of struct elements have needed exactly this since `0031-arrays.md`. Wiring `elementStructType` into `checkFunction`'s `Deque` branch was the only work required:

```text
$ ax regions leak.ax   # leak(d: Deque<Point>) -> Point { return d[0] }
error: function 'leak' cannot return 'd': parameter 'd' is borrowed and does not outlive the call
- declare 'take' if ownership should transfer
```

`pop_front`/`pop_back`, by contrast, need no exception either — they remove, so nothing else still references the popped value, exactly like `List<T>.pop()`/`LinkedList<T>.pop_front()` already established:

```ax
take_front(d: Deque<Point>) -> Point { return d.pop_front() }   # type-checks without `take`
```

---

# `IrGenerator`: `isDequeExpr` Mirrors `isStackExpr` Exactly

New `IrDequeNew`, `IrDequePushFront`, `IrDequePushBack`, `IrDequePopFront`, `IrDequePopBack`. Since `push_front`/`push_back`/`pop_front`/`pop_back` are the same method names `LinkedList<T>` already uses, a best-effort `isDequeExpr` resolver was needed — a sibling to `isStackExpr` (direct `DequeNewExpr`/`LinkedListNewExpr` literal, parameter type, function return type, `IrScope`'s own parallel `isDeque` map), not a generalization of it, per this codebase's "each pass re-derives independently" convention. `IrIndexGet`/`IrIndexSet` needed *no* changes at all — indexing lowering has never been per-collection-type-aware.

```text
$ ax ir examples/deque.ax
Function(build)
  Params:
  region.enter
  %0 = deque.new i32
  %1 = const.i32 10
  %2 = deque.push_back %0, %1
  ...
```

---

# `Interpreter`: `std::vector`, Not `std::deque` — a Deliberate Divergence From `LinkedListInstance`

```cpp
struct DequeInstance { std::vector<Value> elements; };
```

`LinkedListInstance` uses `std::deque<Value>`; `DequeInstance` uses `std::vector<Value>` instead — specifically so it plugs directly into the existing `asIndexable`/`Indexable{std::vector<Value>*, length}` mechanism `Interpreter.cpp` already uses for `Array`/`Slice`/`List`. That one addition gives `.length`, `[i]`, `[i] =`, and `for`-in *all* for free — zero further interpreter code. `push_front`/`pop_front` become `std::vector::insert`/`erase` at `begin()` (O(n) in the interpreter — a pure representation choice, orthogonal to the LLVM backend's genuinely O(1)-per-op `start`-offset design, exactly like `LinkedListInstance`'s own `std::deque` was already an interpreter-only choice unrelated to how the compiled backend represents a `LinkedList<T>`).

`toString` uses `List`/`Stack`'s own bracket format, not `LinkedList`/`Map`/`Set`'s count-only fallback — must match the LLVM backend's own top-level printer byte-for-byte, and both independently arrive at "full contents" for the same underlying reason: the representation genuinely supports it.

---

# `LlvmIrEmitter`: Push Reallocates-and-Copies-Offset-by-`start`; Pop Is Pure Arithmetic

`emitDequeNew` is direct inline C++ (malloc + null-GEP-sizeof, 3 fields zeroed) — mirrors `emitListNew`. `emitDequePushFront`/`emitDequePushBack` mirror `emitListPush`'s own malloc-a-buffer-of-`count+1` + hand-rolled copy loop (`alloca`/`load`/`store` counter, no `phi` — this backend's unnamed sequential registers can't forward-reference a `phi`'s back-edge value) almost exactly, generalized by a shared `emitDequeCopyForPush` helper taking one destination-offset parameter (`0` for `push_back`, appending after the copied range; `1` for `push_front`, shifting the copy one slot in and writing the new element at index `0`) and reading each copied element from `data[start + i]`, not `data[i]`. New `start` is always stored as `0`.

```text
$ ax llvm-ir f.ax   # f(d: Deque<i32>, v: i32) { d.push_front(v) }
define void @f({i32, i32, i32*}* %0, i32 %1) {
entry:
  %2 = getelementptr {i32, i32, i32*}, {i32, i32, i32*}* %0, i32 0, i32 0
  %3 = load i32, i32* %2
  %4 = getelementptr {i32, i32, i32*}, {i32, i32, i32*}* %0, i32 0, i32 1
  %5 = load i32, i32* %4
  %6 = add i32 %3, 1
  ...
  br label %deque.push.copy.header0
deque.push.copy.header0:
  %16 = load i32, i32* %15
  %17 = icmp slt i32 %16, %3
  br i1 %17, label %deque.push.copy.body0, label %deque.push.copy.done0
deque.push.copy.body0:
  %18 = load i32, i32* %15
  %19 = add i32 %5, %18          ; src index = start + i
  %20 = getelementptr i32, i32* %14, i32 %19
  ...
  %23 = add i32 %22, 1           ; dst index = i + 1 (push_front's offset)
  %24 = getelementptr i32, i32* %12, i32 %23
  store i32 %21, i32* %24
  ...
deque.push.copy.done0:
  %27 = getelementptr i32, i32* %12, i32 0
  store i32 %1, i32* %27         ; new element at index 0
  store i32 %6, i32* %2          ; count
  store i32 0, i32* %4           ; start reset to 0
  store i32* %12, i32** %13      ; data
  ret void
}
```

`emitDequePopFront`/`emitDequePopBack` need **no loop and no branch at all** — simpler than even `emitListPop`: `pop_front` reads `data[start]` then does `start += 1, count -= 1`; `pop_back` reads `data[start + count - 1]` then does `count -= 1` only. No bounds check, matching every other out-of-bounds case in this backend. `emitIndexGet`/`emitIndexSet` gain a `Deque` branch identical to `List`'s own plus one `add i32 %start, %index` before the final element GEP. Top-level printing adapts `List`'s own runtime print loop directly, with every data-pointer read offset by `+start` — the same reasoning that makes `[i]` cheap makes a full-content print loop cheap too, unlike `LinkedList`/`Map`/`Set`'s count-only fallback.

Because `emitDequePushFront`/`emitDequePushBack` are genuinely new codegen shape (List's own copy loop, but reading from an offset source and writing the new element at either end of the destination), a self-referential-free `{i32, i32, T*}*` header, both pushes, both pops, and `[i]` indexing were hand-verified in an isolated `.ll` file against real `clang` (`-O0`/`-O1`) before being written into the real backend — matching this codebase's established rule for genuinely new LLVM-level ground.

---

# Worked Example

`examples/deque.ax`:

```ax
build() -> Deque<i32>
{
    numbers = Deque<i32>()
    numbers.push_back(10)
    numbers.push_back(20)
    numbers.push_front(5)
    return numbers
}

setFirst(numbers: Deque<i32>)
{
    numbers[0] = 99
}

sumWithForIn(numbers: Deque<i32>) -> i32
{
    total = 0
    for value in numbers
    {
        total = total + value
    }
    return total
}

drain(numbers: Deque<i32>) -> i32
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
middle = numbers[1]
front = numbers.pop_front()
back = numbers.pop_back()
lengthAfterPops = numbers.length
called = setFirst(numbers)
afterSetFirst = numbers[0]
forSum = sumWithForIn(numbers)
total = drain(numbers)
lengthAfterDrain = numbers.length
```

```text
$ ax run examples/deque.ax
numbers = []
lengthAfterBuild = 3
middle = 10
front = 5
back = 20
lengthAfterPops = 1
called = ()
afterSetFirst = 99
forSum = 99
total = 99
lengthAfterDrain = 0
$ ax llvm-ir examples/deque.ax | clang -x ir -O1 - -o out && ./out
# byte-for-byte identical (also re-verified at -O0)
```

`lengthAfterBuild = 3` confirms `push_back(10)`, `push_back(20)`, `push_front(5)` produce `[5, 10, 20]`; `middle = 10` confirms `[1]` reads the middle element correctly. `front = 5`/`back = 20` confirm `pop_front`/`pop_back` remove from opposite ends, leaving `[10]` (`lengthAfterPops = 1`). `setFirst` writes through the caller's own deque via `write`-capability `[0] =`, and `afterSetFirst = 99` confirms it. `sumWithForIn` is `Deque<T>`'s own genuinely new capability this phase — a `for`-in loop with zero dedicated support anywhere, purely inheriting `.length` + `[i]`'s existing generic machinery — summing to `99`. `drain`'s `while numbers.length > 0` loop pops every element via `write`-capability `pop_front`, and `numbers` prints as `[]` at the very end because it's the same heap-header pointer throughout, printed only once the whole program has finished.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **`push_front`/`push_back` are not actually O(1)**, despite `0029`'s own complexity table claiming it — every push reallocates and copies the entire backing buffer, the identical honest shortfall `List<T>.push` already has and documents. A real amortized-growth-with-headroom strategy would deliver the documented complexity, but nothing else in this codebase does amortized growth either; kept consistent rather than introducing a new pattern for this one collection.
- **No `peek_front`/`peek_back`.** Every operation this phase either adds, removes, or reads by index (which already aliases correctly via the generic `IndexExpr` rule) — keeping dedicated peek methods out avoided needing any new `RegionChecker` exception work at all.
- **No amortized-shrink on pop** — the buffer never shrinks, matching `List<T>.pop()`'s own identical choice.
- **`Deque<T>` is not a valid `Map`/`Set` key type.** Mirrors `LinkedList<T>`'s own deliberate choice — nothing in scope calls for it.
- **`Deque<T>` cannot be a struct field type.** The same restriction every collection in this language has.
- **`push_front`/`push_back`/`pop_front`/`pop_back` are compiler intrinsics, not real methods**, same as every other collection here.

---

# Guiding Rule

> Look for what a representation *actually buys* before choosing it, not what's textbook-correct. A ring buffer is the standard answer for "deque," but the real payoff this phase needed was cheap, correct `[i]` indexing riding on machinery that already existed everywhere else in the compiler - and a growable array with a `start` offset gets that exact payoff without the wraparound arithmetic a true ring buffer would need. The clearest evidence this was the right call: `TypeChecker::isIndexable` needed one new enum value, `RegionChecker`'s aliasing rule needed zero new code (the array/List rule already generalized), and `IrGenerator`'s `IrIndexGet`/`IrIndexSet` needed nothing at all - three different passes, each already built generically enough that the "right" representation choice made adding a whole new indexable collection almost free. A representation that fought those existing abstractions, even a more "textbook" one, would have cost far more than the wraparound math it saved.
