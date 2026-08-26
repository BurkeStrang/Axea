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
    while (true)
    {
        if (const auto* field = dynamic_cast<const FieldExpr*>(current))
        {
            current = field->object.get();
            continue;
        }
        if (const auto* index = dynamic_cast<const IndexExpr*>(current))
        {
            current = index->object.get();
            continue;
        }
        break;
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

void CapabilityChecker::collectReferencedNames(const Expr& expr,
                                               std::unordered_set<std::string>& names)
{
    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        names.insert(name->name);
        return;
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr))
    {
        collectReferencedNames(*binary->left, names);
        collectReferencedNames(*binary->right, names);
        return;
    }
    if (const auto* cast = dynamic_cast<const CastExpr*>(&expr))
    {
        collectReferencedNames(*cast->operand, names);
        return;
    }
    if (const auto* someExpr = dynamic_cast<const SomeExpr*>(&expr))
    {
        collectReferencedNames(*someExpr->value, names);
        return;
    }
    if (const auto* shareExpr = dynamic_cast<const ShareExpr*>(&expr))
    {
        collectReferencedNames(*shareExpr->value, names);
        return;
    }
    if (const auto* okExpr = dynamic_cast<const OkExpr*>(&expr))
    {
        collectReferencedNames(*okExpr->value, names);
        return;
    }
    if (const auto* errExpr = dynamic_cast<const ErrExpr*>(&expr))
    {
        collectReferencedNames(*errExpr->value, names);
        return;
    }
    if (const auto* tryExpr = dynamic_cast<const TryExpr*>(&expr))
    {
        collectReferencedNames(*tryExpr->operand, names);
        return;
    }
    if (const auto* field = dynamic_cast<const FieldExpr*>(&expr))
    {
        collectReferencedNames(*field->object, names);
        return;
    }
    if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(&expr))
    {
        for (const auto& [fieldName, valueExpr] : literal->fields)
        {
            collectReferencedNames(*valueExpr, names);
        }
        return;
    }
    if (const auto* arrayLiteral = dynamic_cast<const ArrayLiteralExpr*>(&expr))
    {
        for (const auto& element : arrayLiteral->elements)
        {
            collectReferencedNames(*element, names);
        }
        return;
    }
    if (const auto* index = dynamic_cast<const IndexExpr*>(&expr))
    {
        collectReferencedNames(*index->object, names);
        collectReferencedNames(*index->index, names);
        return;
    }
    if (const auto* interpolated = dynamic_cast<const InterpolatedStringExpr*>(&expr))
    {
        for (const auto& piece : interpolated->pieces)
        {
            if (piece.expr)
            {
                collectReferencedNames(*piece.expr, names);
            }
        }
        return;
    }
    if (const auto* strSlice = dynamic_cast<const StrSliceExpr*>(&expr))
    {
        collectReferencedNames(*strSlice->object, names);
        if (strSlice->start)
        {
            collectReferencedNames(*strSlice->start, names);
        }
        if (strSlice->end)
        {
            collectReferencedNames(*strSlice->end, names);
        }
        return;
    }
    if (const auto* ifExpr = dynamic_cast<const IfExpr*>(&expr))
    {
        collectReferencedNames(*ifExpr->condition, names);
        collectReferencedNames(*ifExpr->thenBranch, names);
        collectReferencedNames(*ifExpr->elseBranch, names);
        return;
    }
    if (const auto* matchExpr = dynamic_cast<const MatchExpr*>(&expr))
    {
        collectReferencedNames(*matchExpr->scrutinee, names);
        for (const auto& arm : matchExpr->arms)
        {
            collectReferencedNames(*arm.body, names);
        }
        return;
    }
    if (const auto* loopExpr = dynamic_cast<const LoopExpr*>(&expr))
    {
        collectReferencedNames(*loopExpr->body, names);
        return;
    }
    if (const auto* block = dynamic_cast<const BlockExpr*>(&expr))
    {
        for (const auto& statement : block->statements)
        {
            collectReferencedNames(*statement, names);
        }
        if (block->result)
        {
            collectReferencedNames(*block->result, names);
        }
        return;
    }
    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        for (const auto& argument : call->arguments)
        {
            collectReferencedNames(*argument, names);
        }
        return;
    }
    if (const auto* methodCall = dynamic_cast<const MethodCallExpr*>(&expr))
    {
        collectReferencedNames(*methodCall->object, names);
        for (const auto& argument : methodCall->arguments)
        {
            collectReferencedNames(*argument, names);
        }
        return;
    }
    if (const auto* closureExpr = dynamic_cast<const ClosureExpr*>(&expr))
    {
        // A nested closure - collect its own free variables too (a closure declared inside
        // another closure can still reference the outermost enclosing function's own locals),
        // then subtract *its* own params the same way the outer caller will for this one.
        std::unordered_set<std::string> nested;
        collectReferencedNames(*closureExpr->body, nested);
        for (const auto& param : closureExpr->params)
        {
            nested.erase(param.name);
        }
        names.insert(nested.begin(), nested.end());
        return;
    }

    // IntegerExpr, Int64Expr, FloatExpr, BoolExpr, StringExpr, CharExpr: no sub-expressions.
}

void CapabilityChecker::collectReferencedNames(const Stmt& stmt,
                                               std::unordered_set<std::string>& names)
{
    if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(&stmt))
    {
        collectReferencedNames(*assignment->value, names);
        return;
    }
    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt))
    {
        if (returnStmt->value)
        {
            collectReferencedNames(*returnStmt->value, names);
        }
        return;
    }
    if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt))
    {
        collectReferencedNames(*exprStmt->expr, names);
        return;
    }
    if (const auto* fieldAssign = dynamic_cast<const FieldAssignStmt*>(&stmt))
    {
        collectReferencedNames(*fieldAssign->object, names);
        collectReferencedNames(*fieldAssign->value, names);
        return;
    }
    if (const auto* indexAssign = dynamic_cast<const IndexAssignStmt*>(&stmt))
    {
        collectReferencedNames(*indexAssign->object, names);
        collectReferencedNames(*indexAssign->index, names);
        collectReferencedNames(*indexAssign->value, names);
        return;
    }
    if (const auto* incDec = dynamic_cast<const IncDecStmt*>(&stmt))
    {
        collectReferencedNames(*incDec->target, names);
        return;
    }
    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt))
    {
        collectReferencedNames(*whileStmt->condition, names);
        collectReferencedNames(*whileStmt->body, names);
        return;
    }
    if (const auto* breakStmt = dynamic_cast<const BreakStmt*>(&stmt))
    {
        if (breakStmt->value)
        {
            collectReferencedNames(*breakStmt->value, names);
        }
        return;
    }
    // ContinueStmt: nothing to collect.
}

void CapabilityChecker::inferExpr(const Expr& expr, const FunctionDecl& function, bool& changed)
{
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr))
    {
        inferExpr(*binary->left, function, changed);
        inferExpr(*binary->right, function, changed);
        return;
    }

    if (const auto* cast = dynamic_cast<const CastExpr*>(&expr))
    {
        inferExpr(*cast->operand, function, changed);
        return;
    }

    if (const auto* someExpr = dynamic_cast<const SomeExpr*>(&expr))
    {
        inferExpr(*someExpr->value, function, changed);
        return;
    }

    if (const auto* shareExpr = dynamic_cast<const ShareExpr*>(&expr))
    {
        inferExpr(*shareExpr->value, function, changed);
        return;
    }

    // Ok(value)/Err(value) (see docs/language/0063-result.md) - same
    // one-line recursive shape as SomeExpr above.
    if (const auto* okExpr = dynamic_cast<const OkExpr*>(&expr))
    {
        inferExpr(*okExpr->value, function, changed);
        return;
    }

    if (const auto* errExpr = dynamic_cast<const ErrExpr*>(&expr))
    {
        inferExpr(*errExpr->value, function, changed);
        return;
    }

    if (const auto* tryExpr = dynamic_cast<const TryExpr*>(&expr))
    {
        inferExpr(*tryExpr->operand, function, changed);
        return;
    }

    // NoneExpr: no sub-expressions, no capability effect.

    if (const auto* ifExpr = dynamic_cast<const IfExpr*>(&expr))
    {
        inferExpr(*ifExpr->condition, function, changed);
        inferExpr(*ifExpr->thenBranch, function, changed);
        inferExpr(*ifExpr->elseBranch, function, changed);
        return;
    }

    // `match` (see docs/language/0064-enums.md) - recurses into the scrutinee and every arm's
    // own body, the same "walk every sub-expression" shape IfExpr's own condition/branches get
    // just above. A match-bound name (a variant's own extracted payload) is a fresh local, not
    // a parameter reference, so it needs no capability propagation of its own - the same
    // "extracted value, not itself tracked" treatment a struct field-get's own result already
    // gets.
    if (const auto* matchExpr = dynamic_cast<const MatchExpr*>(&expr))
    {
        inferExpr(*matchExpr->scrutinee, function, changed);
        for (const auto& arm : matchExpr->arms)
        {
            inferExpr(*arm.body, function, changed);
        }
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

    if (const auto* arrayLiteral = dynamic_cast<const ArrayLiteralExpr*>(&expr))
    {
        for (const auto& element : arrayLiteral->elements)
        {
            inferExpr(*element, function, changed);
        }
        return;
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(&expr))
    {
        inferExpr(*index->object, function, changed);
        inferExpr(*index->index, function, changed);
        return;
    }

    if (const auto* strSlice = dynamic_cast<const StrSliceExpr*>(&expr))
    {
        // Read-only, same as IndexExpr above - produces a fresh str, never
        // mutates `object` (see docs/language/0045-str-slicing.md).
        // start/end recurse purely so a mutating expression nested inside
        // one of them (unusual, but not disallowed - e.g. a call with a
        // write-capability argument) is still detected.
        inferExpr(*strSlice->object, function, changed);
        if (strSlice->start)
        {
            inferExpr(*strSlice->start, function, changed);
        }
        if (strSlice->end)
        {
            inferExpr(*strSlice->end, function, changed);
        }
        return;
    }

    if (const auto* interpolated = dynamic_cast<const InterpolatedStringExpr*>(&expr))
    {
        // Read-only, same reasoning as StrSliceExpr above - building a
        // fresh String never mutates any of its own pieces' expressions
        // (see docs/language/Axea_Printing_Formatting.md). Recurses
        // purely for the same nested-mutation-detection reason.
        for (const auto& piece : interpolated->pieces)
        {
            if (piece.expr)
            {
                inferExpr(*piece.expr, function, changed);
            }
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

    if (const auto* methodCall = dynamic_cast<const MethodCallExpr*>(&expr))
    {
        inferExpr(*methodCall->object, function, changed);
        for (const auto& argument : methodCall->arguments)
        {
            inferExpr(*argument, function, changed);
        }
        // "push"/"pop" (List, docs/language/0033-lists.md), "set"/"remove"
        // (Map), "add"/"remove" (Set, docs/language/0034-maps-and-sets.md),
        // "push_front"/"push_back"/"pop_front"/"pop_back" (LinkedList/Deque,
        // docs/language/0036-linked-lists.md, docs/language/0037-deques.md),
        // "enqueue"/"dequeue" (Queue, docs/language/0038-queues.md),
        // "append" (String, docs/language/0042-string.md), and
        // "append_line"/"write"/"clear"/"reserve"/"finish" (Buffer,
        // docs/language/0043-buffer.md, docs/language/0061-buffer-write.md -
        // "finish" mutates too, resetting the buffer's own header fields
        // even though its main purpose is to return a value, the same
        // "mutates *and* returns" shape List<T>.pop() already established)
        // all mutate the receiver's own header fields in place; "get"/
        // "contains" don't. Mirrors IndexAssignStmt/FieldAssignStmt raising
        // Write on their own object below.
        if (methodCall->method == "push" || methodCall->method == "pop" ||
            methodCall->method == "set" || methodCall->method == "remove" ||
            methodCall->method == "add" || methodCall->method == "push_front" ||
            methodCall->method == "push_back" || methodCall->method == "pop_front" ||
            methodCall->method == "pop_back" || methodCall->method == "enqueue" ||
            methodCall->method == "dequeue" || methodCall->method == "append" ||
            methodCall->method == "append_line" || methodCall->method == "write" ||
            methodCall->method == "clear" || methodCall->method == "reserve" ||
            methodCall->method == "finish")
        {
            if (const auto paramIndex = rootParamIndex(*methodCall->object, function))
            {
                raise(function.name, *paramIndex, Capability::Write, changed);
            }
        }
        return;
    }

    if (const auto* closureExpr = dynamic_cast<const ClosureExpr*>(&expr))
    {
        // A closure captures by move (see docs/language/0067-closures.md), never by reference -
        // so unlike a plain nested call (whose capability depends on what the *callee* does with
        // an argument), a captured struct-typed param of the *enclosing* function unconditionally
        // needs Capability::Take: the enclosing function is done with it the moment it's
        // captured, regardless of what the closure body subsequently does with its own copy.
        std::unordered_set<std::string> referenced;
        collectReferencedNames(*closureExpr->body, referenced);
        for (const auto& param : closureExpr->params)
        {
            referenced.erase(param.name);
        }
        for (const auto& capturedName : referenced)
        {
            if (const auto paramIndex = ownParamIndex(capturedName, function))
            {
                raise(function.name, *paramIndex, Capability::Take, changed);
            }
        }

        // Struct-typed closure *parameters* (as opposed to captures, handled just above) - see
        // registerClosure's own comment for why this reuses the exact same per-function walk a
        // real top-level FunctionDecl's own body already gets.
        const FunctionDecl& syntheticFn = registerClosure(*closureExpr);
        inferExpr(*closureExpr->body, syntheticFn, changed);
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

    if (const auto* indexAssign = dynamic_cast<const IndexAssignStmt*>(&stmt))
    {
        if (const auto paramIndex = rootParamIndex(*indexAssign->object, function))
        {
            raise(function.name, *paramIndex, Capability::Write, changed);
        }
        inferExpr(*indexAssign->index, function, changed);
        inferExpr(*indexAssign->value, function, changed);
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

    if (const auto* cast = dynamic_cast<const CastExpr*>(&expr))
    {
        checkMovesInExpr(*cast->operand, function, moved);
        return;
    }

    if (const auto* someExpr = dynamic_cast<const SomeExpr*>(&expr))
    {
        checkMovesInExpr(*someExpr->value, function, moved);
        return;
    }

    if (const auto* shareExpr = dynamic_cast<const ShareExpr*>(&expr))
    {
        checkMovesInExpr(*shareExpr->value, function, moved);
        return;
    }

    if (const auto* okExpr = dynamic_cast<const OkExpr*>(&expr))
    {
        checkMovesInExpr(*okExpr->value, function, moved);
        return;
    }

    if (const auto* errExpr = dynamic_cast<const ErrExpr*>(&expr))
    {
        checkMovesInExpr(*errExpr->value, function, moved);
        return;
    }

    if (const auto* tryExpr = dynamic_cast<const TryExpr*>(&expr))
    {
        checkMovesInExpr(*tryExpr->operand, function, moved);
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

    if (const auto* arrayLiteral = dynamic_cast<const ArrayLiteralExpr*>(&expr))
    {
        for (const auto& element : arrayLiteral->elements)
        {
            checkMovesInExpr(*element, function, moved);
        }
        return;
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(&expr))
    {
        checkMovesInExpr(*index->object, function, moved);
        checkMovesInExpr(*index->index, function, moved);
        return;
    }

    if (const auto* interpolated = dynamic_cast<const InterpolatedStringExpr*>(&expr))
    {
        for (const auto& piece : interpolated->pieces)
        {
            if (piece.expr)
            {
                checkMovesInExpr(*piece.expr, function, moved);
            }
        }
        return;
    }

    if (const auto* strSlice = dynamic_cast<const StrSliceExpr*>(&expr))
    {
        checkMovesInExpr(*strSlice->object, function, moved);
        if (strSlice->start)
        {
            checkMovesInExpr(*strSlice->start, function, moved);
        }
        if (strSlice->end)
        {
            checkMovesInExpr(*strSlice->end, function, moved);
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

    // `match` (see docs/language/0064-enums.md) - same per-branch independent move tracking as
    // IfExpr's own thenBranch/elseBranch just above, one `moved` set per arm.
    if (const auto* matchExpr = dynamic_cast<const MatchExpr*>(&expr))
    {
        checkMovesInExpr(*matchExpr->scrutinee, function, moved);
        for (const auto& arm : matchExpr->arms)
        {
            std::unordered_set<std::string> armMoved;
            checkMovesInExpr(*arm.body, function, armMoved);
        }
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

    if (const auto* methodCall = dynamic_cast<const MethodCallExpr*>(&expr))
    {
        checkMovesInExpr(*methodCall->object, function, moved);
        for (const auto& argument : methodCall->arguments)
        {
            checkMovesInExpr(*argument, function, moved);
        }
        return;
    }

    if (const auto* closureExpr = dynamic_cast<const ClosureExpr*>(&expr))
    {
        // Move-only capture (see docs/language/0067-closures.md and inferExpr's own identical
        // reasoning) - every enclosing-scope name the closure body references (minus its own
        // params) is moved out of the enclosing function's scope at the point the closure
        // literal is evaluated, exactly like a `take`-declared call argument already is.
        std::unordered_set<std::string> referenced;
        collectReferencedNames(*closureExpr->body, referenced);
        for (const auto& param : closureExpr->params)
        {
            referenced.erase(param.name);
        }
        for (const auto& capturedName : referenced)
        {
            if (moved.contains(capturedName))
            {
                throw std::runtime_error("use of moved value '" + capturedName + "'");
            }
            moved.insert(capturedName);
        }

        // Struct-typed closure parameters (see docs/language/0067-closures.md) - a fresh
        // `moved` set of its own (a closure's own params are fresh bindings, unrelated to the
        // enclosing function's own moved-set), against the same synthetic FunctionDecl
        // inferExpr's own fixpoint pass already minted for this literal - guaranteed already
        // registered by the time this runs, since check()'s own checkMovesInExpr driver loop
        // always runs after the fixpoint loop has fully finished.
        const FunctionDecl& syntheticFn = *closureFunctions_.at(closureExpr);
        std::unordered_set<std::string> closureMoved;
        checkMovesInExpr(*closureExpr->body, syntheticFn, closureMoved);
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

    if (const auto* indexAssign = dynamic_cast<const IndexAssignStmt*>(&stmt))
    {
        checkMovesInExpr(*indexAssign->object, function, moved);
        checkMovesInExpr(*indexAssign->index, function, moved);
        checkMovesInExpr(*indexAssign->value, function, moved);
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

const FunctionDecl& CapabilityChecker::registerClosure(const ClosureExpr& closureExpr)
{
    if (const auto it = closureFunctions_.find(&closureExpr); it != closureFunctions_.end())
    {
        return *it->second;
    }
    const std::string syntheticName = "closure$" + std::to_string(closureCounter_++);
    auto syntheticFn = std::make_unique<FunctionDecl>(
        syntheticName, closureExpr.params, closureExpr.returnType, nullptr);
    functions_[syntheticName] = syntheticFn.get();
    inferred_[syntheticName] = std::vector<Capability>(closureExpr.params.size(), Capability::Read);
    const FunctionDecl& ref = *syntheticFn;
    closureFunctions_[&closureExpr] = std::move(syntheticFn);
    return ref;
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
        else if (const auto* implDecl = dynamic_cast<const ImplDecl*>(item.get()))
        {
            // Each impl method (see docs/language/0062-display-trait.md)
            // is registered exactly like a top-level FunctionDecl - this
            // is what makes `self`/`buf`'s own capabilities genuinely
            // *inferred* (no explicit read/write prefix on either param
            // in `format(self, buf: Buffer)`), the same fixpoint
            // inference every other unprefixed parameter in the language
            // already gets.
            for (const auto& method : implDecl->methods)
            {
                functions_[method->name] = method.get();
                inferred_[method->name] =
                    std::vector<Capability>(method->params.size(), Capability::Read);
            }
        }
    }

    // Fixpoint: the 3-level read < write < take lattice only ever increases,
    // so this always converges quickly even with (mutual) recursion.
    bool changed = true;
    std::size_t iterations = 0;
    while (changed && iterations < functions_.size() * 4 + 4)
    {
        changed = false;
        // A snapshot, not a live iteration of functions_ itself - inferExpr's own ClosureExpr
        // case (registerClosure) inserts a *new* entry into functions_/inferred_ the first time
        // each closure literal is discovered, which may happen from anywhere inside this very
        // pass (including nested inside another closure discovered earlier in the *same* pass);
        // std::unordered_map iteration is undefined behavior once the map being iterated gets a
        // new key inserted mid-walk. Snapshotting into an ordinary std::vector first sidesteps
        // that entirely - insertions during this pass just extend functions_ for the *next*
        // std::unordered_map read after this pass finishes, never invalidating the snapshot
        // being walked right now. (The bound above reads functions_.size() before any closure is
        // discovered, same as before this phase - a slight undercount once closures exist, but
        // still a generous fixpoint bound, not a correctness requirement.)
        std::vector<const FunctionDecl*> snapshot;
        snapshot.reserve(functions_.size());
        for (const auto& [name, function] : functions_)
        {
            snapshot.push_back(function);
        }
        for (const FunctionDecl* function : snapshot)
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

    // Struct-typed closure parameters (see docs/language/0067-closures.md and registerClosure) -
    // every closure literal discovered above already has its own resolved entry in effective_
    // (keyed by its synthetic name, just like any real function); this just re-exposes that same
    // data keyed by the literal's own identity instead, the form IrGenerator/RegionChecker can
    // actually look it up by.
    for (const auto& [closureExpr, syntheticFn] : closureFunctions_)
    {
        closureEffective_[closureExpr] = effective_.at(syntheticFn->name);
    }

    for (const auto& [name, function] : functions_)
    {
        // Skip a synthetic closure entry here (its own `body` is always null - see
        // registerClosure) - its own move-check already happens via checkMovesInExpr's own
        // ClosureExpr case, reached while walking whatever *real* function's body the closure
        // literal actually lives inside of.
        if (!function->body)
        {
            continue;
        }
        std::unordered_set<std::string> moved;
        checkMovesInExpr(*function->body, *function, moved);
    }
}

const std::unordered_map<std::string, std::vector<Capability>>&
CapabilityChecker::effectiveCapabilities() const
{
    return effective_;
}

const std::unordered_map<const ClosureExpr*, std::vector<Capability>>&
CapabilityChecker::closureEffectiveCapabilities() const
{
    return closureEffective_;
}
