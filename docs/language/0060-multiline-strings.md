# Multiline Strings: `"""..."""` and `r"""..."""`

**Status:** Implemented
**Document:** `0060-multiline-strings.md`

---

# Motivation

`docs/language/Axea_Printing_Formatting.md`'s own "Multiline Strings" section:

```ax
message = """
Hello {name},

Your build completed successfully.

Files: {count}
"""
```

> Normal multiline strings support interpolation.

```ax
json = r"""
{
    "name": "{literal}"
}
"""
```

> Raw multiline strings disable interpolation and escape processing.

This phase generalizes the ordinary single-quote (`"..."`) and raw (`r"..."`, see
`docs/language/0059-raw-strings.md`) string literals both already support to a triple-quote
delimiter (`"""..."""`/`r"""..."""`) that may additionally span real embedded newlines and
contain unescaped, non-consecutive `"` bytes.

---

# Lexing: A `quoteLen`-Parameterized Scan, Not a Third Copy

Both of the lexer's existing scan functions - `scanStringSpan` (interpolation-aware) and the new
`scanRawStringSpan` from `docs/language/0059-raw-strings.md` - gained a `quoteLen` parameter (1 or
3) rather than duplicating each into a `...Triple` variant. `Lexer::lexString` determines
`quoteLen` once, after optionally consuming a raw `r` prefix: three consecutive `"` bytes at the
current position (`current() == '"' && peek(1) == '"' && peek(2) == '"'`) means 3, otherwise 1 -
`peek` safely returns `'\0'` past end of input, never a real `"`, so a lone or doubled `"` right at
EOF is correctly still `quoteLen == 1`, not a false triple match.

**The closing-quote test is the only real behavioral change**, generalized from "a bare `"` at
brace depth 0" to "`quoteLen` consecutive `"` bytes at brace depth 0" - which for `quoteLen == 1`
is exactly the old check, unchanged. For `quoteLen == 3`, a **lone** `"` byte at depth 0 (not part
of a 3-in-a-row run) now falls through every special case in the scan loop and hits the same plain
`advance()` every ordinary content byte does - i.e. it's just literal content, matching the source
doc's own raw JSON example, which contains several standalone `"` bytes (`"name"`, `"{literal}"`)
that must not end the string early. `advance()` already tracked `line_`/`column_` correctly across
embedded `'\n'` bytes before this phase (needed for ordinary comments/whitespace already spanning
lines), so multiline content needed no line-tracking changes at all.

**Nested string literals inside an active interpolation span are always parsed with
`quoteLen = 1`**, regardless of the *outer* literal's own quote length - `scanStringSpan`'s
recursive call for a nested literal (e.g. `"""...{x.join(",")}..."""`) is hardcoded to
`scanStringSpan(1)`, since an expression segment nesting its own *triple*-quoted string literal
is exotic enough that the source doc never shows or motivates it, and ordinary single-quote
nesting is what every existing interpolation example already uses.

---

# Parsing: The Same `parseRawOrInterpolatedString`, One More Case

`Parser::parseRawOrInterpolatedString` (introduced in `docs/language/0059-raw-strings.md`) already
had to decode a token's raw/non-raw prefix from its captured text; this phase adds the matching
quote-length decode right next to it - `text[prefixLen..prefixLen+3] == "\"\"\""` means 3, else 1 -
and slices the inner content using `quoteLen` on both ends. Once `content` is sliced out correctly,
**no further change was needed on either branch**: the raw path already built a `StringExpr`
straight from `content` with zero processing, and the non-raw path already called
`parseStringLiteral(content)`, whose own char-by-char scan for `{`/`}`/`{{`/`}}` never treated `\n`
specially to begin with - an embedded real newline byte in `content` just flows into the current
literal-text accumulator like any other ordinary character, both for a plain `StringExpr` result
and for an `InterpolatedStringExpr`'s own literal pieces.

No new AST node, no new `TokenKind`, no changes below the parser - a multiline string's own
`StringExpr`/`InterpolatedStringExpr` node is indistinguishable from a single-line one carrying the
same content, so `TypeChecker`/`Interpreter`/`IrGenerator`/`LlvmIrEmitter` needed zero changes.

---

# LLVM Backend: Already Safe for Embedded Newlines

`LlvmIrEmitter`'s `llvmEscape` helper (used by `emitStringGlobals` when hoisting every string
literal into an LLVM `.ll` text-format global constant) already escapes every byte outside the
printable-ASCII range as `\XX` hex, including `'\n'` (`c < 0x20`) - added for genuine Unicode
character content (`docs/language/0044-char.md`) well before this phase, but exactly the mechanism
a real embedded newline byte inside a multiline string's own global constant needs too. No change
required here at all; confirmed, not assumed, by hand-verifying the LLVM output for a multiline
literal is valid `.ll` text and produces byte-for-byte identical output to the interpreter.

---

# Worked Example

```ax
name = "Ada"
message = """
Hello {name},

Your build completed successfully.
"""
print(message)

json = r"""
{
    "name": "{literal}"
}
"""
print(json)

s = """She said "hi {name}" to me, set = {{1,2,3}}"""
print(s)
```

```text

Hello Ada,

Your build completed successfully.

{
    "name": "{literal}"
}

She said "hi Ada" to me, set = {1,2,3}
```

Hand-verified byte-for-byte identical between the interpreter and the LLVM backend (`-O0` and
`-O1`), including an empty triple-quoted string (`""""""`), an unterminated triple-quoted string
(only two closing quotes - correctly reported `Invalid`, not accepted as a short literal), a lone
embedded `"` not ending the string early, and `r"""..."""` combining both raw (no interpolation)
and multiline (embedded newlines/quotes) at once.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **No leading/trailing-newline or common-indentation stripping** - unlike some languages'
  triple-quoted strings (e.g. Python's own dedent conventions), Axea's multiline literal keeps
  every byte between the delimiters completely verbatim, including the leading `\n` right after
  the opening `"""` when the content starts on its own line (visible in the worked example's own
  output above, which starts with a blank line). Not shown or asked for by the source doc's own
  example.
- **Four or more consecutive `"` bytes right at the closing delimiter** (e.g. content that itself
  ends in a literal `"` immediately before the real closing `"""`, producing `""""` in the source
  text) is not specially handled - the first 3-in-a-row run found closes the string, which may not
  be the byte position a human reading the source expected. Not exercised by the source doc's own
  example; a real escape mechanism would be the honest fix, and none exists yet (see
  `docs/language/0059-raw-strings.md`'s own Known Imprecision).

---

# Guiding Rule

> Generalizing an existing scan function with one new parameter (`quoteLen`) is worth doing over
> writing a parallel triple-quote-specific copy when the *closing condition* is the only thing that
> actually changes shape - every other line of `scanStringSpan`'s interpolation/nesting logic
> applies identically regardless of whether the delimiter is one quote or three, so parameterizing
> it kept the real diff to a handful of lines instead of duplicating a already-subtle function.
