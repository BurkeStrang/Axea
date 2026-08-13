#include "sema/CapabilityChecker.hpp"

#include <stdexcept>

std::optional<std::size_t> CapabilityChecker::ownParamIndex(const std::string& name,
                                                            const FunctionDecl& function)
{
    for (std::size_t i = 0; i < function.params.size(); ++i)
    {
        if (function.params[i].name == name)
        {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> CapabilityChecker::rootParamIndex(const Expr& expr,
                                                             const FunctionDecl& function)
{
    const Expr* current = &expr;
    while (const auto* field = dynamic_cast<const FieldExpr*>(current))
    {
        current = field->object.get();
    }
    if (const auto* name = dynamic_cast<const NameExpr*>(current))
    {
        return ownParamIndex(name->name, function);
    }
    return std::nullopt;
}

void CapabilityChecker::raise(const std::string& functionName,
                              std::size_t paramIndex,
                              Capability minimum,
                              bool& changed)
{
    auto& capability = inferred_.at(functionName)[paramIndex];
    if (minimum > capability)
    {
        capability = minimum;
        changed = true;
    }
}

Capability CapabilityChecker::effectiveOrInferred(const std::string& functionName,
                                                  std::size_t paramIndex) const
{
    const auto& declared = functions_.at(functionName)->params[paramIndex].declaredCapability;
    return declared ? *declared : inferred_.at(functionName)[paramIndex];
}

void CapabilityChecker::inferExpr(const Expr& expr, const FunctionDecl& function, bool& changed)
{
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr))
    {
        inferExpr(*binary->left, function, changed);
        inferExpr(*binary->right, function, changed);
        return;
    }

    if (const auto* ifExpr = dynamic_cast<const IfExpr*>(&expr))
    {
        inferExpr(*ifExpr->condition, function, changed);
        inferExpr(*ifExpr->thenBranch, function, changed);
        inferExpr(*ifExpr->elseBranch, function, changed);
        return;
    }

    if (const auto* block = dynamic_cast<const BlockExpr*>(&expr))
    {
        for (const auto& statement : block->statements)
        {
            inferStmt(*statement, function, changed);
        }
        if (block->result)
        {
            inferExpr(*block->result, function, changed);
        }
        return;
    }

    if (const auto* loopExpr = dynamic_cast<const LoopExpr*>(&expr))
    {
        inferExpr(*loopExpr->body, function, changed);
        return;
    }

    if (const auto* field = dynamic_cast<const FieldExpr*>(&expr))
    {
        inferExpr(*field->object, function, changed);
        return;
    }

    if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(&expr))
    {
        for (const auto& [fieldName, valueExpr] : literal->fields)
        {
            inferExpr(*valueExpr, function, changed);
        }
        return;
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        for (const auto& argument : call->arguments)
        {
            inferExpr(*argument, function, changed);
        }

        const auto it = functions_.find(call->callee);
        if (it == functions_.end())
        {
            return;
        }
        for (std::size_t i = 0; i < call->arguments.size() && i < it->second->params.size(); ++i)
        {
            const auto* argName = dynamic_cast<const NameExpr*>(call->arguments[i].get());
            if (!argName)
            {
                continue;
            }
            const auto paramIndex = ownParamIndex(argName->name, function);
            if (!paramIndex)
            {
                continue;
            }
            const Capability calleeCapability = effectiveOrInferred(call->callee, i);
            if (calleeCapability == Capability::Write || calleeCapability == Capability::Take)
            {
                raise(function.name, *paramIndex, calleeCapability, changed);
            }
        }
        return;
    }

    // IntegerExpr, BoolExpr, StringExpr, NameExpr: no sub-expressions, and a
    // bare name reference by itself is a read, which is already the floor.
}

void CapabilityChecker::inferStmt(const Stmt& stmt, const FunctionDecl& function, bool& changed)
{
    if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(&stmt))
    {
        inferExpr(*assignment->value, function, changed);
        return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt))
    {
        if (returnStmt->value)
        {
            inferExpr(*returnStmt->value, function, changed);
        }
        return;
    }

    if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt))
    {
        inferExpr(*exprStmt->expr, function, changed);
        return;
    }

    if (const auto* fieldAssign = dynamic_cast<const FieldAssignStmt*>(&stmt))
    {
        if (const auto paramIndex = rootParamIndex(*fieldAssign->object, function))
        {
            raise(function.name, *paramIndex, Capability::Write, changed);
        }
        inferExpr(*fieldAssign->value, function, changed);
        return;
    }

    if (const auto* incDec = dynamic_cast<const IncDecStmt*>(&stmt))
    {
        // A plain-name target (`n++`) only rebinds the function's own local
        // copy of a primitive and is never observable by the caller, so it
        // doesn't affect the capability contract - only a field target does.
        if (const auto* field = dynamic_cast<const FieldExpr*>(incDec->target.get()))
        {
            if (const auto paramIndex = rootParamIndex(*field->object, function))
            {
                raise(function.name, *paramIndex, Capability::Write, changed);
            }
        }
        return;
    }

    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt))
    {
        inferExpr(*whileStmt->condition, function, changed);
        inferExpr(*whileStmt->body, function, changed);
        return;
    }

    if (const auto* breakStmt = dynamic_cast<const BreakStmt*>(&stmt))
    {
        if (breakStmt->value)
        {
            inferExpr(*breakStmt->value, function, changed);
        }
        return;
    }

    // ContinueStmt: nothing to infer.
}

void CapabilityChecker::checkMovesInExpr(const Expr& expr,
                                         const FunctionDecl& function,
                                         std::unordered_set<std::string>& moved) const
{
    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        if (moved.contains(name->name))
        {
            throw std::runtime_error("use of moved value '" + name->name + "'");
        }
        return;
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr))
    {
        checkMovesInExpr(*binary->left, function, moved);
        checkMovesInExpr(*binary->right, function, moved);
        return;
    }

    if (const auto* field = dynamic_cast<const FieldExpr*>(&expr))
    {
        checkMovesInExpr(*field->object, function, moved);
        return;
    }

    if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(&expr))
    {
        for (const auto& [fieldName, valueExpr] : literal->fields)
        {
            checkMovesInExpr(*valueExpr, function, moved);
        }
        return;
    }

    if (const auto* ifExpr = dynamic_cast<const IfExpr*>(&expr))
    {
        checkMovesInExpr(*ifExpr->condition, function, moved);
        // Each branch tracks moves independently - a move in one branch is
        // not visible to the sibling branch or after the `if` (documented
        // simplification; see docs/language/0009-ownership.md).
        std::unordered_set<std::string> thenMoved;
        checkMovesInExpr(*ifExpr->thenBranch, function, thenMoved);
        std::unordered_set<std::string> elseMoved;
        checkMovesInExpr(*ifExpr->elseBranch, function, elseMoved);
        return;
    }

    if (const auto* loopExpr = dynamic_cast<const LoopExpr*>(&expr))
    {
        // Fresh moved-set for the body, same as an if-branch above - a value
        // moved on one iteration isn't tracked as moved on the next
        // (extends the same already-documented per-block limitation one
        // level further; see docs/language/0028-loops.md).
        std::unordered_set<std::string> loopMoved;
        checkMovesInExpr(*loopExpr->body, function, loopMoved);
        return;
    }

    if (const auto* block = dynamic_cast<const BlockExpr*>(&expr))
    {
        for (const auto& statement : block->statements)
        {
            checkMovesInStmt(*statement, function, moved);
        }
        if (block->result)
        {
            checkMovesInExpr(*block->result, function, moved);
        }
        return;
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        for (const auto& argument : call->arguments)
        {
            checkMovesInExpr(*argument, function, moved);
        }

        const auto it = functions_.find(call->callee);
        if (it == functions_.end())
        {
            return;
        }
        for (std::size_t i = 0; i < call->arguments.size() && i < it->second->params.size(); ++i)
        {
            const auto* argName = dynamic_cast<const NameExpr*>(call->arguments[i].get());
            if (!argName)
            {
                continue;
            }
            if (ownParamIndex(argName->name, function) &&
                effectiveOrInferred(call->callee, i) == Capability::Take)
            {
                moved.insert(argName->name);
            }
        }
        return;
    }

    // IntegerExpr, BoolExpr, StringExpr: no sub-expressions.
}

void CapabilityChecker::checkMovesInStmt(const Stmt& stmt,
                                         const FunctionDecl& function,
                                         std::unordered_set<std::string>& moved) const
{
    if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(&stmt))
    {
        checkMovesInExpr(*assignment->value, function, moved);
        return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt))
    {
        if (returnStmt->value)
        {
            checkMovesInExpr(*returnStmt->value, function, moved);
        }
        return;
    }

    if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt))
    {
        checkMovesInExpr(*exprStmt->expr, function, moved);
        return;
    }

    if (const auto* fieldAssign = dynamic_cast<const FieldAssignStmt*>(&stmt))
    {
        checkMovesInExpr(*fieldAssign->object, function, moved);
        checkMovesInExpr(*fieldAssign->value, function, moved);
        return;
    }

    if (const auto* incDec = dynamic_cast<const IncDecStmt*>(&stmt))
    {
        checkMovesInExpr(*incDec->target, function, moved);
        return;
    }

    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt))
    {
        checkMovesInExpr(*whileStmt->condition, function, moved);
        std::unordered_set<std::string> loopMoved;
        checkMovesInExpr(*whileStmt->body, function, loopMoved);
        return;
    }

    if (const auto* breakStmt = dynamic_cast<const BreakStmt*>(&stmt))
    {
        if (breakStmt->value)
        {
            checkMovesInExpr(*breakStmt->value, function, moved);
        }
        return;
    }

    // ContinueStmt: nothing to check.
}

void CapabilityChecker::check(const Program& program)
{
    for (const auto& item : program.items)
    {
        if (const auto* function = dynamic_cast<const FunctionDecl*>(item.get()))
        {
            functions_[function->name] = function;
            inferred_[function->name] =
                std::vector<Capability>(function->params.size(), Capability::Read);
        }
    }

    // Fixpoint: the 3-level read < write < take lattice only ever increases,
    // so this always converges quickly even with (mutual) recursion.
    bool changed = true;
    std::size_t iterations = 0;
    const std::size_t maxIterations = functions_.size() * 4 + 4;
    while (changed && iterations < maxIterations)
    {
        changed = false;
        for (const auto& [name, function] : functions_)
        {
            inferExpr(*function->body, *function, changed);
        }
        ++iterations;
    }

    for (const auto& [name, function] : functions_)
    {
        std::vector<Capability> resolved;
        resolved.reserve(function->params.size());
        for (std::size_t i = 0; i < function->params.size(); ++i)
        {
            const Capability inferredCapability = inferred_.at(name)[i];
            const auto& declared = function->params[i].declaredCapability;
            if (declared && inferredCapability > *declared)
            {
                throw std::runtime_error("function '" + name + "' requires '" +
                                         std::string(capabilityName(inferredCapability)) +
                                         "' for parameter '" + function->params[i].name +
                                         "' but only '" + std::string(capabilityName(*declared)) +
                                         "' was declared");
            }
            resolved.push_back(declared ? *declared : inferredCapability);
        }
        effective_[name] = std::move(resolved);
    }

    for (const auto& [name, function] : functions_)
    {
        std::unordered_set<std::string> moved;
        checkMovesInExpr(*function->body, *function, moved);
    }
}

const std::unordered_map<std::string, std::vector<Capability>>&
CapabilityChecker::effectiveCapabilities() const
{
    return effective_;
}
