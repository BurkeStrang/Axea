# String

**Status:** Implemented - see `docs/language/0042-string.md` for the full
compiler implementation diary.

Owned, growable byte buffer (treated as UTF-8, but every operation this
phase is byte-level, not codepoint/grapheme-aware - see
`0006-unicode.md`). `String("Axea")`/`.append(...)`/`.length` are all real;
`Buffer` is also implemented separately (see `0004-buffer.md` and
`docs/language/0043-buffer.md`); range-slicing (`date[a..b]`) is also
implemented (see `0005-slicing.md` and `docs/language/0045-str-slicing.md`);
single-character indexing (`s[i]`) and interpolation-lowering are not.

```ax
name = String("Axea")
name.append(" Language")

n = name.length
```
