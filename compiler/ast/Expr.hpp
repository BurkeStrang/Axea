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
