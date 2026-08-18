# Slicing

**Status:** Implemented, see `docs/language/0045-str-slicing.md`

```ax
year = date[..4]
month = date[5..7]
day = date[8..]
```
Returns a `str` view with no allocation.

**Deviation from this doc:** the implementation copies the sliced bytes
into a freshly allocated buffer rather than returning a true zero-copy
view - `str`'s own existing representation (a bare, null-terminated
pointer with no separate length field) makes a real bounded view
impossible without a much larger change. Functionally indistinguishable
from a view at every call site that exists in this language today; see
`docs/language/0045-str-slicing.md`'s own Design section for the full
reasoning. `array[a..b]` sub-slicing remains out of scope, per
`docs/language/0032-slices.md`.
