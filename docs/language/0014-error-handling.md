# Error Handling

Proposal:

-   T = value
-   T? = optional
-   T! = value or error

Errors should propagate ergonomically while remaining explicit.

> **Status:** `T?` is implemented as `Optional<T>` - see `docs/language/0052-optional.md`.
> The "value or error" half is implemented too, though as an explicit `Result<T, E>` generic
> type (`Ok(value)`/`Err(error)`) rather than literal `T!` syntax - see
> `docs/language/0063-result.md`. Both share `?` as their propagation operator, matching this
> doc's own "propagate ergonomically" goal.
