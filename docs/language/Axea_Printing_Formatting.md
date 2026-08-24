# Axea Printing & Formatting

> **Status:** Implemented - see `docs/language/0049-printing-formatting.md`,
> `docs/language/0050-collection-join-and-slicing.md`,
> `docs/language/0054-collection-printing.md`,
> `docs/language/0055-numeric-format-specs.md`,
> `docs/language/0056-slice-printing.md`,
> `docs/language/0057-alignment.md`,
> `docs/language/0058-debug-formatting.md`,
> `docs/language/0059-raw-strings.md`,
> `docs/language/0060-multiline-strings.md`,
> `docs/language/0061-buffer-write.md`, and
> `docs/language/0062-display-trait.md`.
> `print()`/`write()` (both usable bare at the top level, no assignment
> required, matching this doc's own examples - see 0049's own Parsing
> section follow-up), basic `{expr}` string interpolation (the "Printing"
> and "String Interpolation" sections below, for `i32`, `i64`, `f64`,
> `bool`, `char`, `str`, `String` - wider than `i32`/`bool`/`char`/`str`/
> `String` alone, once `i64`/`f64` themselves existed - see
> `docs/language/0051-numeric-widening.md` - and now with real support
> for a nested string literal inside an interpolation span, e.g.
> `"{x.join(",")}"`), and collection `.join()`/`array[a..b]` slicing (the
> "Collections" section below, same element-type restriction) are
> implemented. `print()`/`write()`/interpolation now also accept a
> struct, `Optional<T>`, or any collection value (`Array`/`List`/`Map`/
> `Set`/`Deque`/`Queue`/`PriorityQueue`/`LinkedList`/`SortedMap`/
> `SortedSet`/`Stack`/`slice<T>`) directly - see `0054-collection-printing.md`
> and `0056-slice-printing.md`; `slice<T>`'s own `.join()` is supported
> too, same as Array/List. The "Numeric Formatting" section below (`:.2`,
> `:05`, `:x`, `:X`, `:b`, `:o`) is now implemented too - see
> `0055-numeric-format-specs.md`. The "Alignment" section below (`:<20`,
> `:>20`, `:^20`) is implemented too - see `0057-alignment.md`; unlike
> numeric format specs, alignment applies to any text-representable type,
> not just i32/i64/f64. The "Debug Formatting" section below (`{x=}`,
> `{value:?}`) is implemented too - see `0058-debug-formatting.md`;
> `{value:?}` is identical to `{value}` except for `str`/`String`, which
> get quoted, since Axea has no Display/Debug trait distinction beyond
> `Display` itself (see below) for anything else to differ on. Raw strings
> (`r"..."`) and multiline strings (`"""..."""`, `r"""..."""`) are now
> implemented too - see `0059-raw-strings.md` and
> `0060-multiline-strings.md`; note that ordinary (non-raw) string
> literals have no escape-sequence processing at all, a pre-existing gap
> those two docs' own Motivation sections explain rather than silently
> work around. `Buffer.write()` is implemented too - see
> `0061-buffer-write.md`, a same-behavior alias of `.append()` rather
> than a genuinely distinct operation, since interpolation already lowers
> at parse time for any string literal argument regardless of which
> method receives it. The "Formatting Traits" section below is
> implemented too, narrowly - see `0062-display-trait.md`: a real
> `trait`/`impl` mechanism (the first user-defined struct methods in the
> language), but the only trait that drives any runtime dispatch is
> `Display` itself, resolved entirely at compile time (no vtables/dynamic
> dispatch - every stringification call site already knows a struct
> value's exact concrete type). No `Debug` trait exists separately from
> `{value:?}`'s own pre-existing str/String-only quoting.

This document defines Axea's proposed printing, string interpolation,
formatting, debug formatting, raw strings, multiline strings, and
buffered text-output model.

## Design Goals

-   Make ordinary printing as terse as Python.
-   Make interpolation the default formatting mechanism.
-   Avoid special interpolation prefixes such as C# `$"..."`.
-   Support systems-oriented numeric formatting.
-   Use the same formatting machinery for stdout, strings, and `Buffer`.
-   Allow custom types to participate through traits.

## Printing

`print()` writes values followed by a newline.

``` ax
print("Hello world")

name = "Burke"
print(name)
```

Multiple arguments are separated by spaces:

``` ax
print(name, age)
```

`write()` writes without automatically adding a newline:

``` ax
write("Loading...")
```

## String Interpolation

Ordinary strings support interpolation directly:

``` ax
name = "Burke"
age = 35

print("Hello {name}, you are {age}.")
print("Next year you will be {age + 1}.")
print("{user.name} is {user.age} years old.")
```

No `$` prefix is required.

Literal braces are doubled:

``` ax
print("Set = {{1, 2, 3}}")
```

## Numeric Formatting

``` ax
pi = 3.14159265
print("Pi = {pi:.2}")
```

Integer formats:

``` ax
value = 42

print("{value}")       // 42
print("{value:05}")    // 00042
print("{value:x}")     // 2a
print("{value:X}")     // 2A
print("{value:b}")     // 101010
print("{value:o}")     // 52
```

Systems-oriented examples:

``` ax
flags = 0b10110110

print("flags = {flags:08b}")
print("flags = 0x{flags:02X}")
```

## Alignment

``` ax
print("|{name:<20}|")
print("|{name:>20}|")
print("|{name:^20}|")
```

`<` means left aligned, `>` right aligned, and `^` centered.

``` ax
for user in users
{
    print("{user.id:<6} {user.name:<20} {user.score:>8.2}")
}
```

## Debug Formatting

Python-style expression debugging:

``` ax
x = 42
print("{x=}")
```

Output:

``` text
x=42
```

Normal versus debug representation:

``` ax
print("{user}")
print("{user:?}")
```

## Formatting Without Printing

Interpolation itself creates a formatted string:

``` ax
message = "Hello {name}, you have {count} messages."
```

When runtime construction is necessary, the result is an owned `String`.

A separate `format()` function is not required initially.

## Buffer Formatting

`Buffer` is Axea's mutable text-construction type.

``` ax
buf = Buffer()

buf.write("Name: {user.name}\n")
buf.write("Age: {user.age}\n")
buf.write("Score: {user.score:.2}\n")

text = buf.finish()
```

Direct append remains available:

``` ax
buf.append("Hello ")
buf.append(name)
```

The intended distinction is:

``` text
append()  direct append
write()   formatted/interpolated write
```

The compiler may lower interpolation internally to efficient `Buffer`
operations.

## Formatting Traits

Custom types participate through traits:

``` ax
trait Display
{
    format(self, buf: Buffer)
}
```

Example:

``` ax
struct Point
{
    x: f64
    y: f64
}

impl Display for Point
{
    format(self, buf: Buffer)
    {
        buf.write("({self.x}, {self.y})")
    }
}
```

Then:

``` ax
point = Point(10, 20)
print("Position: {point}")
```

Axea should infer that `self` is read and `buf` is written. A separate
`Debug` trait may provide `{value:?}` formatting.

## Collections

``` ax
numbers = [1, 2, 3, 4]

print(numbers)
print(numbers[..2].join(","))
```

Interpolation may contain collection expressions:

``` ax
print("First two: {numbers[..2].join(",")}")
```

The interpolation parser must support nested string literals inside
interpolation expressions.

## Raw Strings

``` ax
path = r"C:\Users\Burke\Documents"
```

Raw strings disable normal escape processing.

## Multiline Strings

``` ax
message = """
Hello {name},

Your build completed successfully.

Files: {count}
"""
```

Normal multiline strings support interpolation.

Raw multiline strings disable interpolation and escape processing:

``` ax
json = r"""
{
    "name": "{literal}"
}
"""
```

## Proposed Core API

``` ax
print("Hello")                 // stdout + newline
write("Hello")                 // stdout, no newline

print("Hello {name}")          // interpolation
message = "Hello {name}"       // formatted String

print("{value:.2}")            // precision
print("{flags:08b}")           // binary
print("{value:08X}")           // hexadecimal
print("{variable=}")           // expression debugging
print("{value:?}")             // debug representation

buf.append(text)               // direct append
buf.write("Hello {name}")      // formatted Buffer write
```

## Guiding Principle

> Formatting should be easy enough that the common case needs no
> formatting API at all: write the string you want, put expressions
> inside `{}`, and let the compiler generate the efficient
> implementation.
