# Axea Standard Library

Mostly not yet implemented — see `docs/language/0023-standard-library.md` and `docs/roadmap.md` (Phase 7).

The `docs/std/strings/` module is substantially implemented as compiler
intrinsics: `str`, `String`, `char`, `Buffer`, range-slicing (`date[a..b]`),
Unicode-aware `.length`/`.bytes`, `.parse<T>()`, and `extern c`/`cstr` FFI
are all real — see `docs/std/strings/` for the design docs and
`docs/language/0042-string.md` through `docs/language/0048-ffi.md` for the
implementation diaries. String interpolation and Unicode case-folding/
normalization remain unimplemented (`docs/std/strings/0006-unicode.md`'s
own `slice<u8>` and `0008-parsing-formatting.md`'s own interpolation
syntax). No other standard-library module exists yet.
