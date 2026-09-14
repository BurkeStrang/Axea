#include "module/UnqualifiedCallResolver.hpp"

#include "ast/Expr.hpp"

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace
{
    void collectCallExprsInExpr(Expr& expr, std::vector<CallExpr*>& calls);
    void collectCallExprsInStmt(Stmt& stmt, std::vector<CallExpr*>& calls);

    void collectCallExprsInBlock(BlockExpr& block, std::vector<CallExpr*>& calls)
    {
        for (auto& stmt : block.statements)
        {
            collectCallExprsInStmt(*stmt, calls);
        }
        if (block.result)
        {
            collectCallExprsInExpr(*block.result, calls);
        }
    }

    // Handles both an ordinary block-nested statement AND a top-level Program item (executable
    // top-level code in the entry file, or a FunctionDecl/ImplDecl declaration wherever one
    // appears) - the same dual role Stmt itself already plays in this grammar.
    void collectCallExprsInStmt(Stmt& stmt, std::vector<CallExpr*>& calls)
    {
        if (auto* s = dynamic_cast<AssignmentStmt*>(&stmt))
        {
            collectCallExprsInExpr(*s->value, calls);
        }
        else if (auto* s = dynamic_cast<ReturnStmt*>(&stmt))
        {
            if (s->value)
            {
                collectCallExprsInExpr(*s->value, calls);
            }
        }
        else if (auto* s = dynamic_cast<WhileStmt*>(&stmt))
        {
            collectCallExprsInExpr(*s->condition, calls);
            collectCallExprsInExpr(*s->body, calls);
        }
        else if (auto* s = dynamic_cast<BreakStmt*>(&stmt))
        {
            if (s->value)
            {
                collectCallExprsInExpr(*s->value, calls);
            }
        }
        else if (auto* s = dynamic_cast<ExprStmt*>(&stmt))
        {
            collectCallExprsInExpr(*s->expr, calls);
        }
        else if (auto* s = dynamic_cast<FieldAssignStmt*>(&stmt))
        {
            collectCallExprsInExpr(*s->object, calls);
            collectCallExprsInExpr(*s->value, calls);
        }
        else if (auto* s = dynamic_cast<IndexAssignStmt*>(&stmt))
        {
            collectCallExprsInExpr(*s->object, calls);
            collectCallExprsInExpr(*s->index, calls);
            collectCallExprsInExpr(*s->value, calls);
        }
        else if (auto* s = dynamic_cast<DerefAssignStmt*>(&stmt))
        {
            collectCallExprsInExpr(*s->pointer, calls);
            collectCallExprsInExpr(*s->value, calls);
        }
        else if (auto* s = dynamic_cast<IncDecStmt*>(&stmt))
        {
            collectCallExprsInExpr(*s->target, calls);
        }
        else if (auto* f = dynamic_cast<FunctionDecl*>(&stmt))
        {
            if (f->body)
            {
                collectCallExprsInExpr(*f->body, calls);
            }
        }
        else if (auto* impl = dynamic_cast<ImplDecl*>(&stmt))
        {
            for (auto& method : impl->methods)
            {
                if (method->body)
                {
                    collectCallExprsInExpr(*method->body, calls);
                }
            }
        }
        // ContinueStmt, TraitDecl, ExternDecl, StructDecl, EnumDecl, ModuleDecl, UseDecl: no
        // nested expression to walk into.
    }

    void collectCallExprsInExpr(Expr& expr, std::vector<CallExpr*>& calls)
    {
        if (auto* e = dynamic_cast<CallExpr*>(&expr))
        {
            for (auto& arg : e->arguments)
            {
                collectCallExprsInExpr(*arg, calls);
            }
            calls.push_back(e);
        }
        else if (auto* e = dynamic_cast<BinaryExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->left, calls);
            collectCallExprsInExpr(*e->right, calls);
        }
        else if (auto* e = dynamic_cast<CastExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->operand, calls);
        }
        else if (auto* e = dynamic_cast<DerefExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->operand, calls);
        }
        else if (auto* e = dynamic_cast<AddressOfExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->operand, calls);
        }
        else if (auto* e = dynamic_cast<UnsafeBlockExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->body, calls);
        }
        else if (auto* e = dynamic_cast<SomeExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->value, calls);
        }
        else if (auto* e = dynamic_cast<ShareExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->value, calls);
        }
        else if (auto* e = dynamic_cast<OkExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->value, calls);
        }
        else if (auto* e = dynamic_cast<ErrExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->value, calls);
        }
        else if (auto* e = dynamic_cast<TryExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->operand, calls);
        }
        else if (auto* e = dynamic_cast<InterpolatedStringExpr*>(&expr))
        {
            for (auto& piece : e->pieces)
            {
                if (piece.expr)
                {
                    collectCallExprsInExpr(*piece.expr, calls);
                }
            }
        }
        else if (auto* e = dynamic_cast<IfExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->condition, calls);
            collectCallExprsInExpr(*e->thenBranch, calls);
            if (e->elseBranch)
            {
                collectCallExprsInExpr(*e->elseBranch, calls);
            }
        }
        else if (auto* e = dynamic_cast<MatchExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->scrutinee, calls);
            for (auto& arm : e->arms)
            {
                if (arm.body)
                {
                    collectCallExprsInExpr(*arm.body, calls);
                }
            }
        }
        else if (auto* e = dynamic_cast<LoopExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->body, calls);
        }
        else if (auto* e = dynamic_cast<FieldExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->object, calls);
        }
        else if (auto* e = dynamic_cast<StructLiteralExpr*>(&expr))
        {
            for (auto& [fieldName, value] : e->fields)
            {
                collectCallExprsInExpr(*value, calls);
            }
        }
        else if (auto* e = dynamic_cast<ArrayLiteralExpr*>(&expr))
        {
            for (auto& element : e->elements)
            {
                collectCallExprsInExpr(*element, calls);
            }
        }
        else if (auto* e = dynamic_cast<IndexExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->object, calls);
            collectCallExprsInExpr(*e->index, calls);
        }
        else if (auto* e = dynamic_cast<StrSliceExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->object, calls);
            if (e->start)
            {
                collectCallExprsInExpr(*e->start, calls);
            }
            if (e->end)
            {
                collectCallExprsInExpr(*e->end, calls);
            }
        }
        else if (auto* e = dynamic_cast<MethodCallExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->object, calls);
            for (auto& arg : e->arguments)
            {
                collectCallExprsInExpr(*arg, calls);
            }
        }
        else if (auto* e = dynamic_cast<StringNewExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->text, calls);
        }
        else if (auto* e = dynamic_cast<BlockExpr*>(&expr))
        {
            collectCallExprsInBlock(*e, calls);
        }
        else if (auto* e = dynamic_cast<ClosureExpr*>(&expr))
        {
            collectCallExprsInExpr(*e->body, calls);
        }
        // IntegerExpr/Int64Expr/FloatExpr/NameExpr/NoneExpr/NullExpr/BoolExpr/StringExpr/
        // CharExpr/MapNewExpr/SortedMapNewExpr/SetNewExpr/LinkedListNewExpr/QueueNewExpr/
        // PriorityQueueNewExpr/SortedSetNewExpr/BufferNewExpr/SizeOfExpr: leaves with no nested
        // expression to walk into.
    }

    // Shared by resolveUnqualifiedCalls and resolveUnqualifiedConstructorCalls - both resolve a
    // bare top-level name (a plain function name for the former, a "new" + StructName
    // constructor name for the latter) the identical "local always wins, then lazy-collision
    // used-module lookup" way.
    struct BareNameIndex
    {
        // A local, entry-file-declared top-level function's own bare name always wins - checked
        // before any candidate lookup, so it's never even a candidate for rewriting.
        std::unordered_set<std::string> localTopLevelNames;
        // Every public, used-module-owned function's own bare name -> its full list of
        // qualified candidates (2+ means ambiguous, checked only once a bare name is actually
        // referenced - see resolveUnqualifiedCalls's own doc comment).
        std::unordered_map<std::string, std::vector<std::string>> candidatesByBareName;
    };

    BareNameIndex buildBareNameIndex(const Program& merged,
                                     const std::vector<std::string>& usedModules)
    {
        BareNameIndex index;
        for (const auto& item : merged.items)
        {
            if (const auto* f = dynamic_cast<const FunctionDecl*>(item.get());
                f && f->name.find('.') == std::string::npos)
            {
                index.localTopLevelNames.insert(f->name);
            }
        }

        const std::unordered_set<std::string> usedModuleSet(usedModules.begin(),
                                                             usedModules.end());
        for (const auto& item : merged.items)
        {
            const auto* f = dynamic_cast<const FunctionDecl*>(item.get());
            if (!f || !f->isPublic)
            {
                continue;
            }
            const auto dot = f->name.find('.');
            if (dot == std::string::npos)
            {
                continue;
            }
            if (!usedModuleSet.contains(f->name.substr(0, dot)))
            {
                continue;
            }
            index.candidatesByBareName[f->name.substr(dot + 1)].push_back(f->name);
        }
        return index;
    }

    // Shared error-message shape for both passes' own ambiguous-candidate case.
    [[noreturn]] void throwAmbiguous(const std::string& bareName,
                                     const std::vector<std::string>& candidates)
    {
        std::string candidatesText;
        for (const auto& candidate : candidates)
        {
            if (!candidatesText.empty())
            {
                candidatesText += ", ";
            }
            candidatesText += candidate;
        }
        throw std::runtime_error("ambiguous unqualified call to '" + bareName + "' - matches " +
                                 candidatesText + "; call it fully qualified instead");
    }
} // namespace

std::vector<CallExpr*> collectAllCallExprs(std::vector<std::unique_ptr<Stmt>>& items)
{
    std::vector<CallExpr*> calls;
    for (auto& item : items)
    {
        collectCallExprsInStmt(*item, calls);
    }
    return calls;
}

void resolveUnqualifiedCalls(Program& merged, const std::vector<std::string>& usedModules)
{
    if (usedModules.empty())
    {
        return;
    }

    const BareNameIndex index = buildBareNameIndex(merged, usedModules);

    std::vector<CallExpr*> calls = collectAllCallExprs(merged.items);

    for (CallExpr* call : calls)
    {
        if (call->callee.find('.') != std::string::npos ||
            index.localTopLevelNames.contains(call->callee))
        {
            continue;
        }
        const auto it = index.candidatesByBareName.find(call->callee);
        if (it == index.candidatesByBareName.end())
        {
            continue;
        }
        if (it->second.size() > 1)
        {
            throwAmbiguous(call->callee, it->second);
        }
        call->callee = it->second.front();
    }
}

void resolveUnqualifiedConstructorCalls(Program& merged,
                                        const std::vector<std::string>& usedModules)
{
    // Unlike resolveUnqualifiedCalls above, this must run even with zero used modules - a
    // purely local `struct Box<T> {...} newBox<T>(...) {...} b = Box<i32>(5)`, no modules
    // involved at all, still needs its bare `Box<i32>(5)` callee rewritten to `newBox<i32>(5)`
    // (a bare struct name is never itself a valid callee - GenericMonomorphizer's own
    // functionTemplatesByName lookup, and every plain function's functions_ registration
    // downstream, only ever know the constructor's own name, never the struct's).
    std::unordered_set<std::string> structNames;
    for (const auto& item : merged.items)
    {
        if (const auto* s = dynamic_cast<const StructDecl*>(item.get()))
        {
            structNames.insert(s->name);
        }
    }
    if (structNames.empty())
    {
        return;
    }

    const BareNameIndex index = buildBareNameIndex(merged, usedModules);

    std::vector<CallExpr*> calls = collectAllCallExprs(merged.items);

    for (CallExpr* call : calls)
    {
        // Already qualified (either originally, or just rewritten by resolveUnqualifiedCalls
        // above) - a real function sharing the struct's own bare name always wins, exactly the
        // same "local/used-module function resolution happens first" precedence
        // resolveUnqualifiedCalls itself already establishes, for free (this pass just never
        // gets a chance to touch a callee that pass or an ordinary local function already
        // claimed).
        if (call->callee.find('.') != std::string::npos ||
            !structNames.contains(call->callee) ||
            index.localTopLevelNames.contains(call->callee))
        {
            continue;
        }
        const std::string conventionalName = "new" + call->callee;
        if (index.localTopLevelNames.contains(conventionalName))
        {
            call->callee = conventionalName;
            continue;
        }
        const auto it = index.candidatesByBareName.find(conventionalName);
        if (it == index.candidatesByBareName.end())
        {
            // No matching constructor anywhere - left untouched. GenericMonomorphizer's own
            // "'<Name>' is not a known generic function" (or the plain-call analogue) surfaces
            // downstream unchanged, same "zero candidates, no-op" philosophy
            // resolveUnqualifiedCalls above already uses.
            continue;
        }
        if (it->second.size() > 1)
        {
            throwAmbiguous(conventionalName, it->second);
        }
        call->callee = it->second.front();
    }
}
