#pragma once

#include "ast/Expr.hpp"

#include <memory>
#include <string>

struct Stmt
{
    virtual ~Stmt() = default;
};

struct AssignmentStmt final : Stmt
{
    AssignmentStmt(std::string name, std::unique_ptr<Expr> value)
        : name(std::move(name)),
          value(std::move(value))
    {
    }

    std::string name;
    std::unique_ptr<Expr> value;
};
