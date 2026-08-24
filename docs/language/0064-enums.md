# `enum`: General Tagged Unions, with a Real `match` Expression

**Status:** Implemented
**Document:** `0064-enums.md`

---

# Motivation

`docs/language/0007-traits.md` and every other stub doc from this codebase's earliest phase left
"real" algebraic data types unaddressed - `Optional<T>` (`docs/language/0052-optional.md`) and
`Result<T,E>` (`docs/language/0063-result.md`) are each a fixed, compiler-built-in two-variant
sum type, not something a user can declare for themselves. This phase builds the general form:

```ax
enum Shape
{
    Circle(f64)
    Rectangle(f64, f64)
    Point
}

area(s: Shape) -> f64
{
    return match s
    {
        Circle(r) => 3.14159 * r * r
        Rectangle(w, h) => w * h
        Point => 0.0
    }
}
```

Each variant carries zero or more *positional* payload values (no named fields - a variant
needing more structure can carry a `struct` as one of its own positional slots, the same
"compose, don't rebuild the feature" answer `Optional<T>`/`Result<T,E>` already gave for "more
than one payload value"). Variants are constructed with an explicit `EnumName.` prefix
(`Shape.Circle(5.0)`, or `Shape.Point` with no parens for a no-payload variant) rather than
`Some`/`Ok`-style bare names, since - unlike the one or two compiler-fixed names Optional/Result
own - nothing stops two different user-declared enums from choosing the same variant name, and
the prefix resolves that unambiguously. `match` is a real expression, with genuine
exhaustiveness checking: every arm's variant name must be a real variant of the scrutinee's own
enum, matched at most once, and either every variant is named or a trailing wildcard (`_`) arm
covers the rest.

---

# Design: An Enum *Is* a Struct, Once It Reaches IrGenerator

The single decision that kept this phase's own scope from ballooning: rather than inventing a
parallel tagged-union runtime representation, an enum is lowered - at every layer from
`IrGenerator` down - into an ordinary **struct**: a flattened `{i32 tag, <variant 0's own fields>,
<variant 1's own fields>, ...}` layout, each variant's own payload fields given a synthetic name
(`"<VariantName>_<index>"`, e.g. `"Circle_0"`, `"Rectangle_0"`/`"Rectangle_1"` - never visible to
real Axea source, the same "internal name, permanently unreachable from user syntax" convention
`docs/language/0062-display-trait.md`'s own mangled `impl` method names already established).
`IrGenerator::generate` builds this flattened field list once per enum and inserts it directly
into `IrProgram::structs` - the *exact* map real `struct` declarations populate - so every piece
of struct machinery downstream works completely unchanged:

- **Construction** (`Shape.Circle(5.0)`) lowers to an ordinary `IrStructNew` - the tag field set
  to the variant's own declared index, each argument mapped to that variant's own synthetic field
  name, every *other* variant's own fields left unset (`undef`, never read - the same "unused slot
  gets `undef`, safe because nothing reads it without checking the tag first" reasoning
  `Result<T,E>`'s own `{i1, T, E}` construction already established for its own unused payload
  slot).
- **`match` arm field binding** (`Circle(r) => ...`) lowers to an ordinary `IrFieldGet` by that
  same synthetic name - no new field-access instruction needed at all.
- **`LlvmIrEmitter`'s type declarations, `malloc`+`GEP`+`store` construction, and `GEP`+`load`
  field reads** - all of it - work on an enum's own flattened struct with *zero* changes, since
  from that layer's perspective it genuinely cannot tell an enum-derived struct entry apart from
  a real one, field names included.

The one place this reuse needed a genuine *exception*, not just "works for free": an enum's own
`@axea.print.<name>`/`@axea.tostring.<name>` cannot be the generic "print every field" one every
real struct gets - that would leak the raw tag and every *other* variant's own garbage fields
(`Shape { __tag: 0, Circle_0: 5, Rectangle_0: 0, Rectangle_1: 0 }` instead of `Circle(5)`). A
second registry, `IrProgram::enums` (enum name -> each variant's own (name, field count), enough
to recompute every variant's own field range within the flattened struct), lets
`emitStructPrintHelpers`/`emitStructToStringHelpers` recognize an enum-derived name and generate a
*real*, tag-aware stringifier instead - a genuine LLVM `switch` on the tag, formatting only the
matched variant's own fields via `emitElementToStrCall` (the same generic per-field stringify
dispatch the *default* struct-to-string body already uses, reused here too - so an enum's own
payload can be any printable type at all, unlike `Result<T,E>`'s own printing, which is
restricted to `i32`/`i64`/`f64`/`bool` on each side). `@axea.print.<name>` just delegates to
`@axea.tostring.<name>` then a single `"%s"` `printf` - the identical shape
`docs/language/0062-display-trait.md`'s own Display-trait branch, sitting right next to it in the
same function, already established for the same reason (a real per-shape body, not the generic
one).

**`match` reuses `IfExpr`'s own nested-branch shape, not a new N-way instruction.** Since this
codebase's `IrBranch` is inherently two-way, an N-arm `match` lowers into a chain of nested
`IrBranch`, mirroring exactly how an `else if` chain already nests `IfExpr` (see
`IrGenerator::lowerExpr`'s own `IfExpr` case: `elseBranch` is itself another `IfExpr` node,
recursively lowered). `lowerMatchArm` builds this chain explicitly (since `MatchExpr` doesn't have
that recursive AST shape the way a parsed `else if` chain does): each non-last arm becomes `tag ==
<its own declared variant index> ? <bind fields, lower body> : <recurse into the next arm>`; the
last arm (`TypeChecker` already guarantees full coverage) skips the comparison entirely and just
lowers its own body directly, the same way a final bare `else` block does. The tag itself is read
*once*, by the caller, and threaded through every recursion level rather than re-extracted at
each one. `LlvmIrEmitter`'s own `emitBranch` needed **zero changes** - it already builds a correct
merge-block `phi` for arbitrarily deep branch nesting, confirmed (not assumed) by this being
exactly how `else if` chains already worked before this phase.

---

# Parsing: No New Grammar for Construction At All

`EnumName.Variant(args)` parses as an entirely ordinary `object.method(args)` postfix expression
(`MethodCallExpr`) - the same shape `point.someMethod(args)` already has. `EnumName.Variant` (no
parens, a no-payload variant) parses as an ordinary `FieldExpr`. **Neither needed a single parser
change** - only `TypeChecker`, `Interpreter`, `IrGenerator`, and `RegionChecker` each need their
own "is `object` a bare name matching a known enum type" check (`asEnumTypeName`/
`enumNameOfExpr`/etc., one per pass, mirroring this codebase's established "each pass owns its own
walk" convention), checked *before* their own generic object-type-resolution logic, which would
otherwise throw "undefined variable" trying to resolve a bare enum type name as if it were bound
to a real value.

`match scrutinee { Variant(a, b) => expr  Other => expr  _ => expr }` needed one new keyword
(`match`) and one new AST node (`MatchExpr`/`MatchArm`) - arms are whitespace-separated (no
commas, the same convention struct fields/enum variants already use), each arm's own binding names
comma-separated inside parens (the same shape a function's own parameter list uses). The
scrutinee is parsed with struct-literal parsing disabled (`allowStructLiteral=false`), the
identical reason `if`'s own condition already disables it - `match x { ... }`'s own `{` must start
the arm list, not be misread as `x`'s own struct literal fields.

---

# Type Checking: Real Exhaustiveness, Not Just Arity Checks

`TypeChecker::checkExpr`'s `MatchExpr` case validates, in order: the scrutinee is actually an enum
value; every non-wildcard arm names a real variant of that enum; no variant is matched twice; a
wildcard arm, if present, is the *last* arm (anything after it would be unreachable dead code - a
real correctness footgun worth rejecting outright, not silently allowing); each arm's own binding
count matches its variant's own declared payload arity; and - the real exhaustiveness check -
either every declared variant is covered by name, or a wildcard arm covers the rest, else a clear
"non-exhaustive match... missing variant(s): ..." error names exactly what's missing. Every arm's
body must produce exactly the same type (no implicit widening, matching this codebase's existing
stance on every other branch-shaped construct's own type unification).

`TypeKind::Enum` reuses `Type::structName` to carry the enum's own name - the identical field
`TypeKind::Struct` already uses for its own name, so a resolved enum `Type` needed no new `Type`
fields at all, just a new enumerator.

---

# Worked Example

```ax
enum Shape
{
    Circle(f64)
    Rectangle(f64, f64)
    Point
}

area(s: Shape) -> f64
{
    return match s
    {
        Circle(r) => 3.14159 * r * r
        Rectangle(w, h) => w * h
        Point => 0.0
    }
}

describe(s: Shape) -> str
{
    return match s
    {
        Circle(r) => "a circle"
        _ => "not a circle"
    }
}

c = Shape.Circle(5.0)
r = Shape.Rectangle(3.0, 4.0)
p = Shape.Point

cArea = area(c)
rArea = area(r)
pArea = area(p)

shapes = [c, r, p]
```

```text
c = Circle(5)
r = Rectangle(3, 4)
p = Point
cArea = 78.5397
rArea = 12
pArea = 0
shapes = [Circle(5), Rectangle(3, 4), Point]
```

Hand-verified byte-for-byte identical across the interpreter, `ax llvm-ir | clang -x ir -O0`, and
`-O1` - including an enum value nested inside an `Array`, nested inside a struct field, printed
directly via `print()`, and printed via string interpolation, all four routing through the
identical two patched functions (`@axea.tostring.<name>`/`@axea.print.<name>`) that every other
struct-stringify call site already shared before this phase.

---

# A Real Bug Found by Running the Worked Example, Not by Reasoning About the Design

`RegionChecker`'s own generic `MethodCallExpr`/`FieldExpr` handling recurses into `object`
*unconditionally*, calling `regionOfExpr` on it before anything else - which, for a bare enum type
name like `Shape` in `Shape.Circle(5.0)`, reaches `RegionEnv::get("Shape")` and throws "undefined
variable: Shape". This is the *only* one of the four passes needing the "is this actually
variant construction" check that got it wrong on the first attempt: `CapabilityChecker`'s own
generic path happens to be safe already (its `inferExpr` has no `NameExpr` case at all - a bare
name read is already documented as "the floor," a deliberate no-op - and `checkMovesInExpr`'s own
`NameExpr` case only checks set membership, never throws for an unknown name), but
`RegionChecker`'s `RegionEnv::get` genuinely does throw for anything not bound. Found only by
running the worked example's own `match` expression through the full pipeline and hitting a real
exception, not by re-reading each pass's own code in isolation beforehand - the same "verify past
the type checker" discipline `docs/language/0053-nested-generics.md`'s own second bug ("Real,
Pre-Existing Bug Found Along the Way" sections throughout this codebase's docs) already
established as necessary. Fixed identically to the other three passes: `RegionChecker` gained its
own `enums_` registry and an early check in both `MethodCallExpr` and `FieldExpr`, mirroring
`TypeChecker`/`Interpreter`/`IrGenerator`'s own already-correct versions exactly.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **No named-field variants** (Rust's own `Variant { x: i32, y: i32 }` struct-variant shape) -
  only positional payloads. A variant needing named fields can carry a real `struct` as one of its
  positional slots instead - genuinely composable with everything else this phase built (a nested
  enum-as-payload already works, hand-verified: `enum Outer { Wrap(Inner) }`).
- **No pattern matching *inside* a variant's own payload** (e.g. matching `Some(Circle(r))` in one
  arm) - a `match` arm's own bindings are always flat names, one per the variant's own declared
  payload position, never a nested pattern. Real nested pattern matching is a substantially larger
  feature (recursive pattern trees, not just a flat binding list) than this phase's own scope.
- **No guard clauses** (`Circle(r) if r > 0.0 => ...`) - an arm's own variant name (plus binding
  count) is the only thing distinguishing it from another arm; no additional boolean condition can
  narrow a single variant into multiple arms.
- **A `match` expression's own arm bodies are single expressions**, not blocks with multiple
  statements - the same simplicity struct field lists/enum variant lists/trait method lists
  already share (no commas, no semicolons, just repeated shapes). A multi-statement arm body can
  still call out to a real function.
- **No exhaustiveness-aware dead-code detection beyond the wildcard-must-be-last rule** - Rust's
  own compiler additionally flags an *unreachable* named arm that duplicates an earlier wildcard
  or an already-fully-covered variant in more elaborate ways (e.g. via or-patterns); this phase's
  own check is real but limited to "no variant matched twice" and "wildcard must be last."
