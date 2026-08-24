# `print`/`write` and String Interpolation: Core Only

**Status:** Implemented (core subset only - see Known Imprecision)
**Document:** `0049-printing-formatting.md`
**Source doc:** `docs/language/Axea_Printing_Formatting.md` (Draft, full scope)

---

# Motivation

`docs/language/Axea_Printing_Formatting.md` proposes a wide surface:
`print()`/`write()`, string interpolation, numeric format specs
(`:05`, `:x`, `:.2`), alignment (`:<20`), debug formatting (`{x=}`,
`{value:?}`), `Buffer.write()`, traits/`Display`, collection `.join()`,
and raw/multiline string literals. That is too much to land as one
change. This phase implements exactly two of that document's Design
Goals - "make ordinary printing as terse as Python" and "make
interpolation the default formatting mechanism" - and nothing past
them:

```ax
print("Hello world")
write("Loading...")

name = "Burke"
age = 35
print("Hello {name}, you are {age}.")
print("Next year you will be {age + 1}.")
```

`print`/`write` are new compiler builtins, special-cased by literal
callee name the same way `.parse<T>()`/`.to_cstr()`/`String()`/`Buffer()`
already are (`docs/language/0046-generic-methods.md`,
`docs/language/0048-ffi.md`). String interpolation is a **parser-only**
feature - the lexer is untouched.

---

# Design: Interpolation Splits Already-Lexed Text, It Doesn't Re-Lex

A string literal's raw text (after the lexer has already stripped the
surrounding quotes) is post-processed by the parser into alternating
literal/expression pieces, splitting on unescaped `{...}` spans; `{{`
and `}}` are literal-brace escapes. A literal with **no** interpolation
span at all is built as a plain `StringExpr`, unchanged from before this
phase - every pre-existing string literal in this codebase (and every
test) is unaffected. A literal with one or more spans becomes a new
`InterpolatedStringExpr` node instead:

```cpp
struct InterpolatedStringExpr final : Expr
{
    struct Piece
    {
        std::string literalText; // used when expr == nullptr
        std::unique_ptr<Expr> expr; // used when non-null
    };
    std::vector<Piece> pieces;
};
```

Each `{expr}` span's own text is lexed and parsed as a standalone
expression via a **fresh, nested** `Lexer`+`Parser` pair - legal to call
`parseExpression()` on another `Parser` instance directly, since C++
access control is per-class, not per-object. This means an interpolation
span can itself contain arbitrary expressions (`{age + 1}`, `{user.name}`)
with zero new grammar.

`InterpolatedStringExpr` is always typed `String` (owned) - matching the
source doc's own "when runtime construction is necessary, the result is
an owned String" line, and reflecting that interpolation genuinely
builds new text at runtime rather than reusing a `str`'s existing
backing bytes.

Only types with a well-defined text representation this phase may
appear as an interpolation piece or a `print`/`write` argument: `i32`,
`bool`, `char`, `str`, `String`. Struct/array/collection printing is
explicitly out of scope (see Known Imprecision).

---

# Parsing: `write` Was Already a Keyword

`"write"` predates this feature as `TokenKind::Write`, a parameter
capability prefix (`write user: User`). The ordinary
"identifier-followed-by-`(`-is-a-call" fallback in `parsePrimary` is
gated behind `current().kind == TokenKind::Identifier`, so it never
fires for a `write` token. A parallel, narrow special case is added
immediately before that block:

```cpp
if (current().kind == TokenKind::Write && peek().kind == TokenKind::LeftParen)
{
    advance();
    expect(TokenKind::LeftParen, "expected '(' after 'write'");
    auto args = parseArgumentList();
    expect(TokenKind::RightParen, "expected ')' after arguments");
    return std::make_unique<CallExpr>("write", std::move(args));
}
```

This is unambiguous: `write` followed immediately by `(` has no other
legal meaning in expression position (the capability-prefix use only
ever appears inside a parameter list, parsed by `parseParam`, a wholly
different code path). `print` needed no equivalent special case - it
was never a keyword, so it already parsed as a plain identifier call.

One consequence, not a regression: a *top-level function declaration*
literally named `write` (e.g. `write(x: i32) -> i32 { ... }`) already
could not parse before this phase either, for the same reason (top-level
item dispatch also gates on `TokenKind::Identifier`). The new
reserved-name rejection in `TypeChecker::registerSignatures` (see below)
therefore only ever fires for `print`, never for `write` - `write`'s
collision is caught structurally by the parser instead, with a less
specific error message. Documented here rather than "fixed" further,
since no code path needs the friendlier message.

**Follow-up: bare top-level `print(...)`/`write(...)` (no assignment)
originally did not parse at all**, contrary to this very doc's own
opening example above and the source doc's own examples - only silently
avoided by wrapping every worked example in a function (see
`examples/print_interpolation.ax`'s original content - the Worked
Example section below now also shows the fix's own top-level style,
appended to that same file). The root cause:
`Parser::parseItem()`'s top-level dispatch only ever offered `struct`/
`extern`/a function declaration (any `Identifier '('`, unconditionally)
or an assignment (`parseAssignment()`, which always `expect`s an
`Identifier` then `':'`/`'='`) - there was no "bare expression, kept for
its side effect" top-level statement shape at all, even though
`parseBlock()` already has exactly that for statements *inside* a
function body. Fixed with `Parser::looksLikeFunctionDecl()`, a bounded
lookahead (mirrors the existing `.method<T>(` 4-token lookahead) that
distinguishes `foo(x: i32) -> i32 { ... }` from `foo("hi")` by checking
what can *only* start a `Param` (`[read|write|take]? Identifier ':'`,
per `parseParam`) versus anything else - empty parens `foo()` need one
more token of lookahead past the `')'` (`->`/`{`/`=>` means a
declaration; anything else means a call). When it's not a declaration,
`parseItem()` now parses the expression and wraps it in an `ExprStmt`,
the exact same node `parseBlock()`'s own "non-trailing expression, kept
for its side effect" case already produces - and `write(...)`'s own
keyword-collision special case (just above) needed the identical
addition at the top-level-item layer, since `TokenKind::Write` never
satisfies `current().kind == TokenKind::Identifier` either.
`TypeChecker::check`/`IrGenerator::generate`/`Interpreter::run` each
needed one new `else if (ExprStmt)` branch in their own top-level-item
loops to actually check/lower/execute it (previously these loops only
ever recognized `FunctionDecl`/`AssignmentStmt` among executable items) -
without it, the new `ExprStmt` would have parsed successfully and then
been silently dropped, never actually running.

---

# Type Checking: A New `isTextRepresentable` Allowlist

`print`/`write` are checked *before* the ordinary function/extern
signature lookup in `checkExpr`'s `CallExpr` handling - like `extern`
calls, they are never registered as real functions. Each argument is
checked against a new helper:

```cpp
bool isTextRepresentable(const Type& type)
{
    return type == kI32 || type == kBool || type == kChar || isStrCoercible(type);
}
```

`InterpolatedStringExpr` uses the same helper for every non-literal
piece, and always types the whole expression as `String`.
`registerSignatures` rejects a user `FunctionDecl` or `ExternDecl`
literally named `"print"` (see the note above for why `"write"` doesn't
need - or get - the same check).

**Follow-up: `print`/`write` accept a struct argument now** (originally
via a one-off `argType.kind != TypeKind::Struct` carve-out at their own
call site, since interpolation still needed a genuine *string* to
concatenate and struct only had a print-direct helper at the time) -
superseded by `docs/language/0054-collection-printing.md`, which gave
every struct (and every collection kind except `slice<T>`) a real
stringify-to-a-string function too, letting `isTextRepresentable` itself
grow to cover all of it - `print`/`write`'s own check collapsed back to
a plain `isTextRepresentable(argType)` call, identical to interpolation's
own, once the allowlist itself widened.

---

# Capability Checking / Region Checking: Zero New Inference

`print`/`write` arguments and interpolation pieces are recursed into for
move-checking and region-inference purposes only - reading a value to
print it never raises a capability past `read`, and an
`InterpolatedStringExpr`'s result region is always `Owned` (it always
allocates a fresh `String`). No new inference logic; both checkers just
needed a `dynamic_cast` case that recurses into the pieces and passes
`print`/`write`'s `CallExpr` through the same generic path an
unregistered call already took.

---

# `IrGenerator`: Desugaring Interpolation to Buffer Operations

Per the source doc's own hint ("the compiler may lower interpolation
internally to efficient Buffer operations"), `InterpolatedStringExpr`
lowers to a real sequence of Buffer IR instructions, reusing every piece
of Buffer machinery `docs/language/0043-buffer.md` already built:
`IrBufferNew` → per-piece `IrBufferAppend` (literal text, via a fresh
`IrConstString`) or `IrBufferAppendValue` (expression pieces) →
`IrBufferFinish`.

Two new IR instructions:

- `IrPrint{args, addNewline}` - unifies `print`/`write`; the only
  difference between them is the trailing newline.
- `IrBufferAppendValue{buffer, value}` - stringify-and-append a value
  that isn't already `str`-coercible on its own (`i32`, `bool`).

Both are unit-typed and, per this codebase's established convention for
every prior unit-returning instruction, never given a real LLVM SSA
value via `defineRegister` - safe, because nothing downstream calls
`ref()` on a `"void"`-typed register.

---

# `LlvmIrEmitter`: Two New Runtime Stringifiers, One Timing Hazard Avoided Again

`resolveStrPtr` already turns any `str`/`String` register into a bare
`i8*`. This phase adds `stringifyValue()`, a superset that also handles
`i32`, `bool`, and `char` (reusing the existing UTF-8 char encoder),
dispatching on the register's inferred LLVM type.

- **`@axea.i32.to_str(i32) -> i8*`** - a real `sprintf("%d", ...)` call
  (not hand-rolled `itoa`): simpler, and correctly handles `INT32_MIN`
  for free. Requires a new libc extern, `@sprintf(i8*, i8*, ...)`.
- **`@axea.bool.to_str(i1) -> i8*`** - hand-rolled: branch on the `i1`,
  each arm mallocs its own buffer and GEP+stores literal ASCII bytes for
  `"true\0"`/`"false\0"`. No global string constant needed.

Both functions' own format-string/branch text is declared as a
**self-contained runtime-text block**, with any string constants it
needs declared *inline*, not via the shared `stringPtrConstant`/
`stringGlobals_` hoisting machinery. This sidesteps a real, previously
discovered timing hazard (first hit while implementing `@axea.parse.bool`
in `docs/language/0046-generic-methods.md`): `emitStringGlobals` runs
*before* instruction emission, so a string hoisted *during* instruction
emission would never make it into the output. LLVM doesn't require
textual declare-before-use ordering for module-level globals, so a
self-contained block with its own inline globals sidesteps the ordering
question entirely.

`emitPrint` registers three fixed format globals once
(`@axea.fmt.s = "%s"`, `@axea.fmt.space = " "`, `@axea.fmt.nl = "\n"`),
then for each argument: `stringifyValue()` it, `printf("%s", ptr)` it,
with a literal-space `printf` between consecutive arguments and an
optional trailing literal-newline `printf` (`print` only).

`emitBufferAppendValue` is a full structural duplicate of the existing
`emitBufferAppend` (same grow-check/copy-loop/null-terminate shape, no
`phi`) - the only difference is calling `stringifyValue()` instead of
`resolveStrPtr()`. Deliberately not factored into a shared helper, per
this codebase's "separate over shared" convention for whole *operations*
(as opposed to small sub-computations, which are shared: `resolveStrPtr`,
`ensureBufferCapacity`, `stringifyValue`, `encodeCharUtf8` are all
reused, not duplicated).

---

# Interpreter: No New Stringification Logic At All

The interpreter already has one universal `toString(const Value&)` free
function covering every printable type generically. `print`/`write`
just loop over arguments and stream `toString(evaluate(...))` straight
to `std::cout`, space-separated, with an optional trailing `'\n'`
(`print` only) - checked *before* the `functions_.find()` lookup, the
same position `extern` calls are checked in.
`InterpolatedStringExpr` evaluation concatenates `toString(evaluate(piece))`
for each piece into one `std::string`, wrapped in a fresh
`StringInstance` - it does **not** mirror the LLVM backend's
Buffer-based desugaring; there was no need to, since the generic
`toString` already existed.

---

# Worked Example

`examples/print_interpolation.ax` (the second block was appended once the
top-level-parsing fix above landed, to demonstrate `print`/`write`
directly at top level - the first block was, and still is, valid too;
it was never *required* to be inside a function, just the only style
that happened to parse before the fix):

```ax
greet(name: str, age: i32) -> i32
{
    print("Hello", name)
    write("Loading...")
    print()
    print("{name} is {age} years old")
    print("next year: {age + 1}")
    print("literal braces: {{1, 2, 3}}")
    return 1
}

called = greet("Burke", 35)

name = "Burke"
age = 35
print("Hello {name}, you are {age}.")
write("Loading...")
print()
```

```text
$ ax run examples/print_interpolation.ax
Hello Burke
Loading...
Burke is 35 years old
next year: 36
literal braces: {1, 2, 3}
Hello Burke, you are 35.
Loading...
called = 1
name = Burke
age = 35
$ ax llvm-ir examples/print_interpolation.ax | clang -x ir -O1 - -o out && ./out
# byte-for-byte identical (also re-verified at -O0)
```

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

Deferred to future, separate phases - all present in the source doc:

- **Numeric format specs (`:05`, `:x`, `:X`, `:b`, `:o`, `:.2`) are now
  supported** - see `docs/language/0055-numeric-format-specs.md`.
- **Alignment (`:<20`, `:>20`, `:^20`) is now supported too** - see
  `docs/language/0057-alignment.md`, unlike numeric format specs, applies
  to any text-representable type (`str`/struct/collection included, not
  just i32/i64/f64).
- **Debug formatting (`{x=}`, `{value:?}`) is now supported too** - see
  `docs/language/0058-debug-formatting.md`. `{value:?}` is identical to
  `{value}` for every type except `str`/`String`, which get quoted - Axea
  has no Display/Debug trait distinction yet, so there's nothing else for
  a generic "debug" form to differ on.
- **No `Buffer.write()`** - only `Buffer.append()`/`.append_line()` exist.
- **No traits/`Display`** - a custom type cannot participate in
  printing/interpolation by implementing an interface.
- **`.join()` is implemented** - see `docs/language/0050-collection-join-and-slicing.md`
  (this bullet originally said otherwise; corrected here since it's directly adjacent to
  the two bullets just below, both fixed by the same later follow-up).
- **No raw strings** (`r"..."`) or **multiline strings** (`"""..."""`).
- **Nested string literals inside an interpolation span are now supported**
  (`"{x.join(",")}"` works) - originally, `Lexer::lexString` had no notion of an active
  interpolation span at all, so it stopped at the *first* `"` it found regardless of context,
  truncating the token well before the real closing quote whenever an expression segment
  contained its own string literal; `parseStringLiteral`'s later depth-tracked re-scan (see
  Parsing above) then correctly reported "unterminated interpolation expression" against
  that already-truncated text - a real bug, not a parser limitation, since the *lexer's* own
  token boundary was already wrong before parsing ever started. Fixed with a new
  `Lexer::scanStringSpan()`, mirroring `parseStringLiteral`'s own depth-tracking (including
  its identical `'{{'/'}}'` escaping rule) one layer earlier, plus genuine recursion for a
  string nested inside a string nested inside a span, to arbitrary depth.
- **`print(...)`/`write(...)` now accept a struct argument**, printing it directly by calling
  the exact same per-struct-type `@axea.print.<Name>` helper the top-level-binding printer
  already uses - no stringification needed, since print()/write() never actually need a
  *string*, only to print directly (unlike interpolation, which does need one to concatenate
  with surrounding text, and so stays restricted to `isTextRepresentable` types below).
- **`print(...)`/`write(...)`/interpolation of Array/List/Map/Set/etc. are now
  supported too** - see `docs/language/0054-collection-printing.md`, a
  genuinely larger follow-up than struct's own case (real per-shape
  "stringify to a heap string" runtime functions, not just reused
  print-direct ones) but built out rather than left as a permanent gap.
  `slice<T>` remains the one unsupported type - see that doc's own Known
  Imprecision section.
- **`write` as a function name cannot be declared**, and fails with a
  generic parser error (`expected identifier`) rather than the friendlier
  `TypeChecker` "reserved builtin name" message `print` gets - see
  Parsing above. Pre-existing behavior, not a new gap.

---

# Guiding Rule

> Two genuinely new problems this phase, both solved without touching
> the lexer or introducing new capability/region logic: how to build a
> second, nested parse pass out of the *existing* `Lexer`/`Parser`
> classes with zero new machinery (nested spans reuse `parseExpression`
> directly), and how to stringify primitives on the LLVM backend without
> re-triggering the mid-function string-hoisting timing hazard already
> discovered once before (solved the same way it was solved then:
> self-contained runtime-text blocks with their own inline globals).
> Everything genuinely new in the source doc past core printing and
> interpolation - formatting, alignment, traits, `.join()` - stays
> written down as explicitly deferred, not silently dropped.
