# Interpolation Alignment: `{expr:<N}` / `{expr:>N}` / `{expr:^N}`

**Status:** Implemented
**Document:** `0057-alignment.md`

---

# Motivation

`docs/language/Axea_Printing_Formatting.md`'s own "Alignment" section shows a specific, bounded
set of examples:

```ax
print("|{name:<20}|")
print("|{name:>20}|")
print("|{name:^20}|")
```

`<` means left aligned, `>` right aligned, and `^` centered, combined with a width to pad within:

```ax
for user in users
{
    print("{user.id:<6} {user.name:<20} {user.score:>8.2}")
}
```

This phase implements exactly that: an optional leading `<`/`>`/`^` on a `{expr:spec}` format
spec, padding the piece's own text representation to `width` with spaces. The second example
above is the reason this phase is broader than `docs/language/0055-numeric-format-specs.md`
turned out to be: `user.id` is an `i32`, but `user.name` is a `str` and `user.score` is an `f64`
combined with a precision - alignment has to work on **any** text-representable value, not just
numeric ones, since the source doc's own first example (`name:<20}`) aligns a plain string.

---

# Grammar and Type Checking

`compiler/sema/FormatSpec.hpp`'s `parseFormatSpec` gained a new optional leading character,
consumed before the existing `['0'][width][.precision][type]` grammar:
`[align: '<'/'>'/'^'] ['0'][width][.precision][type]`. Two combinations are rejected as
nonsensical syntax, independent of the piece's own type: an alignment char with no width (there's
nothing to align within), and an alignment char combined with `'0'` zero-padding (the two are
mutually exclusive fill strategies - alignment always space-fills).

`TypeChecker`'s existing bare-width restriction (`{value:05}` requires `i32`/`i64`) is relaxed
specifically when an alignment char is present: `{name:<20}` is legal for any
`isTextRepresentable` type, not just numeric ones. A radix conversion or precision combined with
alignment keeps its own pre-existing type restriction unchanged (`{n:>10x}` still requires
`i32`/`i64`; `{pi:>8.2}` still requires `f64`) - alignment only relaxes the *bare-width* case,
since that's the only one that was numeric-only for no reason deeper than "that's what
`0055-numeric-format-specs.md` needed at the time."

---

# Runtime: Pad the Already-Computed Text, Not a New Numeric Path

Both backends implement alignment as a **generic post-processing step** over whatever text the
piece would already produce - not a third parallel numeric-formatting code path. This is the key
design choice that keeps this phase small: computing "the text for this value" is a solved
problem (the existing `formatValue`/`registerFormatRuntime` for a radix conversion or precision,
or the plain `toString`/`stringifyValue` every unformatted interpolation piece already uses for
everything else, numeric or not); alignment only ever needs to pad that result.

**Interpreter**: `formatValueCore(value, spec)` produces the *unpadded* text - for a radix
conversion or precision, it calls the existing `formatValue` with `width`/`zeroPad` zeroed out
first (reusing its hex/octal/binary/precision conversion logic exactly, not a second copy of it);
otherwise it's the same `toString(value)` every plain interpolation piece already uses, so a
struct/collection/bool/str value works automatically. `padToWidth(text, align, width)` then pads
with spaces - a no-op if `text` is already at least `width` long (alignment never truncates).

**LLVM backend**: the same split, one level down. When a piece's spec has an alignment char,
`emitBufferAppendValue` first computes a "core text" `i8*` - via `registerFormatRuntime` called
with a modified `spec` (`width = 0`, `zeroPad = false`) for a radix/precision piece, reusing its
existing conversion IR unchanged, or via the existing `stringifyValue` (the same generic dispatch
`print`/unformatted interpolation already use) for everything else - then calls a new shared
runtime function, `@axea.align.pad(i8* text, i32 width, i8 alignCode) -> i8*`, registered **once**
per program regardless of how many differently-aligned spans use it (unlike
`registerFormatRuntime`'s own per-`(type, spec)` memoization - alignment padding is a pure string
operation, independent of what produced the text, so one shared function suffices).
`@axea.align.pad` computes the padding split via `select` on the align char's raw byte code
(`'<'`=60, `'>'`=62, else `'^'`=94, passed as a compile-time-known literal at each call site) and
fills a fresh malloc'd buffer with a hand-rolled loop, the same named-register convention every
other standalone runtime function in this backend uses. For `'^'` with an odd padding amount, the
extra space goes on the right - an arbitrary but consistent tie-break, matched exactly between
both backends (verified, not assumed) so they never disagree on it.

---

# A Real, Pre-Existing Bug Found Along the Way

While verifying the source doc's own combined worked example (a `for`-loop body containing
`print("{user.id:<6} {user.name:<20} {user.score:>8.2}")`), the LLVM backend crashed with
`error: unordered_map::at` - and, critically, this turned out to have **nothing to do with
alignment**: it reproduced identically with no format spec at all
(`print("{a} {b}")` inside a `for` loop), and reproduced against a clean build of this
repository's own `HEAD` commit from before this session touched anything, confirming it as a
genuine pre-existing bug, not a regression introduced by this phase.

Root cause: `collectStrings` - the one-time pre-pass that hoists every string literal in the
program into an LLVM global constant, *before* real code emission begins - recurses into
`IrBranch`'s own `thenBlock`/`elseBlock` to find literals nested inside an `if`, but was never
extended to recurse into `IrLoop`'s `conditionBlock`/`body`. Any string literal appearing inside a
`for`/`while`/`loop` body - including the literal *text* pieces of an interpolated string, e.g.
`"{a} {b}"`'s own literal `" "` between the two expression pieces - was therefore never hoisted,
and its later reference (`stringGlobalByLiteral_.at(...)`, assuming the pre-pass already ran)
threw `std::out_of_range` at real emission time. Fixed by adding an `IrLoop` case to
`collectStrings` that recurses into both blocks, the same way the existing `IrBranch` case
already does (correctly handling nested loops too, since the function is naturally recursive).
This is why it went unnoticed until now: a single-expression interpolation span with no literal
text around it (`"{x}"` alone) produces no `IrConstString` at all, and the existing test suite
had no case combining a *multi-piece* interpolated string with a `for`-loop body before this
phase's own doc-verification pass exercised exactly that combination for the first time.

---

# Worked Example

```ax
name = "Burke"
print("|{name:<20}|")
print("|{name:>20}|")
print("|{name:^20}|")

struct User { id: i32  name: str  score: f64 }

printUsers(users: [User; 2]) -> i32 {
    for user in users
    {
        print("{user.id:<6} {user.name:<20} {user.score:>8.2}")
    }
    return 0
}

users = [User { id: 1, name: "Ada", score: 92.5 }, User { id: 2, name: "Grace", score: 88.125 }]
r = printUsers(users)
```

```text
|Burke               |
|               Burke|
|       Burke        |
1      Ada                     92.50
2      Grace                   88.12
```

Hand-verified byte-for-byte identical across the interpreter, `-O0`, and `-O1`, including edge
cases past the doc's own examples: a value already at least as wide as the target width (no
truncation), an odd center-alignment padding amount, alignment combined with a radix conversion
(`{n:>10x}`), and alignment applied to a `bool`.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **No custom fill character** - the source doc only ever shows space-fill; a Python-style
  `{value:*<10}` (an arbitrary fill char before the align char) isn't part of this phase.
- **No debug formatting** (`{x=}`, `{value:?}`) - a separate source-doc section, unrelated to
  alignment specifically.
- **Alignment never truncates** - a value wider than the target width passes through unchanged,
  matching every other width-related format spec's existing "pad only, never cut" behavior in
  this codebase.

---

# Guiding Rule

The cheapest way to add a new format-spec dimension is often to make it a **post-processing step**
over an already-solved "produce this value's text" problem, rather than a new parallel
value-computation path - alignment needed exactly one new primitive (pad a string to width) built
once and shared, not per-type variants. And a scoped feature's own worked example is still worth
running for real: it's what surfaced a genuinely unrelated, pre-existing bug (`collectStrings`
never recursing into loop bodies) that no amount of reasoning about alignment's own design would
have found, since the bug had nothing to do with alignment at all.
