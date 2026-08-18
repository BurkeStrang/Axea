# Axea Standard Library

Mostly not yet implemented — see `docs/language/0023-standard-library.md` and `docs/roadmap.md` (Phase 7).

`String` (an owned, growable byte buffer) and `Buffer` (a mutable,
amortized-growth text-construction type that hands off to `String` via
`.finish()`) are implemented as compiler intrinsics — see
`docs/std/strings/` for the design docs and `docs/language/0042-string.md`/
`docs/language/0043-buffer.md` for the implementation diaries. No other
standard-library module exists yet.
