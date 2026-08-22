# Axea Type System

**Status:** Draft  
**Document:** `0005-type-system.md`

**Implementation note:** this document is a design vision, well ahead of
what's actually built - most of it (hex/binary/octal literals, numeric
separators, contextual/suffix-less literal typing, overflow
traps/wrapping, `u8`...`u128`, `f32`, `i8`/`i16`/`i128`) remains
unimplemented. `i32` has real arithmetic/comparison; `i64`/`f64` joined it
(explicit-suffix literals only, plus a minimal `as` cast with no
wrapping/checked/saturating variants) in `docs/language/0051-numeric-widening.md`
- that document, not this one, describes what's actually implemented for
numeric types today.

---

# Motivation

Axea needs a type system that provides strong compile-time guarantees without forcing programmers to annotate information the compiler can reliably infer.

The type system should support Axea's broader goals:

- Compile-time memory safety
- Ownership and capability inference
- Non-null values by default
- Explicit representation of absence
- Explicit representation of failure
- Native performance
- Predictable data layout
- Minimal syntax
- Clear diagnostics
- Strong interoperability with low-level code

Axea should feel concise in ordinary code:

```ax
name = "Burke"
age = 35
active = true
```

while still allowing precise types where they matter:

```ax
port: u16 = 8080
timeout: f64 = 2.5
```

The compiler should infer what it can prove and require an annotation when inference is ambiguous or when the type is part of a public contract.

---

# Goals

The Axea type system should:

- Be statically typed.
- Infer local variable types whenever practical.
- Reject invalid type combinations at compile time.
- Make ordinary values non-null by default.
- Represent optional values explicitly with `T?`.
- Represent recoverable failure explicitly with `T!`.
- Support precise integer widths for systems programming.
- Avoid surprising implicit conversions.
- Allow compile-time flow narrowing.
- Integrate with ownership and capability analysis.
- Support generics and traits.
- Provide deterministic object layout where required.
- Make unsafe reinterpretation explicit.

---

# Non-Goals

The initial type system does not attempt to provide:

- Runtime duck typing.
- JavaScript-style coercion.
- Implicit nullable references.
- Implicit numeric narrowing.
- Arbitrary runtime type mutation.
- Automatic boxing of every value.
- A universal `object` base type.
- User-visible lifetime syntax in ordinary code.
- Full dependent types.

---

# 1. Static Typing

Every Axea expression has a compile-time type.

```ax
x = 10
```

The compiler infers:

```text
x: i32
```

Likewise:

```ax
name = "Axea"
```

is inferred as:

```text
name: str
```

Once established, a local variable's type does not change.

```ax
x = 10
x = "hello"
```

Compile-time error:

```text
error AX2001: type mismatch

x was inferred as `i32`

    x = 10
        ^^

cannot assign value of type `str`

    x = "hello"
        ^^^^^^^
```

This is intentionally different from dynamically typed languages.

---

# 2. Primitive Types

The initial primitive type set is:

```text
bool

i8
i16
i32
i64
i128

u8
u16
u32
u64
u128

f32
f64

char
str

unit
never
```

## Boolean

```ax
active = true
finished = false
```

`bool` is not interchangeable with integers.

This is invalid:

```ax
if 1
{
    print("yes")
}
```

Diagnostic:

```text
error AX2002: condition must be `bool`

found `i32`
```

Axea deliberately avoids C-style truthiness for numeric values.

---

# 3. Integer Types

Axea provides explicitly sized signed and unsigned integers.

```ax
small: i8 = 12
count: i32 = 100
huge: i128 = 10_000_000_000

byte: u8 = 255
port: u16 = 443
mask: u64 = 0xff00_ff00
```

The expected default integer type is:

```text
i32
```

So:

```ax
x = 42
```

means:

```ax
x: i32 = 42
```

unless context requires another type.

Example:

```ax
port: u16 = 443
```

The literal `443` is contextually typed as `u16`.

---

# 4. Integer Literals

Supported forms should include:

```ax
decimal = 255
hex     = 0xff
binary  = 0b1111_1111
octal   = 0o377
```

Numeric separators are permitted:

```ax
population = 8_200_000_000
mask       = 0xffff_0000
```

Suffixes may be supported for explicit literal typing:

```ax
x = 10u8
y = 500u16
z = 100i64
```

However, suffixes should be optional when context already determines the type.

---

# 5. Integer Overflow

The recommended default is:

- Compile-time overflow is always an error.
- Debug builds trap on runtime overflow.
- Optimized builds should retain defined overflow semantics rather than silently invoking undefined behavior.
- Explicit wrapping operations are available.

Example:

```ax
x: u8 = 255
x += 1
```

This should not silently become `0`.

The programmer should explicitly request wrapping behavior:

```ax
x = x.wrapping_add(1)
```

or checked behavior:

```ax
x = x.checked_add(1)?
```

Saturating operations may also be provided:

```ax
x = x.saturating_add(1)
```

Axea should never make signed overflow undefined behavior at the language level.

---

# 6. Floating-Point Types

```ax
temperature = 98.6
```

defaults to:

```text
f64
```

Explicit:

```ax
x: f32 = 1.5
y: f64 = 1.5
```

Floating-point behavior should follow IEEE 754 as closely as practical.

Axea should permit:

```ax
nan = f64.nan
inf = f64.infinity
```

but should avoid implicit conversion between integers and floating-point values when precision may be lost.

---

# 7. Character and String Types

A character:

```ax
letter: char = 'A'
```

A string:

```ax
name: str = "Axea"
```

String interpolation:

```ax
print("Hello {name}")
```

`char` represents a Unicode scalar value.

`str` is a valid UTF-8 string abstraction.

The exact owned versus borrowed string representation will be specified with the memory model, but source syntax should not require programmers to manually distinguish common string views unless ownership matters.

---

# 8. Unit

Functions that conceptually return no value return `unit`.

```ax
print_user(user: User)
{
    print(user.name)
}
```

The compiler treats this as:

```ax
print_user(user: User) -> unit
{
    print(user.name)
}
```

The return type may normally be omitted.

An explicit unit literal may be:

```ax
()
```

---

# 9. Never Type

Axea should include a bottom type named `never`.

Functions that never return:

```ax
panic(message: str) -> never
{
    ...
}
```

This allows `never` expressions to participate naturally in expression-oriented control flow.

```ax
user =
    if valid
    {
        load_user()
    }
    else
    {
        panic("invalid user")
    }
```

The second branch has type `never`, so the whole `if` expression has type `User`.

---

# 10. Type Inference

Axea should infer local types aggressively.

```ax
x = 10
name = "Axea"
values = [1, 2, 3]
```

Inferred:

```text
x: i32
name: str
values: List<i32>
```

Function parameters should generally require types at API boundaries:

```ax
square(x: i32) -> i32
{
    x * x
}
```

Private/local functions may eventually permit stronger inference, but parameter inference should not make public signatures unstable or difficult to understand.

---

# 11. Contextual Type Inference

Literals may derive their type from context.

```ax
fn send_port(port: u16)
{
    ...
}

send_port(443)
```

`443` is interpreted as `u16` because that type is required by the call.

Likewise:

```ax
values: List<u8> = [1, 2, 3]
```

The collection elements are contextually typed as `u8`.

---

# 12. No Uninitialized Variables

Every variable must have a valid value when introduced.

Valid:

```ax
user = load_user()
```

Invalid:

```ax
user: User
```

There is no ordinary "uninitialized" state.

This removes an entire class of undefined behavior and simplifies ownership analysis.

Instead, Axea uses expression-oriented control flow.

Instead of:

```text
connection: Connection

if production
{
    connection = connect_prod()
}
else
{
    connection = connect_dev()
}
```

write:

```ax
connection =
    if production
    {
        connect_prod()
    }
    else
    {
        connect_dev()
    }
```

The compiler must verify that all reachable branches produce compatible types.

---

# 13. `if` Expressions

`if` is an expression.

```ax
message =
    if logged_in
    {
        "Welcome back"
    }
    else
    {
        "Please sign in"
    }
```

The result type is `str`.

Both branches must have compatible types.

Invalid:

```ax
value =
    if condition
    {
        42
    }
    else
    {
        "hello"
    }
```

Diagnostic:

```text
error AX2003: incompatible branch types

then branch: `i32`
else branch: `str`

an `if` expression must produce one compatible result type
```

---

# 14. Optional Types

Ordinary Axea values cannot be `null`.

```ax
user: User
```

always contains a valid `User`.

If absence is meaningful:

```ax
user: User?
```

`T?` means:

```text
T or none
```

Example:

```ax
find_user(id: UserId) -> User?
{
    ...
}
```

Usage:

```ax
user = find_user(id)

if user
{
    print(user.name)
}
```

Within the `if`, flow analysis narrows:

```text
User? -> User
```

---

# 15. `none`

The empty optional value is:

```ax
none
```

Example:

```ax
find_user(id: UserId) -> User?
{
    if not exists(id)
    {
        none
    }
    else
    {
        load_user(id)
    }
}
```

`none` receives its optional type from context.

The compiler should reject:

```ax
x = none
```

when there is no contextual type.

Diagnostic:

```text
error AX2004: cannot infer optional type

`none` requires a contextual optional type

consider:

    x: User? = none
```

---

# 16. Optional Flow Narrowing

Axea should make null/absence checks lightweight.

```ax
manager: User? = user.manager

if manager
{
    print(manager.name)
}
```

Inside the branch:

```text
manager: User
```

Outside:

```text
manager: User?
```

Explicit checking is also valid:

```ax
if manager != none
{
    print(manager.name)
}
```

---

# 17. Optional Binding

Axea should support scoped optional binding.

```ax
if manager = user.manager
{
    print(manager.name)
}
```

Inside the body:

```text
manager: User
```

The variable exists only within that scope.

Nested access:

```ax
if city = user.address?.city
{
    print(city)
}
```

---

# 18. Optional Chaining

Axea should support `?.`.

```ax
city = user.address?.city
```

If `address` is absent, the result is `none`.

Given:

```text
user.address: Address?
Address.city: str
```

then:

```text
user.address?.city
```

has type:

```text
str?
```

Chaining:

```ax
country = user.address?.country?.name
```

The compiler propagates absence without dereferencing an invalid reference.

---

# 19. Optional Fallback

Axea should support a readable fallback operator.

Preferred syntax:

```ax
display_name = user.nickname or user.name
```

If `nickname` is present, its value is used; otherwise `user.name`.

For:

```text
nickname: str?
name: str
```

the result is:

```text
str
```

This avoids forcing users to call methods such as `unwrap_or`.

---

# 20. Error Types

Axea distinguishes absence from failure.

```text
T? = optional value
T! = value or error
```

Example:

```ax
read_file(path: str) -> str!
```

means:

> The operation returns a valid `str`, or it fails with an error.

Usage:

```ax
text = read_file("config.json")?
```

The postfix `?` propagates an error to the caller.

Example:

```ax
load_config(path: str) -> Config!
{
    text = read_file(path)?
    return json.parse<Config>(text)?
}
```

A value-producing function must return it explicitly (`0027-explicit-return.md`)
— there is no implicit "the final expression is the return value" fallback.

---

# 21. Optional and Error Types Are Distinct

These mean different things:

```ax
find_user(id) -> User?
connect(address) -> Connection!
```

`User?`:

> A user might legitimately not exist.

`Connection!`:

> The operation can fail.

Axea should not conflate absence and failure.

A future combined form could be considered:

```text
User?!
```

but should not be added until there is strong motivation.

---

# 22. Typed Errors

The initial implementation may use a common `Error` abstraction internally.

A later extension may permit typed errors:

```ax
read_file(path: str) -> str!IOError
parse(text: str) -> Config!ParseError
```

This would conceptually correspond to:

```text
Result<str, IOError>
Result<Config, ParseError>
```

without requiring generic-wrapper syntax in common code.

---

# 23. Struct Types

Structs define product types.

```ax
struct User
{
    id: i64
    name: str
    active: bool
}
```

Construction:

```ax
user = User
{
    id: 42
    name: "Burke"
    active: true
}
```

Shorthand:

```ax
id = 42
name = "Burke"
active = true

user = User { id, name, active }
```

Positional construction may be supported only when explicitly declared or when doing so does not harm API clarity.

---

# 24. Field Access

```ax
print(user.name)
```

The expression:

```ax
user.name
```

has the declared field type.

If:

```ax
struct User
{
    name: str
}
```

then:

```text
user.name: str
```

Capability analysis separately determines whether field access requires `read` or `write`.

---

# 25. Methods and Implicit `self`

Inside a struct implementation, Axea may allow direct field access.

```ax
struct User
{
    name: str
    age: i32

    birthday()
    {
        age++
    }

    display() => print("{name}, {age}")
}
```

The compiler interprets fields as members of the current instance.

Capability inference derives:

```text
birthday -> write self
display  -> read self
```

This is a semantic ownership concern layered on top of the type system.

---

# 26. Collection Types

Initial standard collection types may include:

```text
Array<T>
List<T>
Map<K, V>
Set<T>
slice<T>
```

Literal inference:

```ax
numbers = [1, 2, 3]
```

produces:

```text
List<i32>
```

or another chosen canonical collection type.

The exact literal collection type must be standardized before implementation.

Explicit:

```ax
bytes: List<u8> = [1, 2, 3]
```

---

# 27. Tuple Types

Tuples provide lightweight unnamed product types.

```ax
point = (10, 20)
```

Type:

```text
(i32, i32)
```

Destructuring:

```ax
(x, y) = point
```

Mixed tuple:

```ax
user_info = (42, "Burke", true)
```

Type:

```text
(i32, str, bool)
```

---

# 28. Function Types

Functions are typed values.

```ax
add(a: i32, b: i32) -> i32
{
    a + b
}
```

Conceptual type:

```text
(i32, i32) -> i32
```

Lambda:

```ax
double = x => x * 2
```

When context exists:

```ax
double: (i32) -> i32 = x => x * 2
```

The compiler may infer local lambda types from use.

---

# 29. Generic Types

Generic syntax:

```ax
struct Box<T>
{
    value: T
}
```

Usage:

```ax
number = Box<i32> { value: 42 }
```

Inference where possible:

```ax
number = Box { value: 42 }
```

Compiler infers:

```text
Box<i32>
```

Generics should initially be monomorphized for native performance.

---

# 30. Generic Functions

```ax
identity<T>(value: T) -> T
{
    value
}
```

Usage:

```ax
x = identity(42)
name = identity("Axea")
```

Type arguments are inferred:

```text
identity<i32>
identity<str>
```

Explicit type arguments remain available:

```ax
x = identity<i64>(42)
```

---

# 31. Traits as Constraints

Generic constraints should use traits.

```ax
max<T: Ordered>(a: T, b: T) -> T
{
    if a > b
    {
        a
    }
    else
    {
        b
    }
}
```

The compiler verifies that `T` implements `Ordered`.

Multiple constraints may eventually use:

```ax
fn process<T: Readable + Hashable>(value: T)
```

Exact syntax remains subject to the traits specification.

---

# 32. Type Aliases

Axea should support aliases:

```ax
type UserId = i64
```

This is initially an alias, not a distinct nominal type.

```ax
id: UserId = 42
```

For stronger domain modeling, Axea should also support newtypes.

---

# 33. Newtypes

A distinct zero-cost type:

```ax
newtype UserId = i64
```

Now:

```ax
user_id: UserId = UserId(42)
order_id: OrderId = OrderId(42)
```

These are not implicitly interchangeable.

This provides domain safety without runtime overhead.

---

# 34. Numeric Conversions

Axea should avoid surprising implicit numeric conversions.

Safe widening may be considered:

```text
u8 -> u16
i32 -> i64
f32 -> f64
```

but even these should be carefully evaluated.

Narrowing should always be explicit.

Invalid:

```ax
large: i64 = 1000
small: i8 = large
```

Instead:

```ax
small = large as i8
```

or preferably checked:

```ax
small = i8.from(large)?
```

The language should distinguish:

- checked conversion
- wrapping conversion
- saturating conversion
- reinterpretation

rather than treating them as one generic cast.

---

# 35. No General Implicit Coercion

Axea should reject:

```ax
x = "5" + 10
```

There is no string-to-number coercion.

Write:

```ax
x = parse<i32>("5")? + 10
```

Similarly:

```ax
print("Age: " + age)
```

should either be invalid or require explicit conversion.

String interpolation is preferred:

```ax
print("Age: {age}")
```

---

# 36. Equality

Equality requires compatible types.

```ax
x: i32 = 5
y: i32 = 5

if x == y
{
    ...
}
```

Comparing unrelated types is a compile-time error.

```ax
5 == "5"
```

is invalid.

Traits may define user-type equality.

---

# 37. Pattern-Based Type Narrowing

Pattern matching should narrow values.

```ax
match value
{
    User user =>
    {
        print(user.name)
    }

    none =>
    {
        print("No user")
    }
}
```

Within the `User` arm, the type is known to be `User`.

Matching must integrate with optional types, enums, and future sum types.

---

# 38. Sum / Variant Types

Axea should eventually support explicit sum types.

Possible syntax:

```ax
enum Result
{
    Success(User)
    NotFound
    Unauthorized
}
```

Pattern matching:

```ax
match result
{
    Success(user) => print(user.name)
    NotFound      => print("missing")
    Unauthorized => print("denied")
}
```

The compiler should require exhaustive matches unless explicitly marked otherwise.

---

# 39. Type Compatibility in `match`

Every arm of a value-producing `match` must have compatible types.

```ax
label =
    match state
    {
        Ready   => "ready"
        Running => "running"
        Done    => "done"
    }
```

Result:

```text
label: str
```

Invalid:

```ax
value =
    match state
    {
        Ready => 42
        Done  => "done"
    }
```

Compile-time error due to incompatible arm types.

---

# 40. Public API Type Contracts

Public function types are part of the stable API.

```ax
pub get_user(id: UserId) -> User?
```

Changing this to:

```ax
pub get_user(id: UserId) -> User!
```

is an API-breaking semantic change.

Likewise changing:

```ax
pub process(read user: User)
```

to:

```ax
pub process(write user: User)
```

is a capability contract change.

Axea tooling should eventually be able to detect these changes.

---

# 41. Type Inference and Public APIs

The compiler may infer types internally, but exported declarations should usually have explicit types.

Preferred:

```ax
pub square(x: i32) -> i32
{
    x * x
}
```

Avoid public API inference like:

```ax
pub square(x)
{
    x * x
}
```

because changes to implementation could silently alter the exported type.

Private helpers may allow more inference in the future.

---

# 42. Capability Is Not the Same as Type

These all have the same base type:

```ax
read user: User
write user: User
take user: User
```

The type is:

```text
User
```

The capability describes what the function may do with that value.

This separation is important:

```text
Type       = what the value is
Capability = what access is permitted
Ownership  = who controls its lifetime
Region     = where/how long storage exists
```

The compiler may track all four dimensions independently.

---

# 43. Safe References

A safe reference has a target type:

```text
ref User
```

Safe references are always valid while usable.

There is no null safe reference.

Optional reference:

```text
(ref User)?
```

Raw pointers are separate:

```text
ptr<User>
```

and are governed by unsafe rules.

---

# 44. Raw Pointer Types

Low-level code may use:

```ax
ptr<u8>
ptr<MyStruct>
```

Unlike safe references, a raw pointer may:

- be zero/null
- be dangling
- be misaligned
- refer to invalid memory

Therefore dereference operations require an unsafe context.

```ax
unsafe pointer
{
    value = p.read()
}
```

Raw pointer safety is outside the normal type guarantee.

---

# 45. Slice Types

A safe slice represents a contiguous view.

```ax
bytes: slice<u8>
```

Conceptually:

```text
pointer + length
```

but safely typed.

Examples:

```ax
header = bytes[0..20]
```

Read-only API:

```ax
parse(read bytes: slice<u8>)
```

Mutable API:

```ax
encrypt(write bytes: slice<u8>)
```

The capability system determines mutation permissions.

---

# 46. Type Layout

For ordinary application code, layout is implementation-defined unless otherwise specified.

For FFI and systems code, explicit layout should be supported.

Possible syntax:

```ax
@repr(C)
struct Header
{
    kind: u16
    length: u32
}
```

Other future representations may include:

```text
@repr(packed)
@repr(transparent)
@align(16)
```

The exact attribute syntax belongs to the FFI and memory-model specifications.

---

# 47. Compile-Time Constants

Axea should support compile-time-known values.

Possible syntax:

```ax
const MaxUsers = 1000
const Port: u16 = 443
```

A constant expression must be evaluable at compile time.

This supports:

- array lengths
- bit masks
- generic parameters
- optimization
- embedded programming

---

# 48. Arrays With Compile-Time Length

A fixed array type may be:

```text
[i32; 4]
```

Example:

```ax
values: [i32; 4] = [1, 2, 3, 4]
```

This is distinct from dynamically sized collections.

Potential generic usage:

```ax
fn first<T, const N>(values: [T; N]) -> T
```

Const generics should be considered after basic generics are stable.

---

# 49. Type Errors Should Be Local and Explanatory

Axea diagnostics are part of the language design.

Bad:

```text
type mismatch
```

Better:

```text
error AX2014: incompatible assignment

`timeout` has type `u32`

    timeout = 30
    ------- inferred here

but this assignment produces `str`

    timeout = "30 seconds"
              ^^^^^^^^^^^^

help: parse the string explicitly or keep `timeout` numeric
```

Diagnostics should show:

- expected type
- actual type
- where the expected type came from
- likely fix

---

# 50. Example: Optional Database Lookup

```ax
struct User
{
    id: UserId
    name: str
}

find_user(read db: Database, id: UserId) -> User?
{
    db.users.find(id)
}

display_user(read db: Database, id: UserId)
{
    if user = find_user(db, id)
    {
        print(user.name)
    }
    else
    {
        print("User not found")
    }
}
```

Important type transitions:

```text
find_user(...) : User?

inside `if user = ...`:
user : User
```

There is no null dereference path.

---

# 51. Example: Error Propagation

```ax
struct Config
{
    port: u16
    host: str
}

load_config(path: str) -> Config!
{
    text = file.read(path)?
    json.parse<Config>(text)?
}

main() -> unit!
{
    config = load_config("app.json")?

    print("Listening on {config.host}:{config.port}")
}
```

The compiler knows:

```text
file.read(...)          : str!
json.parse<Config>(...) : Config!
load_config(...)        : Config!
```

After `?` succeeds:

```text
text   : str
config : Config
```

---

# 52. Example: Expression-Oriented Initialization

```ax
connection =
    if environment == "prod"
    {
        connect_prod()?
    }
    else
    {
        connect_dev()?
    }
```

Both branches must produce:

```text
Connection
```

after error propagation.

No uninitialized `connection` exists at any point.

---

# 53. Example: Generic Collection Function

```ax
first<T>(read values: slice<T>) -> T?
{
    if values.empty
    {
        none
    }
    else
    {
        values[0]
    }
}
```

Usage:

```ax
numbers = [10, 20, 30]

if value = first(numbers)
{
    print(value)
}
```

Type inference:

```text
numbers      : List<i32>
first(...)   : i32?
value        : i32
```

---

# 54. Example: Numeric Safety

```ax
packet_length: u16 = read_length()

buffer_size: usize = packet_length as usize
```

If Axea adopts `usize`, it represents an unsigned integer sized for addressing.

Narrowing:

```ax
small: u8 = packet_length
```

must fail unless explicitly checked or converted.

---

# 55. Example: Type + Capability Inference

```ax
display(user: User)
{
    print(user.name)
}

birthday(user: User)
{
    user.age++
}
```

Type checking determines:

```text
user : User
```

Capability analysis separately infers:

```text
display  -> read User
birthday -> write User
```

The programmer need not annotate these private functions.

Public API:

```ax
pub display(read user: User)
pub birthday(write user: User)
```

makes the capability contract explicit.

---

# Compiler Implementation

The initial implementation can use several semantic stages.

```text
Parser
  ↓
AST
  ↓
Name Resolution / Binding
  ↓
Type Inference
  ↓
Type Checking
  ↓
Capability Analysis
  ↓
Ownership Analysis
  ↓
Region Analysis
  ↓
Axea IR
```

Type inference should not attempt to solve ownership at the same time.

The type checker answers:

> What kind of value is this?

Capability analysis answers:

> What operations are performed on it?

Ownership analysis answers:

> Who owns the value and where may ownership move?

Region analysis answers:

> How long must its storage remain valid?

Keeping these concerns separate should make the compiler easier to reason about.

---

# Initial Type Checker Representation

A C++ compiler implementation might begin with:

```cpp
enum class TypeKind
{
    Bool,

    I8,
    I16,
    I32,
    I64,
    I128,

    U8,
    U16,
    U32,
    U64,
    U128,

    F32,
    F64,

    Char,
    String,

    Unit,
    Never,

    Optional,
    Error,
    Struct,
    Tuple,
    Function,
    Generic,
    Reference,
    Pointer,
    Slice
};
```

A semantic type node could conceptually be:

```cpp
struct Type
{
    TypeKind kind;
};
```

and specialized types could contain child types:

```text
Optional<User>
Error<Config>
Pointer<u8>
Slice<i32>
List<User>
```

The surface language remains:

```ax
User?
Config!
ptr<u8>
slice<i32>
List<User>
```

---

# Type Inference Strategy

The first implementation should use local constraint-based inference rather than attempt whole-program inference.

For:

```ax
x = 10
y = x + 20
```

constraints are approximately:

```text
type(x) = integer
type(y) = type(x + 20)
operator +(type(x), type(20)) must exist
```

Default unresolved integer literals to `i32`.

Public APIs should serve as inference boundaries.

This allows fast, predictable compilation without requiring global whole-program solving.

---

# Alternatives Considered

## Nullable references by default

Rejected.

Reason:

It makes every reference potentially invalid and requires either runtime failures or pervasive defensive checking.

Axea instead uses explicit:

```ax
User?
```

---

## Dynamic local variable typing

Rejected.

```ax
x = 10
x = "hello"
```

would make optimization, static reasoning, ownership analysis, and diagnostics substantially harder.

---

## `Option<T>` as mandatory surface syntax

Not preferred for common code.

Axea uses:

```ax
User?
```

instead of:

```text
Option<User>
```

The compiler may internally represent them similarly.

---

## `Result<T, E>` as mandatory surface syntax

Not preferred for ordinary errors.

Axea uses:

```ax
Config!
```

with possible future typed-error syntax.

The goal is to preserve strong typing without generic-wrapper ceremony.

---

## Arbitrary implicit conversions

Rejected.

Axea favors explicit conversion over surprising coercion.

---

# Open Questions

The following still need firm decisions:

- Should the default integer type always be `i32`?
- Should Axea include `isize` and `usize`?
- Which numeric widening conversions, if any, are implicit?
- What are exact release-mode integer overflow semantics?
- Is `str` an owned string, a string view, or a compiler-selected abstraction?
- What concrete collection type does `[1, 2, 3]` produce?
- Exact syntax for typed errors.
- Exact syntax for sum types.
- Exact syntax for newtypes.
- Whether tuple field access uses `.0`, destructuring only, or named syntax.
- Whether public function return types are always mandatory.
- Whether private function parameter types may eventually be inferred.
- Whether option fallback uses `or`, `??`, or both.
- Whether `T?!` should ever be supported.
- How reference types appear in source when explicit references are necessary.
- Whether checked numeric conversion should use `T.from(x)?` or another syntax.
- Exact rules for floating-point comparisons and NaN.
- Whether `unit` and `never` names are user-facing or primarily compiler concepts.

---

# Future Work

Future versions of the type system may add:

- Const generics
- Associated types
- Higher-kinded abstractions if justified
- SIMD/vector types
- Compile-time reflection
- Stronger refinement types
- Typed units
- Algebraic effects
- Better FFI layout types
- Specialized numeric types
- User-defined literal types

These should only be added when they provide enough value to justify their complexity.

---

# Guiding Rule

> If the compiler can prove the type safely and predictably, infer it. If the type is part of a public contract or cannot be determined unambiguously, require the programmer to state it.
