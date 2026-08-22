# `i64`/`f64`: A Second Integer Width, a Real Float Type, and `as`

---

# Motivation

Every numeric value in this language, until now, was `i32` - the type
checker's `requireInt`/`requireOrdered` only ever accepted `i32` operands
for arithmetic and (`char`/`str` aside) ordering, and no float type had any
checking logic wired up at all, despite `F32`/`F64`/`I64`/... all already
being declared `TypeKind` values "for architectural fidelity"
(`docs/language/0005-type-system.md`). This phase makes two of those
declared-but-inert kinds real: `i64` (a second, wider signed integer) and
`f64` (a real IEEE 754 double), plus a new `as` operator to convert between
them and `i32` - deliberately not the other seven declared numeric widths
(`i8`/`i16`/`i128`, every `u*`, `f32`), which stay exactly where they were:
declared, unimplemented, an "unsupported type" error via `resolveType`.

**Why `i64`/`f64` specifically, and why together with `as`.** `i64` is the
obvious next integer width - same signed-arithmetic semantics `i32`
already has, just wider, so it's the cheapest possible next numeric type
to add (see Design below: it needs almost no new machinery). `f64` is the
one that's actually *different* - real floating-point opcodes
(`fadd`/`fdiv`/`fcmp`), a different C++ runtime representation, its own
literal syntax (a decimal point). Adding both together, plus a minimal
`as` cast (no wrapping/checked/saturating variants - `docs/language/0005-type-system.md`'s
own §5 "Integer Overflow" section is aspirational, not implemented here),
is the smallest slice that makes either type *usable*: without `as`, an
`i32` loop counter could never be passed to anything expecting `i64`, and
a value computed as `f64` could never be narrowed back for existing `i32`
code to consume.

---

# Design: Literal Suffixes, and Why `i64` Is Nearly Free

**Literal syntax**: `100i64` (an integer literal with an explicit `i64`
suffix), `1.5` (a decimal-point literal, always `f64` - the only float
type this phase, so no suffix is needed to disambiguate), and `1.5f64`/
`100f64` (an explicit `f64` suffix, redundant but accepted for symmetry
with `i64`'s own explicit form). This is `docs/language/0005-type-system.md`'s
§3-4 "suffixes for explicit literal typing" taken literally, *without* its
own "suffixes should be optional when context already determines the
type" clause - that needs real bidirectional type inference threaded
through variable declarations, which doesn't exist here; `x: i64 = 100`
(no suffix) still infers the bare `100` as `i32` and then fails to match
the declared `i64` type. Explicit suffixes only, this phase.

**Why `i64` needs almost no new machinery, and `f64` needs real new
machinery.** The interpreter's `Value` variant already stores `i32` as a
bare `std::int64_t` (`Interpreter.hpp`) - so `i64` is, at the *value*
level, exactly the same representation as `i32` was already using; the
interpreter never distinguished their width at runtime, only
`TypeChecker` does. Arithmetic reuses the *exact same* `asInt`-based code
path unchanged. Same story at the LLVM layer: `add`/`sub`/`mul`/`sdiv`/
`icmp` are already width-agnostic text (this is the same fact
`char`'s own `i24` already established for `icmp` - see
`docs/language/0044-char.md`) - `i64` arithmetic is `binOpMnemonic`'s
existing opcodes, unchanged, just with `"i64"` substituted for `"i32"` as
the operand type text. `f64` is the opposite: a genuinely new C++
representation (a `double` `Value` alternative), a genuinely new LLVM
opcode table (`floatBinOpMnemonic` - `fadd`/`fsub`/`fmul`/`fdiv`, and
`fcmp` with an *ordered* predicate rather than `icmp`'s signed one), and a
new literal-materialization path (`formatDoubleLiteral` - LLVM's own
16-hex-digit exact form, not plain decimal, since decimal notation is
only guaranteed lossless for values LLVM's parser happens to round-trip
exactly).

**`requireInt` returns a `Type` now, not always `kI32`.** Both operands
must be the *same* numeric kind (`isNumericKind`: `i32`, `i64`, or `f64`)
- `i32 + i64` is still rejected, no implicit widening, exactly like every
other mixed-type binary op in this checker. The result is that shared
kind, not a hardcoded `i32` - `i64 + i64` must yield `i64`. This bug would
have been invisible before this phase (there was only ever one numeric
kind to return), but a matching bug existed in `LlvmIrEmitter`'s own
`inferTypesInList` pre-pass: it hardcoded every `IrBinOp` arithmetic
result as `"i32"`, unconditionally - found and fixed as part of this
phase, now `typeOf(binOp->lhs, ...)`.

**Ordering, hashability, and where `f64`/`i64` do and don't reach.**
`isOrderableKind` (`docs/language/0039-priority-queues.md`) now accepts
`i64` and `f64` alongside `i32`/`char`/`str` - so `PriorityQueue<i64>`,
`SortedMap<f64,V>`, `SortedSet<i64>`, and general `<`/`<=`/`>`/`>=` on
`i64`/`f64` all just work, through the exact same `registerOrderRuntime`
mechanism `str` ordering already established (two more `axeaKeyType`
cases: `@axea.less.i64`, `@axea.less.f64`). `f64` ordering uses `fcmp`'s
*ordered* predicates (`olt`/`ole`/`ogt`/`oge`) - a `NaN` operand simply
compares `false` against everything, including itself, rather than
trapping or being specially rejected; not overengineered further this
phase. Neither `i64` nor `f64` was added to `isHashable` - `Map<K,V>`/
`Set<T>` keys stay exactly as restricted as before this phase (out of
scope; this phase's ask was arithmetic/comparison, not hashing).

**`isTextRepresentable` grew too.** `print`/`write`/string interpolation
now accept `i64`/`f64` arguments - without this, the feature would be
computable but unobservable. `i64` reuses a new `@axea.i64.to_str`
(`sprintf("%lld", ...)`, mirroring `@axea.i32.to_str`'s own `"%d"` call
exactly). `f64` gets `@axea.f64.to_str` (`sprintf("%g", ...)` - not
`"%f"`) - the interpreter's own `toString` calls `snprintf("%g", ...)`
for the identical reason: interpreted and compiled output must stay
character-for-character identical, and `"%g"` (general format, trims
trailing zeros) reads far more naturally than `"%f"`'s fixed six decimal
places for a language with no numeric-formatting syntax yet.

---

# Parsing: A Decimal Point, a Suffix, and Where `as` Sits

**The lexer's `.` ambiguity.** `Lexer::lexNumber` only consumes a `.` as
the start of a fractional part when the *next* character is a digit -
`5..7` (a slice range, `docs/language/0032-slices.md`) must lex as
`Integer(5)`, `DotDot`, `Integer(7)`, not `Float("5.")`, `Dot`,
`Integer(7)`. This is a one-character lookahead (`peek(1)`), checked
before any fractional digits are consumed.

**Suffix matching is a full, non-identifier-continuing match.**
`100i64x` lexes as `Integer("100")` followed by a *separate*
`Identifier("i64x")`, not `Int64("100i64")` followed by a stray `x` -
`matchesSuffix` checks that the character immediately after a candidate
`"i64"`/`"f64"` match isn't itself alphanumeric or `_`. `Token.text` keeps
the suffix (`"100i64"`, not `"100"`) - `Parser::parsePrimary` strips it
back off (`text.substr(0, text.size() - 3)`) before calling
`std::stoll`/`std::stod`, rather than the lexer doing that work twice (a
literal's *canonical form* - suffix included or not - only actually
matters to whichever layer decides how to parse the digits).

**`<expr> as <targetType>` sits in `parsePostfix`, not the binary-operator
precedence table.** `parseExpression`'s Pratt loop (`Parser::precedence`)
only ever builds `BinaryExpr` nodes uniformly - `as`'s right-hand side is
a type name, not an expression, so it can't share that machinery. Instead
it's a `while (match(TokenKind::As))` loop appended after `parsePostfix`'s
own `Dot`/`LeftBracket` loop, applying to the *whole* postfix expression
built so far (`foo.bar() as i64`, not just a bare primary) and binding
tighter than every binary operator (`x as i64 + 1` is `(x as i64) + 1`) -
the same precedence Rust's own `as` has, and this language's only
existing precedent for "a keyword operator with its own binding rules."

---

# Interpreter: One New `Value` Alternative, Two New Switch Branches

`Value` gained exactly one new alternative: `double`. `i32`/`i64` share
the existing `std::int64_t` one (see Design above) - `char32_t`, `bool`,
`std::string`, and every `shared_ptr<...Instance>` alternative are
untouched.

`Int64Expr`/`FloatExpr` evaluate to `int64Expr->value`/`floatExpr->value`
directly, mirroring `IntegerExpr`. The `BinaryExpr` arithmetic switch
(`Plus`/`Minus`/`Star`/`Slash`) now checks `std::get_if<double>(&left)`
first, falling through to the existing `asInt`-based path when it's not a
`double` - real floating-point division has no zero-check (`+inf`/`-inf`/
`NaN` per IEEE 754), unlike the integer path's own `"division by zero"`
throw just below it. `ValueLess::operator()` (already built for `str`
ordering) gained a `double` branch the same way.

`CastExpr` evaluation is three shapes, dispatched by `targetType` and the
operand's own active `Value` alternative: `f64` target, `double` operand
already → pass through; `f64` target, int operand → `static_cast<double>`;
int target, `double` operand → `static_cast<std::int64_t>` (C++'s own
truncate-toward-zero, matching `fptosi`'s LLVM semantics exactly); int
target, int operand → pass through unchanged (a real no-op at *this*
representation level, since `i32`→`i64` is the same `std::int64_t`
alternative on both sides - unlike the LLVM backend, which still emits a
real `sext`/`trunc` instruction, because LLVM's own type system
distinguishes `i32` from `i64` even when this interpreter's C++
representation doesn't).

---

# `LlvmIrEmitter`: A Genuinely Different Opcode Table, and Two Sentinel/Ordering Bugs Found Along the Way

`llvmType("i64")` → `"i64"`; `llvmType("f64")` → `"double"` (LLVM's own
name for the type - `"f64"` only ever appears in Axea source text).
`IrConstInt64`/`IrConstFloat` materialize via the same "trivial no-op
arithmetic" convention every constant here already uses (`add i64 0,
<value>` / `fadd double 0.0, <value>`) - not because it's needed for
correctness (a bare SSA immediate would work at every actual use site),
but for the same reason `IrConstInt`'s own `add i32 0, ...` predates this
phase: every Axea IR register stays uniformly addressable as `"%N"`, and
real LLVM optimization passes fold the no-op away instantly anyway.

**`IrBinOp` picks an opcode table by operand type, not unconditionally
`binOpMnemonic`.** `lhsType == "double"` routes through the new
`floatBinOpMnemonic` (`fadd`/`fsub`/`fmul`/`fdiv`, `fcmp oeq`/`one`/`olt`/
`ole`/`ogt`/`oge`); everything else (`i32`, `i64`, `i24` for `char`) keeps
using `binOpMnemonic`'s existing integer opcodes unchanged, since those
were already width-agnostic text.

**`IrCast` lowers to one of six shapes** based on the (already-inferred)
operand LLVM type vs. `llvmType(targetType)`: same-type (the trivial
no-op-arithmetic materialization above, *not* an identity `bitcast` -
sidesteps any question of whether a same-type `bitcast` is still legal IR
across LLVM versions), `i32`→`i64` (`sext`), `i64`→`i32` (`trunc`),
int→`double` (`sitofp`), `double`→int (`fptosi`).

**Two real bugs, found because this phase was the first thing to reach
them**: `sentinelFor` (the "key not found" `SortedMap<K,V>.get()`
sentinel, and `Map<K,V>`'s own) returned `"null"` for *any* non-`i32`/
non-`i1` value type, including `"i64"`/`"double"` - both scalar, non-
pointer LLVM types `null` isn't valid IR for; `SortedMap<K, i64>` or
`SortedMap<K, f64>` would have produced a real LLVM parse failure the
moment anyone actually used one. Fixed with real sentinels
(`-9223372036854775808`, `0.0`). Separately, `registerOrderRuntime`
(`docs/language/0042-string.md`'s own `str`-ordering work) only had
`i32`/`char`/`str` cases - `PriorityQueue<i64>`/`SortedMap<f64,...>`/
`SortedSet<i64>` would have hit its `"internal error: no order runtime
registered"` throw. Both fixed as part of adding `i64`/`f64`, not before -
neither was reachable until a second orderable numeric kind existed.

**Six separate "print this value" call sites all needed the identical
new branch.** Top-level bindings (`emitMain`), struct fields
(`emitStructPrintHelpers`), and four different collection-element-
printing loops (fixed array, `List<T>`, one more array-like shape, and a
second List-shaped one) each independently branch on LLVM element type
(`i32`/`i1`/`i8*`/`isCharType`/else-assume-nested-struct) - none of them
knew about `i64`/`double`, so each would have silently taken the "must be
a nested struct pointer" fallback and called a nonexistent
`@axea.print.<garbage>` function, an LLVM link-time failure. Each got the
identical fix: route through `stringifyValueOfType` (which already builds
the right `@axea.i64.to_str`/`@axea.f64.to_str` call) and print the
result as `"%s"`, exactly the path `char`/`str`/`String` already use at
each of those same six sites - not a bare `"%d"`, which would misread an
`i64`'s upper 32 bits, or simply not typecheck at all for a `double`.

---

# Worked Example

```ax
struct Point
{
    x: i64
    y: f64
}

p = Point { x: 500i64, y: 2.25 }

a = 100i64
b = 25i64
sum = a + b          // i64, 125

f1 = 1.5
f2 = 2.5
total = f1 + f2       // f64, 4 (via "%g" - no trailing ".0")

n = 5
widened = n as i64    // sext i32 to i64
back = widened as i32 // trunc i64 to i32
asFloat = n as f64    // sitofp
truncated = 9.7 as i32 // fptosi, truncates toward zero: 9

q = PriorityQueue<f64>()
q.push(3.5)
q.push(1.5)
smallest = q.pop()    // 1.5 - ascending, same as PriorityQueue<i32>
```

Hand-verified: interpreted (`ax run`) and compiled (`ax llvm-ir` piped
through `clang`) output is byte-for-byte identical for this program,
including every `%g`-formatted float and every struct/collection print
path touched above.

---

# Known Imprecision / Out of Scope (By Design, Not Oversight)

- **No other declared numeric width.** `i8`/`i16`/`i128`, every unsigned
  width (`u8`...`u128`), and `f32` all stay "unsupported type" errors via
  `resolveType` - only `i64`/`f64` joined `i32` this phase.
- **No overflow semantics.** `docs/language/0005-type-system.md`'s own §5
  ("compile-time overflow is always an error," debug-build traps,
  wrapping operations) is entirely aspirational - `i64` arithmetic
  overflows exactly the way `i32`'s always silently has (real machine
  wraparound, no check, no trap).
- **No `wrapping`/`checked`/`saturating` cast variants, and no numeric
  literal separators, hex/binary/octal literals, or contextual
  (suffix-less) literal typing.** All are in `docs/language/0005-type-system.md`'s
  own vision but need machinery (real bidirectional type inference for
  the last one) this phase doesn't build.
- **`as` only ever converts among `i32`/`i64`/`f64`.** No cast to/from
  `bool`/`char`/`str`/struct/any collection - `isNumericKind` gates both
  sides.
- **`f64` ordering has no NaN special-casing.** `fcmp`'s own *ordered*
  predicates already give well-defined (if perhaps surprising to a reader
  unfamiliar with IEEE 754) behavior for free: `NaN` compares `false`
  against everything, including itself.
- **`i64`/`f64` are not hashable, not `Map`/`Set` key types, not struct-
  field-restricted in any new way** (they're ordinary struct fields, just
  like `i32` already was) - `isHashable` is untouched by this phase.

---

# Guiding Rule

A numeric kind is real here exactly when it has: a literal syntax, real
arithmetic and comparison in every backend, a way to convert to/from the
kinds that already exist, and a way to print it. Half of that (a `TypeKind`
enum value with no checking logic behind it) isn't a numeric type - it's
a placeholder. `i64` reused nearly everything `i32` already had (same
`Value` representation, same LLVM opcodes, same `asInt` unification);
`f64` needed a second, genuinely distinct implementation of almost every
one of those same things - the cost of adding a numeric *kind* is never
uniform, and pretending otherwise (implementing only the parts that
happen to be free) is how a type ends up parseable but silently broken
the first time someone actually uses it in a collection or a print
statement.
