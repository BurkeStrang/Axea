# Parsing & Formatting

**Status:** Implemented - `.parse<T>()`, `Optional<T>`/`?`, `print(...)`,
and string interpolation are all real; see
`docs/language/0046-generic-methods.md`,
`docs/language/0052-optional.md`, and
`docs/language/Axea_Printing_Formatting.md`.

```ax
year = date[..4].parse<i32>()?
print("Hello {name}")
```
Interpolation is preferred over concatenation. The `?` above requires the
enclosing function to itself return `Optional<T>` - see
`docs/language/0052-optional.md` for `.unwrap_or(default)`/`.is_some()`/
`.is_none()`, the non-propagating alternative usable anywhere.

**Deviation from this doc:** `.parse<T>()` is implemented for
`T ∈ {i32, i64, f64, bool}` (`i64`/`f64` added once those numeric types
themselves existed - see `docs/language/0051-numeric-widening.md`;
`.parse<f64>()` is a real `strtod` call, not hand-rolled decimal parsing),
and now genuinely returns `Optional<T>` (`docs/language/0052-optional.md`)
- invalid input is a real `None`, not a silently-returned fallback like
`0`/`0.0`/`false`. `print(...)`/`write(...)` and string interpolation
(`"Hello {name}"`) **are** implemented, correcting this doc's own earlier
claim otherwise - real lexer/parser support for interpolation exists (it
lowers into `Buffer` operations under the hood, see `0004-buffer.md`), and
`print`/`write` are real reserved builtins, not just top-level-binding
auto-printing.
