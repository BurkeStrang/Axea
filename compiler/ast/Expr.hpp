#pragma once

#include "lexer/TokenKind.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct Expr
{
    virtual ~Expr() = default;
};

struct IntegerExpr final : Expr
{
    explicit IntegerExpr(std::int64_t value)
        : value(value)
    {
    }

    std::int64_t value;
};

struct NameExpr final : Expr
{
    explicit NameExpr(std::string name)
        : name(std::move(name))
    {
    }

    std::string name;
};

struct BinaryExpr final : Expr
{
    BinaryExpr(std::unique_ptr<Expr> left, TokenKind op, std::unique_ptr<Expr> right)
        : left(std::move(left)),
          op(op),
          right(std::move(right))
    {
    }

    std::unique_ptr<Expr> left;
    TokenKind op;
    std::unique_ptr<Expr> right;
};

struct BoolExpr final : Expr
{
    explicit BoolExpr(bool value)
        : value(value)
    {
    }

    bool value;
};

struct StringExpr final : Expr
{
    explicit StringExpr(std::string value)
        : value(std::move(value))
    {
    }

    std::string value;
};

// A single Unicode scalar value ('A', 'é', '🚀' - see
// docs/language/0044-char.md), already decoded from the literal's own raw
// UTF-8 bytes by the time the parser builds this node - every later stage
// just carries `codepoint` around as a plain 32-bit value, the same way
// IntegerExpr's own `value` is already a real std::int64_t by the time it
// reaches TypeChecker, not raw digit text.
struct CharExpr final : Expr
{
    explicit CharExpr(std::int32_t codepoint)
        : codepoint(codepoint)
    {
    }

    std::int32_t codepoint;
};

struct IfExpr final : Expr
{
    IfExpr(std::unique_ptr<Expr> condition,
           std::unique_ptr<Expr> thenBranch,
           std::unique_ptr<Expr> elseBranch)
        : condition(std::move(condition)),
          thenBranch(std::move(thenBranch)),
          elseBranch(std::move(elseBranch))
    {
    }

    std::unique_ptr<Expr> condition;
    std::unique_ptr<Expr> thenBranch;
    std::unique_ptr<Expr>
        elseBranch; // BlockExpr, or a nested IfExpr for `else if`; null if no else
};

// Infinite loop, always an expression - unlike `while`, every exit is a
// `break`, so its type is whatever the `break value`s inside agree on (see
// docs/language/0028-loops.md). `body` is always a BlockExpr.
struct LoopExpr final : Expr
{
    explicit LoopExpr(std::unique_ptr<Expr> body)
        : body(std::move(body))
    {
    }

    std::unique_ptr<Expr> body;
};

struct CallExpr final : Expr
{
    CallExpr(std::string callee, std::vector<std::unique_ptr<Expr>> arguments)
        : callee(std::move(callee)),
          arguments(std::move(arguments))
    {
    }

    std::string callee;
    std::vector<std::unique_ptr<Expr>> arguments;
};

struct FieldExpr final : Expr
{
    FieldExpr(std::unique_ptr<Expr> object, std::string field)
        : object(std::move(object)),
          field(std::move(field))
    {
    }

    std::unique_ptr<Expr> object;
    std::string field;
};

struct StructLiteralExpr final : Expr
{
    StructLiteralExpr(std::string typeName,
                      std::vector<std::pair<std::string, std::unique_ptr<Expr>>> fields)
        : typeName(std::move(typeName)),
          fields(std::move(fields))
    {
    }

    std::string typeName;
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> fields;
};

// `[e1, e2, ...]`. Element type and size are both inferred from the elements
// themselves (see docs/language/0031-arrays.md) - there is no bare `[]`
// without a type annotation to infer from.
struct ArrayLiteralExpr final : Expr
{
    explicit ArrayLiteralExpr(std::vector<std::unique_ptr<Expr>> elements)
        : elements(std::move(elements))
    {
    }

    std::vector<std::unique_ptr<Expr>> elements;
};

// `object[index]`. `object` may itself be an IndexExpr/FieldExpr, so nested
// chains (`a[i][j]`, `a[i].field`) fall out for free via parsePostfix.
struct IndexExpr final : Expr
{
    IndexExpr(std::unique_ptr<Expr> object, std::unique_ptr<Expr> index)
        : object(std::move(object)),
          index(std::move(index))
    {
    }

    std::unique_ptr<Expr> object;
    std::unique_ptr<Expr> index;
};

// `List<elem>()` - always empty parens this phase (construction only, no
// initial elements). `elementType` is a single identifier, same one-level
// restriction arrays/slices already have. See docs/language/0033-lists.md.
struct ListNewExpr final : Expr
{
    explicit ListNewExpr(std::string elementType)
        : elementType(std::move(elementType))
    {
    }

    std::string elementType;
};

// `object.method(args)` - e.g. `list.push(x)`, `list.pop()`. Distinct from
// FieldExpr (`object.field`, no parens): parsePostfix decides which based on
// whether '(' follows the identifier after '.'. "push"/"pop" are the only
// recognized methods this phase (both intrinsic to List<T>, not a general
// user-defined method system) - anything else is a TypeChecker error, not a
// parser one, mirroring how ".length" vs. any other field name is resolved
// for arrays/slices.
struct MethodCallExpr final : Expr
{
    MethodCallExpr(std::unique_ptr<Expr> object,
                   std::string method,
                   std::vector<std::unique_ptr<Expr>> arguments)
        : object(std::move(object)),
          method(std::move(method)),
          arguments(std::move(arguments))
    {
    }

    std::unique_ptr<Expr> object;
    std::string method;
    std::vector<std::unique_ptr<Expr>> arguments;
};

// `Map<key,value>()` - always empty parens (construction only, no initial
// entries). The parser accepts any type syntactically here (mirrors
// ListNewExpr/parseTypeName's own "parser stays general" convention) - only
// TypeChecker rejects anything but i32/i32 this phase, with a clear error
// (see docs/language/0034-maps-and-sets.md), rather than the parser silently
// discarding what was actually written.
struct MapNewExpr final : Expr
{
    MapNewExpr(std::string keyType, std::string valueType)
        : keyType(std::move(keyType)),
          valueType(std::move(valueType))
    {
    }

    std::string keyType;
    std::string valueType;
};

// `SortedMap<key,value>()` - always empty parens (construction only). A real
// AVL tree, keeping keys ordered (see docs/language/0040-sorted-maps.md) -
// fielded identically to MapNewExpr, same "parser stays general, TypeChecker
// enforces the real restriction (i32 keys only)" reasoning.
struct SortedMapNewExpr final : Expr
{
    SortedMapNewExpr(std::string keyType, std::string valueType)
        : keyType(std::move(keyType)),
          valueType(std::move(valueType))
    {
    }

    std::string keyType;
    std::string valueType;
};

// `Set<elem>()` - always empty parens. Same "parser permissive, TypeChecker
// enforces i32-only" reasoning as MapNewExpr.
struct SetNewExpr final : Expr
{
    explicit SetNewExpr(std::string elementType)
        : elementType(std::move(elementType))
    {
    }

    std::string elementType;
};

// `Stack<elem>()` - always empty parens (construction only). A LIFO
// collection backed internally by List<T>'s own machinery (see
// docs/language/0035-stacks.md) - fielded identically to ListNewExpr, same
// one-level element-type restriction.
struct StackNewExpr final : Expr
{
    explicit StackNewExpr(std::string elementType)
        : elementType(std::move(elementType))
    {
    }

    std::string elementType;
};

// `LinkedList<elem>()` - always empty parens (construction only). A doubly
// linked, node-based collection (see docs/language/0036-linked-lists.md) -
// fielded identically to ListNewExpr/StackNewExpr, same one-level
// element-type restriction.
struct LinkedListNewExpr final : Expr
{
    explicit LinkedListNewExpr(std::string elementType)
        : elementType(std::move(elementType))
    {
    }

    std::string elementType;
};

// `Deque<elem>()` - always empty parens (construction only). A growable
// array with a `start` offset (see docs/language/0037-deques.md) - fielded
// identically to ListNewExpr/StackNewExpr/LinkedListNewExpr, same one-level
// element-type restriction.
struct DequeNewExpr final : Expr
{
    explicit DequeNewExpr(std::string elementType)
        : elementType(std::move(elementType))
    {
    }

    std::string elementType;
};

// `Queue<elem>()` - always empty parens (construction only). A FIFO
// collection backed internally by Deque<T>'s own machinery (see
// docs/language/0038-queues.md) - fielded identically to DequeNewExpr, same
// one-level element-type restriction.
struct QueueNewExpr final : Expr
{
    explicit QueueNewExpr(std::string elementType)
        : elementType(std::move(elementType))
    {
    }

    std::string elementType;
};

// `PriorityQueue<elem>()` - always empty parens (construction only). A real
// binary heap (see docs/language/0039-priority-queues.md) - fielded
// identically to StackNewExpr; `elementType` is restricted to `i32` by
// TypeChecker (the only orderable type in this language today), not by the
// parser here.
struct PriorityQueueNewExpr final : Expr
{
    explicit PriorityQueueNewExpr(std::string elementType)
        : elementType(std::move(elementType))
    {
    }

    std::string elementType;
};

// `SortedSet<elem>()` - always empty parens (construction only). A real AVL
// tree, keeping elements ordered (see docs/language/0041-sorted-sets.md) -
// fielded identically to SetNewExpr; `elementType` is restricted to `i32`
// by TypeChecker (the only orderable type in this language today, same
// restriction PriorityQueueNewExpr/SortedMapNewExpr's own key already have).
struct SortedSetNewExpr final : Expr
{
    explicit SortedSetNewExpr(std::string elementType)
        : elementType(std::move(elementType))
    {
    }

    std::string elementType;
};

// `String(text)` - always exactly one argument (construction only, no
// generic type parameter - String isn't parameterized, unlike every
// collection above). An owned, growable byte buffer (see
// docs/language/0042-string.md); `text` is a real sub-expression (a str
// literal, a variable, another String - TypeChecker enforces which),
// unlike List/Stack/.../SortedSet's own `elementType` string, since
// String's constructor takes a runtime value to copy, not a type to
// parameterize over.
struct StringNewExpr final : Expr
{
    explicit StringNewExpr(std::unique_ptr<Expr> text)
        : text(std::move(text))
    {
    }

    std::unique_ptr<Expr> text;
};

// `Buffer()` - always empty parens (construction only, no arguments and no
// generic type parameter - see docs/language/0043-buffer.md). Axea's own
// mutable, amortized-growth text-construction type; fielded identically to
// every zero-argument collection constructor (ListNewExpr, SetNewExpr,
// ...) despite Buffer itself not being generic, since - unlike
// StringNewExpr - it takes no value to copy either.
struct BufferNewExpr final : Expr
{
};
