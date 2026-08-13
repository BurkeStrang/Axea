#include "sema/TypeChecker.hpp"

#include <stdexcept>

namespace
{
    const Type kBool{TypeKind::Bool, ""};
    const Type kI32{TypeKind::I32, ""};
    const Type kUnit{TypeKind::Unit, ""};

    void requireInt(const Type& left, const Type& right)
    {
        if (!(left == kI32) || !(right == kI32))
        {
            throw std::runtime_error("arithmetic/comparison requires i32 operands, found " +
                                     typeName(left) + " and " + typeName(right));
        }
    }
} // namespace

std::string typeName(const Type& type)
{
    switch (type.kind)
    {
        case TypeKind::Bool: return "bool";
        case TypeKind::I32: return "i32";
        case TypeKind::String: return "str";
        case TypeKind::Unit: return "unit";
        case TypeKind::Struct: return type.structName;
        default: return "<unsupported type>";
    }
}

TypeEnv::TypeEnv(const TypeEnv* parent)
    : parent_(parent)
{
}

void TypeEnv::define(const std::string& name, Type type)
{
    types_[name] = std::move(type);
}

Type TypeEnv::get(const std::string& name) const
{
    if (const auto it = types_.find(name); it != types_.end())
    {
        return it->second;
    }
    if (parent_)
    {
        return parent_->get(name);
    }
    throw std::runtime_error("undefined variable: " + name);
}

Type TypeChecker::resolveType(const std::string& name) const
{
    static const std::unordered_map<std::string, TypeKind> primitives{
        {"bool", TypeKind::Bool},
        {"i32", TypeKind::I32},
        {"str", TypeKind::String},
        {"unit", TypeKind::Unit},
    };

    if (const auto it = primitives.find(name); it != primitives.end())
    {
        return Type{it->second, ""};
    }
    if (structs_.contains(name))
    {
        return Type{TypeKind::Struct, name};
    }
    throw std::runtime_error("unsupported type: " + name);
}

void TypeChecker::registerSignatures(const Program& program)
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

    // Validated in a second pass, once every struct name is known, so
    // structs/functions may reference each other regardless of order.
    for (const auto& [name, function] : functions_)
    {
        for (const auto& param : function->params)
        {
            resolveType(param.type);
        }
        if (function->returnType)
        {
            resolveType(*function->returnType);
        }
    }
    for (const auto& [name, structDecl] : structs_)
    {
        for (const auto& field : structDecl->fields)
        {
            resolveType(field.type);
        }
    }
}

void TypeChecker::check(const Program& program)
{
    registerSignatures(program);

    TypeEnv globalEnv;
    for (const auto& item : program.items)
    {
        if (const auto* function = dynamic_cast<const FunctionDecl*>(item.get()))
        {
            checkFunction(*function);
        }
        else if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(item.get()))
        {
            checkStmt(*assignment, globalEnv, nullptr);
        }
    }
}

void TypeChecker::checkFunction(const FunctionDecl& function)
{
    TypeEnv env; // no parent: functions don't see top-level globals
    for (const auto& param : function.params)
    {
        env.define(param.name, resolveType(param.type));
    }

    const Type expectedReturn = function.returnType ? resolveType(*function.returnType) : kUnit;
    const auto& block = static_cast<const BlockExpr&>(*function.body);
    checkBlock(
        block, env, &expectedReturn); // validates every return's value type, and the internal
                                      // correctness of any leftover discarded trailing expression

    if (!(expectedReturn == kUnit) && !definitelyReturns(block))
    {
        throw std::runtime_error("function '" + function.name +
                                 "' does not return a value of type " + typeName(expectedReturn) +
                                 " on all paths (did you forget 'return'?)");
    }
}

bool TypeChecker::definitelyReturns(const BlockExpr& block) const
{
    for (const auto& statement : block.statements)
    {
        if (dynamic_cast<const ReturnStmt*>(statement.get()))
        {
            return true;
        }
        if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(statement.get()))
        {
            if (const auto* ifExpr = dynamic_cast<const IfExpr*>(exprStmt->expr.get());
                ifExpr && definitelyReturnsBranch(*ifExpr))
            {
                return true;
            }
        }
    }
    if (const auto* ifExpr = dynamic_cast<const IfExpr*>(block.result.get()))
    {
        return definitelyReturnsBranch(*ifExpr);
    }
    return false;
}

bool TypeChecker::definitelyReturnsBranch(const IfExpr& ifExpr) const
{
    const auto& thenBlock = static_cast<const BlockExpr&>(*ifExpr.thenBranch);
    const auto& elseBlock = static_cast<const BlockExpr&>(*ifExpr.elseBranch);
    return definitelyReturns(thenBlock) && definitelyReturns(elseBlock);
}

Type TypeChecker::checkBlock(const BlockExpr& block,
                             TypeEnv& parentEnv,
                             const Type* expectedReturnType)
{
    TypeEnv env(&parentEnv);
    for (const auto& statement : block.statements)
    {
        checkStmt(*statement, env, expectedReturnType);
    }
    if (block.result)
    {
        return checkExpr(*block.result, env, expectedReturnType);
    }
    return kUnit;
}

void TypeChecker::checkStmt(const Stmt& stmt, TypeEnv& env, const Type* expectedReturnType)
{
    if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(&stmt))
    {
        const Type valueType = checkExpr(*assignment->value, env, expectedReturnType);
        if (assignment->declaredType)
        {
            const Type declared = resolveType(*assignment->declaredType);
            if (!(declared == valueType))
            {
                throw std::runtime_error("variable '" + assignment->name + "' declared as " +
                                         typeName(declared) + " but initialized with " +
                                         typeName(valueType));
            }
        }
        env.define(assignment->name, valueType);
        return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt))
    {
        if (!expectedReturnType)
        {
            throw std::runtime_error("'return' used outside a function");
        }
        const Type valueType =
            returnStmt->value ? checkExpr(*returnStmt->value, env, expectedReturnType) : kUnit;
        if (!(valueType == *expectedReturnType))
        {
            throw std::runtime_error("'return' produces " + typeName(valueType) +
                                     " but function declares " + typeName(*expectedReturnType));
        }
        return;
    }

    if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt))
    {
        checkExpr(*exprStmt->expr, env, expectedReturnType);
        return;
    }

    if (const auto* fieldAssign = dynamic_cast<const FieldAssignStmt*>(&stmt))
    {
        const Type fieldType =
            checkFieldType(*fieldAssign->object, fieldAssign->field, env, expectedReturnType);
        const Type valueType = checkExpr(*fieldAssign->value, env, expectedReturnType);
        if (!(fieldType == valueType))
        {
            throw std::runtime_error("field '" + fieldAssign->field + "' expects " +
                                     typeName(fieldType) + ", got " + typeName(valueType));
        }
        return;
    }

    if (const auto* incDec = dynamic_cast<const IncDecStmt*>(&stmt))
    {
        const Type targetType = checkExpr(*incDec->target, env, expectedReturnType);
        if (!(targetType == kI32))
        {
            throw std::runtime_error("'++'/'--' requires an i32 target, found " +
                                     typeName(targetType));
        }
        return;
    }

    throw std::runtime_error("unsupported statement");
}

Type TypeChecker::checkFieldType(const Expr& object,
                                 const std::string& field,
                                 TypeEnv& env,
                                 const Type* expectedReturnType)
{
    const Type objectType = checkExpr(object, env, expectedReturnType);
    if (objectType.kind != TypeKind::Struct)
    {
        throw std::runtime_error("field access on non-struct type " + typeName(objectType));
    }

    const StructDecl& decl = *structs_.at(objectType.structName);
    for (const auto& declaredField : decl.fields)
    {
        if (declaredField.name == field)
        {
            return resolveType(declaredField.type);
        }
    }
    throw std::runtime_error("struct '" + objectType.structName + "' has no field '" + field + "'");
}

Type TypeChecker::checkExpr(const Expr& expr, TypeEnv& env, const Type* expectedReturnType)
{
    if (dynamic_cast<const IntegerExpr*>(&expr))
    {
        return kI32;
    }

    if (dynamic_cast<const BoolExpr*>(&expr))
    {
        return kBool;
    }

    if (dynamic_cast<const StringExpr*>(&expr))
    {
        return Type{TypeKind::String, ""};
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        return env.get(name->name);
    }

    if (const auto* block = dynamic_cast<const BlockExpr*>(&expr))
    {
        return checkBlock(*block, env, expectedReturnType);
    }

    if (const auto* ifExpr = dynamic_cast<const IfExpr*>(&expr))
    {
        const Type conditionType = checkExpr(*ifExpr->condition, env, expectedReturnType);
        if (!(conditionType == kBool))
        {
            throw std::runtime_error("if condition must be bool, found " + typeName(conditionType));
        }
        const Type thenType = checkExpr(*ifExpr->thenBranch, env, expectedReturnType);
        const Type elseType = checkExpr(*ifExpr->elseBranch, env, expectedReturnType);
        if (!(thenType == elseType))
        {
            throw std::runtime_error("if branches have incompatible types: then is " +
                                     typeName(thenType) + ", else is " + typeName(elseType));
        }
        return thenType;
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        const auto it = functions_.find(call->callee);
        if (it == functions_.end())
        {
            throw std::runtime_error("undefined function: " + call->callee);
        }

        const FunctionDecl& function = *it->second;
        if (call->arguments.size() != function.params.size())
        {
            throw std::runtime_error("function '" + call->callee + "' expects " +
                                     std::to_string(function.params.size()) + " argument(s), got " +
                                     std::to_string(call->arguments.size()));
        }

        for (std::size_t i = 0; i < call->arguments.size(); ++i)
        {
            const Type argType = checkExpr(*call->arguments[i], env, expectedReturnType);
            const Type paramType = resolveType(function.params[i].type);
            if (!(argType == paramType))
            {
                throw std::runtime_error("argument " + std::to_string(i + 1) + " to '" +
                                         call->callee + "' expects " + typeName(paramType) +
                                         ", got " + typeName(argType));
            }
        }

        return function.returnType ? resolveType(*function.returnType) : kUnit;
    }

    if (const auto* field = dynamic_cast<const FieldExpr*>(&expr))
    {
        return checkFieldType(*field->object, field->field, env, expectedReturnType);
    }

    if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(&expr))
    {
        const auto it = structs_.find(literal->typeName);
        if (it == structs_.end())
        {
            throw std::runtime_error("undefined struct type: " + literal->typeName);
        }

        const StructDecl& decl = *it->second;
        if (literal->fields.size() != decl.fields.size())
        {
            throw std::runtime_error("struct literal for '" + literal->typeName + "' has " +
                                     std::to_string(literal->fields.size()) +
                                     " field(s), expected " + std::to_string(decl.fields.size()));
        }

        for (const auto& declaredField : decl.fields)
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
                throw std::runtime_error("struct literal for '" + literal->typeName +
                                         "' is missing field '" + declaredField.name + "'");
            }

            const Type initType = checkExpr(*initializer, env, expectedReturnType);
            const Type declaredFieldType = resolveType(declaredField.type);
            if (!(initType == declaredFieldType))
            {
                throw std::runtime_error(
                    "field '" + declaredField.name + "' of '" + literal->typeName + "' expects " +
                    typeName(declaredFieldType) + ", got " + typeName(initType));
            }
        }

        return Type{TypeKind::Struct, literal->typeName};
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr))
    {
        const Type leftType = checkExpr(*binary->left, env, expectedReturnType);
        const Type rightType = checkExpr(*binary->right, env, expectedReturnType);

        switch (binary->op)
        {
            case TokenKind::Plus:
            case TokenKind::Minus:
            case TokenKind::Star:
            case TokenKind::Slash: requireInt(leftType, rightType); return kI32;
            case TokenKind::Less:
            case TokenKind::LessEqual:
            case TokenKind::Greater:
            case TokenKind::GreaterEqual: requireInt(leftType, rightType); return kBool;
            case TokenKind::EqualEqual:
            case TokenKind::BangEqual:
                if (!(leftType == rightType))
                {
                    throw std::runtime_error("cannot compare " + typeName(leftType) + " and " +
                                             typeName(rightType));
                }
                return kBool;
            default: throw std::runtime_error("unsupported operator");
        }
    }

    throw std::runtime_error("unsupported expression");
}
