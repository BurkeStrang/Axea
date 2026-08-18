# Parsing & Formatting

**Status:** Partially implemented - `.parse<T>()` itself, see
`docs/language/0046-generic-methods.md`.

```ax
year = date[..4].parse<i32>()?
print("Hello {name}")
```
Interpolation is preferred over concatenation.

**Deviation from this doc:** `.parse<T>()` is implemented, restricted to
`T ∈ {i32, bool}`, but returns `T` directly rather than a fallible
result - neither `Optional<T>` nor the `?` operator exist in this
language yet, so invalid input yields a defined fallback (`0`/`false`)
instead of raising an error. `print(...)` and string interpolation
(`"Hello {name}"`) remain entirely unimplemented - no lexer/parser
support for interpolation exists, and there is no `print` builtin at all
(output happens only via top-level binding auto-printing).
