# C-Style Declaration Syntax: Prefix Return Types, Struct-Embedded Methods, `void`

**Status:** Implemented (Phase 1 of a planned full replacement - see "Phasing" below)
**Document:** `0068-c-style-syntax.md`

---

# Motivation

A more C-like declaration syntax: a return type *before* the name instead of a trailing `-> Type`
arrow, and methods declared directly inside a struct's own body instead of in a separate `impl`
block.

```ax
pub struct Counter
{
    i32 value

    pub Counter new(i32 initial)
    {
        return Counter { value: initial }
    }

    pub void increment(self)
    {
        self.value++
    }

    pub i32 current(self)
    {
        return self.value
    }
}
```

**Phasing.** The intended end state is a full replacement of the old `name: Type` field syntax,
trailing `-> Type` function syntax, and separate `impl<T> Type<T> { }` blocks - but that requires the
new grammar to exist *before* anything can be migrated to it, and the old syntax is currently
load-bearing in over a thousand existing unit tests, all of `std/collections.ax`, and every file
under `examples/`. So this is explicitly **Phase 1**: the new syntax is fully supported *additively*
- both spellings parse to the same AST and work side by side, everywhere, in the same file. Phase 2
(migrating existing code to the new syntax) and Phase 3 (removing the old grammar for real) are
separate, not-yet-scheduled future work.

---

# Design

## `void`

No new `TypeKind`, no lexer keyword. Wherever a C-style header's return-type slot is parsed, if the
parsed type text is exactly `"void"`, `FunctionDecl::returnType` is set to `std::nullopt` - the
identical value omitting `-> Type` already produces in old-style syntax. Every other pass already
treats "no return type" as `std::nullopt`, so this has zero downstream effect beyond the parser.

## C-style params: `(i32 initial)` instead of `(initial: i32)`

`Parser::parseParamCStyle()` mirrors `parseParam()` exactly, with the type and name order swapped.
`self` stays a bare, untyped first parameter in both styles (`parseSelfAwareParamCStyle`, mirroring
`parseSelfAwareParam`) - unambiguous, since a real C-style param literally named `self` (with some
other explicit type) is always *two* tokens (`SomeType self`), never one.

## C-style function header: `[pub] ReturnType name[<T,...>](params) { body }` (or `=> expr`)

`Parser::parseCStyleFunctionTail()` parses everything after an already-known `ReturnType name`
prefix - the optional `<T,U>` type-param list, `(params)`, then a body - shared by both a top-level
C-style function and a struct-embedded method.

**Disambiguation.** The header shape is `TypeAtom Identifier (`. A return type can itself be
arbitrarily rich type grammar (`*T`, `[T;N]`, `fn(T)->R`, `List<T,U>`) - too rich to duplicate as a
second hand-rolled lookahead scanner (this file's usual disambiguation style, e.g.
`looksLikeGenericCall`) without it silently drifting out of sync with `parseTypeName()`'s own
grammar over time. `tryParseCStyleFunctionDecl()` instead uses a **speculative parse with
rollback**: snapshot `index_`, attempt the real parse, and on any failure (or the header shape not
panning out) restore `index_` and let the caller fall through to ordinary statement parsing
unchanged. This is a deliberate, narrow deviation from the rest of this file's "peek-only scanner,
never backtrack" convention - used only here, where a scanner would have to duplicate real type
grammar.

Tried *before* every other `Identifier`-led branch in `parseItem`, including the old-style
`name<T,U>(...)` generic-function-decl check - both `List<T> makeList(...)` (a C-style header with a
*generic return type*, colliding on the shared `Identifier '<'` start) and
`newList<T>() -> List<T> { }` (old-style) still resolve correctly: the speculative attempt for the
old-style case parses `newList<T>` as a (spurious but syntactically valid) *type reference*, then
finds `(` where it expected the function's own name - which fails and rolls back cleanly, falling
through to the existing old-style branch untouched.

## Struct bodies: fields and embedded methods, generic-aware

`Parser::parseStructDecl`'s body loop now accepts an optional leading `pub`, then either an
old-style field (`Identifier ':'`, unambiguous, checked first, completely unchanged) or a C-style
member (`Type name`, optionally followed by `(` for a method). A method desugars into a
`FunctionDecl` named `"StructName.methodName"` - the *exact* mangled shape `parseImplMethod`
already produces for old-style `impl` blocks - so every downstream pass (TypeChecker's
`registerSignatures`, RegionChecker, CapabilityChecker, IrGenerator's general struct method
dispatch) needs **zero changes** to run them; they're indistinguishable from old-style impl methods
once parsed.

If the struct collected any embedded methods, a `ImplDecl` is synthesized to hold them (the
struct's own `typeParams`, and for a generic struct, `self`'s own type text built via
`buildSelfTypeText` - the same `Name<T,...>` construction `parseImplDecl` already builds for an
old-style `impl<T> Name<T> { }` block, factored out so both share it verbatim). A struct with no
embedded methods still yields just the one `StructDecl`, so old-style `struct { }` plus a separate
`impl { }` elsewhere is completely unaffected - `parseStructDecl`/`parseItem`/`parseProgram` were
adjusted to let one struct declaration push either one or two top-level items.

## Associated (self-less) functions - `Counter.new(5)`

`new` has no `self`. This works with **no new dispatch mechanism anywhere** - a genuinely pleasant
surprise found while implementing this: every pass that resolves a module-qualified call
(`moduleName.func()`) already derives its own `moduleNames_` set by scanning *every* registered
function's own name for a `.` and taking the text before it:

```cpp
// compiler/sema/TypeChecker.cpp (RegionChecker.cpp, IrGenerator.cpp, Interpreter.cpp all do the
// same thing):
for (const auto& [name, function] : functions_)
{
    if (const auto dot = name.rfind('.'); dot != std::string::npos)
    {
        moduleNames_.insert(name.substr(0, dot));
    }
}
```

This was already a documented "harmless quirk" (an impl method's own mangled `TypeName.method` key
is indistinguishable from a real module by name text alone) - `"Counter.new"` already makes
`"Counter"` look like a module name for free, so `Counter.new(5)` resolves through the *existing*
module-qualified-call machinery in every pass, unmodified. The one real consequence: `new` needs
`pub` for this to work from outside its own function (the same privacy rule a `pub` top-level
function already has) - a bare `Counter.new(5)` with a non-`pub` `new` throws `"function 'new' in
module 'Counter' is private"`, reusing that exact existing error message and mechanism.

**A real, previously-undiscovered bug found and fixed while testing this**: `currentFunctionModule_`
(the field each pass's privacy check consults to exempt a module's own internal self-reference) was
never reset before checking genuine top-level/root code - it silently inherited whatever the
*last*-checked function (in file order) left it as. `struct Counter { Counter new(...) {...} }  x =
Counter.new(5)` would incorrectly treat the top-level `x = ...` line as "inside Counter's own
module", silently exempting a non-`pub` `new` from its own privacy check. Fixed in
`TypeChecker::check`'s own top-level loop (reset to `""` per top-level item, mirroring
`insideUnsafe_`'s own identical reset-per-item pattern right next to it).

## Associated functions on an explicit generic instantiation - `Box<i32>.new(41)`

Unlike the bare `Counter.new(5)` case (an ordinary `NameExpr("Counter")` as the call's object),
`Box<i32>` isn't a bare identifier - it's a generic-type-reference expression shape parsing didn't
recognize in call-object position, so this needed two small additions beyond section "Associated
(self-less) functions" above:

**Parsing.** `Parser::looksLikeGenericTypeRefBeforeDot()` mirrors `looksLikeGenericCall()`'s own
bracket-depth-aware lookahead exactly, except it checks for a trailing `.` instead of `(`. Tried in
`parsePrimary`'s `Identifier '<'` dispatch, right after the existing `looksLikeGenericCall()` check
and before the plain-call check - so `Box<i32>.new(41)` parses `Box<i32>` into a `NameExpr` whose
`.name` holds the bracket-syntax canonical type text `"Box<i32>"` (reusing `NameExpr` rather than
adding a new AST node, matching this codebase's established "type text as a canonical string,
structurally sniffed wherever needed" idiom - see `isBuiltinGenericName`, `mangleTypeText`,
`splitGenericInstantiation`). The result is an ordinary `MethodCallExpr{object: NameExpr("Box<i32>"),
method: "new", args}` - the exact same shape `Counter.new(5)` already produces, just with a
bracket-syntax name instead of a bare one.

**Monomorphization.** A bracket-syntax `NameExpr` in a `MethodCallExpr`'s object position is a type
reference, not a variable reference, so `GenericMonomorphizer`'s `collectTypeRefsInExpr` and
`cloneExpr` both needed a small addition to recognize it (checking `object->name.find('<') !=
npos`) - `collectTypeRefsInExpr` queues `&objectName->name` as an ordinary type ref (so the existing
fixed-point loop instantiates `Box$i32` and rewrites the `NameExpr` in place, exactly like it already
does for a `Box<i32> { ... }` struct literal or a `Box<i32>` return-type annotation), and `cloneExpr`
substitutes type params within it (for correctness when this call shape appears inside another
generic's own body). No changes were needed to *resolve* the call once monomorphized - `Box$i32.new`
is an ordinary dotted function name, so the pre-existing `moduleNames_` machinery (see above) picks it
up for free, identical to `Counter.new`.

**A second real bug found and fixed while testing this**: `synthesizeGenericImplMethods` (the
function that clones a generic struct's `impl` methods into a concrete monomorphized copy, e.g.
`Box<T>.new` into `Box$i32.new`) never copied `FunctionDecl::isPublic` from the template method onto
its synthesized clone - so a correctly-`pub`-annotated `Box<T>.new` still produced a non-`pub`
`Box$i32.new`, tripping the same privacy check `Counter.new` relies on and failing with `"function
'new' in module 'Box$i32' is private"` even though the source said `pub`. Fixed by setting
`clone->isPublic = method->isPublic;` when constructing the clone.

## `pub` on structs/fields/methods

Parsed and accepted wherever the new syntax puts it, for the new syntax to *look* right, but not
wired into new visibility enforcement beyond what `pub` already does on a top-level function today
(gating external module-qualified-call access - see "Associated functions" above). `pub` on a
struct or a field is parsed and discarded, matching how `pub` before an old-style struct decl
already was (structs have no enforced per-declaration visibility gate at all, `pub` or not).

---

# Known Limitations (This Phase)

- **No per-method independent generics** (`pub T convert<U>(self, U input)`) - embedded methods
  only ever inherit the enclosing struct's own type params, exactly like old-style `impl` methods
  already do; no new capability beyond what old-style syntax already supports.
- **No new visibility enforcement** - see `pub` above.
- Phase 2 (migrating `std/collections.ax`, `examples/`, and the test suite to the new syntax) and
  Phase 3 (removing the old grammar) are both separate, not-yet-scheduled future work.

---

# Worked Example

`examples/cstyle_struct.ax` - a `Counter` (associated `new` + instance methods), a generic `Box<T>`
(associated `new` on an explicit instantiation, `Box<i32>.new(41)`, plus embedded instance methods),
and an `OldPoint` (fully old-style `struct` + separate `impl`) all in the same file, plus a top-level
C-style function and one using the `=>` shorthand:

```text
$ ax run examples/cstyle_struct.ax
counterValue = 7
boxValue = 42
pointSum = 30
sumResult = 7
squareResult = 81
$ ax llvm-ir examples/cstyle_struct.ax | clang -x ir -O1 - -o out && ./out
# byte-for-byte identical (also re-verified at -O0)
```
