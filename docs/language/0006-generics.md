# Generics

**Status:** Draft
**Document:** `0006-generics.md`

---

## Motivation

`docs/language/0023-standard-library.md` lists `collections` as a planned standard-library
module, and `docs/roadmap.md` still shows `Standard Library` unchecked under Phase 7. Today,
every collection type (`List<T>`, `Map<K,V>`, `Set<T>`, `Stack<T>`, `LinkedList<T>`, `Deque<T>`,
`Queue<T>`, `PriorityQueue<T>`, `SortedMap<K,V>`, `SortedSet<T>`) is a compiler intrinsic:
hardcoded `TypeKind` enumerators (`compiler/sema/TypeChecker.hpp:95-119`), dedicated parsing
branches in `Parser::parseTypeNameAtom` (`compiler/parser/Parser.cpp:96-198`), dedicated C++
container types in the interpreter's `Value` variant (`compiler/interpreter/Interpreter.hpp`),
and, in the LLVM backend, ~40 hand-written `emitX*` functions spanning roughly
`compiler/llvmir/LlvmIrEmitter.cpp:5231-9760` implementing real hash-table/AVL-tree/heap
algorithms as literal LLVM IR text. Every one of the ten per-type design docs
(`0034-maps-and-sets.md`, `0036-linked-lists.md`, `0040-sorted-maps.md`, etc.) states the same
reason for this in its own "Known Imprecision" section: *"there's no user-definable method/`impl`
system in this language."*

`TypeKind::Generic` (`compiler/sema/TypeChecker.hpp:95`) already exists as an enumerator, but
`grep -rn "TypeKind::Generic" compiler/` turns up zero other uses - it's a vestigial placeholder,
not a real feature. The one real precedent this doc builds on is
`docs/language/0046-generic-methods.md`: `.method<T>()` call syntax already parses unambiguously
against `<`/`>` comparison via a fixed 4-token lookahead, for exactly one compiler-intrinsic
method (`.parse<T>()`). This doc generalizes that precedent from "one hardcoded method" to "real,
user-definable generic structs, functions, and methods" - the actual prerequisite for a future
`std/collections.ax` to exist as genuine Axea source rather than compiler intrinsics.

## Goals

-   Generic struct definitions: `struct Box<T> { value: T }`, with multiple type parameters
    (`struct Pair<A, B> { first: A  second: B }`), continuing the existing `Map<K,V>` naming
    precedent.
-   Generic free functions: `identity<T>(x: T) -> T { return x }`, with the type argument
    inferred from argument expressions where unambiguous, or given explicitly
    (`identity<i32>(5)`) using the exact call syntax `0046-generic-methods.md` already
    established.
-   Generic `impl` blocks/methods, extending `docs/language/0062-display-trait.md`'s existing
    static-dispatch `impl` mechanism to generic receivers: `impl<T> Box<T> { get(self) -> T { ... } }`.
    This is the piece that actually unblocks collections - without it, a generic struct could hold
    data but never have `.push()`/`.get()`-shaped methods of its own.
-   Monomorphization: every instantiated `(GenericName, ConcreteTypeArgs...)` pair gets its own
    compiled specialization, mirroring the "register once per shape" pattern the ten collection
    intrinsics already use (`registerMapInstantiation`/`registerSetInstantiation`/... in
    `compiler/llvmir/LlvmIrEmitter.cpp`).
-   Ownership/capability inference over generic type parameters applies uniformly, computed
    *after* substituting concrete types in - i.e. `Box<Point>.value` move-tracks like a plain
    `Point` field, `Box<i32>.value` doesn't, using the exact same `structType`-based rule
    `RegionChecker` already has, unchanged.

## Non-Goals

-   No trait bounds/constraints (`T: SomeTrait`) this phase. A type parameter accepts any
    concrete type; an instantiation whose body uses an operation the concrete type doesn't
    support fails at instantiation time with a diagnostic, not via a pre-checked bound. Matches
    `0046-generic-methods.md`'s own "ship the syntax + the one concrete use case" scoping.
-   No variance, no higher-kinded types.
-   No const generics (`Array<T, N>` with `N` a value parameter) - fixed-size arrays already use
    the separate, non-generic `[T; N]` syntax and are unaffected by this doc.
-   No dynamic dispatch/trait objects (`dyn Trait`) - matches `0062-display-trait.md`'s existing
    "no vtables anywhere" stance.
-   No specialization - no `impl Box<i32> { ... }` overriding a generic `impl<T> Box<T>` for one
    specific `T`.
-   Does **not** itself move `List`/`Map`/etc. out of the compiler. It only makes that migration
    *possible* as separate future work, once this doc and `docs/language/0019-unsafe.md` (needed
    for the allocation primitives a future `std/collections.ax` would use internally) both land.

## User-Facing Syntax

```ax
struct Box<T>
{
    value: T
}

impl<T> Box<T>
{
    get(self) -> T
    {
        return self.value
    }

    set(write self, newValue: T)
    {
        self.value = newValue
    }
}

struct Pair<A, B>
{
    first: A
    second: B
}

identity<T>(x: T) -> T
{
    return x
}
```

Usage:

```ax
b = Box<i32> { value: 5 }
n = b.get()
b.set(9)

strBox = Box { value: "hello" }   // T inferred from the field value - explicit only when
                                   // inference can't work, same rule as .parse<T>()

x = identity(42)                  // inferred from the argument
y = identity<i32>(42)             // explicit type argument, always legal, same syntax as .parse<T>()

p = Pair<i32, str> { first: 1  second: "one" }
```

## Semantics

Monomorphization is lazy, mirroring `registerMapInstantiation`: a generic struct/function/impl is
only compiled once a concrete instantiation is actually seen. Each distinct tuple of concrete
type arguments gets its own name-mangled instantiation, registered the first time it's used and
reused (not recompiled) on every subsequent use of the same concrete types.

Type-parameter substitution is purely structural: inside a generic body, `T` participates in
field types, parameter types, return types, and locally inferred types exactly as if the concrete
type had been written directly. The type checker performs substitution as a pre-pass over a
cloned copy of the generic AST, then runs its ordinary (already-existing, non-generic) checking
logic over that clone unchanged - no new type-checking algorithm, only a substitution step in
front of the one that already exists.

Because substitution happens before any other pass runs, field access, move-tracking, and
capability inference are computed on the *substituted* copy - `RegionChecker`'s existing
`structType`-based move-tracking already treats `Box<Point>.value` as struct-typed and
`Box<i32>.value` as not, with zero new region-checking logic; it already worked this way for
every concrete type in the program, and now also applies one level through a generic wrapper.

## Type Rules

-   `struct Name<T1, ..., Tn> { ... }` introduces `T1..Tn` as type parameters visible only inside
    its own field type positions.
-   `name<T1, ..., Tn>(params) -> RetType { body }` introduces `T1..Tn` as type parameters visible
    inside `params`, `RetType`, and `body`.
-   At a use site, type arguments are resolved either explicitly (`Name<ConcreteArgs>`) or by
    unifying against argument expressions' inferred types (for functions) or field-literal value
    types (for structs). Unification failure, or an argument list too ambiguous to infer from, is
    a compile error (see Diagnostics).
-   Every concrete instantiation is type-checked as an ordinary, fully concrete struct/function/
    impl - a generic definition is never checked "in the abstract" against `T`; only its
    instantiations are checked. This is the direct consequence of choosing monomorphization over
    erasure (see Alternatives Considered).
-   `impl<T1,...,Tn> TypeName<T1,...,Tn> { ... }` requires the impl's own type-parameter list to
    exactly match the target struct's declared arity; a mismatch is a compile error.

## Ownership & Capability Rules

-   A generic struct's field-capability inference (does the body call a mutating method on a
    field, etc.) is deferred to instantiation time exactly like type checking - computed once per
    monomorphized copy, then cached.
-   A type parameter `T` used as a `self`/parameter's declared type has its region/capability
    computed against the substituted concrete type post-monomorphization - e.g. `impl<T> Box<T> {
    get(self) -> T }` returning `self.value` follows the exact same "returning a field aliases the
    receiver" rule `RegionChecker` already applies to any struct field return, whatever `T` turns
    out to be.
-   `take`/`read`/`write` prefixes on a generic parameter (`set(write self, newValue: T)`) are
    declared once on the generic definition and apply identically to every instantiation - no
    per-instantiation capability re-declaration.

## Compiler Implementation

-   **AST:** `StructDecl` gains an optional `typeParams: std::vector<std::string>` (empty for
    every existing, non-generic struct - unchanged shape otherwise). `FunctionDecl` gains the
    same. `ImplDecl` gains `typeParams`, and its existing `typeName` field becomes a parsed
    generic-instantiation pattern (`TypeName<T1,...,Tn>` matching the struct's own arity) instead
    of always being a bare concrete name.
-   **Parser:** reuses the existing `<T>`/`<K,V>` bracket-list parsing already written for
    `List<T>`/`Map<K,V>` type positions (`Parser::parseTypeNameAtom`) and the existing 4-token
    generic-call lookahead from `0046-generic-methods.md`, extended to also recognize
    `struct Name<...>` / `fnName<...>(` / `impl<...> Name<...>` at declaration sites.
-   **TypeChecker:** a new monomorphization table keyed by `(genericName, concreteTypeArgs...)`,
    mirroring `registerMapInstantiation`'s per-shape registry - each entry stores a
    deep-substituted clone of the generic AST subtree, lazily populated the first time a concrete
    instantiation is encountered, then run through the existing (unchanged) non-generic
    type-checking logic.
-   **CapabilityChecker / RegionChecker / Interpreter / IrGenerator / LlvmIrEmitter:** each walks
    the monomorphized clones exactly like it already walks ordinary top-level
    `FunctionDecl`/`StructDecl`/`ImplDecl` items. Monomorphization happens once, upstream, inside
    `TypeChecker`'s own pass; nothing downstream needs to know a definition was ever generic -
    the same "flatten early, keep every later pass ignorant" strategy `0062-display-trait.md`'s
    own `impl`-to-mangled-`FunctionDecl` desugaring already uses.
-   **Name mangling:** `Box<i32>` mangles to `Box$i32` (LLVM struct/function name), `Pair<i32,str>`
    to `Pair$i32$str`. `$` is chosen because, like `.` in `0062`'s own method mangling, it can
    never appear in a real Axea identifier, guaranteeing mangled names never collide with user
    code.

## Diagnostics

```
error: cannot infer type argument for 'identity' - provide it explicitly, e.g. identity<i32>(...)
error: 'Box' expects 1 type argument, got 2
error: impl type parameter count does not match struct 'Box' (expected 1, got 2)
error: type 'str' does not support '+' (required by generic function 'sum<T>' body)
```

The last diagnostic is an instantiation-time body-checking failure - expected, since this phase
has no trait-bounds system to reject an invalid instantiation earlier (see Non-Goals).

## Optimization Opportunities

Instantiation caching is already required for correctness and doubles as the only "optimization"
this phase needs: repeated uses of the same concrete instantiation reuse one compiled copy,
exactly like `registerMapInstantiation` today. A future trait-bounds system (see Future Work)
could let the type checker reject an invalid instantiation before generating any code rather than
after, but nothing in this phase depends on that.

## Examples

```ax
struct Box<T>
{
    value: T
}

impl<T> Box<T>
{
    get(self) -> T
    {
        return self.value
    }
}

struct Pair<A, B>
{
    first: A
    second: B
}

identity<T>(x: T) -> T
{
    return x
}

intBox = Box<i32> { value: 5 }
n = intBox.get()

strBox = Box { value: "hello" }
s = strBox.get()

p = Pair<i32, str> { first: 1  second: "one" }

x = identity(42)
```

## Alternatives Considered

-   **Type erasure/boxing.** A single shared implementation operating over a uniform boxed
    value representation with a runtime type tag. Rejected: it requires inventing a new universal
    boxed-value representation the language doesn't have today - the interpreter's `Value`
    variant and the LLVM backend's concrete-typed structs are fundamentally different
    representations, so "erase to one shape" means building that shape twice, once per backend,
    before any generic code could run at all. Strictly more upfront work than extending the
    monomorphization pattern the collection intrinsics already prove out.
-   **Always-explicit type arguments, no inference.** Rejected as needlessly verbose for the
    common case (`identity(5)` reads better than requiring `identity<i32>(5)` everywhere), though
    explicit arguments remain always legal as an escape hatch, matching `.parse<T>()`.

## Open Questions

-   Should a generic function's type parameter be inferable from surrounding *return-type*
    context (e.g. `x: i32 = identity(y)` inferring `T=i32` from the annotation, not just from
    `y`'s own type), or only ever from argument types? Deferred - argument-only inference is
    sufficient for the collections use case and simpler to implement first.
-   Should nested generics (`Box<Box<i32>>`, `List<Pair<i32,str>>` once collections are
    user-definable) work from day one? Recommendation: yes, for free - the monomorphization key is
    just a string, so `Box<Box<i32>>` mangles to `Box$Box$i32` via ordinary recursive
    substitution, no new mechanism required.

## Future Work

-   Trait bounds (`T: SomeTrait`), once `docs/language/0062-display-trait.md`'s `trait`/`impl`
    mechanism is generalized beyond `Display`.
-   Once this doc and `docs/language/0019-unsafe.md` both land, a real `std/collections.ax`
    becomes possible - the actual motivating goal, tracked in
    `docs/language/0023-standard-library.md`.
