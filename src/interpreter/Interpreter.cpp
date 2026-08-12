#include "interpreter/Interpreter.hpp"

#include <stdexcept>

namespace
{
    std::int64_t asInt(const Value& value)
    {
        if (const auto* integer = std::get_if<std::int64_t>(&value))
        {
            return *integer;
        }
        throw std::runtime_error("expected an integer operand");
    }

    bool asBool(const Value& value)
    {
        if (const auto* boolean = std::get_if<bool>(&value))
        {
            return *boolean;
        }
        throw std::runtime_error("expected a boolean condition");
    }
} // namespace

std::string toString(const Value& value)
{
    if (const auto* integer = std::get_if<std::int64_t>(&value))
    {
        return std::to_string(*integer);
    }
    if (const auto* boolean = std::get_if<bool>(&value))
    {
        return *boolean ? "true" : "false";
    }
    return std::get<std::string>(value);
}

void Interpreter::execute(const Stmt& stmt)
{
    if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(&stmt))
    {
        variables_[assignment->name] = evaluate(*assignment->value);
        return;
    }

    throw std::runtime_error("unsupported statement");
}

Value Interpreter::evaluate(const Expr& expr) const
{
    if (const auto* integer = dynamic_cast<const IntegerExpr*>(&expr))
    {
        return integer->value;
    }

    if (const auto* boolean = dynamic_cast<const BoolExpr*>(&expr))
    {
        return boolean->value;
    }

    if (const auto* string = dynamic_cast<const StringExpr*>(&expr))
    {
        return string->value;
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        const auto it = variables_.find(name->name);
        if (it == variables_.end())
        {
            throw std::runtime_error("undefined variable: " + name->name);
        }
        return it->second;
    }

    if (const auto* ifExpr = dynamic_cast<const IfExpr*>(&expr))
    {
        if (asBool(evaluate(*ifExpr->condition)))
        {
            return evaluate(*ifExpr->thenBranch);
        }
        return evaluate(*ifExpr->elseBranch);
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr))
    {
        const auto left = evaluate(*binary->left);
        const auto right = evaluate(*binary->right);

        switch (binary->op)
        {
            case TokenKind::Plus: return asInt(left) + asInt(right);
            case TokenKind::Minus: return asInt(left) - asInt(right);
            case TokenKind::Star: return asInt(left) * asInt(right);
            case TokenKind::Slash:
                if (asInt(right) == 0)
                {
                    throw std::runtime_error("division by zero");
                }
                return asInt(left) / asInt(right);
            case TokenKind::EqualEqual: return left == right;
            case TokenKind::BangEqual: return !(left == right);
            case TokenKind::Less: return asInt(left) < asInt(right);
            case TokenKind::LessEqual: return asInt(left) <= asInt(right);
            case TokenKind::Greater: return asInt(left) > asInt(right);
            case TokenKind::GreaterEqual: return asInt(left) >= asInt(right);
            default: throw std::runtime_error("unsupported operator");
        }
    }

    throw std::runtime_error("unsupported expression");
}

const std::unordered_map<std::string, Value>& Interpreter::variables() const
{
    return variables_;
}
