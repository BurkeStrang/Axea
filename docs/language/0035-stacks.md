# `Stack<T>`: A Thin, Type-Distinct Wrapper Over `List<T>`'s Own Machinery

**Status:** Implemented
**Document:** `0035-stacks.md`

---

# Motivation

`docs/language/0029-collections.md`'s "Recommended Hierarchy" lists `Stack<T>` right after `List<T>`, specced as "a LIFO collection backed internally by `List<T>`." With `List<T>` (`0033`), `slice<T>` (`0032`), fixed arrays (`0031`), and generic `Map<K,V>`/`Set<T>` (`0034`) all done, `Stack<T>` is the cheapest next collection: it reuses `List<T>`'s exact heap-header shape and push/pop mechanics almost entirely, adding exactly one new operation.

```ax
s = Stack<i32>()
s.push(10)
s.push(20)
top = s.peek()   // 20 - doesn't remove
val = s.pop()    // 20 - removes
count = s.length // 1
```

**Design stance: `Stack<T>` is a nominally distinct Axea type from `List<T>`** — its own `TypeKind`, not assignable to a `List<T>` parameter — matching `0029`'s own guiding principle that `Stack<T>` exists "to communicate intent." Underneath, at the LLVM level, its header is the *exact same type* as `List<T>`'s own `{i32 length, T* data}*` — genuinely "backed by `List<T>`," not merely similarly shaped.

**API**, per `0029` plus one addition: `push(x)`, `pop() -> T`, `peek() -> T` (read the top without removing), and `.length` — not in `0029`'s original rough sketch, but necessary: without it there's no way to check emptiness before calling `pop`/`peek`, exactly the gap `List<T>.length` already fills. `0029`'s sketch shows `peek()`/`pop()` used in an `if let`-style safe-access pattern implying `Optional` — but `Optional`/`T?` has no support anywhere in this codebase yet, and `List<T>.pop()` already established the precedent for exactly this situation: **throws on empty in the interpreter, no bounds check in compiled code**, matching every other out-of-bounds case here (division, array/slice/List indexing). `Stack<T>.pop()`/`.peek()` follow that same precedent, not `Optional`.

Two real design wrinkles surfaced during this phase, both resolved by precedent rather than guesswork:

1. **`push`/`pop` collide with `List<T>`'s own method names.** `IrGenerator`'s `MethodCallExpr` lowering dispatches purely by method name (`"push"` → an insert instruction, `"pop"` → a remove instruction), so reusing those names for `Stack<T>` creates the exact ambiguity `Map`/`Set`'s shared `"contains"`/`"remove"` names created last phase — resolved the same way, with a best-effort object-type resolver (`isStackExpr`, mirroring `isSetExpr`'s own shape). `peek` needed no such resolver: `List<T>` has no `peek`, so it's unambiguous by name alone.
2. **`peek()` aliases; `pop()` doesn't.** This one was caught by precedent, not guesswork — it's the direct sequel to a real bug fixed in `Map<K,V>.get()` earlier this same session. `pop()` *removes* its element, so nothing else still references it (always safely `Owned`). `peek()` does **not** remove — the stack still holds the same instance afterward, so if `T` is a struct, `peek()`'s result *aliases* the stack's own storage, exactly like `Map<K,V>.get()`'s result aliases the map. `RegionChecker` needed the identical exception already added for `Map`'s `.get()`, now covering `Stack`'s `.peek()` too — implemented directly, verified by hand before any test caught it wrong this time.

---

# Design: Same LLVM Type as `List<T>`, Distinct Everywhere Else

A `Stack<T>` variable is the exact same "stable pointer to a small heap header, mutated in place" model `List<T>` already established — `push`/`pop` mutate the header's own fields in place, so every existing alias sees updates for free, with `peek` added as the one read-only operation neither `List<T>` nor the header shape needed before.

**The distinction between `List<T>` and `Stack<T>` lives entirely above the LLVM layer.** `llvmType("Stack<T>")` produces the *exact same text* `llvmType("List<T>")` would for the same `T` — not a similar shape, the literal same LLVM type (LLVM's anonymous struct types are structurally typed: two identically-shaped anonymous types *are* the same type). Everything that tells a `Stack<T>` apart from a `List<T>` — `TypeChecker::TypeKind::Stack` vs `TypeKind::List`, the `IrStackNew`/`IrStackPush`/`IrStackPop`/`IrStackPeek` instructions vs `IrListNew`/`IrListPush`/`IrListPop` — lives at the Axea/IR level, never in a bare LLVM type string. This has a genuinely useful consequence: `.length`'s field-get code and the top-level runtime print loop, both keyed off `isListType`'s own existing structural check (`front == '{' && back == '*'`), already handle a `Stack<T>` value correctly with **zero new code** — not a coincidence the way `Map`/`Set`'s own count field briefly worked "by accident" before being made explicit last phase, but a direct, provable consequence of the two types being identical at that layer.

---

# Parsing: A Direct Copy of `List<T>`'s Own Two Forms

`Stack<elem>` in type position and `Stack<elem>()` in expression position both mirror `List`'s own already-established branches exactly (including the already-fixed `parseTypeName()`-recursive construction form, not an older single-token one that couldn't parse nested generics). New `StackNewExpr` AST node, fielded identically to `ListNewExpr`.

```text
$ ax ast examples/stack.ax
Function(build)
  Block
    Assignment(numbers)
      StackNew(i32)
    ExprStmt
      MethodCall(push)
        Name(numbers)
        Integer(10)
    ...
```

---

# Type Checking: `push`/`pop` Copied Byte-for-Byte, `peek` Is `pop`'s Twin

`TypeKind::Stack` is new; `Type` reuses the existing flat `elementKind`/`elementStructName` fields exactly like `List`/`Set` already do (one type parameter — no new `Type` fields needed, unlike `Map`/`Set`'s own `elementTypeName`/`valueTypeName` addition). `resolveType`'s `"Stack<elem>"` branch mirrors `"List<elem>"`'s own one-level nesting restriction, extended to also reject a `Stack<Stack<T>>` for symmetry: `Array`/`Slice`/`List`/`Stack` element types are all rejected, everything else (primitives, structs, `Map`/`Set`) is fine.

`MethodCallExpr` gets its own `TypeKind::Stack` case, copying `List`'s `push`/`pop` handling directly, plus `peek`: zero arguments, returns `elementType` — the identical shape `pop` already has, just without removing.

```text
$ ax capabilities bad.ax   # f() { s = Stack<i32>()  s.badmethod() }
error: no such method 'badmethod' on Stack<i32>
```

`Stack<T>` is hashable under the exact same rule `List<T>` already is (both use the flat `elementKind` representation `isHashable` already recurses through) — so `Set<Stack<i32>>` type-checks, for whatever that's worth. The one scope restriction, mirroring `List<T>`'s own: `Stack<T>` is rejected as a struct field type, kept out purely to bound this phase's surface area.

---

# Capability Checking: Zero Changes — The Existing Method-Name List Already Covers It

`CapabilityChecker`'s write-raising is purely method-name-based (`"push"`, `"pop"`, `"set"`, `"remove"`, `"add"`), never object-type-aware — so `Stack<T>.push`/`.pop` were automatically covered the moment those method names were reused, with no code change at all. `peek` needed no addition either: every prior read-only method (`Map`'s `get`/`contains`) already required zero extra code, since a touched-but-never-written parameter defaults to `Read`.

```text
$ ax capabilities examples/stack.ax
Function(pushOne)
  Param(numbers: write)
Function(drain)
  Param(numbers: write)
```

---

# Region Checking: `peek` Gets `Map<K,V>.get()`'s Exact Exception

`RegionChecker`'s heap-type detection (`isStackTypeString`, mirroring `isListTypeString`) and return-type leak check both grow a `Stack` branch identical to `List`'s own. The interesting part is `regionOfExpr`'s `MethodCallExpr` case: the exception already added for `Map<K,V>.get()` (preserving the object's own `Region`/`sourceParam` instead of `pop`'s "always `Owned`" default, when the element is struct-typed) now also covers `"peek"`:

```text
$ ax regions leak.ax   # leak(s: Stack<Point>) -> Point { return s.peek() }
error: function 'leak' cannot return 's': parameter 's' is borrowed and does not outlive the
call - declare 'take' if ownership should transfer
```

`pop`, by contrast, needs no exception at all — it removes, so nothing else still references the popped value, exactly like `List<T>.pop()` already established:

```ax
take_top(s: Stack<Point>) -> Point { return s.pop() }   # type-checks without `take`
```

---

# `IrGenerator`: A Sibling Resolver, Not a Generalization

`IrStackNew` (carries `elementTypeName`, mirrors `IrListNew`), `IrStackPush`, `IrStackPop`, `IrStackPeek` are new, distinct instructions. The real new piece is `isStackExpr` — a best-effort object-type resolver with the identical shape `isSetExpr` already established last phase (a direct `StackNewExpr`/`ListNewExpr` literal, a `Stack<T>`/`List<T>`-typed function parameter, a call to a function with that return type, or a name already recorded in `IrScope`'s own parallel `isStack` map) — used only to disambiguate `.push`/`.pop` between `List` and `Stack`. A sibling resolver, not a generalization of `isSetExpr` itself, per this codebase's "each pass re-derives independently" convention; `.peek` needs no resolver at all, since `List<T>` has no `peek` to collide with.

```text
$ ax ir examples/stack.ax
Function(build)
  Params:
  region.enter
  %0 = stack.new i32
  %1 = const.i32 10
  %2 = stack.push %0, %1
  ...
Function(drain)
  Params: %0=numbers
  ...
  %5 = stack.pop %0
```

---

# `LlvmIrEmitter`: Structurally Identical to `List<T>`'s Own, Plus One New Operation

`emitStackNew`/`emitStackPush`/`emitStackPop` are structurally identical to `emitListNew`/`emitListPush`/`emitListPop` — same malloc+null-GEP-sizeof idiom, same no-bounds-check `pop`. Kept as separate C++ functions rather than shared with `List`'s own, matching this codebase's consistent "separate over shared" preference (already applied to `Map` vs `Set`'s own near-identical resize logic) — and they're dispatched via distinct IR instruction types either way, so there's nowhere to actually share code from without adding an indirection that buys nothing. `push`'s own *growth* logic is the one deliberate exception: it calls the same shared `ensureListCapacity` helper `List<T>.push` uses (doubling capacity, not reallocating to exactly `length + 1` - see `docs/language/0033-lists.md`), since that logic is genuinely identical regardless of which collection is growing (it only ever touches the header's own length/capacity/data fields, never anything LIFO/heap-order-specific) - the "separate over shared" preference above is about each collection's own *distinct* operations, not an absolute rule against ever sharing a sub-computation (`ensureBufferCapacity`, `resolveStrPtr` are earlier precedent for exactly this kind of shared helper).

**`emitStackPeek` is the one genuinely new codegen path this whole feature needed**: GEP+load the element at `length - 1`, *without* the decrement-and-store-back `pop` does.

```text
$ ax llvm-ir peek.ax   # f(s: Stack<i32>) -> i32 { return s.peek() }
define i32 @f({i32, i32*}* %0) {
entry:
  %1 = getelementptr {i32, i32*}, {i32, i32*}* %0, i32 0, i32 0
  %2 = load i32, i32* %1
  %3 = sub i32 %2, 1
  %4 = getelementptr {i32, i32*}, {i32, i32*}* %0, i32 0, i32 1
  %5 = load i32*, i32** %4
  %6 = getelementptr i32, i32* %5, i32 %3
  %7 = load i32, i32* %6
  ret i32 %7
}
```

No named type, no monomorphization registry, no per-instantiation anything — unlike `Map`/`Set`, `Stack<T>`'s header isn't self-referential, so it needed none of that machinery.

---

# Interpreter: `StackInstance` Is `ListInstance`, Renamed

```cpp
struct StackInstance { std::vector<Value> elements; };
```

Byte-identical to `ListInstance` — the distinction is purely at the Axea type-system level, not in how the interpreter represents it. `push`/`pop` are `push_back`/`pop_back` (`pop` throws on empty, matching `List<T>.pop()`); `peek` reads `.back()` *without* removing (throws on empty too, for the same "interpreter checks, compiled code doesn't" reason `pop` already follows). `Stack<T>` prints with the same bracket format `List<T>` does (`[10, 20, 30]`) rather than `Map`/`Set`'s count-only format — a stack's order is just as well-defined as a list's, so there's no reason to hide contents the way the unordered, no-iteration-yet `Map`/`Set` case does.

---

# Worked Example

`examples/stack.ax`:

```ax
build() -> Stack<i32>
{
    numbers = Stack<i32>()
    numbers.push(10)
    numbers.push(20)
    numbers.push(30)
    return numbers
}

pushOne(numbers: Stack<i32>)
{
    numbers.push(99)
}

drain(numbers: Stack<i32>) -> i32
{
    total = 0
    while numbers.length > 0
    {
        total = total + numbers.pop()
    }
    return total
}

numbers = build()
top = numbers.peek()
called = pushOne(numbers)
afterPush = numbers.length
topAfterPush = numbers.peek()
total = drain(numbers)
countAfterDrain = numbers.length
```

```text
$ ax run examples/stack.ax
numbers = []
top = 30
called = ()
afterPush = 4
topAfterPush = 99
total = 159
countAfterDrain = 0
$ ax llvm-ir examples/stack.ax | clang -x ir -O1 - -o out && ./out
numbers = []
top = 30
called = ()
afterPush = 4
topAfterPush = 99
total = 159
countAfterDrain = 0
```

Byte-for-byte identical (also re-verified at `-O0`). `top = 30` confirms `peek` reads the most recently pushed element without removing it (`afterPush` still counts it); `pushOne` writes through the caller's own stack via `write`-capability `push`; `drain` correctly pops every element via `write`-capability `pop` in a `while numbers.length > 0` loop, summing to `10 + 20 + 30 + 99 = 159`; `numbers` prints as `[]` at the very end because it's the same heap-header pointer throughout, printed only once the whole program (including `drain`'s full pop loop) has finished.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **`pop()`/`peek()` throw on an empty stack rather than returning an `Optional`.** Matches `List<T>.pop()`'s own exact precedent; `Optional`/`T?` has no support anywhere in this codebase yet.
- **`push`/`pop`/`peek` are compiler intrinsics, not real methods** — there's no user-definable method/`impl` system in this language.
- **`Stack<T>` cannot be a struct field type.** The one scope restriction that exists, mirroring `List<T>`'s own identical restriction.
- **No `for`-in iteration over a `Stack<T>`** this phase, even though the underlying representation could trivially support it (it's the same buffer `List<T>` already iterates) — kept out purely to match this phase's scope, not a hard limitation.

---

# Guiding Rule

> When a new type is "just" an existing type with a different name and one extra method, look for exactly where that similarity stops holding - twice, this phase, the answer was "at method-name collision" and "at whether the operation removes." Both were resolved not by reasoning about `Stack<T>` in isolation, but by recognizing each as the *same shape* of problem already solved elsewhere in this codebase: `Map`/`Set`'s shared `contains`/`remove` names, already-fixed via a best-effort object-type resolver; `Map<K,V>.get()`'s own aliasing bug, already caught and fixed via a targeted `RegionChecker` exception. Treating a new feature as "find where this matches a solved problem" rather than "design this from scratch" is what kept `Stack<T>` - genuinely the cheapest collection built this session - actually cheap, instead of quietly reintroducing bugs its closest relatives had already paid to fix.
