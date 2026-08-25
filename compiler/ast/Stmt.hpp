#pragma once

#include "ast/Expr.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class Capability
{
    Read,
    Write,
    Take
};

constexpr std::string_view capabilityName(Capability capability)
{
    switch (capability)
    {
        case Capability::Read: return "read";
        case Capability::Write: return "write";
        case Capability::Take: return "take";
    }
    return "unknown";
}

struct Stmt
{
    virtual ~Stmt() = default;
};

// Lives here rather than in Expr.hpp because it holds Stmt children, and a
// virtual-destructor override needs Stmt complete at the point of definition.
struct BlockExpr final : Expr
{
    BlockExpr(std::vector<std::unique_ptr<Stmt>> statements, std::unique_ptr<Expr> result)
        : statements(std::move(statements)),
          result(std::move(result))
    {
    }

    std::vector<std::unique_ptr<Stmt>> statements;
    std::unique_ptr<Expr> result; // null => block is unit-typed
};

struct AssignmentStmt final : Stmt
{
    AssignmentStmt(std::string name,
                   std::optional<std::string> declaredType,
                   std::unique_ptr<Expr> value,
                   bool forceDefine = false)
        : name(std::move(name)),
          declaredType(std::move(declaredType)),
          value(std::move(value)),
          forceDefine(forceDefine)
    {
    }

    std::string name;
    std::optional<std::string> declaredType; // e.g. "i32"; empty if not annotated
    std::unique_ptr<Expr> value;
    // True only for compiler-generated assignments (currently: a `for`
    // loop's desugared induction-variable binding, see Parser::parseFor)
    // that must always introduce a fresh binding regardless of whether a
    // same-named variable already exists in an enclosing scope - never set
    // by anything parsed directly from user syntax. Without this, `for i in
    // ...` would silently mutate an unrelated outer `i` if one happened to
    // exist, under the "assignment mutates an existing outer binding" rule
    // (docs/language/0028-loops.md) - a loop induction variable must always
    // be its own binding. See docs/language/0029-for-loops.md.
    bool forceDefine = false;
};

struct ReturnStmt final : Stmt
{
    explicit ReturnStmt(std::unique_ptr<Expr> value)
        : value(std::move(value))
    {
    }

    std::unique_ptr<Expr> value; // null => bare `return` (unit)
};

// `while cond { body }` - a statement, not an expression: unlike `loop`,
// normal exit (condition false) has no natural value to produce, so `while`
// never produces one at all (see docs/language/0028-loops.md).
struct WhileStmt final : Stmt
{
    WhileStmt(std::unique_ptr<Expr> condition, std::unique_ptr<Expr> body)
        : condition(std::move(condition)),
          body(std::move(body))
    {
    }

    std::unique_ptr<Expr> condition;
    std::unique_ptr<Expr> body; // always a BlockExpr
};

// `break [value]`, statement-only. Always targets the innermost enclosing
// loop (no labeled-loop support). A value is only meaningful inside `loop`.
struct BreakStmt final : Stmt
{
    explicit BreakStmt(std::unique_ptr<Expr> value)
        : value(std::move(value))
    {
    }

    std::unique_ptr<Expr> value; // null => bare `break`
};

// `continue`, statement-only. Always targets the innermost enclosing loop.
struct ContinueStmt final : Stmt
{
};

// A non-trailing expression inside a block, kept for its side effect (e.g. an
// early-return guard clause); its value is evaluated and discarded.
struct ExprStmt final : Stmt
{
    explicit ExprStmt(std::unique_ptr<Expr> expr)
        : expr(std::move(expr))
    {
    }

    std::unique_ptr<Expr> expr;
};

// `object.field = value`. `object` may itself be a FieldExpr, so nested
// chains (`a.b.c = value`) fall out for free.
struct FieldAssignStmt final : Stmt
{
    FieldAssignStmt(std::unique_ptr<Expr> object, std::string field, std::unique_ptr<Expr> value)
        : object(std::move(object)),
          field(std::move(field)),
          value(std::move(value))
    {
    }

    std::unique_ptr<Expr> object;
    std::string field;
    std::unique_ptr<Expr> value;
};

// `object[index] = value`. `object` may itself be an IndexExpr/FieldExpr, so
// nested targets (`a[i][j] = v`, `a[i].field = v`) fall out for free.
struct IndexAssignStmt final : Stmt
{
    IndexAssignStmt(std::unique_ptr<Expr> object,
                    std::unique_ptr<Expr> index,
                    std::unique_ptr<Expr> value)
        : object(std::move(object)),
          index(std::move(index)),
          value(std::move(value))
    {
    }

    std::unique_ptr<Expr> object;
    std::unique_ptr<Expr> index;
    std::unique_ptr<Expr> value;
};

// `target++` / `target--`, statement-only. `target` is a NameExpr or a
// FieldExpr (validated by the parser at construction).
struct IncDecStmt final : Stmt
{
    IncDecStmt(std::unique_ptr<Expr> target, bool increment)
        : target(std::move(target)),
          increment(increment)
    {
    }

    std::unique_ptr<Expr> target;
    bool increment; // true => `++`, false => `--`
};

struct Param
{
    std::string name;
    std::string type;
    std::optional<Capability> declaredCapability; // e.g. from `write user: User`
};

// `fn(x: i32) -> i32 { x + 1 }` (see docs/language/0067-closures.md) - a closure *literal*,
// usable anywhere an expression is: assigned to a local, passed as an argument, returned. Lives
// here rather than in Expr.hpp for the identical reason BlockExpr does - it needs Param, declared
// in this file. Move-only captures: every enclosing-scope local the body references gets copied
// into the closure's own captures at the point the literal is evaluated (no explicit capture
// list, no borrowed captures - see that doc's own Design section for why). `params`/`returnType`
// mirror FunctionDecl's own shape exactly; `body` is always a BlockExpr, same "`=>` desugars to a
// real return" normalization parseFunctionDecl already does.
struct ClosureExpr final : Expr
{
    ClosureExpr(std::vector<Param> params,
                std::optional<std::string> returnType,
                std::unique_ptr<Expr> body)
        : params(std::move(params)),
          returnType(std::move(returnType)),
          body(std::move(body))
    {
    }

    std::vector<Param> params;
    std::optional<std::string> returnType; // empty => unit
    std::unique_ptr<Expr> body;
};

struct Field
{
    std::string name;
    std::string type;
};

struct FunctionDecl final : Stmt
{
    FunctionDecl(std::string name,
                 std::vector<Param> params,
                 std::optional<std::string> returnType,
                 std::unique_ptr<Expr> body)
        : name(std::move(name)),
          params(std::move(params)),
          returnType(std::move(returnType)),
          body(std::move(body))
    {
    }

    std::string name;
    std::vector<Param> params;
    std::optional<std::string> returnType; // empty => unit
    std::unique_ptr<Expr> body; // always a BlockExpr (`=>` shorthand is normalized to one)
    // `pub` (see docs/language/0066-modules.md) - only meaningful for a function that belongs to
    // a module (Program::moduleName set): gates whether outside code can reach it via
    // `alias.name(...)`. Ignored for a root-file function (no module system before this phase
    // ever checked it, and every root function stays callable exactly as before regardless).
    // Set by Parser::parseItem, not by parseFunctionDecl itself - `pub` is consumed one level up,
    // before dispatching to whichever kind of declaration follows it.
    bool isPublic = false;
};

// `trait Name { format(self, buf: Buffer)  ... }` (see
// docs/language/0062-display-trait.md) - declares a trait's required
// method shapes. Parsing stays general (any trait name, any method
// list), but this codebase's own single consumer is `Display`
// (`format(self, buf: Buffer)`) - an `impl` block's own methods are
// checked against a matching `TraitDecl`'s `MethodSig` list by name +
// arity only (not full per-parameter type conformance - see that doc's
// own Known Imprecision section), a real but deliberately minimal check.
struct TraitDecl final : Stmt
{
    struct MethodSig
    {
        std::string name;
        std::size_t paramCount; // includes the leading `self`
    };

    std::string name;
    std::vector<MethodSig> methods;
};

// `impl TraitName for TypeName { method bodies }` - each method desugars
// at parse time (see Parser::parseImplMethod) into an ordinary
// FunctionDecl with a compiler-internal mangled name
// (`typeName + "." + methodName`, e.g. "Point.format" - a '.' can never
// appear in a real Axea identifier, so this name is permanently
// unreachable from ordinary call syntax, the same "special internal
// name" convention `print`/`write` already establish) and its own
// `self` parameter resolved to the concrete `typeName` here (capability
// left uninferred, same as `buf` - see docs/language/0062-display-
// trait.md's own Capability Inference section). Every later pass
// (TypeChecker/CapabilityChecker/RegionChecker/Interpreter/IrGenerator)
// processes each of `methods` exactly like a top-level FunctionDecl,
// just reached one level deeper - see each pass's own top-level item
// loop.
struct ImplDecl final : Stmt
{
    std::string traitName;
    std::string typeName;
    std::vector<std::unique_ptr<FunctionDecl>> methods;
};

// `extern c name(params) [-> returnType]` (see docs/language/0048-ffi.md) -
// a top-level declaration with no body, unlike FunctionDecl: the actual
// implementation is linked in externally (a real libc symbol at compiled-
// code time; a small hand-implemented allowlist at interpreted-code time -
// see Interpreter.cpp). Only the "c" calling convention is recognized
// this phase - a bare identifier check at parse time, not a keyword,
// mirroring how "String"/"Buffer" are recognized by their own literal
// text rather than reserved words.
struct ExternDecl final : Stmt
{
    ExternDecl(std::string name, std::vector<Param> params, std::optional<std::string> returnType)
        : name(std::move(name)),
          params(std::move(params)),
          returnType(std::move(returnType))
    {
    }

    std::string name;
    std::vector<Param> params; // declaredCapability is always nullopt - extern params take no
                               // read/write/take prefix, mirroring a plain C function signature
    std::optional<std::string> returnType; // empty => unit (void)
    // `pub` (see docs/language/0066-modules.md) - same meaning as FunctionDecl::isPublic.
    bool isPublic = false;
    // Modules (see docs/language/0066-modules.md) - which module declared this extern ("" for
    // root), set by main.cpp's own module-loading pass. Unlike FunctionDecl, `name` itself is
    // *never* qualified with a module prefix here - it's the real, externally-linked C symbol
    // (e.g. "sqrt"), and renaming it the way a FunctionDecl's own name is renamed would try to
    // link against a symbol that doesn't exist ("math.sqrt" instead of the real "sqrt"). A
    // module-qualified call site (`math.sqrt(x)`) is resolved by looking up the extern's own
    // bare `name` directly and checking *this* field, not by qualifying the lookup key.
    std::string moduleName;
};

struct StructDecl final : Stmt
{
    StructDecl(std::string name, std::vector<Field> fields)
        : name(std::move(name)),
          fields(std::move(fields))
    {
    }

    std::string name;
    std::vector<Field> fields;
};

// `enum Name { Variant(T1, T2)  Other  ... }` (see docs/language/0064-enums.md) - a genuine
// tagged union/algebraic data type, Rust's own `enum` shape: each variant carries zero or more
// *positional* payload types (no named fields - a variant needing named fields can carry a
// struct as one of its positional slots instead, the same "compose, don't rebuild" answer
// Optional<T>/Result<T,E> already gave for "more than one/two payload values").
struct EnumVariant
{
    std::string name;
    std::vector<std::string> fieldTypes; // positional; empty = a no-payload variant
};

struct EnumDecl final : Stmt
{
    EnumDecl(std::string name, std::vector<EnumVariant> variants)
        : name(std::move(name)),
          variants(std::move(variants))
    {
    }

    std::string name;
    std::vector<EnumVariant> variants;
};

// `module math` (see docs/language/0066-modules.md) - a single declaration, scoping the *whole*
// file it appears in to module "math" (mirrors Rust's own path-less `mod`, not a braced block -
// there's no separate "module body" grammar, since a file already is one). A file with no
// ModuleDecl is the root/anonymous program - today's exact status quo, unchanged. Parser enforces
// at most one per file (a second is a parse error) but doesn't require it to be first; main.cpp's
// own module-loading pass is what actually reads it (see loadProgram/mergeModule) and consumes
// it out of the final merged Program - no later pass ever sees a ModuleDecl.
struct ModuleDecl final : Stmt
{
    std::string name;
};

// `use math [as m]` (see docs/language/0066-modules.md) - records that this file depends on
// module `math`, callable thereafter as `math.foo(...)` (or `m.foo(...)` if aliased). Retained
// as a real Program item (unlike ModuleDecl, which is discarded once its name is read) purely so
// main.cpp's discovery pass can enumerate every file's own dependencies to build its load
// worklist - no later pass consults it directly, since Parser::parsePostfix already rewrites any
// `alias.foo(...)`/`alias.field` call site's own NameExpr to the *real* module name at parse
// time (see Parser::aliases_), so every later pass only ever sees the real name.
struct UseDecl final : Stmt
{
    std::string moduleName;
    std::optional<std::string> alias;
};

struct Program
{
    std::vector<std::unique_ptr<Stmt>> items;
    // Set by Parser::parseProgram when the file it parsed contains a ModuleDecl - nullopt for a
    // root/anonymous program. Consumed by main.cpp's own module-loading pass (see
    // loadProgram/mergeModule) to qualify every function/extern this Program declares; every
    // later pass (TypeChecker onward) only ever sees the already-merged, already-qualified
    // Program, so this field is meaningless past that point.
    std::optional<std::string> moduleName;
};
