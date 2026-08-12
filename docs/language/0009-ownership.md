# Ownership

**Status:** Implemented (Phase 3)
**Document:** `0009-ownership.md`

---

# Motivation

`0000-vision.md`: "Ownership is inferred whenever possible." `0012-memory-model.md` adds the constraint this has to work under: no garbage collector, no runtime borrow checker. Capability inference (`0010-capability-inference.md`) answers *what a function is allowed to do* with a parameter; this document covers the other half — once a value has been handed off (`take`), *is it still safe to use afterward*. That check is what "ownership," concretely, means in this implementation.

Both halves are implemented in the same pass, `compiler/sema/CapabilityChecker`, since move-checking needs to know which calls require `take` — which is exactly what capability inference computes.

---

# The Rule

Passing a parameter, by bare name, into another function's `take`-capability argument position **consumes** it. Using that same name again afterward, within the same block, is a compile-time error:

```ax
struct Packet { id: i32 }

send(take packet: Packet) -> i32 { packet.id }

relay(packet: Packet) -> i32
{
    a = send(packet)
    b = send(packet)   # error: use of moved value 'packet'
    a + b
}
```

```text
error: use of moved value 'packet'
```

This mirrors move semantics in languages like Rust, scaled down to what a tree-walking interpreter with no borrow checker can enforce with a single linear pass over each function body.

---

# Scope: Per-Block, Not Merged Across Control Flow

Move-tracking is deliberately **not** a full dataflow analysis. Each block — a function body, and each branch of an `if`/`else` — tracks its own moved-set independently, starting empty. A move inside one branch has no effect on the sibling branch or on anything after the `if`:

```ax
relay(packet: Packet, flag: bool) -> i32
{
    if flag
    {
        send(packet)     # moves `packet` — but only within this branch
    }
    else
    {
        0
    }
    send(packet)          # allowed: this block's own moved-set is still empty
}
```

This is a known, deliberate simplification, not an oversight — merging move-state across control-flow joins the way a real borrow checker does is a meaningfully larger analysis (dataflow with proper meet/join over branches), and out of scope for this phase. The trade-off: the checker catches the straightforward, common case (moving a value and then immediately reusing it in a straight line of code) without attempting soundness across branches. `tests/CapabilityCheckerTests.cpp` has this exact scenario as a passing (non-throwing) test, so the behavior is pinned down rather than accidental.

A future pass could tighten this (e.g. requiring a value moved in *one* reachable branch to be treated as moved after the `if` too, or supporting a proper conditionally-moved state) — see Open Questions.

---

# What Counts as "Using" a Moved Value

Any `NameExpr` referencing a moved name, anywhere in the same block after the move — as a bare value, inside a binary expression, as another call's argument, as the object of a field access, inside a struct literal's field initializer. The checker walks every expression form the language has (`compiler/sema/CapabilityChecker.cpp`'s `checkMovesInExpr`) looking for exactly this.

What does **not** count as a move:

- Passing the same parameter into a `read` or `write` position — only `take` consumes.
- Passing a *field* of the parameter (`send(p.inner)`) — propagation and move-tracking both only recognize bare-name arguments (see `0010-capability-inference.md`'s "Not Yet Implemented").
- Anything at top-level script scope. Capabilities and moves are a property of a function's *parameters* specifically (borrowed values); top-level bindings are the script's own, always fully owned, and aren't tracked by this pass at all.

---

# Relationship to Capabilities

| Capability | Can read | Can mutate fields | Consumes (moves) |
|---|---|---|---|
| `read` | yes | no | no |
| `write` | yes | yes | no |
| `take` | yes | yes | yes |

Only `take` triggers move-checking. A `write` call mutates the shared struct instance (see below) but the caller can still legitimately use the parameter afterward — mutation isn't consumption.

---

# Why Mutation Needed To Exist First

Before this phase, the language had no assignment target other than a bare name, so `write` had nothing to differentiate itself from `read` — every parameter usage was trivially a read. This phase added the minimum mutation syntax needed to make the whole capability system meaningful:

```ax
obj.field = value
obj.field++
obj.field--
```

Deliberately **not** added: `+=`/`-=`/`*=`/`/=` or bitwise compound assignment (`0001-syntax.md` shows these, but bitwise operators don't exist in the language at all yet — adding compound forms of operators that don't exist is unrelated scope creep), and `++`/`--` as a value-producing *expression* (no doc example uses one as a value, and choosing old-vs-new-value semantics is an unforced decision this phase doesn't need to make).

Struct instances are `std::shared_ptr<StructInstance>` (chosen in Phase 2), so `obj.field = value` mutates the same instance every other reference to that struct sees — this is what makes `write` an observable, meaningful capability rather than a formality: a caller passing a struct into a `write` function really does see the mutation afterward.

---

# Compiler Implementation

`CapabilityChecker::checkMovesInExpr`/`checkMovesInStmt` run after capability inference has produced final `effective_` capabilities for every function (see `0010-capability-inference.md`). For each function, starting from an empty `moved` set:

```text
checkMovesInExpr(expr, function, moved):
    NameExpr referencing a name in `moved`  => error
    CallExpr: check every argument first, then for each bare-name argument
              naming one of function's own parameters, if the callee's
              parameter is effectively `take`, add the name to `moved`
    IfExpr: check the condition against `moved`; check each branch against
            its own fresh, empty set
    everything else: walk sub-expressions with the same `moved`
```

`ax run` invokes this (via `CapabilityChecker::check`) between type-checking and interpretation, so a use-after-move is caught before the program ever executes — the same guarantee type errors already have.

---

# Not Yet Implemented

- Merging move-state across control-flow branches (see "Scope" above).
- Alias-aware moves (`local = param` doesn't currently transfer move-tracking to `local`).
- Any notion of "give it back" (moving a value back out via a return value doesn't un-consume the original binding).
- Loops don't exist yet in the language at all, so there's no move-in-a-loop question to answer yet.

---

# Open Questions

- Should a function that returns a `take`-received parameter (once return-value capability reasoning exists — see `0010`'s Open Questions) be allowed to "give it back" to its caller?
- Is per-block isolation the right long-term default, or should it tighten toward real dataflow merging once loops and richer control flow exist?

---

# Guiding Rule

> A moved value should be as inert as a value that was never bound at all. The checker should catch the mistake exactly where it happens, in a straight line of code, without pretending to a soundness guarantee (across branches, aliases, or loops) it doesn't actually implement.
