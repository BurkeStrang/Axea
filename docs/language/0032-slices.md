# `slice<T>`: Safe, Non-Owning Views Over Arrays

**Status:** Implemented
**Document:** `0032-slices.md`

---

# Motivation

Fixed-size arrays (`[T; N]`, `0031-arrays.md`) shipped with one real usability gap: a function taking `values: [i32; 4]` can only ever be called with an exactly-4-element array. `docs/language/0005-type-system.md` (§45) and `0002-grammar.md` (`slice_type = "slice" "<" type ">"`) already sketched the fix — `slice<T>`, "conceptually: pointer + length, but safely typed", callable with an array of *any* size. This document implements exactly that primary use case: an array implicitly converts to a `slice<T>` at a call boundary.

```ax
sum(read values: slice<i32>) -> i32
{
    total = 0
    for v in values { total = total + v }
    return total
}

sum([1, 2, 3])             // fine
sum([1, 2, 3, 4, 5, 6, 7]) // also fine - same function, different array size
```

**Deliberately scoped to function parameters only** — not a local-variable type, not a return type, not a struct field type. Rejected explicitly, with a clear error, everywhere else `parseTypeName` would otherwise happily accept it (see "Why Parameter-Only" below).

**Explicitly out of scope**, not silently missing: `array[a..b]` sub-slicing syntax (the grammar's own `slice_suffix`), and re-slicing an existing slice into a shorter one.

---

# Design: Reuse Where Possible, Diverge Only Where the Representation Genuinely Does

Same precedent `0031-arrays.md` established for arrays-vs-structs: extend the existing type-agnostic machinery wherever it already generalizes, add a real branch only where a slice's underlying shape is actually different from an array's. An array is one heap pointer to N contiguous elements; a slice is a two-word value — pointer plus a runtime length — that never allocates anything itself and never owns what it points to. That single representational difference is the source of almost everything below.

## Parser: No New Tokens

`slice<i32>` only ever appears in type position (`Parser::parseTypeName`, never `parseExpression`), and `<`/`>` (`Less`/`Greater`) already exist as tokens from ordinary comparison operators — no ambiguity, since types and expressions are parsed by entirely separate grammar productions already. `parseTypeName()` (added for arrays) gained one more shape: if the parsed identifier's text is exactly `"slice"` and the next token is `Less`, it recursively calls itself for the element type and expects a closing `Greater`, producing the canonical `"slice<elem>"` string — the same recursive-call pattern `[elem;N]` already used for its own element type.

## No New AST Nodes

A slice is indexed, `.length`-read, and index-assigned through the exact same `IndexExpr`/`FieldExpr`/`IndexAssignStmt` nodes arrays already use. `TypeChecker`'s array-only checks (`kind == TypeKind::Array`) generalized to `isIndexable(type)` (`Array || Slice`) at every one of those three sites — with one carve-out: the compile-time literal-index bounds check stays **array-only**, since a slice's length is never known until runtime (no static size to check a literal index against). This is the one place a slice's behavior genuinely differs from an array's at the type-checking level.

## The Coercion Rule Lives in Exactly One Place

`CallExpr` argument checking gained a single new rule: if the declared parameter type is `Slice` and the argument's own checked type is `Array` with a matching element type (any size), accept it even though strict `Type` equality fails. An existing slice forwarded to another slice parameter (`wrapper(values: slice<i32>) { helper(values) }`) needs *no* special rule — ordinary `Slice == Slice` equality already covers that case, since `values` inside `wrapper` is already slice-typed.

## Why Parameter-Only

A function *can* legally write `return values` where `values` is its own `slice<i32>` parameter — nothing stops a direct pass-through, since `Slice == Slice` needs no coercion. That would make a slice-typed *return value* reachable, which is exactly what this phase avoids: `resolveType`'s three non-parameter call sites (a function's own return type, a struct field's type, a local variable's declared type) each explicitly reject a resolved `Slice` kind with a clear error:

```text
$ ax capabilities bad.ax   # f(values: slice<i32>) -> slice<i32> { return values }
error: slice<T> is only supported as a function parameter type, not as a function return type
```

This is what keeps `RegionChecker` untouched (see below) and what keeps a `SliceInstance`/fat-pointer value from ever needing to survive past a single function call — it can exist as a parameter, get re-bound to a local name inside that same function body (the `for`-in-over-slice desugaring does exactly this, via its own internal `__for<N>_arr` binding — an *undeclared* local assignment, which is intentionally still allowed, only an explicit `x: slice<i32> = ...` annotation is rejected), or get forwarded to another slice parameter — but it can never leave the function it came from as a value in its own right.

## `CapabilityChecker`: No Changes

`rootParamIndex`'s `IndexExpr`-stripping walk and `inferStmt`'s `IndexAssignStmt` case (both added for arrays) are already fully type-agnostic — they only look at AST shape, never at what kind of value is being indexed. `values[i] = x` on a `slice<i32>` parameter infers `write` exactly like it would on `[i32;4]`, with zero code changes, verified directly rather than just assumed:

```text
$ ax capabilities zeroFirst.ax   # zeroFirst(values: slice<i32>) { values[0] = 0 }
Function(zeroFirst)
  Param(values: write)
```

## `RegionChecker`: No Changes — Deliberately, Not an Oversight

`checkFunction`'s borrowed/owned classification only recognizes struct and array type strings; a `slice<...>` parameter falls through as `Owned`:

```text
$ ax ir sum.ax
Function(sum)
  Params: %0=values
  region.enter
  move %0
  ...
```

`move %0` here is semantically a little odd — a slice isn't really "owned" the way a struct or array is — but it's harmless: `IrMove` has zero effect anywhere downstream (`LlvmIrEmitter` doesn't emit anything for it), and this is the *correct* modeled behavior for this phase regardless. The struct/array borrow-check exists specifically to stop an aliased reference to a *caller's owned value* from escaping through a return; a slice never owned that value in the first place — non-owning is its entire definition — and since it's parameter-scoped, the only thing it could ever "escape" as is a direct pass-through of an already-existing parameter, which was never the caller's exclusive-ownership concern to begin with. Real lifetime tracking for slices (does the backing array still exist when the slice is used?) is out of scope, consistent with this whole backend's stance of not doing real memory management anywhere (nothing is ever freed — see `0022-llvm-backend.md`).

## `IrGenerator`: No Changes At All

Traced through every code path before writing any of it: `lowerExpr`'s `IndexExpr`/`FieldExpr` cases and `lowerStmt`'s `IndexAssignStmt` case only ever deal in registers, never types — lowering is identical whether the object being indexed is array- or slice-shaped. `.length`'s constant-folding helper, `arrayLengthOf`, already only recognizes array-shaped type strings (a leading `'['`), so it correctly returns "unknown" for a `"slice<...>"` parameter with no changes needed — `.length` on a slice falls straight through to a genuine `IrFieldGet`, which is exactly the desired (non-constant-folded, *runtime*) behavior, since a slice's length isn't known until the call that produced it:

```text
$ ax ir sum.ax
    %6 = field.get %0.length
```

(compare to array's own `.length`, which never emits an instruction at all — see `0031-arrays.md`.) Because `IrGenerator` never needed to know a parameter's type for any of this, the array-to-slice *conversion* itself has to live one layer down, in `LlvmIrEmitter`, where type information already exists.

---

# `LlvmIrEmitter`: The Fat Pointer

`llvmType("slice<i32>")` → `"{i32*, i32}"` — an anonymous LLVM struct, no declaration needed (same "no named type, anonymous shape used directly at every reference site" approach arrays already use for `[N x T]`). `i32`, not `i64`, for the length field — consistent with Axea having exactly one integer width for indices and lengths anywhere in the language.

This was the one genuinely new piece of ground the whole feature needed — nothing in this codebase had ever passed an LLVM struct **by value** before (structs are always by pointer) — so before committing to the design, `extractvalue`/`insertvalue`/single-index `getelementptr` over a `{T*, i32}` parameter were hand-verified against the real `clang` toolchain in an isolated `.ll` file, at both `-O0` and `-O1`, matching this project's established discipline of checking real toolchain behavior before designing around an assumption (`0022`/`0028`/`0031` each document a toolchain surprise caught exactly this way). Both compiled and ran correctly.

**Function signature** — `slice<T>` params look like this, verified via `ax llvm-ir`:

```text
$ ax llvm-ir sum.ax
define i32 @sum({i32*, i32} %0) {
```

**The conversion, at the call site.** `IrCall` emission gained a per-argument check: if the callee's declared parameter type (tracked in a new `functionParamTypes_` map, mirroring the existing `functionReturnTypes_`) is `slice<...>` and the argument register's *inferred* type is an array pointer, the conversion is emitted inline, right before the `call` — a GEP down to a flat element pointer, then two `insertvalue`s building the `{T*, i32}` pair (the length comes directly from `arraySizeFromPointerType`, already available since an array's size is always statically known):

```text
$ ax llvm-ir examples/slices.ax
  %28 = getelementptr [3 x i32], [3 x i32]* %6, i32 0, i32 0
  %29 = insertvalue {i32*, i32} undef, i32* %28, 0
  %30 = insertvalue {i32*, i32} %29, i32 3, 1
  %31 = call i32 @sum({i32*, i32} %30)
  %32 = getelementptr [7 x i32], [7 x i32]* %20, i32 0, i32 0
  %33 = insertvalue {i32*, i32} undef, i32* %32, 0
  %34 = insertvalue {i32*, i32} %33, i32 7, 1
  %35 = call i32 @sum({i32*, i32} %34)
```

The same `@sum` called twice, once with a 3-element array and once with a 7-element one — `i32 3` and `i32 7` are the only things that differ between the two conversions. An argument whose inferred type is *already* `{T*, i32}` (the slice-to-slice forwarding case) skips this branch entirely — no double-wrapping, verified by a dedicated test asserting no `insertvalue` appears anywhere in a pure forwarding function.

**Indexing and `.length`.** `emitIndexGet`/`emitIndexSet` gained a slice-shaped branch (detected by the object type starting with `{`): `extractvalue` the pointer field, then a **single-index** GEP (`getelementptr T, T* %ptr, i32 %index`) — no `i32 0, i32 idx` two-index form, since a flat pointer isn't an aggregate the way `[N x T]*` is. `emitFieldGet` gained a matching slice+`"length"` branch: `extractvalue {T*, i32} %s, 1` directly, no GEP, no load — the length is already sitting in the by-value struct:

```text
$ ax llvm-ir zeroFirst.ax
define void @zeroFirst({i32*, i32} %0) {
entry:
  %1 = add i32 0, 0
  %2 = add i32 0, 0
  %3 = extractvalue {i32*, i32} %0, 0
  %4 = getelementptr i32, i32* %3, i32 %1
  store i32 %2, i32* %4
  ret void
}
```

---

# Interpreter

`struct SliceInstance { std::shared_ptr<ArrayInstance> backing; std::size_t length; };`, added to `Value`. `length` is stored explicitly rather than just reading `backing->elements.size()` at every use, to keep the "runtime, not compile-time" distinction real even though the two values always agree in this whole-array-only-conversion phase (no sub-slicing yet to make them diverge).

The array→slice conversion happens right after evaluating each call argument, before binding parameters: if the callee's declared parameter type starts with `"slice<"` and the evaluated value holds a `shared_ptr<ArrayInstance>`, it's wrapped in a fresh `SliceInstance`; an argument that's already a `SliceInstance` (forwarding) passes through unchanged. `IndexExpr`, `IndexAssignStmt`, and `.length` all read through a small shared helper (`asIndexable`) that returns a pointer into the right backing storage plus the right effective length, regardless of whether the underlying value is an `ArrayInstance` or a `SliceInstance` — avoiding three separate near-duplicate implementations of the same bounds-check-then-access logic.

Because a slice literally aliases the same `ArrayInstance` its backing array does, write-through composes for free: `zeroFirst`'s `values[0] = 0` mutates the shared instance directly, visible to the caller the instant the call returns — the exact same reference-semantics guarantee that already made struct field-assignment and array index-assignment write-through work.

---

# Worked Example

`examples/slices.ax`:

```ax
sum(read values: slice<i32>) -> i32
{
    total = 0
    for v in values { total = total + v }
    return total
}

zeroFirst(write values: slice<i32>)
{
    values[0] = 0
}

small = [1, 2, 3]
large = [1, 2, 3, 4, 5, 6, 7]
smallSum = sum(small)
largeSum = sum(large)
cleared = zeroFirst(small)
firstAfterClear = small[0]
```

```text
$ ax run examples/slices.ax
small = [0, 2, 3]
large = [1, 2, 3, 4, 5, 6, 7]
smallSum = 6
largeSum = 28
cleared = ()
firstAfterClear = 0
$ ax llvm-ir examples/slices.ax | clang -x ir -O1 - -o out && ./out
small = [0, 2, 3]
large = [1, 2, 3, 4, 5, 6, 7]
smallSum = 6
largeSum = 28
cleared = ()
firstAfterClear = 0
```

Byte-for-byte identical — the same `sum` correctly handles a 3-element and a 7-element array through one function, and `zeroFirst`'s mutation through the slice is visible on `small` afterward, in both the interpreter and the compiled binary.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **No sub-slicing.** `array[a..b]` (the grammar's own `slice_suffix`, `header = bytes[0..20]` from `0005-type-system.md`) isn't implemented — a slice can only be constructed via the implicit whole-array conversion at a call boundary.
- **No re-slicing.** An existing slice can be forwarded unchanged to another slice parameter, but not narrowed into a shorter one.
- **Slice cannot be a return type, a local variable's declared type, or a struct field's type.** Explicitly rejected with a clear error at each of those three resolution sites, not merely unimplemented — this is what keeps `RegionChecker` correct without any changes (see "Why Parameter-Only" above).
- **No real lifetime tracking.** Nothing checks that a slice's backing array actually outlives the slice's use — consistent with this backend's broader stance of doing no real memory management at all (nothing is ever freed anywhere).
- **`IrMove` on a slice parameter is semantically imprecise** (a non-owning view doesn't really get "moved"), but has zero observable effect — `IrMove` is informational-only in Axea IR and never reaches LLVM emission.

---

# Guiding Rule

> A representation only needs to diverge from its closest existing analog exactly as much as its semantics genuinely require — and no more. A slice is "an array, but the size is a runtime field instead of part of the type"; every place that statement is literally true (indexing, `.length`'s *meaning*, capability inference, region ownership status once parameter-scoped), the existing array/struct machinery was reused untouched. The one place it *isn't* true — the actual bit layout, a two-word value instead of a single heap pointer — is the one and only place genuinely new code was needed, and it was verified against the real toolchain before being built on.
