# Raw Strings: `r"..."`

**Status:** Implemented
**Document:** `0059-raw-strings.md`

---

# Motivation

`docs/language/Axea_Printing_Formatting.md`'s own "Raw Strings" section:

```ax
path = r"C:\Users\Burke\Documents"
```

> Raw strings disable normal escape processing.

**A genuine surprise found while scoping this phase: ordinary Axea string literals have no escape
processing at all today.** `Lexer::lexString`/`Lexer::lexChar`'s own comments already say "no
escapes" - there is no `\n`, `\t`, `\\`, or `\"` handling anywhere in the lexer or in
`Parser::parseStringLiteral`; every byte between quotes (outside of an interpolation span) passes
through completely unmodified except for `{{`→`{`/`}}`→`}` un-escaping. So the source doc's own
motivating line - "disable normal escape processing" - is close to a non-event: there is no escape
processing for a raw string to disable, since ordinary strings never had any to begin with. The one
thing a raw string genuinely still needs to disable, and does, is **string interpolation**: an
ordinary `"..."` literal splits on unescaped `{expr}` spans (`docs/language/0049-printing-
formatting.md`), and `r"..."`'s whole reason to exist is to opt back out of that for literal text
that happens to contain a `{`/`}` byte with no wish for it to mean anything - a Windows path
doesn't (this phase's own worked example), but the raw multiline JSON example in the source doc's
"Multiline Strings" section (`docs/language/0060-multiline-strings.md`) very much does.

---

# Lexing: A Prefix Check Ahead of the Identifier Branch

`Lexer::nextToken`'s dispatch gained one new case, checked *before* the existing
`std::isalpha(c) || c == '_'` branch that would otherwise consume a lone `r` as a one-character
identifier: `c == 'r' && peek() == '"'` routes into `lexString()` instead. This is unambiguous -
Axea has no implicit concatenation or juxtaposition, so an identifier `r` immediately followed by
`"` with zero whitespace between has no other legal meaning in this grammar; `r "hello"` (real
whitespace between) still lexes as `Identifier("r")` then a separate `String` token, confirmed by a
dedicated test.

`Lexer::lexString` now optionally consumes a leading `r` before scanning the quote(s) (see
`docs/language/0060-multiline-strings.md` for the `"""` triple-quote detection added alongside
this), and calls a new `scanRawStringSpan(quoteLen)` instead of the existing (renamed,
now `quoteLen`-parameterized) `scanStringSpan(quoteLen)` when the `r` prefix was present.
`scanRawStringSpan` is deliberately **not** a variant of `scanStringSpan` with a raw flag threaded
through - it is a wholly separate, much simpler loop with no brace-depth tracking and no recursion
into nested string literals at all, since a raw string has no interpolation spans for either of
those to matter for: it just advances until it finds the next closing quote run, full stop. The
whole captured token text (prefix, quotes, and content) is kept as one raw `TokenKind::String`
token, exactly as an ordinary string already was - no new `TokenKind` needed.

---

# Parsing: A Plain `StringExpr`, Zero Processing

`Parser::parsePrimary`'s `TokenKind::String` case now calls a new
`parseRawOrInterpolatedString(token.text)` instead of stripping exactly one leading/trailing quote
byte and calling `parseStringLiteral` directly. This new function decodes the token's own prefix
(`r`?) and quote length (1 or 3 - see `docs/language/0060-multiline-strings.md`) from its raw text,
slices out just the inner content, and then branches: a raw literal becomes a plain `StringExpr`
built directly from that content, completely untouched - not even `{{`/`}}` un-escaping runs,
since a raw string was never scanned with brace-depth tracking in the first place, so a literal
`{`/`}` byte in its content was never validated as balanced or escaped and running
`parseStringLiteral` over it would misinterpret it. A non-raw literal falls through to the
existing `parseStringLiteral(content)` call, unchanged behavior for every string literal that
predates this phase.

No new AST node. A raw string is indistinguishable, downstream of the parser, from any other
literal `StringExpr` that happens to contain a `{` or `\` byte - `TypeChecker`, `Interpreter`,
`IrGenerator`, and `LlvmIrEmitter` all needed zero changes, since `StringExpr` already existed and
already worked for arbitrary content.

---

# Worked Example

```ax
path = r"C:\Users\Burke\Documents"
print(path)

name = "Burke"
brace = r"literal {name} not interpolated"
print(brace)
```

```text
C:\Users\Burke\Documents
literal {name} not interpolated
```

Hand-verified byte-for-byte identical between the interpreter and the LLVM backend (`-O0` and
`-O1`), including an empty raw string (`r""`), an unterminated raw string (reported `Invalid` by
the lexer, then a normal parser "expected expression" error - no crash), and a raw string
immediately preceded by a real identifier named `r` with whitespace between (confirmed still two
separate tokens, not misread as a raw-string prefix).

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **There is nothing for a raw string to disable escape-wise**, because ordinary strings have no
  escape processing at all yet (see Motivation) - if/when real escape sequences (`\n`, `\t`, `\\`,
  `\"`) are ever added to ordinary string literals, raw strings should continue to disable them,
  the same way this phase's `r"..."` already disables interpolation; not built now since the
  underlying feature it would disable doesn't exist yet either.
- **`r"""..."""` (raw + multiline together)** is covered in
  `docs/language/0060-multiline-strings.md`, not here - the two features share their lexer/parser
  implementation but are documented against their own source-doc sections separately, matching
  that doc's own "Raw Strings" / "Multiline Strings" split.

---

# Guiding Rule

> A feature's own motivating example can be worth re-examining against the codebase's actual
> current state before implementing it as written - "raw strings disable escape processing" reads
> as obvious until a quick check shows ordinary strings never had escape processing to disable in
> the first place, at which point the *real* remaining job (disabling interpolation) turns out to
> be both smaller and better-motivated than the doc's own framing suggested.
