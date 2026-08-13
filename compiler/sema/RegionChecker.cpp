#include "sema/RegionChecker.hpp"

#include <stdexcept>

RegionEnv::RegionEnv(const RegionEnv* parent)
    : parent_(parent)
{
}

void RegionEnv::define(const std::string& name, RegionInfo info)
{
    bindings_[name] = std::move(info);
}

RegionInfo RegionEnv::get(const std::string& name) const
{
    if (const auto it = bindings_.find(name); it != bindings_.end())
    {
        return it->second;
    }
    if (parent_)
    {
        return parent_->get(name);
    }
    throw std::runtime_error("undefined variable: " + name);
}

void RegionChecker::registerDecls(const Program& program)
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
}

void RegionChecker::requireOwned(const RegionInfo& info, const FunctionDecl& function) const
{
    if (info.kind == Region::Borrowed)
    {
        throw std::runtime_error(
            "function '" + function.name + "' cannot return '" + info.sourceParam +
            "': parameter '" + info.sourceParam +
            "' is borrowed and does not outlive the call - declare 'take' if ownership "
            "should transfer");
    }
}

void RegionChecker::checkFunction(const FunctionDecl& function,
                                  const std::vector<Capability>& capabilities)
{
    std::vector<Region> paramRegions;
    paramRegions.reserve(function.params.size());

    RegionEnv env;
    for (std::size_t i = 0; i < function.params.size(); ++i)
    {
        const auto& param = function.params[i];
        const std::string structType = structs_.contains(param.type) ? param.type : "";
        // Only struct-typed parameters carry any aliasing risk (Value stores
        // primitives by value); a primitive parameter is always Owned
        // regardless of its read/write/take capability.
        const bool borrowed = !structType.empty() && capabilities[i] != Capability::Take;
        env.define(param.name,
                   RegionInfo{borrowed ? Region::Borrowed : Region::Owned,
                              borrowed ? param.name : "",
                              structType});
        paramRegions.push_back(borrowed ? Region::Borrowed : Region::Owned);
    }
    regions_[function.name] = std::move(paramRegions);

    // Nothing can leak through a non-struct return type: primitives are
    // always copied by value, and unit carries no value at all.
    if (!function.returnType || !structs_.contains(*function.returnType))
    {
        return;
    }

    // Every individual `return` site is already checked independently,
    // however deeply nested in if/else (regionOfStmt's ReturnStmt case,
    // reached via this same walk) - functions require an explicit `return`
    // for anything but unit (docs/language/0023), so the body's own
    // top-level trailing value is no longer itself a return and must not be
    // wrapped in requireOwned. Still walk it for that recursive side effect.
    regionOfExpr(*function.body, env, function);
}

RegionInfo
RegionChecker::regionOfExpr(const Expr& expr, RegionEnv& env, const FunctionDecl& function)
{
    if (dynamic_cast<const IntegerExpr*>(&expr) || dynamic_cast<const BoolExpr*>(&expr) ||
        dynamic_cast<const StringExpr*>(&expr))
    {
        return RegionInfo{Region::Owned, "", ""};
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        return env.get(name->name);
    }

    if (const auto* field = dynamic_cast<const FieldExpr*>(&expr))
    {
        const RegionInfo objectInfo = regionOfExpr(*field->object, env, function);
        if (!objectInfo.structType.empty())
        {
            const auto it = structs_.find(objectInfo.structType);
            if (it != structs_.end())
            {
                for (const auto& declaredField : it->second->fields)
                {
                    if (declaredField.name != field->field)
                    {
                        continue;
                    }
                    if (structs_.contains(declaredField.type))
                    {
                        // Struct-typed field: aliases the same shared instance as the object.
                        return RegionInfo{
                            objectInfo.kind, objectInfo.sourceParam, declaredField.type};
                    }
                    // Primitive field: Value stores it by value, always a fresh copy.
                    return RegionInfo{Region::Owned, "", ""};
                }
            }
        }
        return RegionInfo{Region::Owned, "", ""};
    }

    if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(&expr))
    {
        RegionInfo borrowed{Region::Owned, "", ""};
        bool anyBorrowed = false;
        for (const auto& [fieldName, fieldExpr] : literal->fields)
        {
            const RegionInfo fieldInfo = regionOfExpr(*fieldExpr, env, function);
            if (fieldInfo.kind == Region::Borrowed && !anyBorrowed)
            {
                anyBorrowed = true;
                borrowed = fieldInfo;
            }
        }
        if (anyBorrowed)
        {
            return RegionInfo{Region::Borrowed, borrowed.sourceParam, literal->typeName};
        }
        return RegionInfo{Region::Owned, "", literal->typeName};
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        for (const auto& argument : call->arguments)
        {
            regionOfExpr(*argument, env, function);
        }

        std::string structType;
        const auto it = functions_.find(call->callee);
        if (it != functions_.end() && it->second->returnType &&
            structs_.contains(*it->second->returnType))
        {
            structType = *it->second->returnType;
        }
        // A call's result is always Owned: if the callee actually leaked a
        // borrow through its return, the callee's own check rejects it
        // independently - the caller never needs to re-verify it.
        return RegionInfo{Region::Owned, "", structType};
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr))
    {
        regionOfExpr(*binary->left, env, function);
        regionOfExpr(*binary->right, env, function);
        return RegionInfo{Region::Owned, "", ""}; // arithmetic/comparison always yields a primitive
    }

    if (const auto* ifExpr = dynamic_cast<const IfExpr*>(&expr))
    {
        regionOfExpr(*ifExpr->condition, env, function);
        const RegionInfo thenInfo = regionOfExpr(*ifExpr->thenBranch, env, function);
        const RegionInfo elseInfo = regionOfExpr(*ifExpr->elseBranch, env, function);
        if (thenInfo.kind == Region::Borrowed)
        {
            return thenInfo;
        }
        if (elseInfo.kind == Region::Borrowed)
        {
            return elseInfo;
        }
        return thenInfo;
    }

    if (const auto* block = dynamic_cast<const BlockExpr*>(&expr))
    {
        RegionEnv blockEnv(&env);
        for (const auto& statement : block->statements)
        {
            regionOfStmt(*statement, blockEnv, function);
        }
        if (block->result)
        {
            return regionOfExpr(*block->result, blockEnv, function);
        }
        return RegionInfo{Region::Owned, "", ""};
    }

    return RegionInfo{Region::Owned, "", ""};
}

void RegionChecker::regionOfStmt(const Stmt& stmt, RegionEnv& env, const FunctionDecl& function)
{
    if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(&stmt))
    {
        env.define(assignment->name, regionOfExpr(*assignment->value, env, function));
        return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt))
    {
        if (returnStmt->value)
        {
            requireOwned(regionOfExpr(*returnStmt->value, env, function), function);
        }
        return;
    }

    if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt))
    {
        regionOfExpr(*exprStmt->expr, env, function);
        return;
    }

    if (const auto* fieldAssign = dynamic_cast<const FieldAssignStmt*>(&stmt))
    {
        regionOfExpr(*fieldAssign->object, env, function);
        regionOfExpr(*fieldAssign->value, env, function);
        return;
    }

    if (const auto* incDec = dynamic_cast<const IncDecStmt*>(&stmt))
    {
        regionOfExpr(*incDec->target, env, function);
        return;
    }
}

void RegionChecker::check(
    const Program& program,
    const std::unordered_map<std::string, std::vector<Capability>>& capabilities)
{
    registerDecls(program);

    for (const auto& item : program.items)
    {
        if (const auto* function = dynamic_cast<const FunctionDecl*>(item.get()))
        {
            checkFunction(*function, capabilities.at(function->name));
        }
    }
}

const std::unordered_map<std::string, std::vector<Region>>& RegionChecker::regions() const
{
    return regions_;
}
