# Standard Library

Planned modules:

-   io
-   fs
-   net
-   json
-   collections
-   async
-   math

Keep the standard library small and composable.

`collections` is currently implemented as compiler intrinsics
(`docs/language/0029-collections.md` and its per-type docs), not as real Axea source, because the
language has no user-definable generics or manual-memory-management primitive. Moving it into a
real `std/collections.ax` depends on `docs/language/0006-generics.md` (generic structs/functions/
methods) and `docs/language/0019-unsafe.md` (raw pointers + `unsafe` blocks, needed to allocate a
buffer from Axea source itself) - see those docs' own "Future Work" sections.
