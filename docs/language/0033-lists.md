# `List<T>`: A Growable, Heap-Allocated List

**Status:** Implemented
**Document:** `0033-lists.md`

---

# Motivation

`docs/language/0029-collections.md` frames `List<T>` as "everyday storage" — the thing you reach for by default — and the backing type for `Stack<T>`/`Queue<T>` in its own hierarchy sketch. With fixed arrays (`0031-arrays.md`) and `slice<T>` (`0032-slices.md`) done, this is the natural next collection: `push`, `pop`, indexing (read and write), and `.length`.

```ax
numbers = List<i32>()
numbers.push(4)
numbers.push(5)
last = numbers.pop()
first = numbers[0]
count = numbers.length
```

Two genuinely new pieces of ground, both hand-verified against the real `clang` toolchain in isolated `.ll` files (at both `-O0` and `-O1`) before being built on, matching this project's established discipline:

1. **Method-call syntax** (`list.push(x)`). Nothing in the language could call anything *on* a value before this — only free functions and field access existed.
2. **A runtime malloc-and-copy growth loop** for `push`. Unlike everything built so far (structs, fixed arrays, slices), a list's storage has to be able to grow *after* creation.

`List<T>` is **not** scoped to function parameters only, the way `slice<T>` was. It may be a parameter, a return type, or a local's declared type, uniformly — because, unlike a slice, a list genuinely owns its own storage (see "Design" below for why that's what makes the difference).

---

# Design: A Heap Pointer, Like Struct and Array — Not a Fat Pointer, Like Slice

A `List<T>` variable is a stable pointer to a heap-allocated 2-field header, `{i32 length, T* data}`. `push` and `pop` mutate the header's own fields *in place* — the pointer itself never changes, even when `push` reallocates the backing buffer (only the header's `data` field is updated to point at the new buffer). This is exactly the same shape that already makes struct field-assignment and array index-assignment write-through correctly: because the outward-facing pointer is stable, every existing alias to it sees a mutation immediately, with no extra plumbing needed anywhere.

This is the opposite representation from `slice<T>`, which is a two-word *value* passed by copy (a "fat pointer": pointer + length, with no heap allocation of its own). The difference is exactly the difference in what the two types are *for*: a slice is a temporary, non-owning view into someone else's storage; a list owns its storage outright, and that storage can change shape over its lifetime. Reusing struct/array's proven pointer-based mutation model for `List<T>`, rather than inventing a third representation, was the whole reason implementing this collection was more `0031`/`0032`-shaped than not.

---

# Parsing: Two New Things, Both Sidestepping a General Ambiguity On Purpose

**`List<i32>()` construction.** `List<i32>` uses the exact same `<`/`>`-based syntax `slice<i32>` already established in type position — but here it needs to work in *expression* position (`numbers = List<i32>()`), which is new. Rather than adding real generic-call parsing (which would create a genuine ambiguity with `<`/`>` as comparison operators — `List < i32 > ()` could otherwise parse as two chained comparisons), `parsePrimary` special-cases the one literal identifier `"List"` the same way `parseTypeName` already special-cases `"slice"`: no lexer changes, no new tokens, just a lookahead check on an identifier's exact text.

```text
$ ax tokens x.ax          # source: numbers = List<i32>()
1:1  Identifier  numbers
1:9  Equal       =
1:11 Identifier  List
1:15 Less        <
1:16 Identifier  i32
1:19 Greater     >
1:20 LeftParen   (
1:21 RightParen  )
```

**Method-call syntax.** `parsePostfix`'s existing `.field` handling (already there for `object.field`) gained one more check: if a `LeftParen` follows the identifier after `.`, it's a method call, not a field. `object.push(x)` builds a new `MethodCallExpr` node; `object.field` (no parens) still builds the existing `FieldExpr`. `"push"`/`"pop"` are the only recognized methods — they're compiler intrinsics tied to `List<T>` specifically, not a general user-defined method-dispatch system; anything else is a `TypeChecker` error ("no such method"), the same way an unrecognized field name already is for arrays/slices' `.length`.

`examples/list.ax`'s `build`, verified via `ax ast`:

```ax
build() -> List<i32>
{
    numbers = List<i32>()
    numbers.push(10)
    numbers.push(20)
    numbers.push(30)
    return numbers
}
```

```text
$ ax ast list.ax
Function(build)
  Block
    Assignment(numbers)
      ListNew(i32)
    ExprStmt
      MethodCall(push)
        Name(numbers)
        Integer(10)
    ...
    Return
      Name(numbers)
```

---

# Type Checking: Reusing `isIndexable`, No New Restriction Beyond One Struct-Field Guard

`TypeKind::List` is new; `Type` reuses the `elementKind`/`elementStructName` fields arrays and slices already added — no new `Type` fields needed. `resolveType` recognizes `"List<elem>"` exactly like it already recognizes `"slice<elem>"` and `"[elem;N]"`. `isIndexable` (already `Array || Slice`) grows to `Array || Slice || List`, so `numbers[i]` and `numbers.length` type-check through the exact same code path arrays and slices already use — no new branch needed for either.

`MethodCallExpr` gets its own small case: the object must resolve to `List`; `"push"` requires exactly one argument matching the element type and produces `unit`; `"pop"` requires zero arguments and produces the element type; anything else throws. Compile-time literal-index bounds checking (the one thing arrays get that slices/lists don't) stays array-only, since a list's length is never statically known — exactly the same reasoning that already applies to slice indexing.

The **only** scope restriction: `List<T>` is rejected as a struct field type (one small guard, mirroring the pattern `slice<T>`'s parameter-only restriction already established) — purely to keep this phase's surface area bounded, not for any deeper reason. Everywhere else — parameter, return type, local declared type — it's unrestricted:

```ax
build() -> List<i32> { x: List<i32> = List<i32>()  return x }
use(numbers: List<i32>) -> i32 { return numbers.length }
```
type-checks cleanly; `struct Wrapper { items: List<i32> }` is rejected.

---

# Capability and Region Checking: `push`/`pop` Raise `write`, `List<T>` Is a Heap Type Like Struct/Array

`CapabilityChecker`'s `MethodCallExpr` case recurses into the object and arguments (mirroring `CallExpr`), and — since both `push` and `pop` mutate the receiver's own header fields in place — raises `Write` on the owning parameter through the same `rootParamIndex` chain-walk `IndexAssignStmt` already uses:

```text
$ ax capabilities appendOne.ax   # appendOne(numbers: List<i32>) { numbers.push(1) }
Function(appendOne)
  Param(numbers: write)
```

`RegionChecker`'s struct/array heap-type detection (`checkFunction`) grows to also recognize a `List<...>` parameter type — same aliasing risk, same `Borrowed`-unless-`take` treatment, since a list is exactly as heap-allocated and reference-semantic as a struct or array:

```text
$ ax regions identity.ax   # identity(numbers: List<i32>) -> List<i32> { return numbers }
error: function 'identity' cannot return 'numbers': parameter 'numbers' is borrowed and
does not outlive the call - declare 'take' if ownership should transfer
```

---

# `IrGenerator`: No Changes to Indexing, `.length`, or Anything Already Generic

Traced through every code path: `IndexExpr`/`IndexAssignStmt`/`FieldExpr`'s `.length` case were already fully type-agnostic after the array/slice work (they only ever deal in registers, never types), so a `List<T>` value flows through them with zero changes. `arrayLengthOf` (the constant-folding helper behind array's zero-cost `.length`) only recognizes array-shaped type strings, so it correctly never matches a `List<...>` parameter or a `ListNewExpr` — `.length` on a list falls straight through to a genuine, non-folded `IrFieldGet`, exactly the "runtime, not compile-time" behavior a mutable length needs.

Two new instructions cover the two genuinely new operations: `IrListNew` (carries the element type name explicitly — unlike `IrArrayNew`, which infers it from its own elements, a brand-new *empty* list has no elements to infer from) and `IrListPush`/`IrListPop`.

```text
$ ax ir list.ax
Function(build)
  Params:
  region.enter
  %0 = list.new i32
  %1 = const.i32 10
  %2 = list.push %0, %1
  %3 = const.i32 20
  %4 = list.push %0, %3
  ...
  return %0
```

---

# `LlvmIrEmitter`: No Phi Nodes — Alloca/Load/Store for the Copy-Loop Counter, for the Same Reason Carried Loop Variables Already Use It

`llvmType("List<i32>")` → `"{i32, i32*}*"` — an anonymous 2-field record, pointer-suffixed. This is what distinguishes it from every other shape already handled: struct is `%Name*`, array is `[N x T]*`, slice is `{T*, i32}` (no trailing `*` — passed by value), list is `{i32, T*}*` (trailing `*` — a pointer to a small heap record). `isSliceType`/`isListType` both just check the first/last character.

**`push`'s growth sequence was the one piece of genuinely new ground.** The obvious implementation uses a `phi` node for the copy loop's counter — but this codebase's `LlvmIrEmitter` always emits *unnamed, sequentially-numbered* registers (`%0`, `%1`, `%2`, ...), and (per `0028-loops.md`, which hit this exact issue first) those must appear in strictly increasing textual order. A `phi` node's back-edge value is defined *after* the `phi` that references it — a forward reference that's fine for LLVM IR in general, but not compatible with this codebase's specific unnamed-sequential-numbering emission convention. That's exactly why loop-carried variables already use `alloca`/`load`/`store` instead of `phi` here, and `push`'s copy-loop counter follows the identical pattern rather than reopening that already-settled question. Verified correct (hand-written `.ll`, matching this exact unnamed-register-numbering shape) at both `-O0` and `-O1` before being implemented for real.

**No amortized growth — a deliberate simplification, not an oversight.** `0029-collections.md` states `push` as amortized O(1); this implementation reallocates to *exactly* `length + 1` elements and copies every existing element across on *every* push — O(n) per push, O(n²) total for n pushes. This is a real, measurable difference from the doc's stated goal, made deliberately to avoid an entire second axis of complexity (a separate `capacity` field, doubling logic, tracking when to actually grow vs. reuse existing slack) in this first pass. Critically, it's a **pure internal implementation detail**: nothing about `emitListPush`'s growth strategy is visible to any Axea program — a future capacity-doubling rewrite changes zero language surface.

```text
$ ax llvm-ir appendOne.ax   # appendOne(numbers: List<i32>) { numbers.push(99) }
define void @appendOne({i32, i32*}* %0) {
entry:
  %1 = add i32 0, 99
  %2 = getelementptr {i32, i32*}, {i32, i32*}* %0, i32 0, i32 0
  %3 = load i32, i32* %2
  %4 = add i32 %3, 1
  %5 = getelementptr i32, i32* null, i32 1
  %6 = ptrtoint i32* %5 to i64
  %7 = zext i32 %4 to i64
  %8 = mul i64 %7, %6
  %9 = call i8* @malloc(i64 %8)
  %10 = bitcast i8* %9 to i32*
  %11 = getelementptr {i32, i32*}, {i32, i32*}* %0, i32 0, i32 1
  %12 = load i32*, i32** %11
  %13 = alloca i32
  store i32 0, i32* %13
  br label %list.push.copy.header0
list.push.copy.header0:
  %14 = load i32, i32* %13
  %15 = icmp slt i32 %14, %3
  br i1 %15, label %list.push.copy.body0, label %list.push.copy.done0
list.push.copy.body0:
  %16 = load i32, i32* %13
  %17 = getelementptr i32, i32* %12, i32 %16
  %18 = load i32, i32* %17
  %19 = load i32, i32* %13
  %20 = getelementptr i32, i32* %10, i32 %19
  store i32 %18, i32* %20
  %21 = load i32, i32* %13
  %22 = add i32 %21, 1
  store i32 %22, i32* %13
  br label %list.push.copy.header0
list.push.copy.done0:
  %23 = getelementptr i32, i32* %10, i32 %3
  store i32 %1, i32* %23
  store i32 %4, i32* %2
  store i32* %10, i32** %11
  ret void
}
```
GEP-load the old length, `malloc` a fresh buffer sized to `length + 1`, `alloca`/`load`/`store`-loop-copy the old elements across, append the pushed value, then store the new length/data back into the header's own fields — in place, so `numbers` in the caller sees the update the instant this returns.

**`pop`** is the mirror image, deliberately simpler: decrement `length` and load the element that was at the old `length - 1`. No bounds check (matches every other out-of-bounds case in this backend — division, array/slice indexing: the interpreter checks and throws, compiled code does not). No shrink or realloc (matches the "no capacity tracking" choice above — the buffer just stays at its previous size; only the logical `length` field shrinks).

**Indexing and `.length`** reuse the exact GEP-chain shape slice indexing already established, extended by one extra hop: GEP+load the header's `data` field (field 1) to get the flat element pointer, then the same single-index GEP slice indexing already uses. `.length` is GEP+load of field 0 — a genuine runtime read, unlike array's compile-time constant.

**Top-level printing needed a real runtime loop**, not the compile-time-unrolled one arrays use — a list's length isn't known until the program runs. Structured to need only one runtime branch (empty vs. not), not a per-iteration "is this the first element" check: print element 0 unconditionally if the list isn't empty, then loop `i = 1..length` printing `", "` + each remaining element. Same alloca-based loop-counter idiom as `push`, for the identical reason.

---

# Interpreter: `std::vector` Already Does the Hard Part

```cpp
struct ListInstance { std::vector<Value> elements; };
```

`std::vector` already provides real amortized growth internally — the LLVM backend is the *only* place growth needed to be hand-rolled, since Axea IR/the interpreter never need to know how a `std::vector` grows itself. `push`/`pop` are direct `push_back`/`pop_back` calls (the latter throwing on an empty list first, matching the LLVM backend's *lack* of a check with a real one, same "interpreter checks, compiled code doesn't" split used everywhere else). Indexing/`.length` reuse `asIndexable` (the array/slice-shared helper from `0032-slices.md`), which grew a third branch for `ListInstance` — no new logic at the three call sites that already use it.

---

# Worked Example

`examples/list.ax`:

```ax
build() -> List<i32>
{
    numbers = List<i32>()
    numbers.push(10)
    numbers.push(20)
    numbers.push(30)
    return numbers
}

sum(numbers: List<i32>) -> i32
{
    total = 0
    for v in numbers { total = total + v }
    return total
}

appendOne(numbers: List<i32>)
{
    numbers.push(99)
}

numbers = build()
total = sum(numbers)
called = appendOne(numbers)
totalAfterAppend = sum(numbers)
last = numbers.pop()
count = numbers.length
```

```text
$ ax run examples/list.ax
numbers = [10, 20, 30]
total = 60
called = ()
totalAfterAppend = 159
last = 99
count = 3
$ ax llvm-ir examples/list.ax | clang -x ir -O1 - -o out && ./out
numbers = [10, 20, 30]
total = 60
called = ()
totalAfterAppend = 159
last = 99
count = 3
```

Byte-for-byte identical (also re-verified at `-O0`, given `0028-loops.md`'s documented unoptimized-codegen crash class for this exact toolchain) — `sum` correctly reads through both the freshly-built list and the same list after `appendOne` mutated it via `write`-capability push, and `pop` correctly returns the pushed `99` and shrinks the length back to 3.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **No amortized growth.** Every `push` reallocates and copies the entire list — see "Design" above. A pure internal fix, not a language change, whenever it's worth doing.
- **No `insert(i, x)` / `remove_at(i)`.** Only append (`push`) and remove-from-the-end (`pop`) this phase.
- **`List<T>` cannot be a struct field type.** The one scope restriction that exists; not a hard architectural limit, just kept out of this phase's surface area.
- **`pop()` throws on an empty list rather than returning an `Optional`.** `Optional`/`T?` has no support anywhere in this codebase yet (`TypeKind::Optional` is declared, unused) — wiring that up is its own feature. Matches indexing's own out-of-bounds behavior exactly (interpreter throws; LLVM backend has no check at all).
- **`push`/`pop` are compiler intrinsics, not real methods.** There's no user-definable method/`impl` system in this language; `"push"`/`"pop"` are the only two names `MethodCallExpr` recognizes, hardcoded to `List<T>`'s own semantics.
- **No `List<T>` literal syntax** (e.g. `[1, 2, 3]` inferring `List<i32>` the way it infers a fixed array) — construction is always the explicit `List<T>()` call, then `push` to populate.

---

# Guiding Rule

> Pick the representation the *semantics* actually need, not the one that's easiest to bolt onto what already exists. `List<T>` could have copied slice's by-value fat-pointer shape - but a value that needs to grow after creation needs a stable identity to grow *behind*, which is exactly what struct and array's heap-pointer model already provides for free. Recognizing "this is struct/array-shaped, not slice-shaped" up front is what made every layer *except* the two genuinely novel operations (method-call syntax, the growth loop) a direct reuse of already-proven machinery - and even inside that novel growth loop, reusing loops.md's own alloca-not-phi lesson, rather than rediscovering it the hard way, is the same instinct one level down.
