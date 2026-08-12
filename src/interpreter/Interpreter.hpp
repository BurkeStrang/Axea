#pragma once

#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>

using Value = std::variant<std::int64_t, bool, std::string>;

std::string toString(const Value& value);

class Interpreter
{
public:
    void execute(const Stmt& stmt);
    Value evaluate(const Expr& expr) const;

    const std::unordered_map<std::string, Value>& variables() const;

private:
    std::unordered_map<std::string, Value> variables_;
};
