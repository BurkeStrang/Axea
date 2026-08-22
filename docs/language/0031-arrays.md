# Fixed-Size Arrays

**Status:** Implemented
**Document:** `0031-arrays.md`

---

# Motivation

`docs/language/0029-collections.md` is a draft sketching a full collection library — `List<T>`, `Map<K, V>`, `SortedSet<T>`, a sorting DSL, comprehensions, set algebra — that assumes infrastructure this compiler didn't have: generics (`0006-generics.md` is a completely empty draft), an array type, indexing syntax, closures, and operator overloading. Implementing that document as written is several roadmap phases at once.

Scoped down to the smallest real, useful slice: **fixed-size arrays**, `[T; N]`, exactly matching the sketch `0001-syntax.md` and `0002-grammar.md` already had on file (`array_type = "[" type ";" expression "]"`) — array literals, indexing, `.length`, index-assignment, and `for`-in iteration over an array's elements. No generics needed: both `T` and `N` are written out at every declaration site, so nothing here is polymorphic over element type or size.

**Explicitly out of scope**, documented at the end of this file, not silently missing: arrays as function return types, array-typed struct fields, the `[value; count]` repeat-literal form, multi-dimensional arrays, `index++`/`index--`, and any bounds checking in the LLVM backend.

---

# Design: Arrays Mirror `struct`, Not a New Category

Every new AST/IR node here is a direct sibling of an existing struct-related one, and every checker pass gets the same new case mirroring its existing `FieldExpr`/`FieldAssignStmt`/`StructLiteralExpr` handling — because a fixed array is, architecturally, exactly what a struct already is: a heap-allocated (`malloc`), reference-semantics (`std::shared_ptr<ArrayInstance>` in the interpreter, mirroring `std::shared_ptr<StructInstance>`) value, capability/region-checked as a heap/pointer type. `object[index]` is `IndexExpr`, sibling to `FieldExpr`; `object[index] = value` is `IndexAssignStmt`, sibling to `FieldAssignStmt`; `[e1, e2, ...]` is `ArrayLiteralExpr`, sibling to `StructLiteralExpr`. Nothing new was invented at the architecture level — the whole feature is "give struct's existing shape a second, anonymous instantiation."

Types stay strings everywhere they already were (`Param.type`, `Field.type`, `declaredType`, `returnType`): an array type is just the canonical string `"[i32;4]"` (no spaces), produced by one new parser helper, `Parser::parseTypeName()`, which replaced every previous `expect(TokenKind::Identifier, "expected ... type")` call site. `"User"` already threaded through the whole pipeline as an opaque string before this; `"[i32;4]"` does the same now.

---

# Parsing: One New Type-Name Grammar, Two New Postfix Forms

```text
$ ax tokens x.ax          # source: x: [i32; 4] = [1, 2, 3, 4]
1:1  Identifier    x
1:2  Colon         :
1:4  LeftBracket   [
1:5  Identifier    i32
1:8  Semicolon     ;
1:10 Integer       4
1:11 RightBracket  ]
1:13 Equal         =
1:15 LeftBracket   [
1:16 Integer       1
1:17 Comma         ,
1:19 Integer       2
1:20 Comma         ,
1:22 Integer       3
1:23 Comma         ,
1:25 Integer       4
1:26 RightBracket  ]
```

`[`, `]`, `;` (`LeftBracket`, `RightBracket`, `Semicolon`) are new single-character tokens, no lookahead needed (mirrors `LeftBrace`/`RightBrace`). `parseTypeName()` consumes either a plain `Identifier` (unchanged for `i32`, `User`, ...) or `[ Identifier ; Integer ]`, returning the canonical no-spaces string. `parsePrimary` gained a `LeftBracket` case for array literals; `parsePostfix`'s existing `.field` loop gained a `[index]` case alongside it, so `a[i].field`, `a.items[i]`, and `a[i][j]`-shaped chains (the last one meaningless without nested arrays, but syntactically free) all fall out from the same loop `.field` chaining already used.

`examples/arrays.ax`'s `bumpFirst`, verified via `ax ast`:

```ax
bumpFirst(values: [i32; 4]) -> i32
{
    values[0] = values[0] + 1
    return values[0]
}
```

```text
$ ax ast arrays.ax
Function(bumpFirst)
  Param(values: [i32;4])
  Block
    IndexAssign
      Name(values)
      Integer(0)
      Binary(Plus)
        Index
          Name(values)
          Integer(0)
        Integer(1)
    Return
      Index
        Name(values)
        Integer(0)
```

`values[0] = values[0] + 1` parses to `IndexAssignStmt` directly — mirrors how `parseBlock`'s existing `match(Equal)` branch already special-cased `NameExpr` vs. `FieldExpr` for `x = ...` vs. `obj.field = ...`; it now special-cases `IndexExpr` too.

---

# `for`-in Grows a Second Form: Iterating an Array's Elements

`0030-for-loops.md` shipped `for i in a..b` as pure syntactic sugar over `while`. `parseFor` parsed a range unconditionally after `for x in`; this phase changes that to a lookahead: parse the first expression, then check for `..`. If present, take the existing range-desugaring path unchanged. If absent, treat the already-parsed expression as an *array* expression and take a new, parallel desugaring path:

```
{ __for<N>_arr = arr   __for<N>_i = -1
  while true
  {
    __for<N>_i++
    if __for<N>_i >= __for<N>_arr.length { break }
    v = __for<N>_arr[__for<N>_i]
    body...
  } }
```

Same increment-first/bound-check/`forceDefine` shape as the range form, for the identical `continue`-safety reason `0030-for-loops.md` already worked out — the only two differences are what the bound check compares against (`__for<N>_arr.length` instead of a plain end variable) and how the loop variable is bound (`__for<N>_arr[__for<N>_i]`, an `IndexExpr`, instead of the counter directly). `examples/arrays.ax`'s `sum`, verified via `ax ast`:

```ax
sum(values: [i32; 4]) -> i32
{
    total = 0
    for v in values
    {
        total = total + v
    }
    return total
}
```

```text
$ ax ast arrays.ax
Function(sum)
  Param(values: [i32;4])
  Block
    Assignment(total)
      Integer(0)
    ExprStmt
      Block
        Assignment(__for0_arr)
          Name(values)
        Assignment(__for0_i)
          Integer(-1)
        While
          Bool(true)
          Block
            Increment
              Name(__for0_i)
            ExprStmt
              If
                Binary(GreaterEqual)
                  Name(__for0_i)
                  Field(length)
                    Name(__for0_arr)
              Then
                Block
                  Break
              Else
                Block
            Assignment(v)
              Index
                Name(__for0_arr)
                Name(__for0_i)
            Assignment(total)
              Binary(Plus)
                Name(total)
                Name(v)
    Return
      Name(total)
```

---

# Type Checking: A Flat `Type`, Not a Recursive One

`TypeKind` gained `Array` (declared but never wired up before this phase). `Type` originally gained three fields, populated only when `kind == Array`:

```cpp
TypeKind elementKind{};
std::string elementStructName{};
int arraySize{};
```

**Originally deliberately flat, not recursive; superseded by `docs/language/0053-nested-generics.md`.** The obvious design would give an array `Type` a `std::shared_ptr<Type> elementType` for full generality (arrays of arrays, etc.) — rejected at the time, because `Type::operator==` is `= default`, and a defaulted comparison over a `shared_ptr` member compares *pointer identity*, not the pointee's contents. Two structurally-identical array types built independently (two separate calls to `resolveType("[i32;4]")`, say) would silently compare unequal, breaking every declared-type check, every call-argument check, every array-literal-element-unification check that relies on `Type::operator==` — a correctness bug that would only show up as a mysterious "type mismatch" on code that's obviously correct. So the fields were originally flat scalars (`elementKind`/`elementStructName`, not a nested `Type`), keeping the defaulted `operator==` correct with zero special-casing, and nested arrays were rejected outright. `docs/language/0052-optional.md`'s own follow-up found this flat representation silently corrupting `List<Optional<i32>>` (a *different* kind entirely, but the identical representation), and `0053-nested-generics.md` fixed it properly: `elementKind`/`elementStructName` were replaced with a single `std::string elementTypeName` (`arraySize` stayed, still `Array`-only) — a string sidesteps the exact `shared_ptr`-identity problem above (`std::string::operator==` is already structural) while still supporting arbitrary nesting, the same representation `Map<K,V>`/`Set<T>` had already been using since `docs/language/0034-maps-and-sets.md`.

`resolveType` recognizes the `"[elem;N]"` shape (a leading `[`) before falling through to the existing primitive/struct lookup, parsing `elem` through one more (non-array) call to itself. `checkExpr` gained cases for `ArrayLiteralExpr` (unify every element's type to the first, same rule as `if`/`else` branch unification; empty literals are rejected — this architecture has no bidirectional/expected-type inference to fall back on) and `IndexExpr` (object must be `Array`, index must be `i32`); `checkFieldType` gained an `Array` branch for `.length` (returns `i32`; any other field name throws a "did you mean 'length'?" error); `checkStmt` gained `IndexAssignStmt`, mirroring `FieldAssignStmt`.

**A literal index gets a real compile-time bounds check**, not just a runtime one: when `index->index` is itself an `IntegerExpr`, `checkExpr`/`checkStmt` range-check it against the array's `arraySize` right then, before anything runs:

```text
$ ax run bad.ax          # x: [i32; 3] = [1, 2, 3]  y = x[3]
error: array index 3 out of bounds for array of size 3
```

A non-literal index (`x[i]`) isn't checked here — only the interpreter checks it at runtime (see below); the LLVM backend doesn't check it at all (see "Out of Scope").

---

# `.length` Is Zero-Cost: Constant-Folded, Not a Runtime Load

A fixed array's length is a compile-time constant by construction — loading it through a pointer at runtime, the way a struct field read works, would be correct but wasteful, and would violate the collections doc's own stated goal ("keep collections zero-cost where possible"). `IrGenerator::lowerExpr`'s `FieldExpr` case checks `field == "length"` first: if it can resolve the object's array size, it emits a plain `IrConstInt` directly, skipping any array-access instruction entirely.

`IrGenerator` doesn't carry a real type table (an established convention here — `isObviouslyStructTyped` is the same kind of approximate, self-contained re-derivation), so a small sibling helper, `arrayLengthOf`, resolves just enough shapes: a direct array literal (element count), an array-typed function parameter (size parsed from `Param.type`), or a name already recorded in a small parallel scope map (`IrScope::arrayLengths_`, alongside the existing register map) — populated wherever `lowerStmt`'s `AssignmentStmt` case binds a name whose value itself resolves through this same helper. Verified via `ax ir` on `count = numbers.length`:

```text
$ ax ir arrays.ax
TopLevel
  ...
  %4 = array.new [%0, %1, %2, %3]
  ...
  %9 = const.i32 4
```

`%9` is a bare `const.i32 4` — no `index.get`, no runtime access of `%4` at all. The same folding happens inside `sum`'s desugared `for`-in loop, where `values.length` (bound against `__for0_arr`, a function parameter) also folds to a constant bound check every iteration re-uses, rather than re-reading it from the pointer each time.

---

# IR and LLVM Backend: No Named Type, Anonymous `[N x T]` Everywhere

Three new Axea IR instructions, direct siblings of the struct ones:

```cpp
struct IrArrayNew final : IrInst { std::vector<int> elements; };
struct IrIndexGet final : IrInst { int object; int index; };
struct IrIndexSet final : IrInst { int object; int index; int value; };
```

`IrArrayNew` carries no element-type field at all (unlike `IrStructNew::typeName`) — by the time `LlvmIrEmitter::inferTypesInList` reaches an `IrArrayNew`, every one of its element registers has already been lowered (and therefore already type-inferred, appearing earlier in the same flat instruction list), so the element's LLVM type is read directly off `elements.front()`. `TypeChecker` already guarantees every element agrees, so the first one is representative — the same "first is representative" pattern `IrLoop`'s break-value typing already used.

Unlike `struct`, no named LLVM type declaration is emitted at all: `llvmType("[i32;4]")` computes `"[4 x i32]*"` directly from the canonical type string, every time it's needed, since LLVM's array type is anonymous. `emitArrayNew`/`emitIndexGet`/`emitIndexSet` reuse the exact same `malloc` + null-pointer-GEP-sizeof idiom `emitStructNew`/`emitFieldGet`/`emitFieldSet` already established — the only real difference is that an array's GEP index is a register reference (`ref(index, fctx)`, e.g. `%7`) rather than a struct field's constant index. `examples/arrays.ax`'s `bumpFirst`, verified via `ax llvm-ir`:

```text
$ ax llvm-ir arrays.ax
define i32 @bumpFirst([4 x i32]* %0) {
entry:
  %1 = add i32 0, 0
  %2 = add i32 0, 0
  %3 = getelementptr [4 x i32], [4 x i32]* %0, i32 0, i32 %2
  %4 = load i32, i32* %3
  %5 = add i32 0, 1
  %6 = add i32 %4, %5
  %7 = getelementptr [4 x i32], [4 x i32]* %0, i32 0, i32 %1
  store i32 %6, i32* %7
  %8 = add i32 0, 0
  %9 = getelementptr [4 x i32], [4 x i32]* %0, i32 0, i32 %8
  %10 = load i32, i32* %9
  ret i32 %10
}
```

**Top-level printing has no per-shape helper.** Structs get one named `@axea.print.<TypeName>` function per struct, since a struct name is a stable identifier to hang a function name off of. Arrays have no name — so rather than inventing one (`@axea.print.array.i32.4`, or similar), `emitMain`'s top-level-binding printer unrolls the whole print inline: since the array's size is already known at the point this loop runs (from the register's inferred type string), it emits `N` GEP+load+`printf` calls directly, comma-separated and bracketed, using the exact same per-element-type branching `emitStructPrintHelpers` already uses for struct fields (`i32` → `%d`, `bool` → a `select` between `"true"`/`"false"`, `str` → `%s`, a nested struct pointer → a recursive `@axea.print.Name` call). Verified end to end:

```text
$ ax run arrays.ax
numbers = [11, 20, 30, 40]
total = 100
bumped = 11
firstNow = 11
count = 4
$ ax llvm-ir arrays.ax | clang -x ir -O1 - -o out && ./out
numbers = [11, 20, 30, 40]
total = 100
bumped = 11
firstNow = 11
count = 4
```

Byte-for-byte identical, confirming `numbers[0]`'s mutation inside `bumpFirst` (a `write`-capability, borrowed, in-place-mutating parameter — inferred automatically, not declared) is correctly visible to every subsequent read of `numbers`, in both the interpreter and the compiled binary.

---

# Capability and Region Checking: Arrays Are Heap Types, Same as Structs

`CapabilityChecker`'s `rootParamIndex` (which strips `FieldExpr` layers to find the owning parameter for capability-raising) gained a parallel `IndexExpr` branch, so `values[i] = x` inside a function correctly attributes the write back to `values`' own parameter — `bumpFirst(values: [i32; 4])` above gets `write` inferred with zero explicit declaration, exactly like a struct field-assignment would.

`RegionChecker`'s `RegionInfo` gained a fourth field, `elementStructType` — kept separate from the existing `structType` (which means "this value itself is a struct", used by `FieldExpr`), since an array's own aliasing category (whether the *array* can be returned without `take`) is independent of whether its *elements* happen to be struct-typed (which only matters for chained access like `arr[i].field`). `checkFunction`'s borrowed/owned decision generalized from "is this param a struct" to "is this param a struct or an array" — both are heap-allocated, reference-semantics values that must not escape a function's return without an explicit `take`:

```text
$ ax regions bad.ax   # identity(values: [i32; 3]) -> [i32; 3] { return values }
error: function 'identity' cannot return 'values': parameter 'values' is borrowed and
does not outlive the call - declare 'take' if ownership should transfer
```

---

# Interpreter: `ArrayInstance`, a Direct Sibling of `StructInstance`

```cpp
struct ArrayInstance { std::vector<Value> elements; };
```

Always accessed via `std::shared_ptr<ArrayInstance>`, mirroring `std::shared_ptr<StructInstance>` — the same reference semantics that make struct field-assignment write through a shared instance make array index-assignment do the same. `evaluate`'s `IndexExpr` case is where the only *runtime* bounds check lives (a non-literal index can't be checked at compile time — see above):

```text
$ ax run bad.ax   # f(i: i32) -> i32 { x = [1,2,3]  return x[i] }  y = f(5)
error: array index 5 out of bounds for array of size 3
```

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **No bounds checking anywhere in the LLVM backend.** Mirrors the existing division-by-zero precedent exactly: the interpreter checks and throws a clear error; compiled code does not check at all, trusting the index the same way it already trusts a divisor is non-zero. A real systems-language backend would eventually need a `panic`/trap mechanism for this; out of scope here.
- **Arrays cannot be function return types**, and **struct fields cannot be array-typed** — not rejected outright by the parser (`parseTypeName` is used uniformly, so the syntax is accepted), but untested and not guaranteed correct. In particular, a struct field of array type would be silently mis-tracked by `RegionChecker` (its aliasing propagation only recognizes struct-typed fields, not array-typed ones) — a real but narrow gap, deliberately not fixed here since the feature itself is out of scope.
- **No `[value; count]` repeat-literal syntax** (`zeros: [i32; 1024] = [0; 1024]`, from the original collections draft) — only comma-separated list literals (`[0, 0, 0]`) are supported. Avoids a real grammar ambiguity with the array *type* syntax's own `;`.
- **No multi-dimensional / nested arrays** — `Type`'s array fields are flat by design (see above); `resolveType` explicitly rejects an array-of-arrays type string.
- **No `index++`/`index--`.** `values[i]++` is rejected at parse time with a clear "invalid increment/decrement target" error (the same path that already rejects `f(x)++`), rather than silently doing something wrong.
- **No first-class collection beyond fixed size.** `List<T>`, `slice<T>`, and everything else in `0029-collections.md` remain future work, now gated on real generics (`0006-generics.md`) rather than on arrays specifically.

---

# Guiding Rule

> A new value category doesn't need a new architecture if it's shaped like one that already exists. Every layer of this feature — parser, type checker, capability/region inference, interpreter, IR, LLVM backend — is a direct structural sibling of the corresponding struct-handling code, differing only in the details a heap-allocated, reference-semantics, GEP-indexable value actually needs to differ in (a runtime index instead of a compile-time field name; no named LLVM type since the shape is anonymous; a constant-fold for `.length` since arrays, unlike structs, carry their own size as part of the type itself). Where struct and array diverge, it's because the underlying semantics genuinely diverge - not because two independent designs happened to disagree.
