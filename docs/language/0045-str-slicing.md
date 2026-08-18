# `str[a..b]`: Range Slicing, and the Honest Limits of "No Allocation"

**Status:** Implemented
**Document:** `0045-str-slicing.md`

---

# Motivation

`docs/std/strings/0005-slicing.md` specs range-slicing over `str`:

```ax
year = date[..4]
month = date[5..7]
day = date[8..]
```

"Returns a `str` view with no allocation." This phase implements the
syntax and semantics exactly as shown - bounded (`a..b`), open-start
(`..b`), open-end (`a..`), and (for completeness, not shown in the
original doc but the same shape) fully-open (`..`) ranges, applied to
anything str-coercible (`str` or `String`, reusing `isStrCoercible` a
third way) - **except the "no allocation" part**, which turns out to be
impossible to deliver honestly without a much larger change to `str`
itself (see Design below). This is a deliberate, documented deviation,
not an oversight - the same kind of scope call `Buffer`'s own `.len` ->
`.length` rename and `String.append`'s "no amortized growth" already
made.

```ax
date = "2026-08-18"
year = date[..4]     // "2026"
month = date[5..7]   // "08"
day = date[8..]      // "18"
whole = date[..]      // "2026-08-18"
```

**Deliberately not extended to arrays/slices.** `docs/language/0032-slices.md`
already named `array[a..b]` sub-slicing as "Explicitly out of scope" for
`slice<T>`; this phase keeps that boundary - the new `a..b` syntax in
index-bracket position is restricted entirely to str-coercible objects, a
type error otherwise. Two genuinely different features share the
lexer's own pre-existing `..` token (`DotDot`, previously only consumed
by `for i in a..b`), but nothing else.

---

# Design: Why "No Allocation" Doesn't Survive Contact With `str`'s Own Representation

`str` (`docs/language/0042-string.md`) has no length field of its own -
it's a bare `i8*`, and every operation that needs its length (`@strlen`,
printing via `%s`) relies on the buffer being **null-terminated all the
way to its true end**. A genuine zero-copy "view" of `date[5..7]` would
have to be a pointer into the *middle* of `date`'s own buffer, 2 bytes
"long" - but there is no way to null-terminate a view 2 bytes in without
either overwriting the original buffer's own byte at that position
(corrupting `date` itself for anyone still holding it) or representing
`str` as a fat pointer (pointer + explicit length) everywhere, which
would ripple through every existing `str` call site in this backend
(`resolveStrPtr`, every `@strlen` call, every `%s` printf, `str` as a
`Map`/`Set` key, ...) for a single new feature.

Given that choice, this implementation does the honest thing instead:
`emitStrSlice` **allocates a fresh, appropriately-sized, null-terminated
buffer and copies the relevant bytes into it** - a real substring, safe
and correct, indistinguishable from a "true" zero-copy view from the
caller's own perspective (nothing about `str`'s own type or usage
changes), just not actually zero-copy. This is the same call `String.append`'s
own docs already made ("no amortized growth... the same 'reallocate every
time' honesty `List<T>.push`'s own docs already established") - implement
the visible behavior correctly, document the internal complexity honestly
instead of chasing the literal wording.

**Missing bounds default via the object's own runtime length, computed
once.** `emitStrSlice` resolves `object` to a bare `i8*` (`resolveStrPtr`,
shared with `emitStringNew`/`emitStringAppend`/`emitBufferAppend`'s own
identical str-coercion resolution). A present `start`/`end` is used
directly (already an `i32` register); a missing `start` defaults to the
literal `0`; a missing `end` triggers a real `@strlen` call - the same
"runtime-computed copy length" story `String`'s own construction/append
already established, now reused a third way. `length = end - start` is
computed once, then mallocs `length + 1` bytes and copies `length` bytes
via the same hand-rolled alloca/load/store counter loop (no `phi`) every
other copy operation in this backend uses, offset by `start` on the read
side. A trailing `\0` is written at `[length]`, keeping the result a
valid, fully self-describing `str` like any other.

**No bounds checking in compiled code.** Matches the established "no
bounds check" convention for out-of-bounds array/slice indexing in this
backend - an invalid range (`start > end`, or beyond the object's own true
length) produces garbage or a crash, not a checked error, when compiled.
The interpreter *does* check (see Interpreter below), the same split every
other indexing operation here already has.

---

# Parsing: One Postfix Branch, Reusing an Existing Token

`Lexer::DotDot` already existed (previously consumed only by
`for i in a..b`'s own range-loop desugaring) - no lexer changes needed at
all. `Parser::parsePostfix`'s existing `[` branch (which used to
unconditionally parse a single index expression) now parses an optional
`start` expression first, then checks for `..`: if absent, the original
`IndexExpr` path is taken unchanged (`arr[i]` continues to work exactly
as before - this is a real regression risk category, since the two shapes
share their opening `[`, tested explicitly); if present, an optional `end`
follows, and a new `StrSliceExpr` node is built instead. `StrSliceExpr` is
deliberately a *separate* AST node from `IndexExpr`, not a variant of it -
the same "separate over shared" call this codebase makes for every other
pair of genuinely different operations, here made sharper by the fact
that `IndexExpr`'s own object type (array/slice/List/Deque) and
`StrSliceExpr`'s own object type (str-coercible) don't even overlap.

```text
$ ax ast examples/str_slicing.ax
Assignment(year)
  StrSlice
    Name(date)
    Integer(4)
```

---

# Type Checking: A Third Reuse of `isStrCoercible`

`StrSliceExpr`'s own `checkExpr` case checks `object` against
`isStrCoercible` (the same "str, or String" rule `String(text)` and
`.append` already share), and - independently, only if present - `start`/
`end` against `kI32`. The result type is always `kStr`, regardless of
whether `object` was a `str` or a `String` - slicing a `String` produces
a plain `str`, not another `String`, matching "String lends a str"'s own
established one-way-coercion story.

```text
$ ax capabilities bad.ax   # x = [1, 2, 3][..2]
error: slicing requires str, got [i32; 3]
$ ax capabilities bad.ax   # x = date["a"..4]
error: slice start must be i32, found str
```

---

# Capability Checking / Region Checking: Read-Only, Always-Owned

`CapabilityChecker::inferExpr`'s new `StrSliceExpr` case mirrors
`IndexExpr`'s own exactly - recurses into `object`/`start`/`end` for
nested-mutation detection, but raises no write capability of its own,
since slicing never mutates anything. `checkMovesInExpr` gained the
identical recursive case, so a slice expression referencing an
already-moved `take` parameter is still caught.

`RegionChecker::regionOfExpr`'s new case mirrors `StringNewExpr`'s own
"always Owned, still walk sub-expressions" reasoning exactly - a slice is
a fresh allocation (see Design above), so its region is `Owned`
regardless of `object`'s own region, letting a slice taken from a
*borrowed* parameter be returned freely:

```text
$ ax run examples/str_slicing.ax
# firstFour(d: str) -> str { return d[..4] } compiles and runs fine -
# `d` is borrowed, but the slice itself owns its own fresh buffer
```

---

# `IrGenerator`: One New Instruction, `-1`-Sentinel Optional Operands

`IrStrSlice{object, start, end}` mirrors `IrInst`'s own `dest == -1`
"no destination" convention for its *own* optional operands: `start`/`end`
are each `-1` when the corresponding AST field was `nullptr`, lowered via
a plain ternary at the `StrSliceExpr` lowering site - no new `IrScope`
tracking needed (unlike `Buffer`/`String`'s own `isBufferExpr` resolver),
since `str.slice` is a brand-new method-free syntax form with no shared
name to disambiguate against anything else.

```text
$ ax ir examples/str_slicing.ax
%2 = str.slice %0[..%1]
%5 = str.slice %0[%3..%4]
%7 = str.slice %0[%6..]
%8 = str.slice %0[..]
```

---

# Interpreter: The One Place This Feature Actually Checks Its Own Bounds

`asStrContent(evaluate(*strSlice->object, env))` resolves `object` (str
or `String`) to a real `std::string` copy - the interpreter's own
`std::string::substr` already does exactly what `emitStrSlice`'s
hand-rolled malloc-and-copy loop does, just via the standard library
instead of hand-emitted LLVM IR. Unlike the compiled backend, this *does*
validate the range at runtime (`start < 0`, `end > length`, or
`start > end` all throw) before calling `substr` - the same
"interpreter checks, compiled code does not" split every other
out-of-bounds case in this backend already has.

```text
$ ax run bad.ax   # x = "hi"[0..5]
error: invalid slice range [0..5] for str of length 2
```

The result being a genuine copy (not a view into the original
`std::string`) was verified directly, not just assumed: a slice taken
*before* a later `.append()` on the source `String` does not retroactively
change - the identical aliasing-vs-copying test shape `String`'s own
interpreter bug fix (`docs/language/0042-string.md`) first established.

---

# Worked Example

`examples/str_slicing.ax`:

```ax
extractYear(date: str) -> str
{
    return date[..4]
}

date = "2026-08-18"
year = date[..4]
month = date[5..7]
day = date[8..]
whole = date[..]

viaFn = extractYear(date)

name = String("Axea Language")
firstFour = name[0..4]
```

```text
$ ax run examples/str_slicing.ax
date = 2026-08-18
year = 2026
month = 08
day = 18
whole = 2026-08-18
viaFn = 2026
name = Axea Language
firstFour = Axea
$ ax llvm-ir examples/str_slicing.ax | clang -x ir -O1 - -o out && ./out
# byte-for-byte identical (also re-verified at -O0)
```

`viaFn = 2026` confirms slicing works identically through a borrowed
`str` function parameter, not just a top-level literal binding;
`firstFour = Axea` confirms slicing a `String` (via the same
str-coercion `.append`'s own argument already uses) produces the correct
plain `str` result.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **Not actually zero-copy**, despite `docs/std/strings/0005-slicing.md`'s
  own "no allocation" framing - see Design above for why `str`'s existing
  representation makes a true view impossible without a much larger
  change. Functionally indistinguishable from a view at every call site
  that exists in this language today.
- **No bounds checking in compiled code** - an invalid range produces
  garbage or a crash when compiled, matches every other indexing
  operation's own identical simplification.
- **Not extended to arrays/slices.** `array[a..b]` remains out of scope,
  per `docs/language/0032-slices.md`'s own original exclusion.
- **No negative indices** (`date[-4..]`) - not shown in the design doc,
  and no signed-index convention exists anywhere else in this language.
- **Every slice is compiler-intrinsic syntax, not a real method or
  operator overload**, same as every other feature in this codebase.

---

# Guiding Rule

> A design doc's own words ("no allocation") are a *description of the
> intended developer experience*, not a binding implementation
> constraint - the same lesson `Buffer`'s `.len` rename and `String`'s
> "reallocate every append" already taught, now applied to a case where
> keeping the literal wording honest would have meant redesigning `str`'s
> entire existing representation for one feature. The right question
> isn't "can I match every word in the spec" but "does the *visible*
> behavior match, and is every place I diverged internally written down
> where the next person will find it" - `date[5..7]` behaves exactly like
> the view the doc promises to every caller in this language today; only
> the internals underneath quietly copy instead of pointing.
