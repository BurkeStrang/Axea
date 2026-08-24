# `Buffer.write()`

**Status:** Implemented
**Document:** `0061-buffer-write.md`

---

# Motivation

`docs/language/Axea_Printing_Formatting.md`'s own "Buffer Formatting" section:

```ax
buf.write("Name: {user.name}\n")
```

> The intended distinction is:
>
> ```text
> append()  direct append
> write()   formatted/interpolated write
> ```

**This distinction doesn't correspond to any real runtime difference in Axea today, and this
phase implements it honestly as such.** String interpolation (`docs/language/0049-printing-
formatting.md`) is a **parser-level** transformation: any string literal argument, at any call
site, containing a `{expr}` span is already split into an `InterpolatedStringExpr` before
`Buffer.append` ever sees it - `buf.append("Age: {age}\n")` has interpolated its argument exactly
as much as `buf.write("Age: {age}\n")` would, because the interpolation happened before either
method name was even looked up. There is no "direct, unformatted" append operation to contrast
`write` against - `.append()` was always a formatted write, for any literal that happens to
contain an interpolation span, since `docs/language/0049-printing-formatting.md` first landed.

So `write` is added as a plain, same-behavior alias of `append`: it exists purely so callers can
use the source doc's own naming convention without it changing what actually happens. This is the
honest scope decision, in the same spirit as `docs/language/0058-debug-formatting.md`'s own
`{value:?}` call - rather than inventing a fake distinction to make the two names differ, or
silently dropping `write` as unimplementable, `write` is built as exactly what it already is: a
second spelling of `append`.

---

# Implementation: One New Case at Each Existing `"append"` Dispatch Site

No new AST node, no new IR instruction, no new runtime function. `"write"` was added as a
recognized method name alongside `"append"`/`"append_line"` at each of the four places `Buffer`'s
own method dispatch already lived:

- **`TypeChecker`** (`checkExpr`'s `TypeKind::Buffer` branch) - `"write"` joins the same
  1-argument, `isStrCoercible`-checked, unit-returning case `"append"`/`"append_line"` already
  share. Deliberately **not** added to the sibling `TypeKind::OwnedString` branch just above -
  `String` has no `write`, only `Buffer` does, matching the source doc's own example (`buf.write`,
  never `str.write`).
- **`CapabilityChecker`** - `"write"` joins the existing list of method names that raise `Write`
  on the receiver's own root parameter (the same list `"append"`/`"append_line"`/`"clear"`/
  `"reserve"`/`"finish"` already populate).
- **`Interpreter`** - `"write"` is checked in the exact same `if` as `"append"` inside the
  `BufferInstance` dispatch block, running the identical body (grow-check, `+=`).
- **`IrGenerator`** - a new `methodCall->method == "write"` case, placed right next to (not merged
  into) the existing `"append"` case: `"append"` itself has to disambiguate between `String` and
  `Buffer` via `bufferKind` (the two types share the method name), but `write` is Buffer-only per
  `TypeChecker`, so it always lowers straight to `IrBufferAppend` - the same instruction `"append"`
  itself lowers to on its own `Buffer` branch - with no disambiguation needed.

`LlvmIrEmitter` needed **zero** changes: `IrBufferAppend` already existed and already lowered
correctly; `write` reaching it via `IrGenerator`'s new case is indistinguishable from `append`
reaching it via its own. Confirmed, not assumed - a test asserts the two methods produce
byte-for-byte identical emitted IR for the same argument.

---

# A Pre-Existing Parser Gap, Found and Fixed Along the Way

`buf.write("hi")` failed to parse at all before this phase's own parser fix, with an unrelated-
looking error: `"expected field name after '.'"`. Root cause: `"write"` predates this phase as
`TokenKind::Write`, the parameter capability-prefix keyword (`write user: User`, see
`docs/language/0049-printing-formatting.md`'s own precedent for the *identical* problem at
`write(...)` call position) - `Parser::parsePostfix`'s `.`-handling unconditionally called
`expect(TokenKind::Identifier, ...)` for the name after a dot, which `TokenKind::Write` never
satisfies. Fixed with the same shape of fix `0049` already used for bare `write(...)` calls: right
after `match(TokenKind::Dot)`, if the current token is `TokenKind::Write`, it's consumed directly
instead of going through `expect(Identifier, ...)` - unambiguous, since `TokenKind::Write` only
ever otherwise appears as a capability prefix inside `parseParam`'s own parameter-list parsing, a
wholly different code path from postfix `.` access, so this can never misparse a real capability
prefix as a field/method name.

This fix is general, not `Buffer`-specific - `anything.write(...)` and (if such a field ever
existed) `anything.write` now both parse, the same way `.length`/`.append`/any other ordinary
identifier-named member already did. Found only because this phase's own worked example was run
for real, not just reasoned about on paper - the same lesson `docs/language/0057-alignment.md`'s
own "Real, Pre-Existing Bug Found Along the Way" section already drew.

---

# Worked Example

```ax
struct User { name: str  age: i32  score: f64 }

run() -> i32
{
    user = User { name: "Ada", age: 30, score: 92.5 }
    buf = Buffer()
    buf.write("Name: {user.name}\n")
    buf.write("Age: {user.age}\n")
    buf.write("Score: {user.score:.2}\n")
    text = buf.finish()
    print(text)
    return 0
}

x = run()
```

```text
Name: Ada\nAge: 30\nScore: 92.50\n
```

(The literal `\n` bytes print as-is, backslash and `n` both, rather than a real newline - ordinary
string literals have no escape processing at all yet, see `docs/language/0059-raw-strings.md`'s
own Motivation section; unrelated to `write` specifically.)

Hand-verified byte-for-byte identical between the interpreter and the LLVM backend (`-O0` and
`-O1`), and confirmed the emitted LLVM IR for `b.write("hi")` is textually identical to
`b.append("hi")`'s own.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **`write` is a pure alias, not a distinct operation** - see Motivation. If a real distinction
  ever becomes meaningful (e.g. if `.append()` is ever changed to *not* interpolate, treating its
  argument as always-literal), `write` would be the natural place to keep the formatted behavior
  and `append` the place to drop it - not attempted now, since nothing in the source doc or the
  current design calls for `.append()` to stop interpolating.

---

# Guiding Rule

> When a source doc proposes two names for what the codebase's existing design already makes one
> operation, the honest move is to implement the second name as a real, working alias - not to
> invent an artificial distinction just to make the names differ, and not to skip the feature
> because "there's nothing new to build." Running the worked example for real, as always, is what
> caught the one genuine gap here (`.write` failing to parse at all) that reasoning about the
> feature's own design would never have surfaced.
