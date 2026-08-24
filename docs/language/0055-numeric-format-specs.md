# Numeric Format Specs: `{expr:spec}` for Precision, Zero-Padding, and Radix

**Status:** Implemented
**Document:** `0055-numeric-format-specs.md`

---

# Motivation

`docs/language/Axea_Printing_Formatting.md`'s own "Numeric Formatting" section shows a specific,
bounded set of examples:

```ax
pi = 3.14159265
print("Pi = {pi:.2}")

value = 42
print("{value:05}")    // 00042
print("{value:x}")     // 2a
print("{value:X}")     // 2A
print("{value:b}")     // 101010
print("{value:o}")     // 52

flags = 0b10110110
print("flags = {flags:08b}")
print("flags = 0x{flags:02X}")
```

This phase implements exactly that: an optional `:spec` suffix on an interpolation span, where
`spec` is `['0'][width digits]['.' precision digits][type char]` - zero-padded width, float
precision, and `x`/`X`/`b`/`o` radix conversion. It does **not** implement the source doc's
separate "Alignment" section (`:<20`, `:>20`, `:^20` - a textually adjacent but conceptually
distinct feature, left/right/center-justifying a field, not discussed anywhere in the "Numeric
Formatting" section itself), and it does not implement `0b`/`0x` binary/hex *literal* syntax as
integer *input* (the `flags = 0b10110110` example above is written using a bare decimal literal
in this phase's own tests instead - literal syntax is an unrelated, separate piece of surface
area). Neither was asked for; both remain Draft.

---

# Grammar and Parsing

`Parser::parseStringLiteral` already depth-tracks an interpolation span's `{...}` (including
quote-awareness, so a `:` inside a nested string-literal argument like `{x.join(":")}` doesn't
get misread as a format-spec separator). This phase adds one more scan on top: within an already-
found span, look for a **top-level** `:` (same quote-awareness, plus depth-tracking for
`{[(`/`}])` so an expression like `{m.get(a:b)}` - not real Axea syntax, but the scan must still
not misfire on it - can't confuse a bracket/paren-nested `:` for the split point). Everything
before that `:` is `exprText` (parsed as an ordinary expression, exactly as before); everything
after it is the raw `formatSpec` text, stored unparsed on `InterpolatedStringExpr::Piece` (a new
third field, empty for a plain `{expr}` span with no `:`). An empty spec after `:` (`{value:}`)
or an empty expression before it (`{:05}`) both throw a clear parse error, matching this parser's
existing "reject empty interpolation expression" discipline for `{}`.

`compiler/sema/FormatSpec.hpp` (new, header-only) parses that raw text into a small
`FormatSpec { zeroPad, width, precision, type }` struct via `parseFormatSpec`. This is the
**first** case in this codebase of deliberately *sharing* logic across TypeChecker/Interpreter/
LlvmIrEmitter, rather than the separate-per-layer duplication this codebase otherwise
consistently prefers (see e.g. `docs/language/0054-collection-printing.md`'s own choice to
hand-transcribe `Buffer`'s growth algorithm a second time rather than share it) - justified
because spec-*text* parsing is pure syntax, with zero legitimate room for the three backends to
diverge on what `"08b"` means, unlike genuine runtime value computation where each backend's own
independent implementation is the actual point.

---

# Type Checking: Which Combinations Are Legal

A format spec's `type` char picks a mutually exclusive family, checked in
`TypeChecker::checkExpr`'s existing `InterpolatedStringExpr` piece loop, after the pre-existing
`isTextRepresentable` check:

- **Radix (`x`/`X`/`b`/`o`)** requires the piece's type to be `i32` or `i64`, and forbids a
  precision (`{n:.2x}` is rejected) - a radix conversion has no notion of "decimal places".
- **Precision (`.N`, with no radix type char)** requires `f64` - precision means "round to N
  decimal places", which only makes sense for a float.
- **Plain width (a bare digit count, no type char, no precision)** requires `i32` or `i64` -
  right-justifying (space- or zero-padding) a non-numeric value isn't part of this phase's scope
  (that's the separate, unimplemented Alignment section).

Each violation throws a specific error naming the actual mismatch, rather than a generic "invalid
format spec".

---

# Runtime: Interpreter and LLVM Backend

**Interpreter** (`formatValue` in `compiler/interpreter/Interpreter.cpp`): for every case except
binary, builds a `%[0][width][.precision][ll]<type>` format string dynamically and calls
`std::snprintf` - the same approach `toString` already uses for unformatted `i64`/`f64` output,
just with the extra flags/width/precision spliced in. Binary (`b`) has no `printf` conversion
specifier, so it's hand-rolled: extract bits LSB-to-MSB via right-shift-and-mask, prepend each
digit into a string (avoiding a separate reverse step), then pad to `spec.width` with `'0'`
(zero-padded) or `' '` (space-padded).

**LLVM backend** (`LlvmIrEmitter::registerFormatRuntime`): memoized per `(elementType, spec)` key,
so the same spec reused across multiple interpolation spans in one program only emits one helper
function. Two branches:

- **Binary** is hand-rolled with named registers, mirroring the interpreter's own logic
  structurally but in IR: a loop counts significant bits (`countHdr`/`countBody`/`countDone`,
  floored at 1 so zero prints `"0"` rather than an empty string via a `select`, not a separate
  branch), computes `totalWidth = max(specWidth, digitCount)` and the resulting `padCount`, fills
  padding (`padHdr`/`padBody`/`padFin`), then fills digits **MSB-first** directly
  (`digitHdr`/`digitBody`/`digitFin`, computing each digit's absolute bit position as
  `(digitCount - 1) - digitIdx` up front) - avoiding a string-reversal pass a naive LSB-first
  fill loop would otherwise need.
- **Everything else** (`x`/`X`/`o`, plain decimal width, float precision) builds a fixed
  format-string text (`"%05lld"`, `"%.2f"`, `"%08llX"`, ...) as a **self-contained global**
  (`@axea.fmt.spec.<id>`) declared directly in `toStrRuntimeText_`, deliberately **not** routed
  through `stringPtrConstant`/`hoistString` - this sidesteps the exact lazy-registration timing
  bug `docs/language/0054-collection-printing.md` found and fixed for collection-printing
  punctuation strings (a global referenced from deep inside a function body, registered after
  `emitStringGlobals`'s one-time snapshot, ends up referenced but never declared - an "undefined
  value" error at `clang` compile time). Applying the same fix proactively here, rather than
  waiting to rediscover the bug a third time, was a deliberate choice.

**Radix conversions always operate on the full 64-bit bit pattern**, regardless of whether the
piece's checked type was `i32` or `i64` - `i32` values are sign-extended to `i64` first
(`sext i32 %v to i64` / a matching cast in the interpreter). This is a deliberate simplification:
TypeChecker's inferred type is never persisted back onto the AST anywhere in this codebase, and
the interpreter's `Value` variant has no `i32`-vs-`i64` distinction at runtime (both share the
same `std::int64_t` alternative) - so there is no cheap way to recover "this was actually an
`i32`" at the point a radix conversion runs, and reinterpreting the full 64-bit two's-complement
pattern (matching what e.g. Rust's `{:x}` does for a negative signed integer) is a reasonable,
consistent choice rather than an accident. Plain decimal width/precision use the value's native
width unchanged (`%d`/`%lld`/`%f`) - only radix conversions need the 64-bit widening, since only
they reinterpret the value's bit pattern rather than its numeric value.

---

# Worked Example

```ax
pi = 3.14159265
print("Pi = {pi:.2}")

value = 42
print("{value}")
print("{value:05}")
print("{value:x}")
print("{value:X}")
print("{value:b}")
print("{value:o}")

flags = 182
print("flags = {flags:08b}")
print("flags = 0x{flags:02X}")
```

```text
Pi = 3.14
42
00042
2a
2A
101010
52
flags = 10110110
flags = 0xB6
```

Hand-verified byte-for-byte identical across the interpreter, `-O0`, and `-O1`, including edge
cases past the doc's own examples: negative numbers (zero-padded/hex/binary/octal
reinterpretation via the full 64-bit two's-complement pattern), `i64` values, zero (binary floors
at one `"0"` digit rather than printing empty), width without zero-pad (space-padding), precision
variations (`.0`, `.4`), and multiple format specs combined with a nested string literal in one
interpolation string.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **No alignment** (`:<20`, `:>20`, `:^20`) - the source doc's own separate "Alignment" section,
  not shown anywhere in "Numeric Formatting" and not asked for this phase.
- **No `0b`/`0x` literal input syntax** - a binary/hex integer *literal* (as opposed to a
  formatted *output* radix) is unrelated surface area; this phase's own tests construct such
  values from ordinary decimal literals instead.
- **No debug formatting** (`{x=}`, `{value:?}`) - a separate source-doc section, unrelated to
  numeric format specs specifically.
- **Radix conversions of a negative value reinterpret the full 64-bit pattern, not the
  minimal-width one** - see Runtime above for why this is a deliberate, unavoidable choice given
  this codebase's lack of a persisted static-type-per-AST-node mechanism, not an oversight.
- **Combining a radix type char with a precision, or a plain width with a non-integer type, are
  both rejected outright** rather than given some fallback interpretation - see Type Checking
  above; each is a `TypeChecker`-time error naming the actual mismatch.

---

# Guiding Rule

Scope a feature to exactly what its own source material demonstrates, not to the full breadth of
a document section's heading - "Numeric Formatting" the section header covers more surface area
(alignment lives right below it) than "Numeric Formatting" the bullet list the user actually
quoted. When a piece of logic has zero legitimate room for cross-backend divergence (spec-text
parsing, here), share it explicitly rather than defaulting to this codebase's usual
separate-per-layer convention - and say so, since the default is otherwise easy to mistake for an
inconsistency rather than a deliberate exception.
