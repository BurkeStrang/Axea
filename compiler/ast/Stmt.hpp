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
                   std::unique_ptr<Expr> value)
        : name(std::move(name)),
          declaredType(std::move(declaredType)),
          value(std::move(value))
    {
    }

    std::string name;
    std::optional<std::string> declaredType; // e.g. "i32"; empty if not annotated
    std::unique_ptr<Expr> value;
};

struct ReturnStmt final : Stmt
{
    explicit ReturnStmt(std::unique_ptr<Expr> value)
        : value(std::move(value))
    {
    }

    std::unique_ptr<Expr> value; // null => bare `return` (unit)
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
