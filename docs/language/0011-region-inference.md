# Region Inference

**Status:** Implemented (Phase 4)
**Document:** `0011-region-inference.md`

---

# Motivation

`docs/language/0012-memory-model.md`: compile-time memory safety, no garbage collector, no runtime borrow checker, safe references inferred. `0020-compiler-architecture.md`'s pipeline places `Escape Analysis` and `Region Analysis` right after `Capability Analysis`:

```text
... -> Capability Analysis -> Escape Analysis -> Region Analysis -> Axea IR -> ...
```

`0010-capability-inference.md` and `0009-ownership.md` (Phase 3) built `read`/`write`/`take` capability inference and use-after-move checking, but explicitly deferred one question — Phase 3's decision #8: *"Return-value-based capability/escape reasoning is out of scope. Whether a returned value 'escapes' with borrowed capability is squarely Phase 4's 'Escape Analysis'/'Regions.'"* This document is that deferred question, answered.

**The problem this solves.** The interpreter has no manual memory management — structs are `std::shared_ptr<StructInstance>`, kept alive by ordinary C++ reference counting no matter what a static analysis concludes about them. So, like Phase 3 (which had to invent field mutation before capability inference had anything to infer), this phase had to find an analysis that's actually *load-bearing* here, not a label with no consequence. The one that is: a `read`/`write` (borrowed) struct parameter must not outlive the call it was borrowed for. Concretely, a function must not *return* a value derived from a borrowed parameter — the classic "cannot return reference to borrowed content" rule, applied with exactly the vocabulary Phase 3 already established (`take` = ownership in, safe to return; `read`/`write` = borrowed in, not safe to return).

---

# Two Regions

```text
Owned      - safe to return; either constructed locally or ownership was
             transferred in (a `take` parameter)
Borrowed   - not safe to return; reached through a `read`/`write` parameter
             without ownership transferring
```

No named or parameterized lifetimes (`'a`-style) — that's `0013-lifetimes.md`'s territory, and nothing on the roadmap needs it yet. Two regions are exactly enough to answer the one question this phase asks: *is it safe to return this value from this function?*

---

# The Rule

```ax
struct User { name: str }

display(user: User) -> str { user.name }   # fine - see "Primitives Don't Alias" below
get_ref(user: User) -> User { user }        # rejected
```

```text
error: function 'get_ref' cannot return 'user': parameter 'user' is borrowed
and does not outlive the call - declare 'take' if ownership should transfer
```

`user` in `get_ref` is inferred `read` (Phase 3's capability inference — the body never mutates or takes it), so it's `Borrowed`. Returning it directly hands the caller a reference to the *same shared instance* the original owner still holds, with no lifetime relationship the compiler can verify — exactly what a borrow checker exists to prevent. Declaring the parameter `take` instead (`get_ref(take user: User) -> User`) makes it `Owned`, and the same function is accepted.

Borrowed-ness propagates through struct construction, not just direct return:

```ax
struct Wrapper { inner: User }

wrap(user: User) -> Wrapper
{
    Wrapper { inner: user }   # rejected: Wrapper now holds a borrowed User
}
```

A struct literal is `Owned` unless *some* field initializer is itself `Borrowed` — the same reasoning Rust applies to a struct holding a `&'a T`: the container can't outlive what it borrows.

---

# Primitives Don't Alias

This is the refinement that keeps the check from being useless-ly conservative. `Value` (`compiler/interpreter/Interpreter.hpp`) stores `i32`/`bool`/`str` **by value** — `std::variant<std::int64_t, bool, std::string, ...>` — so reading a primitive field is always a fresh copy, never an alias. A field access only inherits the object's region when the *field's own declared type* is itself a struct name:

```ax
struct User { name: str  age: i32 }

display(user: User) -> str { user.name }   # str field -> Owned, regardless of `user`'s own region
```

`display` is accepted even though `user` is borrowed, because `user.name` is a primitive extraction, not an alias of anything. This is the exact function from `examples/capabilities.ax` (Phase 3) — this phase had to not regress it, and the primitive/struct distinction is precisely what makes both `display` (accepted) and `get_ref` (rejected) come out correctly from the same rule.

The same reasoning applies to parameters directly: a *primitive-typed* parameter (`x: i32`) is always `Owned`, regardless of its `read`/`write`/`take` capability — capabilities exist to gate struct mutation; primitives have no aliasing to protect against in the first place. (An earlier draft of this checker got this wrong — see "A Bug Worth Naming" below.)

---

# The Algorithm

```text
for each function fn:
    if fn has no return type, or its return type doesn't name a struct: skip entirely
        (nothing can leak through a primitive/unit return)

    env = { param -> Owned  if param is struct-typed and effectively `take`
                     Borrowed(param), structType   if param is struct-typed and read/write
                     Owned                          if param is primitive-typed, unconditionally }

    walk fn's body; at the trailing result and at every `return`, require the
    computed region is Owned (throw naming the offending parameter otherwise)
```

Per-expression-kind region rules (`compiler/sema/RegionChecker.cpp`'s `regionOfExpr`, structurally the same recursive-descent shape `TypeChecker::checkExpr` and `CapabilityChecker::inferExpr` already use):

| Expression | Region |
|---|---|
| integer/bool/string literal | `Owned` |
| name | whatever its binding says (parameter, or a local's own computed region) |
| `obj.field` | `Owned` if the field's declared type is primitive; otherwise inherits `obj`'s region |
| `Type { ... }` | `Owned` unless some field initializer is `Borrowed` |
| `f(...)` | always `Owned` — see below |
| `a op b` | always `Owned` (arithmetic/comparison only ever produce `i32`/`bool`) |
| `if c { a } else { b }` | `Borrowed` if *either* branch is `Borrowed` |
| `{ ... }` | the region of its trailing result (`Owned`/unit if none) |

**Why a call result is always trusted `Owned`, with no fixpoint needed.** If a callee actually leaked a borrow through its own return, *that callee's own check* rejects it independently, the moment `RegionChecker::check` reaches it — a caller never has to re-verify an already-validated callee. This is different from capability inference (Phase 3), which needed a fixpoint over the call graph because a caller's *requirement* depends on a callee's *requirement*. Here, a caller only cares whether a callee's return is *safe*, and safety is exactly what "the callee didn't get rejected" already establishes. So every function is checked exactly once, independently, in any order.

---

# Relationship to Capabilities

| Effective capability | Region |
|---|---|
| `read` | `Borrowed` |
| `write` | `Borrowed` |
| `take` | `Owned` |

Region inference is a direct consumer of Phase 3's `CapabilityChecker::effectiveCapabilities()` — it runs after capability analysis and before interpretation, and doesn't recompute capabilities itself. A parameter's *declared* capability (if present) still wins over its inferred one here too, exactly as it does for capability checking itself.

---

# A Bug Worth Naming

The first implementation marked *every* non-`take` parameter `Borrowed`, including primitive ones. That flagged `make(x: i32, y: i32) -> Point { Point { x: x, y: y } }` as an error — `x` and `y` are `i32`, but the check didn't realize that, and treated them as if they aliased something. `examples/regions.ax`'s `make` function is exactly this case, and `tests/RegionCheckerTests.cpp` pins it down permanently. The fix — decision "Primitives Don't Alias" above — checks whether the parameter's *declared type* names a struct before ever considering it a borrow risk.

---

# Compiler Implementation

`RegionChecker::check(const Program&, const std::unordered_map<std::string, std::vector<Capability>>& capabilities)` takes Phase 3's capability results directly (not a `CapabilityChecker&`), so it's independently testable the same way `tests/CapabilityCheckerTests.cpp` already chains `TypeChecker` into `CapabilityChecker`. `ax run` invokes it between capability-checking and interpretation — a region violation is a compile-time error, exactly like a type or capability error, never a runtime one. `ax regions <file>` lists every struct-returning function's parameters with their resolved region (`owned`/`borrowed`), mirroring `ax capabilities`.

---

# Not Yet Implemented

- **Named/parameterized lifetimes.** Two regions (`Owned`/`Borrowed`) is enough for "safe to return," not enough for "these two borrows must have the same lifetime" or similar — that's `0013-lifetimes.md`.
- **Loops.** The language doesn't have any yet, so there's no loop-scoped region question to answer (e.g. "does a borrow outlive one iteration") — deferred until loops exist.
- **Alias tracking**, same limitation Phase 3 already documented: `local = user; return local` isn't currently recognized as returning something borrowed, since `local`'s region is computed from what it was assigned (correctly `Borrowed`, actually — this one *does* work, since `AssignmentStmt` propagates the initializer's region into the local's binding). The real gap is aliasing through anything the analysis doesn't structurally walk (e.g. a future collection/container type).
- **Index/slice expressions** — don't exist in the language yet, so no region rule for them either.

---

# Open Questions

- Once generics exist, does a generic function's region reasoning need to be parameterized by its type arguments, or does the primitive/struct distinction still cleanly separate?
- Should a function be allowed to declare that it returns a borrow tied to one specific parameter's lifetime (an explicit region/lifetime annotation), once real reference types (`ref T` — already in `0001-syntax.md`'s Safe References section, not yet implemented) exist?

---

# Guiding Rule

> A function's return value should never let a caller outlive what it was only lent. Where a value came from — constructed here, or handed over with ownership — should be provable from the function's own body, without runtime cost and without asking the programmer to write down anything the compiler can already see.
