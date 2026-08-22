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

struct Program
{
    std::vector<std::unique_ptr<Stmt>> items;
};
