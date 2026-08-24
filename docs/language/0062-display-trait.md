# The `Display` Trait

**Status:** Implemented (a real, if deliberately narrow, `trait`/`impl` mechanism - see Known
Imprecision)
**Document:** `0062-display-trait.md`

---

# Motivation

`docs/language/Axea_Printing_Formatting.md`'s own "Formatting Traits" section:

```ax
trait Display
{
    format(self, buf: Buffer)
}
```

```ax
struct Point
{
    x: f64
    y: f64
}

impl Display for Point
{
    format(self, buf: Buffer)
    {
        buf.write("({self.x}, {self.y})")
    }
}
```

```ax
point = Point(10, 20)
print("Position: {point}")
```

> Axea should infer that `self` is read and `buf` is written. A separate `Debug` trait may
> provide `{value:?}` formatting.

Before this phase, Axea had **no trait/`impl` system at all** - no `trait`/`impl` keywords, no AST
nodes, nothing. This phase builds a real (if narrow) one, scoped to what actually drives runtime
behavior: a struct opts into custom text formatting by defining `impl Display for TypeName`, and
every place a struct value gets stringified - `print`/`write`, string interpolation, a top-level
auto-printed binding, and a struct nested inside another struct or inside a collection - calls the
user's own `format` function instead of the default `Name { field: value, ... }` printer.

---

# Design: Static Dispatch, Not Vtables - `self`'s Concrete Type Is Always Known

Axea has no dynamic dispatch or trait-object syntax anywhere in the language (no `dyn Trait`, no
trait used as a parameter type) - every place a value gets stringified, the compiler already knows
its exact concrete struct type at compile time. This means `impl Display for Point` needs no
vtable, no runtime type tag, and no indirection at all: it desugars, at parse time, into an
ordinary compiler-internal function - `Point.format(self: Point, buf: Buffer)`, mangled with a
`.` (an Axea identifier can never contain one, so this name is permanently unreachable from
ordinary call syntax, the same "special internal name" convention `print`/`write` themselves
already establish, see `docs/language/0049-printing-formatting.md`) - and every later pass treats
it exactly like a top-level `FunctionDecl`, just reached one level deeper through a new `ImplDecl`
AST node. The only genuinely new piece of runtime machinery, at either backend, is: *when
stringifying a struct value, check whether its type has a registered Display impl; if so, call
that function into a fresh `Buffer` and use what it wrote instead of the default field-by-field
text.*

`trait Display { format(self, buf: Buffer) }` itself parses into a real `TraitDecl` (name +
method signatures, name and parameter count only - see Known Imprecision), consulted for a genuine
(if minimal) conformance check on any `impl` naming a known trait. But nothing downstream of
`TypeChecker` ever consults a `TraitDecl` again - the trait declaration is documentation-with-
verification, not a live contract object the runtime carries around.

---

# Parsing: `self` as Sugar, Not a New Grammar Rule

A method inside `trait`/`impl` may start with a bare `self` (no `: Type`, no capability prefix)
instead of `parseParam`'s ordinary `[read|write|take]? name: Type` shape - `parseSelfAwareParam`
checks for `Identifier("self")` not immediately followed by `:` and, if so, synthesizes
`Param{"self", <concrete or placeholder type>, std::nullopt}` directly, falling back to the
ordinary `parseParam` otherwise (so a real parameter literally named `self` with an explicit type,
e.g. `self: i32`, still parses as such - never ambiguous, since that shape always has a `:` right
after the identifier). For an `impl` method, the concrete type is the `impl`'s own target
(`Point`); for a `trait` method signature (which has no body and never gets type-checked as one -
see `TraitDecl`'s own comment), it's the placeholder text `"Self"`, never resolved to a real type
since nothing ever runs `resolveType` on a trait signature's own param list.

`Parser::parseImplMethod` otherwise mirrors `parseFunctionDecl` exactly (param list, optional
`-> ReturnType`, `=>`-shorthand-or-`{...}`-body), just with the mangled name and
`parseSelfAwareParam` swapped in.

---

# Type Checking: Conformance Is Real But Minimal, and a Pre-Existing Parser Gap Is Not New Here

`TypeChecker` validates two things about every `impl TraitName for TypeName`:

1. `TypeName` must be a known struct (checked *before* the generic per-function param-type
   resolution pass, so this reports a specific "impl target 'Ghost' is not a known struct" rather
   than a generic "unsupported type" from the same failure one pass later).
2. If `TraitName` matches a real `TraitDecl` in the same program, every one of its declared methods
   must be implemented with a matching name and **parameter count** - not full per-parameter type
   conformance, see Known Imprecision. `Display` specifically is *additionally* required to define
   a `format` method with exactly 2 parameters even when no matching `trait Display { ... }` was
   ever written in source - this is the one compiler-recognized trait name whose shape actually
   drives runtime dispatch, so `impl Display for Point { render(self) {} }` (no usable `format`) is
   rejected here rather than silently compiling and falling back to default struct printing later.

Every impl method is then registered straight into `functions_` (the same map ordinary top-level
functions live in - no separate impl-aware call-checking path needed anywhere in `checkExpr`) and
type-checked via the exact same `checkFunction` a real `FunctionDecl` gets, with `self`'s type
already resolved to the concrete struct name by the parser.

---

# Capability Inference: `self`/`buf` Need No Special-Casing At All

The source doc's own request - "Axea should infer that `self` is read and `buf` is written" - falls
out for free from `CapabilityChecker`'s **pre-existing** fixpoint inference pass, unmodified. That
pass already infers a capability for *any* parameter with no explicit `read`/`write`/`take` prefix
by watching how the parameter is used in the body (a mutating method call like `buf.write(...)`
raises `Write` on `buf`; `self.x`/`self.y` read-only access never raises past `Read`). Since
`format(self, buf: Buffer)` has no explicit prefix on either parameter (matching the source doc's
own example exactly), and impl methods are registered into `CapabilityChecker`'s own `functions_`/
`inferred_` maps exactly like a top-level function, `self` and `buf` are inferred correctly with
zero new logic - confirmed by a direct test on `effectiveCapabilities().at("Point.format")`.

---

# Interpreter: A File-Local Active-Interpreter Pointer, and the Bug Its First Version Had

The interpreter's one universal `toString(const Value&)` is a **free function**, not an
`Interpreter` member - it has no `functions_`/`displayImpls_` map of its own to look a `format`
method up in, and no `callFunction` to invoke it through. Rather than threading an `Interpreter*`
through every one of `toString`'s dozens of existing call sites, a single file-local pointer,
`g_activeInterpreter`, is set once at the top of `Interpreter::run()`. `toString`'s `StructInstance`
branch checks it first: if set and `tryFormatStructWithDisplay` finds a registered impl for that
struct's type name, it constructs a fresh `Buffer` (`std::make_shared<BufferInstance>()`, identical
to a real `Buffer()` construction), calls the format function through the *existing*
`callFunction(decl, {self, buf})` with no new call machinery at all, and returns the buffer's own
resulting content - otherwise `toString` falls through to the default field-by-field text
unchanged.

**A real bug in this design's first version, caught by running the worked example for real, not
by reasoning about it on paper:** `g_activeInterpreter` was originally reset back to `nullptr` at
the *end* of `run()` (via an RAII guard), on the theory that nothing needs it once execution
finishes. But `compiler/main.cpp`'s own top-level auto-print of a binding
(`print("{}" = {}", name, toString(...))`) calls `toString` *after* `interpreter.run(program)` has
already returned - while the `Interpreter` instance itself is still alive (it's a local variable in
`main.cpp` that outlives both the `run()` call and this later print loop). With the reset-at-end-of-
run() design, this one call site saw a `nullptr` `g_activeInterpreter` and silently fell back to
the default printer - producing `p3 = Point { x: 7, y: 8 }` from the interpreter while the compiled
LLVM backend (whose own top-level print is just an ordinary, always-live function call, no timing
window at all) correctly produced `p3 = (7, 8)` for the exact same program. A real
interpreter-vs-compiled-backend divergence, not a hypothetical one - caught only because this
phase's own verification discipline re-ran the worked example's top-level-auto-print line and
diffed the two backends' output, exactly the discipline `docs/language/0057-alignment.md`'s own
"Real, Pre-Existing Bug Found Along the Way" section already established as necessary.

**The fix**: reset `g_activeInterpreter` in `~Interpreter()` instead of at the end of `run()` (and
only if it still points at *this* instance, so a still-live newer `Interpreter` is never clobbered
by an older one being destroyed) - correct for both real call sites: `main.cpp`'s `interpreter`
local stays the active pointer for as long as it's genuinely alive, including the post-`run()`
auto-print loop, while the *other* real risk this whole mechanism has to guard against - this
codebase's own common test pattern, `toString(runProgram(source).at("x"))`, where a temporary
`Interpreter` is constructed, run, and destroyed *before* `toString` is ever called on the value it
produced - is exactly what the destructor's own reset makes safe: by the time `toString` runs
there, the pointer has already been cleared, Display dispatch is correctly skipped (there is no
live interpreter left to invoke a function on), and the default printer runs instead - the only
behavior that could possibly be correct for a `Value` outliving the interpreter that produced it.

---

# LLVM Backend: Patching Two Already-Shared Per-Struct-Type Functions

Two existing functions are each the **one shared entry point** every one of their own respective
call sites already funnels through - patching each once, at its own definition, propagates
correctly to everywhere it's called from, with no call-site-by-call-site changes needed anywhere:

- **`@axea.tostring.<Name>`** (`emitStructToStringHelpers`, from
  `docs/language/0054-collection-printing.md`) - the "stringify a struct to a heap `i8*`" function
  every interpolation piece and every collection-of-structs element already calls through
  `stringifyValueOfType`. When `IrProgram::displayImpls` has an entry for `<Name>`, its body becomes
  three lines - `@axea.strbuf.new()`, call the user's own already-compiled format function, then
  `@axea.strbuf.finish()` - instead of the default per-field `openBrace`/`fieldLabel`/
  `emitElementToStrCall`/`closeBrace` sequence. **No bitcast needed anywhere**: confirmed, not
  assumed, that `@axea.strbuf.new()`'s own return type (`{i32, i32, i8*}*`) is textually identical
  to `llvmType("Buffer")` - the exact same header shape `docs/language/0043-buffer.md` already
  established, so the strbuf pointer passes straight into the format function's own `Buffer`-typed
  parameter with zero conversion.
- **`@axea.print.<Name>`** (`emitStructPrintHelpers`) - the shared *direct*-print path (no
  intermediate string) for a top-level auto-printed binding, `print()`/`write()`'s own struct
  argument, and any nested struct-typed field inside *another* struct's own default print helper.
  Patched the identical way: build a strbuf, call the user's format function, `printf("%s", ...)`
  the result, instead of the existing field-by-field `printf` sequence.

Together these two patches cover every struct-stringification call site in the backend - confirmed
by a worked example exercising direct `print()`, interpolation, a struct nested inside another
struct's default printer, a struct nested inside an `Array`, and a top-level auto-printed binding,
all hand-verified byte-for-byte identical to the interpreter's own output (see Worked Example).

`IrGenerator` compiles every impl method as a completely ordinary `IrFunction` (pushed into
`irProgram.functions` exactly like a top-level `FunctionDecl` is), and separately records
`typeName -> mangledFormatFunctionName` into the new `IrProgram::displayImpls` map - populated only
for `traitName == "Display"`, since no other trait name drives any runtime dispatch yet (an `impl`
of some other trait still compiles its own methods as real, callable-in-principle functions, just
ones nothing currently calls).

---

# Worked Example

```ax
struct Point { x: i32  y: i32 }

trait Display
{
    format(self, buf: Buffer)
}

impl Display for Point
{
    format(self, buf: Buffer)
    {
        buf.write("({self.x}, {self.y})")
    }
}

struct Line { a: Point  b: Point }

run() -> i32
{
    p1 = Point { x: 1, y: 2 }
    p2 = Point { x: 3, y: 4 }
    print(p1)
    points = [p1, p2]
    print(points)
    line = Line { a: p1, b: p2 }
    print(line)
    return 0
}

x = run()
p3 = Point { x: 7, y: 8 }
```

```text
(1, 2)
[(1, 2), (3, 4)]
Line { a: (1, 2), b: (3, 4) }
x = 0
p3 = (7, 8)
```

Hand-verified byte-for-byte identical between the interpreter and the LLVM backend (`-O0` and
`-O1`), including the top-level-auto-print line (`p3 = ...`) specifically - the one call site the
interpreter's first design got wrong (see its own section above) before the `g_activeInterpreter`
lifetime fix.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **`impl` conformance checks method name + parameter count only, not full per-parameter type
  conformance** - `impl Display for Point { format(self, buf: i32) { } }` (wrong type for `buf`,
  right arity) passes `TypeChecker`'s own trait-conformance check and only fails later, and
  separately, from ordinary type-checking of the method body itself if it tries to call a
  `Buffer`-only method on an `i32`. Real per-parameter conformance is a bigger feature (matching
  each declared param's own resolved `Type`, including handling `Self`-typed positions generically)
  deferred to if/when a second real trait consumer ever needs it.
- **Only `Display` drives any runtime dispatch** - `trait`/`impl` parse and type-check generally
  (any trait name, any method shapes), and an `impl` of some other trait name compiles its methods
  as real, callable-in-principle functions, but nothing currently *calls* them: no generic
  `value.method(...)` dispatch-through-a-trait exists, no `Debug` trait (the source doc's own
  aside, "a separate `Debug` trait may provide `{value:?}` formatting" - still exactly
  `docs/language/0058-debug-formatting.md`'s pre-existing str/String-only quoting, unchanged by
  this phase), no way to write a function generic over "any type implementing Display." This phase
  builds exactly the one consumer the source doc's own worked example demonstrates.
- **No dynamic dispatch / trait objects** - not attempted, since nothing in the source doc asks for
  it and Axea has no existing polymorphism mechanism (a function parameter, a collection element)
  that could hold "some type implementing Display" without already knowing its concrete type.
- **A self-referential `format` body (e.g. `buf.write("{self}")`) infinitely recurses**, the same
  way it would in any language with this shape of trait (Rust's own `Display::fmt` calling
  `write!(f, "{}", self)` has the identical failure mode) - not a bug specific to this
  implementation, and not guarded against, matching established precedent elsewhere.
- **No user-callable `.method()` syntax for an impl's own methods** - `point.format(buf)` is not
  reachable from ordinary Axea source; the mangled `Point.format` name is deliberately unreachable
  from call syntax (see Design). Only the compiler's own stringification dispatch ever calls it.
  General struct methods (callable via ordinary `.method(...)` syntax, independent of any trait)
  are a different, larger feature this phase does not build.

---

# Guiding Rule

> A trait system sounds like it demands dynamic dispatch until a concrete check confirms the
> language never actually needs it here - every stringification call site already knows a struct
> value's exact concrete type at compile time, so `impl Display for Point` could desugar entirely
> into an ordinary, staticly-dispatched function call, with the only genuinely new runtime
> machinery being *which* function to call, decided once per struct type, not once per value.
> And a lifetime bug in a "set a pointer for the duration of an operation" design is easy to get
> wrong in exactly the direction this one was - resetting eagerly, at the end of the operation that
> set it, rather than at the end of the *owning object's own lifetime*, which is the actual
> invariant that needed protecting; the fix (move the reset into the destructor) was smaller than
> the bug it fixed, but only became visible by running the worked example's own top-level-auto-
> print line, not by re-reading the design's own reasoning about it.
