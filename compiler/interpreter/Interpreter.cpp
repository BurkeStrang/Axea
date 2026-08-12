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

    // Thrown by `return` and caught at the function-call boundary; deliberately
    // does not derive from std::exception so it can't be caught by generic
    // exception handlers along the way.
    struct ReturnSignal
    {
        Value value;
    };
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
    if (const auto* string = std::get_if<std::string>(&value))
    {
        return *string;
    }
    if (const auto* instance = std::get_if<std::shared_ptr<StructInstance>>(&value))
    {
        std::string result = (*instance)->typeName + " { ";
        for (std::size_t i = 0; i < (*instance)->fields.size(); ++i)
        {
            if (i > 0)
            {
                result += ", ";
            }
            result += (*instance)->fields[i].first + ": " + toString((*instance)->fields[i].second);
        }
        result += " }";
        return result;
    }
    return "()";
}

Environment::Environment(Environment* parent)
    : parent_(parent)
{
}

void Environment::define(const std::string& name, Value value)
{
    values_[name] = std::move(value);
}

void Environment::assign(const std::string& name, Value value)
{
    if (const auto it = values_.find(name); it != values_.end())
    {
        it->second = std::move(value);
        return;
    }
    if (parent_)
    {
        parent_->assign(name, std::move(value));
        return;
    }
    throw std::runtime_error("undefined variable: " + name);
}

Value Environment::get(const std::string& name) const
{
    if (const auto it = values_.find(name); it != values_.end())
    {
        return it->second;
    }
    if (parent_)
    {
        return parent_->get(name);
    }
    throw std::runtime_error("undefined variable: " + name);
}

const std::unordered_map<std::string, Value>& Environment::bindings() const
{
    return values_;
}

void Interpreter::run(const Program& program)
{
    for (const auto& item : program.items)
    {
        if (const auto* function = dynamic_cast<const FunctionDecl*>(item.get()))
        {
            functions_[function->name] = function;
        }
        else if (const auto* structDecl = dynamic_cast<const StructDecl*>(item.get()))
        {
            structs_[structDecl->name] = structDecl;
        }
    }

    for (const auto& item : program.items)
    {
        if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(item.get()))
        {
            execute(*assignment, globalEnv_);
        }
    }
}

void Interpreter::execute(const Stmt& stmt, Environment& env)
{
    if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(&stmt))
    {
        env.define(assignment->name, evaluate(*assignment->value, env));
        return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt))
    {
        Value value =
            returnStmt->value ? evaluate(*returnStmt->value, env) : Value{std::monostate{}};
        throw ReturnSignal{std::move(value)};
    }

    if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt))
    {
        evaluate(*exprStmt->expr, env);
        return;
    }

    if (const auto* fieldAssign = dynamic_cast<const FieldAssignStmt*>(&stmt))
    {
        auto objectValue = evaluate(*fieldAssign->object, env);
        const auto* instance = std::get_if<std::shared_ptr<StructInstance>>(&objectValue);
        if (!instance)
        {
            throw std::runtime_error("field assignment on a non-struct value");
        }
        auto newValue = evaluate(*fieldAssign->value, env);
        for (auto& [fieldName, fieldValue] : (*instance)->fields)
        {
            if (fieldName == fieldAssign->field)
            {
                fieldValue = std::move(newValue);
                return;
            }
        }
        throw std::runtime_error("no such field: " + fieldAssign->field);
    }

    if (const auto* incDec = dynamic_cast<const IncDecStmt*>(&stmt))
    {
        const std::int64_t delta = incDec->increment ? 1 : -1;

        if (const auto* name = dynamic_cast<const NameExpr*>(incDec->target.get()))
        {
            env.assign(name->name, asInt(env.get(name->name)) + delta);
            return;
        }

        if (const auto* field = dynamic_cast<const FieldExpr*>(incDec->target.get()))
        {
            auto objectValue = evaluate(*field->object, env);
            const auto* instance = std::get_if<std::shared_ptr<StructInstance>>(&objectValue);
            if (!instance)
            {
                throw std::runtime_error("field increment/decrement on a non-struct value");
            }
            for (auto& [fieldName, fieldValue] : (*instance)->fields)
            {
                if (fieldName == field->field)
                {
                    fieldValue = asInt(fieldValue) + delta;
                    return;
                }
            }
            throw std::runtime_error("no such field: " + field->field);
        }

        throw std::runtime_error("invalid increment/decrement target");
    }

    throw std::runtime_error("unsupported statement");
}

Value Interpreter::evaluate(const Expr& expr, Environment& env)
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
        return env.get(name->name);
    }

    if (const auto* ifExpr = dynamic_cast<const IfExpr*>(&expr))
    {
        if (asBool(evaluate(*ifExpr->condition, env)))
        {
            return evaluate(*ifExpr->thenBranch, env);
        }
        return evaluate(*ifExpr->elseBranch, env);
    }

    if (const auto* block = dynamic_cast<const BlockExpr*>(&expr))
    {
        Environment blockEnv(&env);
        for (const auto& statement : block->statements)
        {
            execute(*statement, blockEnv);
        }
        if (block->result)
        {
            return evaluate(*block->result, blockEnv);
        }
        return Value{std::monostate{}};
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        const auto it = functions_.find(call->callee);
        if (it == functions_.end())
        {
            throw std::runtime_error("undefined function: " + call->callee);
        }

        std::vector<Value> args;
        args.reserve(call->arguments.size());
        for (const auto& argument : call->arguments)
        {
            args.push_back(evaluate(*argument, env));
        }

        return callFunction(*it->second, std::move(args));
    }

    if (const auto* field = dynamic_cast<const FieldExpr*>(&expr))
    {
        auto objectValue = evaluate(*field->object, env);
        const auto* instance = std::get_if<std::shared_ptr<StructInstance>>(&objectValue);
        if (!instance)
        {
            throw std::runtime_error("field access on a non-struct value");
        }
        for (const auto& [fieldName, fieldValue] : (*instance)->fields)
        {
            if (fieldName == field->field)
            {
                return fieldValue;
            }
        }
        throw std::runtime_error("no such field: " + field->field);
    }

    if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(&expr))
    {
        const auto it = structs_.find(literal->typeName);
        if (it == structs_.end())
        {
            throw std::runtime_error("undefined struct type: " + literal->typeName);
        }

        auto instance = std::make_shared<StructInstance>();
        instance->typeName = literal->typeName;

        for (const auto& declaredField : it->second->fields)
        {
            const Expr* initializer = nullptr;
            for (const auto& [fieldName, fieldExpr] : literal->fields)
            {
                if (fieldName == declaredField.name)
                {
                    initializer = fieldExpr.get();
                    break;
                }
            }
            if (!initializer)
            {
                throw std::runtime_error("missing field in struct literal: " + declaredField.name);
            }
            instance->fields.emplace_back(declaredField.name, evaluate(*initializer, env));
        }

        return instance;
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr))
    {
        const auto left = evaluate(*binary->left, env);
        const auto right = evaluate(*binary->right, env);

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

Value Interpreter::callFunction(const FunctionDecl& decl, std::vector<Value> args)
{
    if (args.size() != decl.params.size())
    {
        throw std::runtime_error("wrong number of arguments to " + decl.name);
    }

    Environment env; // no parent: no closures over globals or other calls' locals
    for (std::size_t i = 0; i < decl.params.size(); ++i)
    {
        env.define(decl.params[i].name, std::move(args[i]));
    }

    try
    {
        return evaluate(*decl.body, env);
    }
    catch (ReturnSignal& signal)
    {
        return std::move(signal.value);
    }
}

const std::unordered_map<std::string, Value>& Interpreter::variables() const
{
    return globalEnv_.bindings();
}
