# `.length`/`.bytes`: Unicode-Aware Counting, and the First Breaking Change to Shipped Semantics

**Status:** Implemented
**Document:** `0047-unicode.md`

---

# Motivation

`docs/std/strings/0006-unicode.md`, in full: "String operations work on
Unicode characters. Raw bytes are accessed through `text.bytes`." This
directly contradicts `0042-string.md`'s own original framing of
`.length` as "a byte count, not a codepoint or grapheme count" - and,
unlike every other doc implemented so far this session, honoring it
requires **changing the meaning of an already-shipped, already-tested
property**, not just adding something new. `.length` on `str`/`String`/
`Buffer` now counts Unicode scalar values (codepoints); `.bytes` is the
new name for what `.length` itself used to mean.

```ax
s = "héllo"
s.length   // 5 - five codepoints ('h', 'é', 'l', 'l', 'o')
s.bytes    // 6 - 'é' is a 2-byte UTF-8 sequence
```

**Confirmed as an intentional swap, not a guess.** Given the scale of the
behavior change (every prior `.length` call site on these three types,
across every doc/example/test already shipped), this was confirmed
directly rather than inferred silently - the alternative (leave `.length`
as bytes, add `.bytes` as a same-valued alias) would have satisfied the
doc's `.bytes` wording without delivering "operations work on Unicode
characters" at all.

**Also closes a real, previously-documented gap**: bare `str` had **no**
field access whatsoever before this phase - `docs/std/strings/0001-str.md`
flagged `.length` on `str` as an explicit known gap. Since both new
properties needed the identical underlying runtime machinery `String`/
`Buffer` already needed, `str` gained `.length`/`.bytes` in the same pass
rather than staying a separate, still-open gap.

---

# Design: `.bytes` Rides the Old Field, `.length` Becomes a Real Scan

**`.bytes` keeps the O(1) shape `.length` used to have.** For `String`/
`Buffer`, it's the exact same GEP+load of the stored byte-count field
every construction/append already maintains - no new runtime cost, just
a renamed accessor. For `str`, `.bytes` is the same `@strlen` call
`String`/`Buffer`'s own construction/append already issues for a
str-coercible operand.

**`.length` is now a genuine `O(n)` runtime scan**, not a field read - a
real complexity cost of correctness, not hidden. A new shared runtime
function, `@axea.utf8.count(i8*) -> i32`, implements the standard
"count non-continuation bytes" algorithm: every byte whose top two bits
aren't `10` (`byte & 0xC0 != 0x80`) starts a new codepoint. Lazily
registered once (mirroring `registerParseRuntime`'s own pattern, at the
smallest possible scale - exactly one function, ever). `.length` on
`String`/`Buffer` first extracts the data pointer from the header (the
same GEP `.bytes` used to read field 0 does, just field 1/field 2
respectively) and calls this on it; `.length` on a bare `str` calls it
directly, since `str` *is* already the data pointer.

**Three separate dispatch sites needed real changes, not one.** `str`
(a bare `i8*`) had **no** case in `emitFieldGet` at all before this phase
- reaching it was previously impossible (`TypeChecker` rejected every str
field access). `String` previously had **no dedicated branch either** -
its `.length` rode `isListType`'s own shared "field 0" chain for free
(`docs/language/0042-string.md`'s own "free ride" story), since a raw
byte-count read and a List's own `.length` read are identical in shape.
That free ride breaks the moment `.length` needs *different* behavior
per field name within the same type - String now needs its own
dedicated, self-contained branch (mirroring `Buffer`'s own precedent
from `docs/language/0043-buffer.md`), checked before the shared chain,
handling `.bytes` (the old field-0 read) and `.length` (the new scan)
explicitly rather than leaning on any shared code. `Buffer`'s own
existing dedicated branch (already required, for the `.length`-vs-
`.capacity` index distinction) simply grew a third case.

**A real register-ordering bug, caught by the diff discipline, not by
inspection.** The first version of the bare-`str` `.bytes` branch called
`defineRegister` for the destination *before* allocating the
`@strlen`-call's own intermediate register - defining a *lower*-numbered
SSA value whose own instruction textually appears *after* a
higher-numbered one it depends on, which LLVM's parser rejects outright
(`"instruction expected to be numbered '%N' or greater"`). Every other
new branch in this feature got this ordering right on the first attempt;
this one didn't, and it only surfaced by actually compiling a multi-byte
smoke test, not from reading the code - fixed by moving `defineRegister`
to strictly the last step in each branch, after every intermediate
register the branch itself needs.

---

# Type Checking / Capability Checking / Region Checking: Additive, Not Structural

`checkFieldType` gained a brand-new `TypeKind::String` (`str`, per
`docs/language/0042-string.md`'s own confusing-but-deliberate naming)
branch - the first field access `str` has ever supported - plus one new
accepted field name each on the existing `OwnedString`/`Buffer` branches.
No new *kind* of check anywhere: `.length`/`.bytes`/`.capacity` are all
still plain `i32`-returning field reads, the same shape every collection's
own `.length` already has.

`CapabilityChecker`/`RegionChecker` needed **zero** changes - a field
read has always been inherently read-only in this codebase (only method
calls and assignment targets ever raise `Write`), and the result is
always a plain `i32`, the simplest possible region (`Owned`, no aliasing
question at all). Adding a new field *name* to an existing read-only
access pattern is invisible to both passes by construction.

```text
$ ax capabilities bad.ax   # x = "hi".foo
error: str has no field 'foo' (did you mean 'length' or 'bytes'?)
```

---

# Interpreter: A Second Independent Scan, Verified Byte-for-Byte

`countCodepoints(const std::string&)` mirrors `@axea.utf8.count`'s own
byte-mask test exactly (`(byte & 0xC0) != 0x80`), written in plain C++ -
the same "separate over shared, verified via diffing" story every prior
interpreter-vs-backend pair in this codebase already has.
`str`/`String`/`Buffer`'s own `FieldExpr` cases each gained the identical
`.length`-now-scans/`.bytes`-is-the-old-value split their LLVM
counterparts have; `str`'s own case is entirely new (previously no
`FieldExpr` branch matched a bare `std::string`-typed `Value` at all).

```text
interpreter:  "héllo".length = 5   "héllo".bytes = 6
compiled:     "héllo".length = 5   "héllo".bytes = 6
"🚀🚀🚀".length = 3   "🚀🚀🚀".bytes = 12   (both backends agree)
```

---

# Worked Example

`examples/unicode.ax`:

```ax
describe(text: str) -> i32
{
    return text.length
}

ascii = "hello"
asciiLength = ascii.length
asciiBytes = ascii.bytes

accented = "héllo wörld"
accentedLength = accented.length
accentedBytes = accented.bytes

rockets = "🚀🚀🚀"
rocketsLength = rockets.length
rocketsBytes = rockets.bytes

viaFn = describe(accented)

owned = String("héllo")
ownedLength = owned.length
ownedBytes = owned.bytes

buf = Buffer()
appended = buf.append("héllo")
bufLength = buf.length
bufBytes = buf.bytes
bufCapacity = buf.capacity
```

```text
$ ax run examples/unicode.ax
ascii = hello
asciiLength = 5
asciiBytes = 5
accented = héllo wörld
accentedLength = 11
accentedBytes = 13
rockets = 🚀🚀🚀
rocketsLength = 3
rocketsBytes = 12
viaFn = 11
owned = héllo
ownedLength = 5
ownedBytes = 6
buf = héllo
appended = ()
bufLength = 5
bufBytes = 6
bufCapacity = 15
$ ax llvm-ir examples/unicode.ax | clang -x ir -O1 - -o out && ./out
# byte-for-byte identical (also re-verified at -O0), except bufCapacity
# (documented pre-existing divergence - see docs/language/0043-buffer.md)
```

`accentedLength = 11`/`accentedBytes = 13` confirms mixed-accent content
(`"héllo wörld"` has two 2-byte characters, `é` and `ö`) counts correctly
on both axes; `rocketsLength = 3`/`rocketsBytes = 12` confirms the same
for 4-byte codepoints; `asciiLength == asciiBytes` for pure-ASCII content
confirms the two views coincide exactly when there's nothing multi-byte
to distinguish them - the case every *existing* test in this codebase
happened to exercise before this phase, which is precisely why the
meaning swap didn't silently break anything already shipped.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **Codepoints, not grapheme clusters.** `.length` counts Unicode scalar
  values, not user-perceived "characters" - a combining-mark sequence or
  emoji built from multiple codepoints (e.g. a skin-tone-modified emoji,
  or ZWJ sequences) counts as more than one. Real grapheme-cluster
  segmentation needs Unicode property tables this compiler doesn't have.
- **`.length` is `O(n)`, not `O(1)`**, for all three types now - a real,
  accepted complexity cost, not an oversight. `.bytes` remains `O(1)` for
  `String`/`Buffer` (a stored field) and `O(n)` for `str` (`@strlen`,
  unchanged from before this phase).
- **No validation that content is well-formed UTF-8.** The counting
  algorithm assumes well-formed input (matching every other place this
  codebase trusts its own already-validated invariants) - malformed bytes
  produce an unspecified but deterministic count, not a crash or error.
- **No case-folding, normalization, or other Unicode text operations** -
  this phase is purely about counting, matching the design doc's own
  narrow two-sentence scope.

---

# Guiding Rule

> Every feature built earlier this session was purely additive - a new
> type, a new syntax form, a new method, never touching what an existing
> piece of already-shipped syntax *meant*. `.length`'s swap is the first
> genuine exception, and the discipline that made it safe wasn't
> "avoid breaking changes" but "confirm the breaking change is actually
> intended before making it, then make it completely, in every backend,
> not just the one convenient to change first." A property that's
> `O(1)` in one phase and honestly `O(n)` in the next isn't a design
> flaw to hide - it's the visible cost of a real semantic upgrade,
> written down here specifically so nobody rediscovers it as a surprise.
