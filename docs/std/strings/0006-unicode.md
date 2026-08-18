# Unicode

**Status:** Partially implemented - `.length`/`.bytes`, see
`docs/language/0047-unicode.md`.

String operations work on Unicode characters. Raw bytes are accessed through `text.bytes`.

`.length` (on `str`, `String`, and `Buffer`) now counts Unicode scalar
values (codepoints), and `.bytes` gives the raw byte count - implemented
exactly as specced, for all three text types, not just `String`.
Codepoints, not grapheme clusters: no Unicode property tables exist in
this compiler, so a multi-codepoint grapheme (combining marks, ZWJ
sequences) still counts as more than one. No other Unicode-aware
operations (case-folding, normalization, `slice<u8>`) are implemented.
