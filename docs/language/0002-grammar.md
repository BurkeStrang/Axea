# Axea Grammar

**Status:** Draft  
**Document:** `0002-grammar.md`

---

# Purpose

This document defines the initial formal grammar for Axea.

The grammar is intentionally incomplete in areas where language semantics are still evolving, but it should be concrete enough to guide lexer and parser implementation.

Axea uses:

- recursive-descent parsing for declarations/statements
- Pratt parsing for expressions
- newline-aware statement termination
- Allman braces for blocks

The grammar below uses an EBNF-like notation.

---

# Notation

```text
A B        sequence
A | B      alternative
[A]        optional
{A}        zero or more
(A)        grouping
"A"        literal token
IDENT      lexer token
```

---

# Source File

```ebnf
source_file =
    { top_level_item } EOF ;
```

---

# Top-Level Items

```ebnf
top_level_item =
      import_decl
    | function_decl
    | struct_decl
    | trait_decl
    | impl_decl
    | enum_decl
    | type_alias_decl
    | newtype_decl
    | const_decl
    ;
```

---

# Visibility

```ebnf
visibility =
    [ "pub" ] ;
```

---

# Imports

```ebnf
import_decl =
      "import" module_path
    | "from" module_path "import" import_list
    ;
```

```ebnf
module_path =
    IDENT { "." IDENT } ;
```

```ebnf
import_list =
    IDENT { "," IDENT } ;
```

---

# Function Declarations

```ebnf
function_decl =
    visibility
    [ "async" ]
    IDENT
    [ generic_params ]
    "(" [ parameter_list ] ")"
    [ return_type ]
    ( block | "=>" expression )
    ;
```

---

# Parameters

```ebnf
parameter_list =
    parameter { "," parameter } ;
```

```ebnf
parameter =
    [ capability ]
    IDENT
    ":"
    type
    ;
```

```ebnf
capability =
      "read"
    | "write"
    | "take"
    ;
```

---

# Return Type

```ebnf
return_type =
    "->" type ;
```

---

# Generic Parameters

```ebnf
generic_params =
    "<" generic_param { "," generic_param } ">" ;
```

```ebnf
generic_param =
    IDENT [ ":" trait_bound ] ;
```

```ebnf
trait_bound =
    type_path { "+" type_path } ;
```

---

# Struct Declarations

```ebnf
struct_decl =
    visibility
    "struct"
    IDENT
    [ generic_params ]
    "{"
        { struct_member }
    "}"
    ;
```

```ebnf
struct_member =
      field_decl
    | function_decl
    ;
```

```ebnf
field_decl =
    IDENT ":" type ;
```

---

# Trait Declarations

```ebnf
trait_decl =
    visibility
    "trait"
    IDENT
    [ generic_params ]
    "{"
        { trait_member }
    "}"
    ;
```

```ebnf
trait_member =
      function_signature
    | function_decl
    ;
```

```ebnf
function_signature =
    [ "async" ]
    IDENT
    [ generic_params ]
    "(" [ parameter_list ] ")"
    [ return_type ]
    ;
```

---

# Implementations

```ebnf
impl_decl =
    "impl"
    [ generic_params ]
    type_path
    [ "for" type ]
    "{"
        { function_decl }
    "}"
    ;
```

---

# Enums / Variant Types

```ebnf
enum_decl =
    visibility
    "enum"
    IDENT
    [ generic_params ]
    "{"
        { enum_variant }
    "}"
    ;
```

```ebnf
enum_variant =
    IDENT [ "(" type_list ")" ] ;
```

---

# Type Alias

```ebnf
type_alias_decl =
    visibility
    "type"
    IDENT
    [ generic_params ]
    "="
    type
    ;
```

---

# Newtype

```ebnf
newtype_decl =
    visibility
    "newtype"
    IDENT
    "="
    type
    ;
```

---

# Constants

```ebnf
const_decl =
    visibility
    "const"
    IDENT
    [ ":" type ]
    "="
    expression
    ;
```

---

# Blocks

```ebnf
block =
    "{"
        { statement }
        [ expression ]
    "}"
    ;
```

The final expression may be the block value.

The parser must distinguish a trailing value expression from an expression statement.

---

# Statements

```ebnf
statement =
      let_statement
    | assignment_statement
    | return_statement
    | break_statement
    | continue_statement
    | expression_statement
    ;
```

Axea currently has no explicit `let` keyword. `let_statement` here refers to an initialized local declaration.

---

# Local Declaration

```ebnf
let_statement =
    IDENT
    [ ":" type ]
    "="
    expression
    ;
```

Examples:

```ax
x = 10
port: u16 = 443
```

---

# Assignment

```ebnf
assignment_statement =
    assignable
    assignment_operator
    expression
    ;
```

```ebnf
assignment_operator =
      "="
    | "+="
    | "-="
    | "*="
    | "/="
    | "%="
    | "&="
    | "|="
    | "^="
    | "<<="
    | ">>="
    ;
```

```ebnf
assignable =
      IDENT
    | member_expression
    | index_expression
    ;
```

---

# Return

```ebnf
return_statement =
    "return" [ expression ] ;
```

---

# Break

```ebnf
break_statement =
    "break" [ expression ] ;
```

---

# Continue

```ebnf
continue_statement =
    "continue" ;
```

---

# Expression Statement

```ebnf
expression_statement =
    expression ;
```

---

# Expressions

Expression parsing should use a Pratt parser.

Approximate precedence, highest to lowest:

```text
1. postfix
2. unary
3. multiplicative
4. additive
5. shift
6. relational
7. equality
8. bitwise AND
9. bitwise XOR
10. bitwise OR
11. logical AND
12. logical OR
13. fallback
14. lambda
```

---

# Primary Expressions

```ebnf
primary_expression =
      literal
    | IDENT
    | "(" expression ")"
    | tuple_expression
    | list_expression
    | struct_literal
    | if_expression
    | match_expression
    | loop_expression
    | try_expression
    | unsafe_expression
    | spawn_expression
    ;
```

---

# Literals

```ebnf
literal =
      INTEGER
    | FLOAT
    | STRING
    | CHAR
    | "true"
    | "false"
    | "none"
    ;
```

---

# Postfix Expressions

```ebnf
postfix_expression =
    primary_expression
    { postfix_operator }
    ;
```

```ebnf
postfix_operator =
      call_suffix
    | member_suffix
    | optional_member_suffix
    | index_suffix
    | slice_suffix
    | error_propagation_suffix
    ;
```

---

# Calls

```ebnf
call_suffix =
    "(" [ argument_list ] ")" ;
```

```ebnf
argument_list =
    expression { "," expression } ;
```

---

# Member Access

```ebnf
member_suffix =
    "." IDENT ;
```

Optional member access:

```ebnf
optional_member_suffix =
    "?." IDENT ;
```

---

# Indexing

```ebnf
index_suffix =
    "[" expression "]" ;
```

---

# Slicing

```ebnf
slice_suffix =
    "["
        [ expression ]
        ".."
        [ "=" ]
        [ expression ]
    "]"
    ;
```

Examples:

```ax
items[0..10]
items[..10]
items[5..]
items[0..=10]
```

---

# Error Propagation

```ebnf
error_propagation_suffix =
    "?" ;
```

This applies to error-bearing expressions.

Optional chaining uses `?.`, so the parser can distinguish them lexically.

---

# Unary Expressions

```ebnf
unary_expression =
      ("!" | "-" | "+" | "~" | "ref") unary_expression
    | postfix_expression
    ;
```

---

# Multiplicative

```ebnf
multiplicative_expression =
    unary_expression
    { ("*" | "/" | "%") unary_expression }
    ;
```

---

# Additive

```ebnf
additive_expression =
    multiplicative_expression
    { ("+" | "-") multiplicative_expression }
    ;
```

---

# Shift

```ebnf
shift_expression =
    additive_expression
    { ("<<" | ">>") additive_expression }
    ;
```

---

# Relational

```ebnf
relational_expression =
    shift_expression
    { ("<" | "<=" | ">" | ">=") shift_expression }
    ;
```

---

# Equality

```ebnf
equality_expression =
    relational_expression
    { ("==" | "!=") relational_expression }
    ;
```

---

# Bitwise AND

```ebnf
bitwise_and_expression =
    equality_expression
    { "&" equality_expression }
    ;
```

---

# Bitwise XOR

```ebnf
bitwise_xor_expression =
    bitwise_and_expression
    { "^" bitwise_and_expression }
    ;
```

---

# Bitwise OR

```ebnf
bitwise_or_expression =
    bitwise_xor_expression
    { "|" bitwise_xor_expression }
    ;
```

---

# Logical AND

```ebnf
logical_and_expression =
    bitwise_or_expression
    { "&&" bitwise_or_expression }
    ;
```

Axea may later consider `and` as a keyword alias, but symbolic logical operators are simpler for the initial grammar.

---

# Logical OR

```ebnf
logical_or_expression =
    logical_and_expression
    { "||" logical_and_expression }
    ;
```

---

# Optional Fallback

Preferred user syntax:

```ax
name = nickname or full_name
```

Grammar:

```ebnf
fallback_expression =
    logical_or_expression
    { "or" logical_or_expression }
    ;
```

`or` is therefore a contextual/operator keyword and must not conflict with `||`.

---

# Lambda

```ebnf
lambda_expression =
      IDENT "=>" expression_or_block
    | "(" [ lambda_param_list ] ")" "=>" expression_or_block
    ;
```

```ebnf
lambda_param_list =
    lambda_param { "," lambda_param } ;
```

```ebnf
lambda_param =
    IDENT [ ":" type ] ;
```

---

# If Expression

```ebnf
if_expression =
    "if" expression block
    { "else" "if" expression block }
    [ "else" block ]
    ;
```

A value-producing `if` must contain an `else` unless static analysis proves the missing branch is unreachable.

---

# Match Expression

```ebnf
match_expression =
    "match" expression
    "{"
        { match_arm }
    "}"
    ;
```

```ebnf
match_arm =
    pattern
    [ "if" expression ]
    "=>"
    expression_or_block
    ;
```

---

# Patterns

```ebnf
pattern =
      "_"
    | literal
    | IDENT
    | type_pattern
    | variant_pattern
    | tuple_pattern
    ;
```

```ebnf
type_pattern =
    type IDENT ;
```

```ebnf
variant_pattern =
    IDENT [ "(" [ pattern_list ] ")" ] ;
```

```ebnf
pattern_list =
    pattern { "," pattern } ;
```

---

# Loop Expression

```ebnf
loop_expression =
    "loop" block ;
```

---

# While Expression / Statement Form

```ebnf
while_expression =
    "while" expression block ;
```

Whether `while` is a true value-producing expression remains an open question.

---

# For Expression / Statement Form

```ebnf
for_expression =
    "for" pattern "in" expression block ;
```

---

# Try Expression

Proposed:

```ebnf
try_expression =
    "try" block
    [ "catch" [ IDENT ] block ]
    ;
```

This syntax is provisional.

---

# Unsafe Expression

```ebnf
unsafe_expression =
    "unsafe"
    [ IDENT ]
    block
    ;
```

Examples:

```ax
unsafe pointer
{
    ...
}
```

---

# Spawn Expression

```ebnf
spawn_expression =
    "spawn" block ;
```

---

# Await Expression

`await` is prefix syntax:

```ebnf
await_expression =
    "await" expression ;
```

In Pratt parsing, `await` belongs at unary precedence.

---

# List Literal

```ebnf
list_expression =
    "["
        [ list_contents ]
    "]"
    ;
```

```ebnf
list_contents =
      expression { "," expression }
    | comprehension
    ;
```

---

# Comprehension

```ebnf
comprehension =
    expression
    "for"
    pattern
    "in"
    expression
    [ "if" expression ]
    ;
```

Example:

```ax
[x * x for x in values if x > 0]
```

---

# Tuple Expression

```ebnf
tuple_expression =
    "("
        expression ","
        [ expression { "," expression } ]
    ")"
    ;
```

A parenthesized single expression is not a tuple.

---

# Struct Literal

```ebnf
struct_literal =
    type_path
    "{"
        [ struct_field_init { "," struct_field_init } ]
    "}"
    ;
```

```ebnf
struct_field_init =
      IDENT ":" expression
    | IDENT
    ;
```

Axea examples may omit commas in canonical formatting if newline separation is unambiguous. This is still an open grammar decision.

---

# Types

```ebnf
type =
      primitive_type
    | type_path
    | generic_type
    | optional_type
    | error_type
    | reference_type
    | pointer_type
    | slice_type
    | array_type
    | tuple_type
    | function_type
    ;
```

---

# Primitive Types

```ebnf
primitive_type =
      "bool"
    | "i8" | "i16" | "i32" | "i64" | "i128"
    | "u8" | "u16" | "u32" | "u64" | "u128"
    | "f32" | "f64"
    | "char"
    | "str"
    | "unit"
    | "never"
    ;
```

---

# Type Path

```ebnf
type_path =
    IDENT { "." IDENT } ;
```

---

# Generic Type

```ebnf
generic_type =
    type_path "<" type_list ">" ;
```

```ebnf
type_list =
    type { "," type } ;
```

---

# Optional Type

```ebnf
optional_type =
    type_atom "?" ;
```

Examples:

```ax
User?
str?
```

---

# Error Type

```ebnf
error_type =
    type_atom "!" ;
```

Examples:

```ax
Config!
Connection!
```

Typed errors may later extend this syntax.

---

# Reference Type

```ebnf
reference_type =
    "ref" type ;
```

---

# Pointer Type

```ebnf
pointer_type =
    "ptr" "<" type ">" ;
```

---

# Slice Type

```ebnf
slice_type =
    "slice" "<" type ">" ;
```

---

# Array Type

```ebnf
array_type =
    "[" type ";" expression "]" ;
```

The array length must be compile-time evaluable.

---

# Tuple Type

```ebnf
tuple_type =
    "(" type "," [ type { "," type } ] ")" ;
```

---

# Function Type

```ebnf
function_type =
    "(" [ type_list ] ")" "->" type ;
```

---

# Type Postfix Binding

`?` and `!` bind tightly to the preceding type.

```ax
User?
Config!
```

For complex types, parentheses may be required:

```ax
(ref User)?
```

---

# Ranges

```ebnf
range_expression =
      [ expression ] ".." [ expression ]
    | [ expression ] "..=" [ expression ]
    ;
```

Range precedence should be lower than arithmetic but higher than fallback/lambda.

---

# Newlines and Statement Termination

Axea does not require semicolons.

The lexer should emit newline information, either as explicit `NEWLINE` tokens or as source-position metadata.

A newline terminates a statement when the parser has completed a syntactically valid statement and the next token cannot continue the current expression.

Newlines do **not** terminate expressions inside:

```text
(...)
[...]
{...}
```

or after continuation tokens such as:

```text
.
?.
+
-
*
/
&&
||
,
=>
```

Example:

```ax
names = users
    .filter(.active)
    .map(.name)
    .sort()
```

must parse as one expression.

---

# Optional Semicolons

The language should either:

1. reject semicolons entirely, or
2. allow them as optional separators but never require them.

Preferred initial design:

```text
allow optional semicolons
formatter removes them
```

This makes generated code and migrations easier without making semicolons idiomatic.

---

# Operator Precedence Table

Proposed precedence from highest to lowest:

| Level | Operators |
|---|---|
| 15 | call `()`, member `.`, optional member `?.`, index `[]`, postfix `?` |
| 14 | unary `! - + ~ ref await` |
| 13 | `* / %` |
| 12 | `+ -` |
| 11 | `<< >>` |
| 10 | range `.. ..=` |
| 9 | `< <= > >=` |
| 8 | `== !=` |
| 7 | `&` |
| 6 | `^` |
| 5 | `|` |
| 4 | `&&` |
| 3 | `||` |
| 2 | `or` fallback |
| 1 | lambda `=>` |

Assignment is statement-level in the initial grammar rather than an expression operator.

---

# Associativity

```text
postfix         left
multiplicative  left
additive        left
shift           left
relational      non-chainable initially
equality        non-chainable initially
bitwise         left
logical         left
fallback        right or left TBD
lambda          right
```

Axea should likely reject:

```ax
a < b < c
```

unless chained comparisons are deliberately added later.

---

# Lexical Ambiguities

Important token distinctions:

```text
->
=>
?
?.
!
!=
..
..=
<
<<
>
>>
```

The lexer should use maximal munch.

Examples:

```text
?.   one token
..=  one token
>>=  one token
```

---

# Grammar Examples

## Function

```ax
square(x: i32) -> i32
{
    x * x
}
```

Parse shape:

```text
FunctionDecl
├── name: square
├── parameters
│   └── x: i32
├── return: i32
└── Block
    └── Binary(*)
        ├── x
        └── x
```

---

## If Expression

```ax
value =
    if condition
    {
        1
    }
    else
    {
        2
    }
```

Parse shape:

```text
LocalDecl
├── value
└── IfExpr
    ├── condition
    ├── then: 1
    └── else: 2
```

---

## Capability Parameter

```ax
pub update(write user: User)
{
    user.age++
}
```

Parse shape:

```text
FunctionDecl
├── pub
├── update
├── Parameter
│   ├── capability: write
│   ├── name: user
│   └── type: User
└── Block
```

---

# Parser Strategy

Recommended implementation:

- recursive descent for top-level declarations
- Pratt parser for expressions
- explicit parse functions for `if`, `match`, `loop`, `try`, lambdas, literals, and postfix operators

Conceptual C++ interface:

```cpp
std::unique_ptr<Expr> parse_expression(int min_precedence = 0);
std::unique_ptr<Expr> parse_prefix_expression();
std::unique_ptr<Expr> parse_postfix_expression(std::unique_ptr<Expr> left);
```

---

# Error Recovery

The parser should synchronize at:

```text
}
pub
struct
trait
impl
enum
type
newtype
const
import
from
```

Within blocks, likely synchronization points include line boundaries and `}`.

Diagnostics should preserve parsing progress whenever possible.

---

# Open Questions

- Are commas required between struct literal fields?
- Are semicolons accepted optionally?
- Is `or` reserved only as an optional-fallback operator?
- Should logical operators be `&&`/`||`, `and`/`or`, or both?
- Is `while` an expression or only a statement-like construct?
- Is `for` an expression?
- Exact grammar for typed errors.
- Exact grammar for destructuring assignment.
- Exact syntax for annotations such as `@repr(C)`.
- Whether `self` may be explicitly written.
- Whether fields inside struct bodies can use newline-only separation.
- Whether function declarations need a `fn` keyword. Current Axea design says no.
- Whether property shorthand `.name` needs a dedicated grammar production.
- Whether pattern type forms like `User user` conflict with ordinary variant patterns.
- Exact parsing rules for `none` and optional binding.
- Whether assignment ever becomes an expression.
- Whether `unsafe pointer` uses arbitrary categories or a fixed keyword set.

---

# Guiding Rule

> The grammar should remain simple enough that syntax errors are easy to explain and tooling can parse Axea predictably.
