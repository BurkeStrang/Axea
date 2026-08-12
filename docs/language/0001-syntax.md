# Axea Syntax

**Status:** Draft  
**Document:** `0001-syntax.md`

---

# Motivation

Axea syntax should feel familiar to programmers coming from C-family languages while remaining significantly less verbose.

The syntax should optimize for:

- readability
- minimal ceremony
- predictable parsing
- expressive control flow
- strong tooling
- clear distinction between safe and unsafe operations

Axea uses **Allman-style braces** for blocks.

---

# Design Principles

- Braces are required for multi-line blocks.
- Semicolons are not required.
- The final expression of a block may be its value.
- `if`, `match`, `try`, and `loop` are expressions.
- Local variable types are usually inferred.
- Public APIs may expose explicit capabilities.
- Null is not implicit.
- Unsafe operations are syntactically obvious.

---

# Comments

Single-line comments:

```ax
// comment
```

Block comments:

```ax
/*
    comment
*/
```

Nested block comments are not required initially.

---

# Identifiers

Identifiers begin with a letter or underscore.

```ax
user
user_name
_user
Connection
HTTPClient
```

Axea is case-sensitive.

---

# Naming Conventions

Recommended style:

```text
Types        PascalCase
Functions    snake_case
Variables    snake_case
Constants    PascalCase or SCREAMING_SNAKE_CASE
Modules      snake_case
Traits       PascalCase
```

Exact style guidance is defined in `0026-style-guide.md`.

---

# Variables

Variables are initialized when introduced.

```ax
name = "Axea"
age = 35
active = true
```

Explicit type:

```ax
port: u16 = 443
```

Uninitialized variables are not allowed.

Invalid:

```ax
user: User
```

---

# Assignment

```ax
x = 10
x += 1
x -= 1
x *= 2
x /= 2
```

Bitwise assignment:

```ax
flags |= Write
mask &= 0xff
value ^= bit
```

---

# Functions

Basic function:

```ax
square(x: i32) -> i32
{
    x * x
}
```

No explicit return type for `unit`:

```ax
print_user(user: User)
{
    print(user.name)
}
```

Single-expression shorthand:

```ax
square(x: i32) => x * x
```

Public function:

```ax
pub get_user(id: UserId) -> User?
{
    ...
}
```

Capabilities:

```ax
pub display(read user: User)
{
    print(user.name)
}

pub update(write user: User)
{
    user.age++
}

pub send(take packet: Packet)
{
    ...
}
```

---

# Return Values

The final expression of a block is returned automatically.

```ax
square(x: i32) -> i32
{
    x * x
}
```

Explicit return remains available:

```ax
find(x: i32) -> i32?
{
    if x < 0
    {
        return none
    }

    x
}
```

---

# Structs

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

Shorthand field initialization:

```ax
user = User { id, name, active }
```

---

# Methods

Methods may omit explicit `self`.

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

The compiler infers:

```text
birthday -> write self
display  -> read self
```

---

# If Expressions

```ax
message =
    if logged_in
    {
        "Welcome"
    }
    else
    {
        "Please sign in"
    }
```

All reachable branches of a value-producing `if` must produce compatible types.

---

# Else If

```ax
result =
    if score >= 90
    {
        "A"
    }
    else if score >= 80
    {
        "B"
    }
    else
    {
        "C"
    }
```

---

# Match Expressions

```ax
label =
    match state
    {
        Ready   => "ready"
        Running => "running"
        Done    => "done"
    }
```

Block arms:

```ax
match result
{
    Success(user) =>
    {
        print(user.name)
    }

    NotFound =>
    {
        print("missing")
    }
}
```

---

# Loops

Basic loop:

```ax
loop
{
    work()
}
```

Loop returning a value:

```ax
connection =
    loop
    {
        conn = connect()

        if conn.ready
        {
            break conn
        }
    }
```

---

# While

```ax
while running
{
    tick()
}
```

---

# For

```ax
for user in users
{
    print(user.name)
}
```

Ranges:

```ax
for i in 0..10
{
    print(i)
}
```

Inclusive ranges may use:

```ax
0..=10
```

---

# Collections

List literal:

```ax
numbers = [1, 2, 3]
```

Indexing:

```ax
x = numbers[0]
```

Slicing:

```ax
header = bytes[0..20]
```

Comprehensions may be supported:

```ax
squares = [x * x for x in 0..10]
```

Filtered comprehension:

```ax
active = [u for u in users if u.active]
```

---

# Lambdas

```ax
double = x => x * 2
```

Multiple parameters:

```ax
add = (a, b) => a + b
```

Block lambda:

```ax
process = user =>
{
    print(user.name)
    user.age++
}
```

---

# Property Shorthand

Axea may support shorthand member lambdas:

```ax
users.filter(.active)
users.map(.name)
users.sort(.age)
```

Equivalent to:

```ax
users.filter(u => u.active)
```

---

# Optional Values

Optional type:

```ax
User?
```

Absent value:

```ax
none
```

Checking:

```ax
if user
{
    print(user.name)
}
```

Optional binding:

```ax
if user = find_user(id)
{
    print(user.name)
}
```

Optional chaining:

```ax
city = user.address?.city
```

Fallback:

```ax
name = user.nickname or user.name
```

---

# Error Values

Fallible return type:

```ax
Config!
```

Propagation:

```ax
config = load_config(path)?
```

Example:

```ax
load_config(path: str) -> Config!
{
    text = file.read(path)?
    json.parse<Config>(text)?
}
```

---

# Pattern Matching

Optional:

```ax
match user
{
    User user => print(user.name)
    none      => print("missing")
}
```

Variant types:

```ax
match result
{
    Success(user) => display(user)
    NotFound      => print("missing")
}
```

---

# Imports

Simple import:

```ax
import net
import json
```

Specific item:

```ax
from models import User
```

Filesystem mapping is defined by the module specification.

---

# Visibility

```ax
pub struct User
{
    ...
}

pub get_user(id: UserId) -> User?
{
    ...
}
```

Everything is private by default.

---

# Traits

Proposed syntax:

```ax
trait Drawable
{
    draw(read self, write renderer: Renderer)
}
```

Implementation:

```ax
impl Drawable for Sprite
{
    draw(read self, write renderer: Renderer)
    {
        renderer.sprite(self)
    }
}
```

---

# Generics

```ax
identity<T>(value: T) -> T
{
    value
}
```

Generic struct:

```ax
struct Box<T>
{
    value: T
}
```

Trait constraint:

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

---

# Type Aliases

```ax
type UserId = i64
```

Distinct newtype:

```ax
newtype UserId = i64
```

---

# Bitwise Operators

```ax
x & y
x | y
x ^ y
~x
x << 3
x >> 3
```

Binary literals:

```ax
mask = 0b1111_0000
```

---

# Bit Helpers

Possible standard syntax:

```ax
value.bit(3)
value.bits(0..4)

value.set_bit(3)
value.clear_bit(3)
value.toggle_bit(3)
```

---

# Safe References

Explicit reference creation, when needed:

```ax
r = ref user
```

Explicit reference type:

```ax
ref User
```

Optional reference:

```ax
(ref User)?
```

---

# Raw Pointers

```ax
ptr<u8>
ptr<Header>
```

Unsafe pointer operations:

```ax
unsafe pointer
{
    value = p.read()
}
```

---

# MMIO

Possible syntax:

```ax
uart = mmio<u32>(0x4000_1000)

uart.write(1)
```

MMIO semantics are specified in the unsafe and memory-model documents.

---

# Casts

Numeric conversion:

```ax
x = value as i64
```

Checked conversion may use:

```ax
x = i32.from(value)?
```

Pointer reinterpretation must be unsafe.

---

# Strings

```ax
name = "Axea"
```

Interpolation:

```ax
print("Hello {name}")
```

Formatting:

```ax
print("Price: {price:.2}")
```

---

# Constants

```ax
const MaxUsers = 1000
const Port: u16 = 443
```

---

# Arrays

Fixed-size array:

```ax
values: [i32; 4] = [1, 2, 3, 4]
```

---

# Tuples

```ax
point = (10, 20)
(x, y) = point
```

---

# Try Expressions

Proposed form:

```ax
config =
    try
    {
        load_config()?
    }
    catch err
    {
        default_config()
    }
```

Exact error-handling syntax remains under design.

---

# Async

```ax
async get_user(id: UserId) -> User!
{
    response = await http.get("/users/{id}")?
    response.json<User>()?
}
```

---

# Spawn

```ax
task = spawn
{
    process(data)
}

await task
```

Ownership transfer into tasks is handled by capability and ownership analysis.

---

# Unsafe Blocks

```ax
unsafe pointer
{
    ...
}
```

Possible categories:

```text
unsafe pointer
unsafe cast
unsafe ffi
```

This categorization is still under design.

---

# Reserved Keywords

Initial proposed set:

```text
async
await
break
catch
const
continue
else
enum
false
for
from
if
impl
import
in
loop
match
newtype
none
pub
read
ref
return
spawn
struct
take
trait
true
try
type
unsafe
while
write
```

The keyword set should stay intentionally small.

---

# Semicolons

Semicolons are not required.

Preferred:

```ax
x = 10
y = 20
```

Not:

```ax
x = 10;
y = 20;
```

The parser uses line structure, delimiters, and grammar context rather than mandatory semicolons.

---

# Allman Style

Canonical:

```ax
if condition
{
    work()
}
```

Not canonical:

```ax
if condition {
    work()
}
```

The formatter should enforce the canonical style.

---

# Guiding Rule

> Axea syntax should be terse where meaning is obvious and explicit where correctness depends on programmer intent.
