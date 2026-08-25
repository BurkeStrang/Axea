# Modules: `module`/`use`, Explicit Identity, File-Path Discovery Only

**Status:** Implemented
**Document:** `0066-modules.md`

---

# Motivation

Every feature before this phase lived in one file - there was no way to split a program across
several, and no way to build a reusable library (`math`, `collections`, ...) without pasting its
source directly into whatever used it. This phase adds a real module system:

```ax
// math_utils.ax
module math_utils

extern c abs(x: i32) -> i32

square(x: i32) -> i32
{
    return x * x
}

pub distance(x: i32, y: i32) -> i32
{
    return math_utils.abs(math_utils.square(x) - math_utils.square(y))
}
```

```ax
// main.ax
use math_utils as mu

run() -> i32
{
    return mu.distance(5, 3)
}

x = run()
```

Two decisions were settled before implementation began, both explicit user choices rather than
defaults picked here:

- **Module identity is a declared name, not a file path.** `module math_utils` is what makes
  `math_utils.ax` module `math_utils` - the file could be renamed, or live anywhere the compiler
  is told to look, without changing what code that `use`s it writes. The file path is used only
  for *discovery* (finding the file that declares a given name), never for identity.
- **Access is always qualified, with optional aliasing** - `math_utils.square(x)`, or
  `mu.square(x)` after `use math_utils as mu`. No bare-import form (`use math_utils.{square}` ->
  bare `square(x)`) exists this phase - matches this language's own existing dot-access
  convention everywhere else (`struct.field`, `EnumName.Variant`), and avoids the collision risk
  a bare import would introduce.

---

# Design: Qualified Names Are Just Names, Discovery Is a Separate Pre-Pass

## `module math_utils` scopes the whole file, no braces

A single declaration, always the file's own module (or its absence: the file is the root/
anonymous program, today's exact status quo - unchanged for any file that never uses `module` at
all). There's no separate "module body" grammar - a file already *is* one, so scoping the whole
file to a name needs nothing more than reading that one declaration.

## Qualified access is `object.method(args)` - zero new grammar

`math_utils.square(x)` parses as an ordinary `MethodCallExpr` (`object.method(args)`), exactly
`EnumName.Variant(args)` construction already does (see `docs/language/0064-enums.md`) - no new
grammar needed at the call site at all. Every pass that resolves a `MethodCallExpr` gains one more
"is `object` a bare name matching something known, checked before generic resolution" branch,
mirroring `asEnumTypeName`'s own shape exactly: is `object` a bare `NameExpr` matching a *real
module name*? If so, resolve `<module>.<method>` directly, before ever treating `object` as an
ordinary value expression (which would otherwise throw "undefined variable: math_utils").

## Aliasing is resolved once, at parse time, in the aliased file only

`use math_utils as mu` records `mu -> math_utils` in the parser's own `aliases_` map. Every later
`mu.foo(...)`/`mu.field` postfix access rewrites `mu`'s own `NameExpr::name` to `math_utils`
*at the exact moment it's parsed* (`Parser::parsePostfix`, right where `object` is about to
become a `MethodCallExpr`'s/`FieldExpr`'s own object) - not anywhere else a bare `mu` might
appear, so an ordinary local variable that happens to share a name with an in-scope alias is
unaffected everywhere except that one position (the same latent shadowing risk this whole
codebase already accepts for a variable named after a declared enum type). By the time TypeChecker
or any later pass sees the AST, every reference already carries the *real* module name - no pass
past the parser needs to know an alias was ever written, and the alias table is never threaded
through any later pass's own API.

## `pub` gates external visibility, not internal use

A module's own function/extern is module-private by default; `pub` opts it into being reachable
via `use`. Enforced once, in `TypeChecker::checkExpr`'s `MethodCallExpr` case: a qualified
reference from *outside* the declaring module needs `isPublic`; the module's own code calling its
own sibling functions - always fully qualified too (`math_utils.square(x)`, even from within
`math_utils` itself - there's no bare-self-call convenience this phase, see Known Imprecision) -
is exempt. The exemption needs to know "which module is the *calling* code in," without a new
parameter threaded through `checkExpr`/`checkStmt`/`checkBlock`'s entire recursive signature: a
single member field, `currentFunctionModule_`, set once at the top of `checkFunction` (derived
from the enclosing function's own already-qualified name) and read - never reentered, since this
language has no nested function declarations/closures - through that whole function body's own
recursive traversal.

## `math.foo` and `math.bar` are literally `functions_["math.foo"]`/`functions_["math.bar"]`

A `FunctionDecl`'s own `name` is renamed to `"<module>.<name>"` the moment it's merged into the
final program - exactly the '.'-mangling `impl` methods already use for
`"TypeName.methodName"` (see `ImplDecl`'s own comment), reused verbatim rather than inventing a
parallel scheme. Every pass's own registration (`functions_[function->name] = function`) needs
*zero* changes to accept this - a qualified function is indistinguishable, at registration time,
from any other. Each pass separately, cheaply *re-derives* the set of real module names by
scanning its own already-built `functions_`/`externs_` for a '.' in the key (mirroring this whole
codebase's "each pass owns its own walk, no shared resolved-type state" convention) - main.cpp's
own module-loading pass never has to hand that set to anyone.

## Externs are the one asymmetric case: a real C symbol can't be renamed

`ExternDecl::name` is a *real, externally-linked C symbol* (`sqrt`, `abs`, ...) - renaming it to
`"math.sqrt"` the way a `FunctionDecl` is renamed would try to link against a symbol that doesn't
exist. So an extern keeps its own bare name untouched, gaining a `moduleName` field instead (set
by the module loader, "" for a root-file extern). A qualified call site (`math_utils.abs(x)`) is
resolved by looking the extern up by its own *bare* name and checking that field, not by
qualifying the lookup key - the one place where "qualified function" and "qualified extern"
resolution genuinely differ. Confirmed with a real, if predictable, bug found before this ever
worked: an early draft renamed `ExternDecl::name` too, which typechecked and even *emitted*
(`.`-qualified names are legal LLVM identifiers, unlike `|` - see `docs/language/0065-unions.md`'s
own naming clash), but tried to link against a nonexistent `@math_utils.abs` instead of the real
`@abs` - caught immediately by actually running the compiled output, not by reasoning about the
design.

## Discovery: the entry file's own directory, plus one `std/` found by walking up

`ModuleLoader::loadProgram` (a genuine standalone compiler component, `compiler/module/`, not
folded into `main.cpp` - both to keep `main.cpp` a thin CLI driver and so this whole pass is
independently testable, see `tests/ModuleLoaderTests.cpp`) parses the entry file, collects its own
`use` statements into a worklist, then for each wanted module name searches, in order: (1) *the
entry file's own directory*, and (2) a `std` directory found by walking up from there, one level
at a time, stopping at the first one found - so `examples/modules/main.ax` reaches this repo's own
`std/math.ax` two directories up, without needing `std/` to be a sibling of every program that
uses it. Whichever directory is checked first wins on a name collision (a module in the entry
file's own directory shadows a same-named one in `std/`). Candidate `.ax` files are parsed (and
discarded, if their own `module` name doesn't match) along the way. A loaded module's own `use`
statements are folded into the same worklist, so a chain of modules resolves transitively,
breadth-first. A module file with top-level executable code (`AssignmentStmt`/`ExprStmt` outside
any function) is rejected outright - only the entry file's own top-level code is meant to run.

This is deliberately narrow, not a general configurable search path - no CLI flag, no environment
variable, no install-relative path. It's the smallest extension that makes a `std/`-rooted library
layout (this repo's own `std/math.ax`, `std/io.ax`) actually reachable from a program that lives
anywhere below it, while still keeping "which module does `use x` resolve to" answerable by
reading two directories, never an open-ended tree walk.

A real, non-deterministic bug was found (and fixed) building this: `Lexer` stores its `source`
argument as a non-owning `std::string_view`, not an owned copy. An early draft wrote
`Lexer lexer(readFile(path.string()));` directly - `readFile(...)`'s returned `std::string` is a
temporary, destroyed at the end of that one statement, leaving `lexer`'s own view dangling for
every subsequent `lexer.lex()` call. This produced genuinely non-deterministic parse failures
(different, wrong errors on different runs of the identical input - a classic use-after-free
signature, confirmed by how the symptom changed shape under debug instrumentation and a fresh
build). The fix: keep the read source in a named local (`const std::string src = ...; Lexer
lexer(src);`) whose lifetime actually spans the lexer's own use, exactly the shape `main()`'s own
top-level `source`/`lexer` pair already had (which is why this bug was invisible in every
single-file command until module loading needed its own second parse site).

---

# Worked Example

```ax
// math_utils.ax
module math_utils

extern c abs(x: i32) -> i32

square(x: i32) -> i32
{
    return x * x
}

pub distance(x: i32, y: i32) -> i32
{
    return math_utils.abs(math_utils.square(x) - math_utils.square(y))
}

pub magnitude(x: i32) -> i32
{
    return math_utils.abs(x)
}
```

```ax
// main.ax
use math_utils as mu

run() -> i32
{
    d = mu.distance(5, 3)
    m = mu.magnitude(0 - 7)
    return d + m
}

x = run()
```

`x = 23` (`distance(5,3) = abs(25-9) = 16`, `magnitude(-7) = 7`) - verified byte-for-byte identical
across the interpreter, `-O0`, and `-O1`, including the emitted LLVM IR calling the real `@abs`
libc symbol directly (never a synthetic `@math_utils.abs`).

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **No bare self-module calls.** A module's own function must call its own sibling fully
  qualified (`math_utils.square(x)`), even from within `math_utils` itself - unlike Rust, where
  sibling items in a `mod` call each other bare. This was a deliberate scope cut made mid-
  implementation: supporting it would require threading "which module is the calling code in"
  through every pass's own call-resolution logic as a *new parameter* (not just TypeChecker's own
  `pub`-exemption, which only needed a single ambient member field since it's consulted in exactly
  one place); qualified-everywhere access needed no such threading anywhere, since a qualified
  reference already carries the real module name syntactically. A real ergonomics gap, not a
  correctness one.
- **No cross-module `struct`/`enum` sharing.** A `struct`/`enum` declared inside a module merges
  into the final program unqualified, usable freely by that module's own code, but there's no
  qualified type syntax (`x: math_utils.Vector2`) to reference it from outside. A module's own
  struct/enum names must not collide with anything else in the final merged program (they aren't
  namespaced at all, unlike functions/externs) - a real, if narrow, constraint given every real
  module this phase's own tests exercise sticks to functions/externs (the concrete "math library"
  motivating case).
- **Discovery searches exactly two directories** - the entry file's own, and one `std/` found by
  walking up - not a general configurable search path. A module used by a program must currently
  live either as a sibling `.ax` file next to the file being compiled, or inside the nearest
  ancestor `std/` directory.
- **Alias collisions across different files aren't specially diagnosed.** Two different files
  both aliasing a different module to the same short name is fine (aliases are resolved per-file,
  at parse time, before any cross-file merge happens) - but this was a simplifying design choice,
  not something separately tested against pathological cases.

---

# Guiding Rule

*A qualified name is still just a name.* The entire feature is "settle two things - what makes a
module a module (declared identity, decoupled from path), and how you reach into it (always
qualified, exactly the shape `object.method(...)` already has)". Every downstream mechanism -
registration, `pub` enforcement, LLVM symbol emission - falls out of choosing `'.'`-qualification
consistent with the mangling `impl` already established, rather than inventing a second, parallel
namespacing scheme. The one place that consistency has to bend is externs, where a real external
symbol's own identity can never be renamed - and that exception is exactly as large as the real
constraint forcing it, no larger.
