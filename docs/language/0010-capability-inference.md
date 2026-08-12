# Capability Inference

**Status:** Implemented (Phase 3)
**Document:** `0010-capability-inference.md`

---

# Motivation

`docs/language/0000-vision.md` and `0001-syntax.md` describe capabilities (`read`, `write`, `take`) as parameter annotations the compiler mostly shouldn't need — a function that only reads a struct's fields shouldn't have to say so; a function that mutates one should have that inferred; only exported APIs need to state a contract explicitly.

This document specifies the algorithm actually implemented in `compiler/sema/CapabilityChecker.{hpp,cpp}`, which runs as its own pipeline stage after the type checker, matching `0020-compiler-architecture.md`:

```text
... -> Type Checker -> Capability Analysis -> Escape Analysis -> Region Analysis -> ...
```

It reuses `docs/language/0009-ownership.md`'s vocabulary; that document covers the ownership/use-after-move half of the same pass.

---

# Capabilities

```text
read < write < take
```

Three levels, in increasing strength. A parameter's capability is the *weakest* one its function body actually requires:

- `read` — the parameter (or a field of it) is only ever read.
- `write` — a field reachable from the parameter is mutated (`obj.field = ...` or `obj.field++`/`--`).
- `take` — the parameter is passed into another function's `take`-capability position, i.e. ownership of it moves out of the current function.

---

# What Triggers Each Capability

## `write`

The only source of `write` is a field mutation whose target is rooted, possibly through a chain of field accesses, at one of the function's own parameters:

```ax
struct User
{
    name: str
    age: i32
}

birthday(user: User) -> i32
{
    user.age++
    user.age
}
```

`birthday` is inferred `write` on `user` because `user.age++` mutates a field reached directly from the parameter. Nested chains work the same way — `a.b.c = value` requires `write` on whichever parameter `a` resolves to, found by walking `object` through `FieldExpr` nodes down to the root `NameExpr`.

Incrementing a *plain* parameter (`n++` where `n` itself, not a field of it, is the target) does **not** require any capability. Primitive parameters are passed by value with no aliasing — mutating the function's own local copy is never observable by the caller, so it's exactly as inert as any other local rebinding.

## `take`

Nothing in the language can force a `take` requirement structurally — there's no standard library, no consuming built-ins. A parameter becomes `take` only by:

1. **Explicit declaration** on some function, e.g. `send(take packet: Packet)`, or
2. **Propagation**: a function passes one of its own parameters, by bare name, as an argument into another function's parameter whose *effective* capability (declared, or currently-inferred if not declared) is `take` — the caller then needs at least `take` on its own parameter too.

```ax
send(take packet: Packet) -> i32
{
    packet.id
}

relay(packet: Packet) -> i32
{
    send(packet)     # relay's own `packet` is now inferred `take`
}
```

`write` propagates the same way — a function that forwards its parameter into another function's `write` position needs at least `write` itself. `examples/capabilities.ax` has a worked case (`celebrate` calling `birthday`).

Propagation only recognizes a **bare name** argument (`send(packet)`), not a field-access chain (`send(a.b)`) — see "Not Yet Implemented" below.

---

# The Inference Algorithm

```text
register every function; inferred[fn][param] = read for all parameters

repeat:
    changed = false
    for each function fn, walk its body:
        - a FieldAssignStmt/IncDecStmt target rooted at fn's own parameter P
              => require inferred[fn][P] >= write
        - a CallExpr argument that is a bare name referring to fn's own
          parameter P, passed positionally to callee g's parameter Q, where
          effective(g, Q) is write or take
              => require inferred[fn][P] >= effective(g, Q)
        note if anything increased
until nothing changed
```

This is a small fixpoint over the call graph: each function's requirement can only ever increase (the lattice has 3 levels), so it always converges, and it needs no special-casing for (mutual) recursion — `factorial` calling itself is handled by the same loop as any other call, it just never raises anything because its parameter is `i32`.

`effective(fn, param)` is the function's **declared** capability if it has one, otherwise its current best-known **inferred** value. Using declared values during propagation (rather than waiting for a callee's own inference to finish) means propagation is correct regardless of the order functions happen to be visited in.

---

# Declared vs. Inferred

If a parameter has an explicit capability, the checker verifies the body doesn't need *more* than declared:

```text
error: function 'birthday' requires 'write' for parameter 'user' but only 'read' was declared
```

Declaring *more* than necessary is allowed — `write` on a parameter that's only ever read is wasteful but not an error, matching `0010`'s original one-line spec: "the compiler verifies implementations satisfy those contracts." This check applies uniformly, not only to `pub` functions; visibility isn't enforced yet regardless (Phase 2 left `pub` parsed-and-discarded), so gating capability verification on it would be an arbitrary distinction with no other effect.

The **effective** capability — what `ax capabilities` prints, and what other functions see when deciding their own propagation — is `declared ?? inferred`.

---

# Worked Example

```ax
struct User
{
    name: str
    age: i32
}

display(user: User) -> str  { user.name }        # read
birthday(user: User) -> i32 { user.age++  user.age }  # write
celebrate(user: User) -> i32 { birthday(user) }  # write, propagated
archive(take user: User) -> str { user.name }    # take, declared
```

```text
$ ax capabilities examples/capabilities.ax
Function(display)
  Param(user: read)
Function(birthday)
  Param(user: write)
Function(celebrate)
  Param(user: write)
Function(archive)
  Param(user: take)
```

---

# Compiler Implementation

`CapabilityChecker::check(const Program&)`:

1. Register every `FunctionDecl` and initialize `inferred_[fn]` to `read` for each parameter.
2. Run the fixpoint above (`inferExpr`/`inferStmt`, bounded to `functions_.size() * 4 + 4` iterations as a safety cap — in practice it converges in one or two passes).
3. For each parameter with a declared capability, verify `inferred <= declared`; compute `effective_[fn] = declared ?? inferred` for every parameter.
4. Run ownership/move-checking (see `0009-ownership.md`) using the now-final effective capabilities.

`ax run` invokes this pass between type-checking and interpretation — a capability or ownership violation is a compile-time error, the same as a type error, never a runtime one.

---

# Not Yet Implemented

- **Alias tracking.** Only a parameter used *directly* by name counts. `local = param; local.field = 5` is invisible to the analysis — assigning through an alias doesn't currently affect `param`'s inferred capability. Full alias/points-to analysis is a substantially larger undertaking left for later.
- **Field-chain propagation into calls.** Passing `param.nested` (rather than `param` itself) into a `write`/`take` position doesn't propagate anything back to `param`.
- **Return-value-based reasoning.** Whether a returned value "escapes" carrying some capability requirement is `0011-region-inference.md` / Escape Analysis territory (roadmap Phase 4), not this pass.
- Compound assignment operators (`+=`, `-=`, ...) beyond plain `=` and `++`/`--` — see `0009-ownership.md` for why.

---

# Open Questions

- Should capability inference eventually consider a struct's own methods (once `struct_member = function_decl` — the formal grammar already allows it, Phase 2 deliberately didn't implement it) as part of a caller's requirement, the way free-function calls already are?
- Should `write`/`take` ever be inferable through means other than field mutation and call propagation (e.g. a future indexing/slice syntax)?

---

# Guiding Rule

> A function's capability contract should describe what its body actually does, not what its author remembered to write down. Inference exists so the common case needs no annotation at all; declarations exist so a public contract can promise no more than it delivers.
