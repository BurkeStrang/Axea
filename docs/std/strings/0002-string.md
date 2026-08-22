# String

**Status:** Implemented - see `docs/language/0042-string.md` for the full
compiler implementation diary.

Owned, growable byte buffer. `String("Axea")`/`.append(...)` are real;
`.length` counts Unicode codepoints (not raw bytes, not grapheme
clusters), `.bytes` gives the raw byte count instead - see
`0006-unicode.md`. `String == String` compares by real content, not
identity. `Buffer` is also implemented separately (see `0004-buffer.md`
and `docs/language/0043-buffer.md`); range-slicing (`date[a..b]`) is also
implemented (see `0005-slicing.md` and `docs/language/0045-str-slicing.md`);
single-character indexing (`s[i]`, a real Unicode codepoint index,
returning a `char`) is implemented too; string interpolation
(`"Hello {name}"`) lowers into `Buffer` operations under the hood exactly
as `0004-buffer.md`'s own "Compiler Optimizations" section describes -
implemented, not aspirational.

```ax
name = String("Axea")
name.append(" Language")

n = name.length
```
