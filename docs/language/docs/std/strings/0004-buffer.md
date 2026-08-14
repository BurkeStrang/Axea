# Buffer

**Status:** Draft

`Buffer` is Axea's mutable text construction type.

It replaces the traditional `StringBuilder` name with a shorter, more general API.

## Purpose

Use `Buffer` whenever text is constructed incrementally.

```ax
buf = Buffer()

buf.append("Hello ")
buf.append(name)
buf.append("!")

message = buf.finish()
```

`finish()` transfers ownership and returns an immutable `String`.

## Common Operations

```ax
buf.append(text)
buf.append_line(text)

buf.clear()

buf.len
buf.capacity

buf.reserve(1024)

message = buf.finish()
```

## Compiler Optimizations

The compiler may automatically lower string interpolation into a `Buffer`.

Source:

```ax
message = "Hello {name}, score = {score}"
```

Conceptually becomes:

```ax
buf = Buffer()
buf.append("Hello ")
buf.append(name)
buf.append(", score = ")
buf.append(score)
message = buf.finish()
```

This optimization is invisible to the programmer and avoids unnecessary allocations.

## Guiding Principle

> Most programs should rarely construct a `Buffer` manually. String interpolation should be the preferred syntax, while `Buffer` exists for loops and high-performance text generation.
