# Unsafe

**Status:** Draft
**Document:** `0019-unsafe.md`

---

## 2026 Update: `null` Is Implemented

This doc's own "Open Questions" section below asked whether a `null` literal should exist. It now
does — added as a prerequisite for porting `LinkedList<T>` out of the compiler (see
`docs/language/0036-linked-lists.md`'s own "2026 Update"): the first retired collection whose own
node type genuinely needs to represent "no node here," which nothing in the language could
express before this.

`null` is a bare, context-independent literal (mirrors `Optional<T>`'s own `None`, but unlike
`None`, type-checks against *any* `*T` in every position a pointer value is expected — assignment,
function-call arguments, `return`, struct-literal field initializers, and `==`/`!=` comparison
against a real pointer in either direction), not a lexer keyword. Represented at runtime as a
pointer whose backing arena is absent (`arena == nullptr`, the sentinel state).

**One deliberate departure from this doc's own recommendation below**: dereferencing `null`
*does* get special-cased detection, at least in the interpreter — it throws a clear "dereferenced
a null pointer" error rather than being silently undefined behavior. This matches the "interpreter
catches more than compiled UB does" contract every other empty-collection/out-of-bounds operation
in this codebase already has (`List<T>.pop()`'s own bounds check being the closest precedent), not
a departure from `unsafe`'s "zero extra static checking" stance — the check is a runtime one, and
the compiled backend still emits a bare `getelementptr`/`load` off address `0` with no check at
all, so a null-deref bug still reads as ordinary UB in compiled code, exactly as this doc
originally proposed.

Pointer equality (`==`/`!=` between two `*T` values, including against `null`) is now genuine
structural comparison — `(arena, offset)` value equality, not C++ object identity — fixing a
latent bug this port's own research surfaced: nothing in this codebase ever compared two pointer
values before `LinkedList<T>`'s `while ptr != null`-shaped traversal needed it, so the gap had
gone untested.

---

## Motivation

`docs/language/0012-memory-model.md` already states the goal: *"Raw pointers require explicit
unsafe contexts."* Neither exists yet. Today, the **only** way any Axea program allocates heap
memory is implicitly, through compiler-intrinsic constructs (struct/array literals, the ten
built-in collection types) that emit `malloc`/`free` themselves inside the compiler
(`compiler/llvmir/LlvmIrEmitter.cpp:11887,11891` declares `@malloc`/`@free`, called from ~35
sites the compiler itself controls). There is no user-reachable allocation primitive at all: `cstr`
(`compiler/sema/TypeChecker.hpp:112-119`) is deliberately opaque and non-dereferenceable, and
`extern c` can already declare `malloc`/`free` (see `docs/language/0048-ffi.md`), but with no
pointer type to store the result in, calling them from Axea source is useless.

This blocks `docs/language/0006-generics.md`'s own motivating goal: a future
`std/collections.ax` cannot build a growable buffer itself without some way to allocate and index
raw memory. It also blocks any FFI use case that needs to read or write through a pointer rather
than just pass an opaque `cstr` through unchanged. This doc is the second (and last) prerequisite
`0006-generics.md` names before collections can move out of the compiler.

## Goals

-   A raw pointer type, `*T`, for any concrete `T` (primitives, user structs, and - once
    `docs/language/0006-generics.md` lands - concrete generic instantiations like `*Box<i32>`). A
    plain address: no ownership tracking, no automatic freeing.
-   `unsafe { ... }` blocks: the only place a raw pointer may be dereferenced, indexed, or have
    arithmetic performed on it. Outside `unsafe`, a `*T` value may still be stored, passed around,
    and handed to `extern c` functions - it just can't be dereferenced.
-   Pointer arithmetic inside `unsafe`: `ptr + i` / `ptr - i`, offsetting by `i` whole elements of
    `T` (not raw bytes) - the usual systems-language convention.
-   Dereference/assignment through a pointer inside `unsafe`: `*ptr` (read), `*ptr = value`
    (write).
-   `&value`, taking the address of an existing addressable value, producing `*T` - always legal
    outside `unsafe` (creating a raw pointer is inherently safe; only *using* it unsafely needs the
    block), mirroring the safe/unsafe split most systems languages with this feature already make.
-   `extern c malloc(size: i64) -> *i32` / `extern c free(ptr: *i32)`-shaped declarations become
    meaningful for the first time, letting Axea source - specifically, a future
    `std/collections.ax` - allocate and free its own buffers.

## Non-Goals

-   No borrow-checked lifetime tracking for raw pointers. Once inside `unsafe`, the programmer is
    fully responsible for validity - the same "unsafe superpowers, zero extra static checking"
    model most languages with this escape hatch use, not a weaker halfway design.
-   No pointer-to-generic-parameter restriction beyond what `docs/language/0006-generics.md`
    already provides - `*T` composes with a generic `T` for free once that doc lands, no separate
    mechanism needed.
-   No `unsafe fn` (marking an entire function's body unsafe) this phase - only `unsafe { }`
    blocks. Simpler to start with; whole-function unsafe is compatible future work if a std module
    wants to avoid re-wrapping every pointer operation individually.
-   No memory-safety guarantees whatsoever inside `unsafe`. Use-after-free, double-free,
    out-of-bounds access, and misaligned access are all the programmer's responsibility.
-   Does not change how any *existing* Axea construct (struct/array/collection allocation) is
    compiled - those keep using their own compiler-emitted `malloc`/`free` exactly as today.
    `unsafe`/`*T` is a purely additive capability for user code.

## User-Facing Syntax

```ax
extern c malloc(size: i64) -> *i32
extern c free(ptr: *i32)

grow(count: i32) -> *i32
{
    bytes: i64 = count * 4
    ptr = malloc(bytes)
    unsafe
    {
        *ptr = 0
        *(ptr + 1) = 42
    }
    return ptr
}

buf = grow(4)
first = unsafe { *buf }
free(buf)

x = 5
p = &x
xViaPtr = unsafe { *p }
```

## Semantics

`*T` is a distinct pointer type carrying `T` as its pointee, represented as a bare address (an
`i8*`/`iN*` in the LLVM backend; see Compiler Implementation for the interpreter's own
representation).

`&value` is legal in any context (safe) and produces a `*T` aliasing `value`'s storage. The
pointee must already be an addressable, heap- or stack-resident lvalue (a local variable, a
struct field, or the pointee of another pointer) - taking the address of a temporary is a compile
error.

`*ptr` (read) and `*ptr = value` (write) are legal **only lexically inside an `unsafe { }`
block**; using either outside one is a compile error naming the specific operation. Pointer
arithmetic (`ptr + i`, `ptr - i`) is likewise legal only inside `unsafe { }`, and is
element-scaled (offsetting by `i * sizeof(T)` bytes transparently - the user only ever sees
whole-`T`-element offsets, the same convention C uses).

`unsafe { }` is a plain block expression, like `if`/`loop` bodies already are - usable as a
statement, or, if its last expression has a value, as an expression (`first = unsafe { *buf }`
above).

Nothing about `unsafe` disables ordinary capability/region checking for anything *outside*
raw-pointer operations within the block - calling an ordinary function or mutating an ordinary
struct field inside an `unsafe` block is checked exactly as if the block weren't there. `unsafe`
only widens what syntax is legal for `*T` operations specifically; it changes zero rules for every
other value kind.

## Type Rules

-   `*T` is well-formed for any complete, already-declared `T` (primitive, struct, or a concrete
    generic instantiation). No restriction on which `T` may be pointed to.
-   `&expr` requires `expr` to resolve to an lvalue (`NameExpr`, `FieldExpr`, or a dereferenced
    pointer `*p`); its type is `*T` where `T` is `expr`'s own type.
-   `*ptrExpr` (deref) requires `ptrExpr`'s type to be `*T` for some `T`; the result type is `T`.
    Applying `*` to a non-pointer type is a compile error.
-   `ptr + i` / `ptr - i` require `ptr: *T` and `i: i32`/`i64`; the result type is `*T`, unchanged
    from `ptr`'s own type.
-   Every raw-pointer-specific operation (`*ptrExpr` read/write, `ptr +/- i`) additionally requires
    lexical enclosure inside an `unsafe { }` block - checked as a separate rule from ordinary type
    well-formedness, the same way capability rules are already checked as their own pass distinct
    from type-correctness.

## Ownership & Capability Rules

-   `*T` is **not** move-tracked the way a struct/enum value is (`RegionChecker`'s existing
    `structType`-based tracking, reused by `docs/language/0006-generics.md`) - a raw pointer is a
    plain copyable address, the same as any other primitive (`i32`, `bool`, ...), not a struct.
-   `*T` does not participate in the existing safe-reference capability inference (`read`/`write`/
    `take` parameter capabilities) - a `*T` parameter is always passed as a bare value, with no
    capability annotation accepted or required, mirroring how `cstr` parameters already work
    (`ExternDecl::params` already documents that extern params "take no read/write/take prefix").
-   Because `unsafe` explicitly opts out of the inferred-safe-reference model for the operations it
    gates, `RegionChecker` performs no escape-analysis or aliasing reasoning inside an `unsafe`
    block for pointer operations - the programmer, not the checker, is responsible for a
    returned/stored `*T` remaining valid. This is the one deliberate, load-bearing hole in
    `docs/language/0012-memory-model.md`'s "compile-time memory safety, no runtime borrow checker"
    goal, and it is the entire point of the feature: an explicit, lexically-scoped, grep-able
    escape hatch rather than an implicit one.

## Compiler Implementation

-   **AST:** reuses the already-declared-but-dead `TypeKind::Pointer`
    (`compiler/sema/TypeChecker.hpp:96`) instead of adding a new enumerator; `Parser::parseTypeNameAtom`
    gains a `*` prefix production (`*i32`, `*Point`, ...). New `AddressOfExpr` (`&expr`),
    `DerefExpr` (`*expr` - both as an lvalue read and, via the existing `FieldAssignStmt`-shaped
    handling, an assignment target), and `UnsafeBlockExpr` (wrapping the existing block-expression
    shape - no new statement kinds needed inside it) AST nodes.
-   **Parser:** `unsafe { ... }` reuses the existing block-expression parsing (`if`/`loop` bodies)
    verbatim under a new leading keyword. `*expr` reuses the existing prefix-unary-operator
    parsing slot (alongside unary `-`); type-position `*T` and expression-position `*expr` are
    never ambiguous, the same way every other type-position/expression-position pairing in this
    grammar already isn't.
-   **TypeChecker:** threads a `bool insideUnsafe` flag through `checkExpr`, set true while
    checking an `UnsafeBlockExpr`'s body and restored after - the same shape as any other scoped
    checker flag in this codebase. `DerefExpr`/pointer-arithmetic checking consults this flag and
    raises the new diagnostic (see Diagnostics) when false.
-   **Interpreter:** represents a `*T` as a `PointerInstance` - a `shared_ptr<vector<Value>>`
    arena plus an integer offset, since the interpreter has no real flat address space to point
    into. `malloc`/`free` are hand-implemented against a fresh, appropriately-sized arena (one
    `std::vector<Value>` per allocation), the same "fake the real C runtime call the interpreter
    can't literally execute" pattern `extern c`'s existing small hand-implemented allowlist in
    `Interpreter.cpp` already uses. `&name` (Milestone 2, address-of a local) reuses this exact
    same shape rather than a second representation: it boxes the named local into a
    **single-element** arena and points a `PointerInstance` at it with `offset` always `0` - every
    existing deref/assign/pointer-arithmetic/bounds-check code path already handles it unmodified,
    including getting "arithmetic past a singleton is a runtime error" for free.
-   **LlvmIrEmitter:** `*T` lowers to a literal LLVM `T*` (or `i8*` for an untyped pointer);
    `&expr`/`*expr`/pointer arithmetic lower to literal `getelementptr`/`load`/`store` IR - the
    only new LLVM constructs this doc introduces, since every existing allocation was previously
    compiler-managed end to end. `malloc`/`free` reuse the exact same
    `declare i8* @malloc(i64)` / `declare void @free(i8*)` externs the backend already emits for
    its own intrinsic collection machinery (`compiler/llvmir/LlvmIrEmitter.cpp:11887,11891`) -
    user code calling `malloc` directly shares the identical declaration, not a second one.

## Diagnostics

```
error: dereferencing a raw pointer requires an 'unsafe' block
error: pointer arithmetic requires an 'unsafe' block
error: cannot take the address of a temporary value
error: '*' requires a pointer type, got 'i32'
```

## Optimization Opportunities

None specific to this phase. `unsafe` is a checking-time gate only - it emits no different code
than the equivalent operation would if the check didn't exist. Alias analysis and other
pointer-aware optimizations are left entirely to LLVM's own existing optimization passes.

## Examples

```ax
extern c malloc(size: i64) -> *i32
extern c free(ptr: *i32)

sumFirstFour(ptr: *i32) -> i32
{
    total = 0
    unsafe
    {
        total = total + *ptr
        total = total + *(ptr + 1)
        total = total + *(ptr + 2)
        total = total + *(ptr + 3)
    }
    return total
}

buf = malloc(16)
unsafe
{
    *buf = 10
    *(buf + 1) = 20
    *(buf + 2) = 30
    *(buf + 3) = 40
}
total = sumFirstFour(buf)
free(buf)
```

## Alternatives Considered

-   **No `unsafe` keyword at all - allow raw-pointer deref/arithmetic anywhere `*T` is in scope.**
    Rejected: loses the "grep for `unsafe` to find every place memory safety is the programmer's
    responsibility" property that is the entire value of the feature, and contradicts
    `docs/language/0012-memory-model.md`'s own explicit "raw pointers require explicit unsafe
    contexts" goal.
-   **`unsafe fn` instead of `unsafe { }` blocks.** Rejected for this phase as coarser-grained than
    necessary - a function doing one unsafe pointer bump alongside otherwise-ordinary logic would
    lose checking for its *entire* body, not just the pointer operation. Left as compatible future
    work rather than a replacement.
-   **Bounds-checked "fat pointers" (pointer + length) instead of bare addresses.** Rejected: this
    reintroduces exactly the safety net `unsafe` exists to opt out of, and duplicates what
    `slice<T>` (already implemented, `docs/language/0032-slices.md`) provides for the safe case.
    `*T` is deliberately the raw, unchecked complement to `slice<T>`, not a competitor to it.

## Open Questions

-   Should `*T` support pointer-to-pointer (`**T`) from day one? Recommendation: yes, for free -
    nothing in the type-rule or lowering design above is arity-limited; `**T` is just `*T` where
    `T = *U`.
-   ~~Should there be a `null` literal...~~ **Resolved, see this doc's own "2026 Update" above** -
    `null` is implemented, type-checks against any `*T`. The interpreter throws a clear error on
    dereference (one deliberate departure from the recommendation below); the compiled backend
    still emits ordinary unchecked `getelementptr`/`load` off address 0, undefined behavior on
    deref exactly as originally proposed.

## Future Work

-   `unsafe fn` (see Alternatives Considered).
-   Once this doc and `docs/language/0006-generics.md` both land, a real `std/collections.ax`
    becomes possible - `List<T>` reimplemented as an ordinary generic struct wrapping a
    `*T`-backed buffer grown via `malloc`/`free` inside `unsafe` blocks, the ten
    compiler-intrinsic collection types retired in favor of genuine Axea source, tracked in
    `docs/language/0023-standard-library.md`.
