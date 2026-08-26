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

// An `i64`-suffixed integer literal ("100i64" - see
// docs/language/0005-type-system.md) - a genuinely distinct AST node from
// IntegerExpr (i32), not a shared node with a width flag, matching how
// CharExpr below is its own node rather than an IntegerExpr variant.
struct Int64Expr final : Expr
{
    explicit Int64Expr(std::int64_t value)
        : value(value)
    {
    }

    std::int64_t value;
};

// A float literal - always `f64`, the only float type this phase (a
// decimal point, an "f64" suffix, or both - see
// docs/language/0005-type-system.md).
struct FloatExpr final : Expr
{
    explicit FloatExpr(double value)
        : value(value)
    {
    }

    double value;
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

// `<expr> as <targetType>` - a numeric conversion between i32/i64/f64 only
// this phase (see docs/language/0005-type-system.md) - no wrapping/
// checked/saturating variants, just a direct value conversion (sign-
// extend/truncate between i32/i64, convert between int and f64). `as`
// binds tighter than every binary operator except unary (see
// Parser::parseUnary/parseCast) - `-x as i64` is `(-x) as i64`, `x as i64
// + 1` is `(x as i64) + 1`, matching the common convention this
// language's own precedent (Rust) already established for `as`.
struct CastExpr final : Expr
{
    CastExpr(std::unique_ptr<Expr> operand, std::string targetType)
        : operand(std::move(operand)),
          targetType(std::move(targetType))
    {
    }

    std::unique_ptr<Expr> operand;
    std::string targetType;
};

// `Some(value)` (see docs/language/0052-optional.md) - wraps `value` into a
// present `Optional<T>`; T is synthesized bottom-up from `value`'s own
// checked type (unlike NoneExpr below, never needs surrounding context).
// Parsed identically to StringNewExpr's own "identifier + one parenthesized
// argument" shape.
struct SomeExpr final : Expr
{
    explicit SomeExpr(std::unique_ptr<Expr> value)
        : value(std::move(value))
    {
    }

    std::unique_ptr<Expr> value;
};

// `None` - an absent `Optional<T>`. Unlike SomeExpr, carries no expression
// to synthesize T from, so TypeChecker can only resolve a bare `None` in a
// context that already supplies an expected Optional<T> (a declared-type
// assignment or the enclosing function's own Optional<U> return type) -
// see TypeChecker::checkStmt's AssignmentStmt/ReturnStmt cases. Fielded as
// an empty struct, same convention as BufferNewExpr's own no-argument
// constructor.
struct NoneExpr final : Expr
{
};

// `Shared(value)` - the explicit, opt-in escape hatch for genuine multi-owner sharing (the sole
// use of runtime refcounting left in the language now that plain structs/enums use pure move
// semantics - see the move-semantics RFC). Structurally identical to SomeExpr above: T is
// synthesized bottom-up from `value`'s own checked type (must be struct- or enum-typed - see
// TypeChecker's own ShareExpr case), never needs surrounding context. `value` is moved into the
// new Shared<T> allocation, exactly like any other consuming use.
struct ShareExpr final : Expr
{
    explicit ShareExpr(std::unique_ptr<Expr> value)
        : value(std::move(value))
    {
    }

    std::unique_ptr<Expr> value;
};

// `Ok(value)`/`Err(value)` (see docs/language/0063-result.md) - wrap `value`
// into a `Result<T,E>`, structurally identical to SomeExpr above (one
// bottom-up-typed payload expression). Unlike SomeExpr, though, *neither*
// constructor alone can fully synthesize `Result<T,E>` - `Ok(x)` gives T but
// not E, `Err(e)` gives E but not T - so both need the identical
// "surrounding context supplies the other half" treatment NoneExpr's own
// comment describes, just for one type parameter instead of both (see
// TypeChecker::checkStmt's AssignmentStmt/ReturnStmt cases).
struct OkExpr final : Expr
{
    explicit OkExpr(std::unique_ptr<Expr> value)
        : value(std::move(value))
    {
    }

    std::unique_ptr<Expr> value;
};

struct ErrExpr final : Expr
{
    explicit ErrExpr(std::unique_ptr<Expr> value)
        : value(std::move(value))
    {
    }

    std::unique_ptr<Expr> value;
};

// `<expr>?` (see docs/language/0052-optional.md, generalized to Result<T,E>
// by docs/language/0063-result.md) - postfix, valid only inside a function
// whose own declared return type is Optional<U> or Result<U,E> (matching
// the operand's own kind - no automatic error-type conversion between a
// differently-`E`-typed operand and the enclosing function, see
// 0063-result.md's own Known Imprecision). On the success case (Some(v)/
// Ok(v)), evaluates to v; on the failure case (None/Err(e)), immediately
// returns the failure (None, or a fresh Err(e) carrying the same `e`) from
// the enclosing function - the language's only expression-context early
// return.
struct TryExpr final : Expr
{
    explicit TryExpr(std::unique_ptr<Expr> operand)
        : operand(std::move(operand))
    {
    }

    std::unique_ptr<Expr> operand;
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

// `"Hello {name}, you are {age}."` (see docs/language/Axea_Printing_Formatting.md)
// - built by the parser only when a string literal's own raw content
// contains at least one unescaped `{...}` span; a literal with none stays
// a plain StringExpr (see Parser::parsePrimary), so every pre-existing
// string literal in this codebase is unaffected. Each Piece is either a
// literal span (`expr == nullptr`, `literalText` used) or a parsed
// sub-expression (`expr` set, `literalText` unused) - alternating in
// source order.
struct InterpolatedStringExpr final : Expr
{
    struct Piece
    {
        std::string literalText;    // used when expr == nullptr
        std::unique_ptr<Expr> expr; // used when non-null; literalText then unused
        // The raw text after a top-level ':' inside this piece's own
        // `{expr:spec}` span (see docs/language/0055-numeric-format-specs.md
        // and docs/language/0057-alignment.md) - e.g. "05", ".2", "08b",
        // "<20". Empty when the piece has no format spec, OR when `debug`
        // below is set (`{expr:?}`'s own "?" is never itself a FormatSpec-
        // grammar string - see docs/language/0058-debug-formatting.md).
        // Only meaningful when `expr` is non-null; parsed once,
        // structurally, by parseFormatSpec (shared by TypeChecker/
        // Interpreter/LlvmIrEmitter - pure syntax, not a runtime
        // computation, so unlike genuine value-formatting logic there's no
        // "separate over shared" reason to duplicate it).
        std::string formatSpec;
        // `{expr=}` (see docs/language/0058-debug-formatting.md) - the raw
        // source text of `expr` exactly as written between `{` and `=`,
        // captured verbatim (not re-rendered from the parsed AST), printed
        // followed by '=' immediately before the expression's own value.
        // Empty when the piece has no self-doc marker - never legitimately
        // empty when it does, since an empty expression before '=' is
        // rejected at parse time, so emptiness alone is an unambiguous
        // "not self-doc" sentinel, the same convention `formatSpec` above
        // already uses.
        std::string selfDocPrefix;
        // `{expr:?}` (see docs/language/0058-debug-formatting.md) - the
        // "debug representation": identical to the unformatted case for
        // every type except str/String, where it additionally wraps the
        // text in double quotes. Mutually exclusive with `formatSpec`
        // being non-empty (the parser sets one or the other, never both).
        bool debug = false;
    };

    explicit InterpolatedStringExpr(std::vector<Piece> pieces)
        : pieces(std::move(pieces))
    {
    }

    std::vector<Piece> pieces;
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

// `match scrutinee { Variant(a, b) => expr  Other => expr  _ => expr }` (see
// docs/language/0064-enums.md) - `variantName` is a real declared variant name on the
// scrutinee's own enum type, or the literal text "_" for a wildcard catch-all (parsed the same
// "special-cased identifier text" way None/Some/Ok/Err already are - no reserved token needed).
// `bindingNames` has one entry per the variant's own declared positional payload type (empty for
// a no-payload variant or a wildcard arm) - each bound, in the arm's own body only, to that
// payload slot's value. A wildcard arm, if present, must be the last arm (enforced by
// TypeChecker, not the parser - see that check's own comment for why): every arm after it would
// otherwise be unreachable dead code, a real correctness footgun worth rejecting outright rather
// than silently allowing.
struct MatchArm
{
    std::string variantName;
    std::vector<std::string> bindingNames;
    std::unique_ptr<Expr> body;
};

struct MatchExpr final : Expr
{
    MatchExpr(std::unique_ptr<Expr> scrutinee, std::vector<MatchArm> arms)
        : scrutinee(std::move(scrutinee)),
          arms(std::move(arms))
    {
    }

    std::unique_ptr<Expr> scrutinee;
    std::vector<MatchArm> arms;
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

// `object[start..end]` / `object[..end]` / `object[start..]` / `object[..]`
// - a zero-copy-*conceptually* (see docs/language/0045-str-slicing.md for
// why this implementation actually copies) range slice, restricted to a
// str-coercible `object` (str or String). Deliberately a separate AST
// node from IndexExpr, not a variant of it - a bare `arr[i]` and a range
// `str[a..b]` apply to entirely different object types and produce
// entirely different results, the same "separate over shared" call this
// codebase already makes for every other pair of genuinely different
// operations. `start`/`end` are each independently optional (null means
// "from the beginning" / "to the end"), never both null and no `..` at
// the same time - that shape is a plain IndexExpr instead, decided by
// the parser.
struct StrSliceExpr final : Expr
{
    StrSliceExpr(std::unique_ptr<Expr> object,
                 std::unique_ptr<Expr> start,
                 std::unique_ptr<Expr> end)
        : object(std::move(object)),
          start(std::move(start)),
          end(std::move(end))
    {
    }

    std::unique_ptr<Expr> object;
    std::unique_ptr<Expr> start; // null => 0
    std::unique_ptr<Expr> end;   // null => the object's own runtime length
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
// `object.method(args)`, and now also `object.method<TypeArg>(args)` (see
// docs/language/0046-generic-methods.md) - `typeArgument` is empty for
// every ordinary method call (every collection method up to this phase),
// non-empty only for a generic call's own explicit type argument
// (currently only `.parse<T>()` uses this). Reusing this same node rather
// than adding a dedicated one keeps every existing pass that already
// walks `object`/`arguments` generically (CapabilityChecker,
// RegionChecker) working unchanged for the new shape too.
struct MethodCallExpr final : Expr
{
    MethodCallExpr(std::unique_ptr<Expr> object,
                   std::string method,
                   std::vector<std::unique_ptr<Expr>> arguments,
                   std::string typeArgument = "")
        : object(std::move(object)),
          method(std::move(method)),
          arguments(std::move(arguments)),
          typeArgument(std::move(typeArgument))
    {
    }

    std::unique_ptr<Expr> object;
    std::string method;
    std::vector<std::unique_ptr<Expr>> arguments;
    std::string typeArgument; // empty => not a generic call
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
