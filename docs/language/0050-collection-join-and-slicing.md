# `.join()` and `array[a..b]`/`list[a..b]`: Slicing Generalized Beyond `str`

**Status:** Implemented
**Document:** `0050-collection-join-and-slicing.md`

---

# Motivation

`docs/language/Axea_Printing_Formatting.md`'s own "Collections" section shows both features
together:

```ax
numbers = [1, 2, 3, 4]
print(numbers[..2].join(","))
```

This phase implements exactly that: `.join(separator)` on an Array or `List<T>`, and the same
`[a..b]`/`[..b]`/`[a..]`/`[..]` range-slice syntax `docs/language/0045-str-slicing.md` already gave
`str`, now also accepted for Array/`List<T>` objects. Both are scoped to elements with a
well-defined text representation - `i32`/`bool`/`char`/`str`/`String`, the exact
`isTextRepresentable` set `docs/language/0049-printing-formatting.md` already established for
`print()`/interpolation - not arbitrary `T`; struct-typed elements are explicitly out of scope this
phase (see Design below for why).

`docs/language/0032-slices.md` had previously named `array[a..b]` sub-slicing "explicitly out of
scope" - but that scope call was about producing a `slice<T>` (a non-owning, parameter-only fat
pointer view). This phase sidesteps that restriction entirely by producing something else: a fresh,
owned `List<T>`, the same "real copy, not a zero-copy view" honesty `str` slicing already committed
to.

---

# Design: One Instruction, Widened by Object Type - and Why Struct Elements Are Out

**`IrStrSlice` (and its AST counterpart `StrSliceExpr`) is widened, not duplicated.** The parser
already builds this node for *any* `object[a..b]` shape with zero type information (types don't
exist yet at parse time) - `TypeChecker` was always the layer deciding whether `object` was
actually sliceable. Extending that acceptance from "str-coercible" to "str-coercible, or an
Array/`List<T>` of a text-representable element" is a straight generalization of the same dispatch
`IndexExpr`'s own `isIndexable` already established for indexing across Array/Slice/List/Deque.
Both AST/IR node names keep their original "Str" naming despite the widened scope - the codebase's
own precedent (`IrIndexGet` was never renamed to reflect the four collection kinds it already
spans) is to dispatch by the object's own resolved type inside one shared instruction rather than
multiply near-identical node types, and a doc-comment update carries the honesty burden the name
itself no longer fully does.

**Why struct-typed elements are out of scope.** Structs in this backend are heap pointers
(`%StructName*`), not inlined values. A slice's "malloc a new buffer, copy `length` elements"
implementation copies whatever bit pattern each element slot holds - for `i32`/`bool`/`char` that's
a real value copy (safe, no aliasing question), but for a struct-typed element it would copy the
*pointer*, producing a new `List<Point>` whose elements alias the very same `Point` instances the
source array/list still holds. Whether that's the right default, and what `RegionChecker` would
need to say about it (mirroring the existing `elementStructType`-tracking machinery
`List<T>.pop()`/`.get()` already use for exactly this aliasing question), is a real design
question this phase doesn't take on - restricting to `isTextRepresentable` elements sidesteps it
entirely, since none of those five types carry `RegionChecker`-tracked ownership at all. Documented
here as a deliberate scope line, not an oversight.

**`.join()` builds a `String` the same way interpolation does.** Both go through the identical
`stringifyValue`/`isTextRepresentable` machinery `docs/language/0049-printing-formatting.md`
built - `.join()` is, in effect, "interpolate every element of a collection with a separator
between them" using the exact same per-type stringification.

---

# Parsing: Zero Changes

Both features parse for free. `object[a..b]` already built a `StrSliceExpr` regardless of
`object`'s shape (`Parser::parsePostfix`, unchanged since `0045-str-slicing.md`) - a fixed-size
array literal, a `List<T>` variable, and a `str` all take the identical code path today.
`object.join(separator)` is an ordinary `MethodCallExpr` - the same generic
`identifier-dot-identifier-parens` grammar `.push()`/`.parse()`/every other method call already
uses, with `"join"` unambiguous by name alone (no other collection method shares it), so no new
grammar and no disambiguation logic (unlike `push`/`pop`, which need `isStackExpr`/etc. to pick
between colliding method names on different collection kinds).

---

# Type Checking: Widened Acceptance, One New Method

`StrSliceExpr`'s check now branches three ways: str-coercible → `str` (unchanged); Array/`List<T>`
with a text-representable element → a fresh `arrayLikeType(TypeKind::List, ...)` of the same
element type; anything else (including struct-element Array/List, and any other type entirely) →
a clear error. `.join(separator)` is checked before the per-collection-kind dispatch chain (the
same placement `.parse`/`.to_cstr` already established for checks that apply across more than one
`TypeKind`): requires an Array or `List<T>` with a text-representable element, exactly one
str-coercible argument, and always returns `String` (owned) - the same "runtime construction
implies OwnedString" rule `InterpolatedStringExpr` already follows.

---

# Capability Checking / Region Checking: Zero New Code

Both operations are read-only (neither is in `CapabilityChecker`'s existing mutating-method
allowlist, so both already default to read-only correctly with no new entry needed) and both
always produce a fresh, independently-owned result (`RegionChecker`'s existing "method call result
is Owned by default" rule, and `StrSliceExpr`'s own existing unconditional-Owned handling, already
cover them) - exactly the same "the generic default was already right" outcome
`docs/language/0048-ffi.md`'s `.to_cstr()` and `0049`'s `print`/interpolation both already hit.

---

# `IrGenerator`: One New Instruction, Zero Changes to the Existing One

`StrSliceExpr` lowering is untouched - it already only ever moves `object`/`start`/`end` registers
around, with no type-specific logic of its own. `.join(separator)` lowers to a new `IrJoin{object,
separator}`, checked unconditionally (unambiguous by name, mirroring `parse`/`to_cstr`'s own
placement) before the per-kind method dispatch chain.

---

# `LlvmIrEmitter`: A Shared View, Element-Wise Copy, and a Two-Phase Join Loop

**`resolveIndexableView`** is the one new shared sub-computation: given an object register, returns
`{elementType, dataPtrRef, lengthRef}` - a flat element pointer plus a length (a runtime `i32` load
for `List<T>`'s own length field, a compile-time literal for a fixed array's own static size).
Shared by `emitStrSlice`'s new Array/List branch and `emitJoin` - the one thing both operations
genuinely need in common, mirroring `resolveStrPtr`'s own "small shared sub-computation, not a
whole operation" precedent.

**`emitStrSlice`'s Array/List branch** is a real, separate implementation from the `str` branch
below it - not threaded through the same byte-indexed logic. It mallocs `length *
sizeof(elementType)` bytes (the same null-pointer-GEP `sizeof` idiom `emitListPush` already uses
for its own growth allocation), copies `length` elements via an **element-wise** GEP loop (not
byte-indexed - LLVM computes the correct stride for `elementType` automatically, so the exact same
loop shape works whether `elementType` is `i32`, `i8*`, or `i24`), then builds a fresh `List<T>`
header exactly like `emitListNew` does, just storing the runtime `lengthReg` instead of a literal
`0`.

**`emitJoin`** builds a local `Buffer` inline (no `IrBufferNew` instruction backs it - it has no
Axea-level variable, so its final `{length, data}` fields are read directly rather than going
through a real `IrBufferFinish`). To avoid a per-iteration `i != 0` branch deciding whether to
prepend the separator, the loop is restructured instead: append element 0 unconditionally (skipped
entirely, via a leading `icmp eq length, 0` guard, if the collection is empty), then loop from
index 1 appending `separator` followed by each further element. Each element is stringified via
**`stringifyValueOfType`/`resolveStrPtrOfType`** - new raw-ref-parameterized siblings of
`stringifyValue`/`resolveStrPtr` (the originals are now thin wrappers over these), needed because a
loop-loaded element has no Axea IR register of its own to call `typeOf`/`ref` against, only a
freshly GEP+loaded SSA value this backend mints on the spot. Appending itself goes through
**`appendTextToBuffer`**, a new raw-ref-parameterized primitive (structurally the same grow-check/
copy-loop/null-terminate shape `emitBufferAppend`/`emitBufferAppendValue` already use, kept
separate from both per this codebase's own "separate over shared" convention for whole operations -
already applied to keep those two themselves distinct).

---

# Interpreter: `asIndexable` and `toString`, Already General Enough

Both features cost almost nothing new here. `StrSliceExpr` evaluation now checks `asIndexable`
first (Array/List, via the interpreter's existing shared indexing helper) before falling back to
`asStrContent` - a slice of an Array/List builds a fresh `ListInstance` via `std::vector::assign`
over the appropriate sub-range, with the exact same runtime bounds check (`start < 0 || end >
length || start > end`) `str` slicing already performs (the "interpreter checks, compiled code
doesn't" split every other indexing operation here already follows). `.join(separator)` loops over
`asIndexable`'s elements, concatenating `toString(element)` - the interpreter's own universal
stringifier, already generic over every printable type since `docs/language/0049-printing-formatting.md` - with `separator` between them, wrapped into a fresh `StringInstance`.

---

# Worked Example

`examples/collection_join_and_slicing.ax`:

```ax
describe(numbers: [i32; 4]) -> String
{
    firstTwo = numbers[..2]
    comma = ", "
    plus = "+"
    return "{numbers.join(comma)} (first two: {firstTwo.join(plus)})"
}

report = describe([10, 20, 30, 40])

names = List<str>()
pushed1 = names.push("ada")
pushed2 = names.push("grace")
pushed3 = names.push("linus")

separator = ", "
allNames = names.join(separator)
ampersand = " & "
tailNames = names[1..].join(ampersand)
```

```text
$ ax run examples/collection_join_and_slicing.ax
report = 10, 20, 30, 40 (first two: 10+20)
...
allNames = ada, grace, linus
tailNames = grace & linus
$ ax llvm-ir examples/collection_join_and_slicing.ax | clang -x ir -O1 - -o out && ./out
# byte-for-byte identical (also re-verified at -O0)
```

(The separators are bound to variables rather than written as string literals directly inside
`{...}` - see Known Imprecision below for why.)

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **Struct-typed elements cannot be sliced or joined.** See Design above - a real, documented
  scope line about element aliasing through a bulk copy, not an oversight.
- **No re-slicing distinction from `slice<T>`.** `array[a..b]`/`list[a..b]` always produces an
  owned `List<T>`, never the non-owning `slice<T>` fat-pointer view `docs/language/0032-slices.md`
  defines - the two features remain entirely separate, and `slice<T>` itself is untouched (still
  parameter-only, still no re-slicing of its own).
- **No compile-time bounds checking**, even for a literal range against a fixed-size array's own
  known length (e.g. `[1,2,3][..10]` is not statically rejected) - matches `str` slicing's own
  existing "no static check, runtime check only in the interpreter" precedent exactly.
- **`.join()`'s separator is str-coercible only** (`str`/`String`) - it cannot itself be, say, a
  `char`.
- **Nested string literals inside an interpolation span still don't work**
  (`"{x.join(", ")}"` fails to parse) - a pre-existing gap from
  `docs/language/0049-printing-formatting.md`, newly relevant here since `.join()`'s separator is
  so often itself a short string literal. Workaround demonstrated in the worked example above:
  bind the separator to a variable first.

---

# Guiding Rule

> Both features cost far less than their surface area suggests, because almost every layer had
> already generalized past `str` once before: `IndexExpr`/`IrIndexGet` already dispatch by object
> type across four collection kinds, `CapabilityChecker`/`RegionChecker`'s defaults were already
> correct for any new read-only, always-fresh operation, and `stringifyValue`'s per-type
> dispatch already existed from `print()`/interpolation. The only genuinely new work was at the
> `LlvmIrEmitter` layer, where a loop-loaded value with no Axea IR register of its own needed a
> raw-ref-parameterized sibling of two existing register-based helpers - a small, honest
> refactor (thin wrappers, not rewrites) rather than a workaround. The one real design decision -
> what to do about struct-typed elements - was resolved by narrowing scope to the same
> `isTextRepresentable` set already established elsewhere, not by inventing new aliasing rules.
