# Debug Formatting: `{expr=}` and `{expr:?}`

**Status:** Implemented
**Document:** `0058-debug-formatting.md`

---

# Motivation

`docs/language/Axea_Printing_Formatting.md`'s own "Debug Formatting" section shows two distinct
features under one heading:

```ax
x = 42
print("{x=}")
```

```text
x=42
```

```ax
print("{user}")
print("{user:?}")
```

The first, Python-style "self-documenting expressions", is unambiguous: echo the raw source text
of the expression, `=`, then its normal value. The second, `{value:?}`'s "debug representation",
is genuinely underspecified by the source doc - it shows the two print calls side by side but
never shows `{user:?}`'s actual output text, and Axea has no Display/Debug trait system (that
part of the source doc - "Formatting Traits" - remains Draft). Since a struct already prints with
field names (`Point { x: 1, y: 2 }`, effectively a debug-style view already), there's nothing a
generic "debug" representation would differ on for most types today. Asked directly, the answer
chosen for this phase: **`{value:?}` is identical to `{value}` for every type except
`str`/`String`, which get wrapped in quotes** - the one place today's single stringification
mechanism (no Display/Debug split) genuinely has room for a second, more literal representation,
mirroring Rust's/Python's own convention that `Debug` shows a string as `"text"`, not bare `text`.

---

# `{expr=}`: Self-Documenting Expressions

A trailing `=` immediately before the `:spec` split point (or the closing `}` when there's no
spec) marks a piece as self-documenting: the raw source text between `{` and `=`, verbatim
(never re-rendered from the parsed AST), followed by `=`, followed by the expression's own value.
`{age + 1=}` on `age = 30` prints `age + 1=31` - the exact text the user wrote, not some
canonicalized re-serialization.

**Never ambiguous with a comparison operator.** Axea has no expression-level `=` at all
(assignment is a statement, not an expression), so a bare trailing `=` can only be this marker -
`==`/`!=`/`<=`/`>=` are explicitly excluded by checking the character before a trailing `=` isn't
one of `=`/`!`/`<`/`>`, so `{a >= b}` parses as an ordinary comparison, not a truncated
self-doc expression.

**Combines with a format spec or debug mode for free**, since the split happens in
`Parser::parseStringLiteral` *after* the existing `:` colon-scan: `{pi=:.2}` prints `pi=3.14`,
`{s=:?}` prints `s="hi"`. Neither combination needed any special-casing - both just fall out of
doing the self-doc check on `exprText` after the colon split has already happened, since the
trailing `=` sits at the same position regardless of whether a spec follows.

**Lowered as an ordinary literal-text append.** `IrGenerator` needed no new IR field at all for
this: `{expr=}` desugars to a plain `IrConstString`/`IrBufferAppend` of the literal
`"<raw text>="`, emitted immediately before the existing `IrBufferAppendValue` for the expression
itself - the exact same mechanism a literal piece between two expressions already uses. The
Interpreter mirrors this by prepending `piece.selfDocPrefix + "="` to the accumulated content
directly, with no new stringification logic needed either.

---

# `{expr:?}`: Debug Representation

Parsed as a wholly separate, orthogonal piece-level flag (`Piece::debug`), not folded into
`FormatSpec.hpp`'s shared grammar at all: `"?"` is never itself a valid `FormatSpec` string (the
grammar's only legal type chars are `x`/`X`/`b`/`o`), and debug mode is conceptually "which
stringifier to call", not "how to format a number" - the doc never shows it combined with
width/precision/radix/alignment either. `TypeChecker` needs no additional validation beyond the
existing `isTextRepresentable` check every interpolation piece already gets: debug mode doesn't
narrow which types are legal, since it's defined identically to the unformatted case for
everything except `str`/`String`.

**Interpreter**: `toStringDebug` wraps `str`/`String` values in quotes and falls back to the
ordinary `toString` for everything else.

**LLVM backend**: a new standalone runtime function, `@axea.debug.quote_str(i8* text) -> i8*`
(registered at most once per program, the same "one shared function, no per-key memoization"
reasoning `registerAlignPadRuntime` already established - quoting is independent of what produced
the text), mallocs `len + 3` bytes and copies the text between two `"` bytes, no internal
escaping of embedded quotes/backslashes (real further work, not built this phase - nothing in the
source doc's own example asks for it). `stringifyValueDebug` calls it for `str`/`String` and
falls back to the ordinary `stringifyValue` dispatch for every other type - so a struct/collection/
numeric/bool piece under `:?` reuses its existing stringifier completely unchanged.

---

# Worked Example

```ax
x = 42
print("{x=}")

struct User { id: i32  name: str }
user = User { id: 1, name: "Ada" }
print("{user}")
print("{user:?}")

s = "hello"
print("{s}")
print("{s:?}")

age = 30
print("{age + 1=}")

pi = 3.14159
print("{pi=:.2}")
```

```text
x=42
User { id: 1, name: Ada }
User { id: 1, name: Ada }
hello
"hello"
age + 1=31
pi=3.14
```

Hand-verified byte-for-byte identical across the interpreter, `-O0`, and `-O1`, including edge
cases past the doc's own examples: a self-doc marker on an arbitrary expression (not just a bare
name), self-doc combined with a numeric format spec and with `:?`, an owned `String` quoted the
same as a bare `str`, and comparison operators (`==`, `!=`, `<=`, `>=`) confirmed *not*
misdetected as a truncated self-doc marker.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **`{value:?}` on a struct/collection is identical to `{value}`** - see Motivation above for
  why: there's no Display/Debug trait split yet for anything to differ on beyond `str`/`String`'s
  own quoting. Real differentiation (e.g. a struct's debug form additionally showing type
  information, or a collection's debug form differing from its own already-implemented printing)
  is deferred to whenever `docs/language/Axea_Printing_Formatting.md`'s own "Formatting Traits"
  section lands, if ever.
- **No escaping of embedded quotes/backslashes when quoting a debug string** - `{s:?}` on
  `s = "he said \"hi\""` produces the quote characters unescaped inside the outer quotes, not a
  real Rust/Python-style escaped debug string. Not shown in the source doc's own example.
- **`{expr=}` never truncates or reformats the echoed source text** - long expressions are
  echoed verbatim, in full, exactly as written.

---

# Guiding Rule

When a source document names a feature but the feature's own example is genuinely underspecified
(here: `{value:?}`'s actual output was never shown), the honest move is to name the ambiguity
explicitly and make a deliberate, documented choice rather than silently picking an interpretation
and hoping it matches what was meant - especially when the codebase has no existing mechanism
(here: a Display/Debug trait split) that would make one interpretation obviously "the" correct
one. And once again, small orthogonal pieces of syntax (a trailing `=`, a bare `?`) are cheapest
to implement as their own independent flags rather than forced into an existing grammar that
wasn't designed for them - `{expr=}` needed zero new IR fields at all, falling out entirely from
the pre-existing literal-append mechanism.
