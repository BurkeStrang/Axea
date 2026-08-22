#include "ir/IrGenerator.hpp"

#include <stdexcept>

namespace
{
    // True if every path through this straight-line instruction list is
    // guaranteed to hit a Return - either directly, or via a Branch whose
    // thenBlock and elseBlock both alwaysTerminate. Mirrors
    // TypeChecker::definitelyReturns (same shape, over lowered IR instead of
    // AST - kept as a separate, pure implementation per this codebase's
    // convention of each pass owning its own walk). Used so generateFunction
    // never appends a second terminator after a body that's already fully
    // covered by explicit `return`s (docs/language/0027-explicit-return.md).
    bool alwaysTerminates(const std::vector<std::unique_ptr<IrInst>>& instructions)
    {
        for (const auto& inst : instructions)
        {
            if (dynamic_cast<const IrReturn*>(inst.get()))
            {
                return true;
            }
            if (const auto* branch = dynamic_cast<const IrBranch*>(inst.get());
                branch && alwaysTerminates(branch->thenBlock) &&
                alwaysTerminates(branch->elseBlock))
            {
                return true;
            }
        }
        return false;
    }

    // "Optional<T>" -> "T" (see docs/language/0052-optional.md) - the
    // canonical form Parser::parseTypeName/TypeChecker::typeName always
    // produce, mirroring the substr-based unwrapping every generic type
    // string elsewhere in this codebase already uses (e.g.
    // LlvmIrEmitter::llvmType's own "slice<elem>" case). Used only for a
    // bare `None`, which - unlike Some(x) - carries no expression to infer
    // its payload type from at IrGenerator level (which keeps no real type
    // table by design), so its payload type is read directly out of the
    // declared/expected type string already present in the AST instead.
    std::string optionalPayloadTypeName(const std::string& optionalTypeName)
    {
        return optionalTypeName.substr(9, optionalTypeName.size() - 10);
    }
} // namespace

IrScope::IrScope(IrScope* parent, bool isBarrier)
    : parent_(parent),
      isBarrier_(isBarrier)
{
}

void IrScope::define(const std::string& name, int registerId)
{
    registers_[name] = registerId;
}

void IrScope::assign(const std::string& name, int registerId)
{
    if (const auto it = registers_.find(name); it != registers_.end())
    {
        it->second = registerId;
        return;
    }
    if (parent_ && !isBarrier_)
    {
        parent_->assign(name, registerId);
        return;
    }
    if (!parent_)
    {
        throw std::runtime_error("undefined variable: " + name);
    }
    // Barrier: don't let the mutation escape into the (possibly-not-taken)
    // sibling branch or the shared enclosing scope. Define it locally
    // instead - subsequent code within this same branch sees the update.
    registers_[name] = registerId;
}

int IrScope::find(const std::string& name) const
{
    if (const auto it = registers_.find(name); it != registers_.end())
    {
        return it->second;
    }
    if (parent_)
    {
        return parent_->find(name);
    }
    throw std::runtime_error("undefined variable: " + name);
}

bool IrScope::contains(const std::string& name) const
{
    if (registers_.contains(name))
    {
        return true;
    }
    return parent_ && parent_->contains(name);
}

std::unordered_map<std::string, int> IrScope::snapshot() const
{
    std::unordered_map<std::string, int> result;
    if (parent_)
    {
        result = parent_->snapshot();
    }
    for (const auto& [name, registerId] : registers_)
    {
        result[name] = registerId; // inner scope's own binding shadows the parent's
    }
    return result;
}

void IrScope::defineArrayLength(const std::string& name, int length)
{
    arrayLengths_[name] = length;
}

std::optional<int> IrScope::findArrayLength(const std::string& name) const
{
    if (const auto it = arrayLengths_.find(name); it != arrayLengths_.end())
    {
        return it->second;
    }
    return parent_ ? parent_->findArrayLength(name) : std::nullopt;
}

void IrScope::defineIsSet(const std::string& name, bool isSet)
{
    isSetKinds_[name] = isSet;
}

std::optional<bool> IrScope::findIsSet(const std::string& name) const
{
    if (const auto it = isSetKinds_.find(name); it != isSetKinds_.end())
    {
        return it->second;
    }
    return parent_ ? parent_->findIsSet(name) : std::nullopt;
}

void IrScope::defineIsStack(const std::string& name, bool isStack)
{
    isStackKinds_[name] = isStack;
}

std::optional<bool> IrScope::findIsStack(const std::string& name) const
{
    if (const auto it = isStackKinds_.find(name); it != isStackKinds_.end())
    {
        return it->second;
    }
    return parent_ ? parent_->findIsStack(name) : std::nullopt;
}

void IrScope::defineIsDeque(const std::string& name, bool isDeque)
{
    isDequeKinds_[name] = isDeque;
}

std::optional<bool> IrScope::findIsDeque(const std::string& name) const
{
    if (const auto it = isDequeKinds_.find(name); it != isDequeKinds_.end())
    {
        return it->second;
    }
    return parent_ ? parent_->findIsDeque(name) : std::nullopt;
}

void IrScope::defineIsPriorityQueue(const std::string& name, bool isPriorityQueue)
{
    isPriorityQueueKinds_[name] = isPriorityQueue;
}

std::optional<bool> IrScope::findIsPriorityQueue(const std::string& name) const
{
    if (const auto it = isPriorityQueueKinds_.find(name); it != isPriorityQueueKinds_.end())
    {
        return it->second;
    }
    return parent_ ? parent_->findIsPriorityQueue(name) : std::nullopt;
}

void IrScope::defineIsSortedMap(const std::string& name, bool isSortedMap)
{
    isSortedMapKinds_[name] = isSortedMap;
}

std::optional<bool> IrScope::findIsSortedMap(const std::string& name) const
{
    if (const auto it = isSortedMapKinds_.find(name); it != isSortedMapKinds_.end())
    {
        return it->second;
    }
    return parent_ ? parent_->findIsSortedMap(name) : std::nullopt;
}

void IrScope::defineIsSortedSet(const std::string& name, bool isSortedSet)
{
    isSortedSetKinds_[name] = isSortedSet;
}

std::optional<bool> IrScope::findIsSortedSet(const std::string& name) const
{
    if (const auto it = isSortedSetKinds_.find(name); it != isSortedSetKinds_.end())
    {
        return it->second;
    }
    return parent_ ? parent_->findIsSortedSet(name) : std::nullopt;
}

void IrScope::defineIsBuffer(const std::string& name, bool isBuffer)
{
    isBufferKinds_[name] = isBuffer;
}

std::optional<bool> IrScope::findIsBuffer(const std::string& name) const
{
    if (const auto it = isBufferKinds_.find(name); it != isBufferKinds_.end())
    {
        return it->second;
    }
    return parent_ ? parent_->findIsBuffer(name) : std::nullopt;
}

int IrGenerator::freshRegister(Context& ctx)
{
    return (*ctx.registerCount)++;
}

int IrGenerator::emit(Context& ctx, std::unique_ptr<IrInst> inst)
{
    const int dest = freshRegister(ctx);
    inst->dest = dest;
    ctx.out->push_back(std::move(inst));
    return dest;
}

void IrGenerator::emitVoid(Context& ctx, std::unique_ptr<IrInst> inst)
{
    ctx.out->push_back(std::move(inst));
}

void IrGenerator::registerStructs(const Program& program)
{
    for (const auto& item : program.items)
    {
        if (const auto* structDecl = dynamic_cast<const StructDecl*>(item.get()))
        {
            structs_[structDecl->name] = structDecl;
        }
        // Only the return type is needed here (see isSetExpr) - not a
        // general function table, so this stays folded into registerStructs
        // rather than becoming its own pass.
        if (const auto* function = dynamic_cast<const FunctionDecl*>(item.get()))
        {
            functions_[function->name] = function;
        }
    }
}

bool IrGenerator::isObviouslyStructTyped(const Expr& expr, const FunctionDecl& function) const
{
    if (dynamic_cast<const StructLiteralExpr*>(&expr))
    {
        return true;
    }
    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        for (const auto& param : function.params)
        {
            if (param.name == name->name)
            {
                return structs_.contains(param.type);
            }
        }
    }
    return false;
}

std::optional<int> IrGenerator::arrayLengthOf(const Expr& expr,
                                              const FunctionDecl* function,
                                              const IrScope& scope) const
{
    if (const auto* arrayLiteral = dynamic_cast<const ArrayLiteralExpr*>(&expr))
    {
        return static_cast<int>(arrayLiteral->elements.size());
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        if (function)
        {
            for (const auto& param : function->params)
            {
                if (param.name == name->name && !param.type.empty() && param.type.front() == '[')
                {
                    // "[elem;N]" - the canonical, no-spaces form
                    // Parser::parseTypeName always produces.
                    const auto semicolon = param.type.find(';');
                    const auto closeBracket = param.type.rfind(']');
                    return std::stoi(
                        param.type.substr(semicolon + 1, closeBracket - semicolon - 1));
                }
            }
        }
        return scope.findArrayLength(name->name);
    }

    return std::nullopt;
}

std::optional<bool>
IrGenerator::isSetExpr(const Expr& expr, const FunctionDecl* function, const IrScope& scope) const
{
    if (dynamic_cast<const SetNewExpr*>(&expr))
    {
        return true;
    }
    if (dynamic_cast<const MapNewExpr*>(&expr))
    {
        return false;
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        if (function)
        {
            for (const auto& param : function->params)
            {
                if (param.name == name->name)
                {
                    if (param.type.starts_with("Set<"))
                    {
                        return true;
                    }
                    if (param.type.starts_with("Map<"))
                    {
                        return false;
                    }
                }
            }
        }
        return scope.findIsSet(name->name);
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        const auto it = functions_.find(call->callee);
        if (it != functions_.end() && it->second->returnType)
        {
            if (it->second->returnType->starts_with("Set<"))
            {
                return true;
            }
            if (it->second->returnType->starts_with("Map<"))
            {
                return false;
            }
        }
    }

    return std::nullopt;
}

std::optional<bool>
IrGenerator::isStackExpr(const Expr& expr, const FunctionDecl* function, const IrScope& scope) const
{
    if (dynamic_cast<const StackNewExpr*>(&expr))
    {
        return true;
    }
    if (dynamic_cast<const ListNewExpr*>(&expr))
    {
        return false;
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        if (function)
        {
            for (const auto& param : function->params)
            {
                if (param.name == name->name)
                {
                    if (param.type.starts_with("Stack<"))
                    {
                        return true;
                    }
                    if (param.type.starts_with("List<"))
                    {
                        return false;
                    }
                }
            }
        }
        return scope.findIsStack(name->name);
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        const auto it = functions_.find(call->callee);
        if (it != functions_.end() && it->second->returnType)
        {
            if (it->second->returnType->starts_with("Stack<"))
            {
                return true;
            }
            if (it->second->returnType->starts_with("List<"))
            {
                return false;
            }
        }
    }

    return std::nullopt;
}

std::optional<bool>
IrGenerator::isDequeExpr(const Expr& expr, const FunctionDecl* function, const IrScope& scope) const
{
    if (dynamic_cast<const DequeNewExpr*>(&expr))
    {
        return true;
    }
    if (dynamic_cast<const LinkedListNewExpr*>(&expr))
    {
        return false;
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        if (function)
        {
            for (const auto& param : function->params)
            {
                if (param.name == name->name)
                {
                    if (param.type.starts_with("Deque<"))
                    {
                        return true;
                    }
                    if (param.type.starts_with("LinkedList<"))
                    {
                        return false;
                    }
                }
            }
        }
        return scope.findIsDeque(name->name);
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        const auto it = functions_.find(call->callee);
        if (it != functions_.end() && it->second->returnType)
        {
            if (it->second->returnType->starts_with("Deque<"))
            {
                return true;
            }
            if (it->second->returnType->starts_with("LinkedList<"))
            {
                return false;
            }
        }
    }

    return std::nullopt;
}

std::optional<bool> IrGenerator::isPriorityQueueExpr(const Expr& expr,
                                                     const FunctionDecl* function,
                                                     const IrScope& scope) const
{
    if (dynamic_cast<const PriorityQueueNewExpr*>(&expr))
    {
        return true;
    }
    if (dynamic_cast<const StackNewExpr*>(&expr) || dynamic_cast<const ListNewExpr*>(&expr))
    {
        return false;
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        if (function)
        {
            for (const auto& param : function->params)
            {
                if (param.name == name->name)
                {
                    if (param.type.starts_with("PriorityQueue<"))
                    {
                        return true;
                    }
                    if (param.type.starts_with("Stack<") || param.type.starts_with("List<"))
                    {
                        return false;
                    }
                }
            }
        }
        return scope.findIsPriorityQueue(name->name);
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        const auto it = functions_.find(call->callee);
        if (it != functions_.end() && it->second->returnType)
        {
            if (it->second->returnType->starts_with("PriorityQueue<"))
            {
                return true;
            }
            if (it->second->returnType->starts_with("Stack<") ||
                it->second->returnType->starts_with("List<"))
            {
                return false;
            }
        }
    }

    return std::nullopt;
}

std::optional<bool> IrGenerator::isSortedMapExpr(const Expr& expr,
                                                 const FunctionDecl* function,
                                                 const IrScope& scope) const
{
    if (dynamic_cast<const SortedMapNewExpr*>(&expr))
    {
        return true;
    }
    if (dynamic_cast<const MapNewExpr*>(&expr) || dynamic_cast<const SetNewExpr*>(&expr))
    {
        return false;
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        if (function)
        {
            for (const auto& param : function->params)
            {
                if (param.name == name->name)
                {
                    if (param.type.starts_with("SortedMap<"))
                    {
                        return true;
                    }
                    if (param.type.starts_with("Map<") || param.type.starts_with("Set<"))
                    {
                        return false;
                    }
                }
            }
        }
        return scope.findIsSortedMap(name->name);
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        const auto it = functions_.find(call->callee);
        if (it != functions_.end() && it->second->returnType)
        {
            if (it->second->returnType->starts_with("SortedMap<"))
            {
                return true;
            }
            if (it->second->returnType->starts_with("Map<") ||
                it->second->returnType->starts_with("Set<"))
            {
                return false;
            }
        }
    }

    return std::nullopt;
}

std::optional<bool> IrGenerator::isSortedSetExpr(const Expr& expr,
                                                 const FunctionDecl* function,
                                                 const IrScope& scope) const
{
    if (dynamic_cast<const SortedSetNewExpr*>(&expr))
    {
        return true;
    }
    if (dynamic_cast<const SetNewExpr*>(&expr) || dynamic_cast<const MapNewExpr*>(&expr) ||
        dynamic_cast<const SortedMapNewExpr*>(&expr))
    {
        return false;
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        if (function)
        {
            for (const auto& param : function->params)
            {
                if (param.name == name->name)
                {
                    if (param.type.starts_with("SortedSet<"))
                    {
                        return true;
                    }
                    if (param.type.starts_with("Set<") || param.type.starts_with("Map<") ||
                        param.type.starts_with("SortedMap<"))
                    {
                        return false;
                    }
                }
            }
        }
        return scope.findIsSortedSet(name->name);
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        const auto it = functions_.find(call->callee);
        if (it != functions_.end() && it->second->returnType)
        {
            if (it->second->returnType->starts_with("SortedSet<"))
            {
                return true;
            }
            if (it->second->returnType->starts_with("Set<") ||
                it->second->returnType->starts_with("Map<") ||
                it->second->returnType->starts_with("SortedMap<"))
            {
                return false;
            }
        }
    }

    return std::nullopt;
}

std::optional<bool> IrGenerator::isBufferExpr(const Expr& expr,
                                              const FunctionDecl* function,
                                              const IrScope& scope) const
{
    if (dynamic_cast<const BufferNewExpr*>(&expr))
    {
        return true;
    }
    if (dynamic_cast<const StringNewExpr*>(&expr))
    {
        return false;
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        if (function)
        {
            for (const auto& param : function->params)
            {
                if (param.name == name->name)
                {
                    // String/Buffer aren't generic, so this is an exact
                    // match, not a starts_with prefix check (see
                    // docs/language/0043-buffer.md).
                    if (param.type == "Buffer")
                    {
                        return true;
                    }
                    if (param.type == "String")
                    {
                        return false;
                    }
                }
            }
        }
        return scope.findIsBuffer(name->name);
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        const auto it = functions_.find(call->callee);
        if (it != functions_.end() && it->second->returnType)
        {
            if (*it->second->returnType == "Buffer")
            {
                return true;
            }
            if (*it->second->returnType == "String")
            {
                return false;
            }
        }
    }

    return std::nullopt;
}

int IrGenerator::lowerExpr(const Expr& expr, IrScope& scope, Context& ctx)
{
    if (const auto* integer = dynamic_cast<const IntegerExpr*>(&expr))
    {
        auto inst = std::make_unique<IrConstInt>();
        inst->value = integer->value;
        return emit(ctx, std::move(inst));
    }

    if (const auto* int64Expr = dynamic_cast<const Int64Expr*>(&expr))
    {
        auto inst = std::make_unique<IrConstInt64>();
        inst->value = int64Expr->value;
        return emit(ctx, std::move(inst));
    }

    if (const auto* floatExpr = dynamic_cast<const FloatExpr*>(&expr))
    {
        auto inst = std::make_unique<IrConstFloat>();
        inst->value = floatExpr->value;
        return emit(ctx, std::move(inst));
    }

    if (const auto* boolean = dynamic_cast<const BoolExpr*>(&expr))
    {
        auto inst = std::make_unique<IrConstBool>();
        inst->value = boolean->value;
        return emit(ctx, std::move(inst));
    }

    if (const auto* charExpr = dynamic_cast<const CharExpr*>(&expr))
    {
        auto inst = std::make_unique<IrConstChar>();
        inst->codepoint = charExpr->codepoint;
        return emit(ctx, std::move(inst));
    }

    if (const auto* string = dynamic_cast<const StringExpr*>(&expr))
    {
        auto inst = std::make_unique<IrConstString>();
        inst->value = string->value;
        return emit(ctx, std::move(inst));
    }

    if (const auto* interpolated = dynamic_cast<const InterpolatedStringExpr*>(&expr))
    {
        // Desugars into "build a Buffer, then finish() into a String"
        // (see docs/language/Axea_Printing_Formatting.md's own "the
        // compiler may lower interpolation internally to efficient
        // Buffer operations" hint) - a literal piece appends its own
        // IrConstString directly (str-coercible already); an expression
        // piece appends via IrBufferAppendValue, which stringifies at
        // the LLVM layer based on the value's own inferred type (i32,
        // bool, char are all not str-coercible on their own, unlike str/
        // String, so a plain IrBufferAppend wouldn't work for those -
        // this is why a distinct instruction is needed at all).
        auto bufferNewInst = std::make_unique<IrBufferNew>();
        const int bufferReg = emit(ctx, std::move(bufferNewInst));

        for (const auto& piece : interpolated->pieces)
        {
            if (piece.expr)
            {
                const int valueReg = lowerExpr(*piece.expr, scope, ctx);
                auto appendInst = std::make_unique<IrBufferAppendValue>();
                appendInst->buffer = bufferReg;
                appendInst->value = valueReg;
                emit(ctx, std::move(appendInst));
            }
            else if (!piece.literalText.empty())
            {
                auto constInst = std::make_unique<IrConstString>();
                constInst->value = piece.literalText;
                const int textReg = emit(ctx, std::move(constInst));
                auto appendInst = std::make_unique<IrBufferAppend>();
                appendInst->buffer = bufferReg;
                appendInst->text = textReg;
                emit(ctx, std::move(appendInst));
            }
        }

        auto finishInst = std::make_unique<IrBufferFinish>();
        finishInst->buffer = bufferReg;
        return emit(ctx, std::move(finishInst));
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        return scope.find(name->name);
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr))
    {
        const int lhs = lowerExpr(*binary->left, scope, ctx);
        const int rhs = lowerExpr(*binary->right, scope, ctx);
        auto inst = std::make_unique<IrBinOp>();
        inst->op = binary->op;
        inst->lhs = lhs;
        inst->rhs = rhs;
        return emit(ctx, std::move(inst));
    }

    if (const auto* cast = dynamic_cast<const CastExpr*>(&expr))
    {
        const int operand = lowerExpr(*cast->operand, scope, ctx);
        auto inst = std::make_unique<IrCast>();
        inst->operand = operand;
        inst->targetType = cast->targetType;
        return emit(ctx, std::move(inst));
    }

    if (const auto* someExpr = dynamic_cast<const SomeExpr*>(&expr))
    {
        const int value = lowerExpr(*someExpr->value, scope, ctx);
        auto inst = std::make_unique<IrOptionalNew>();
        inst->value = value;
        // payloadTypeName left empty - LlvmIrEmitter's own type inference
        // reads the payload type directly off `value`'s already-inferred
        // register type instead (see docs/language/0052-optional.md); only
        // a bare None (no value register to infer from) needs it set.
        return emit(ctx, std::move(inst));
    }

    if (dynamic_cast<const NoneExpr*>(&expr))
    {
        // Unreachable in a well-typed program - TypeChecker::checkStmt's
        // AssignmentStmt/ReturnStmt cases (and their IrGenerator::lowerStmt
        // mirrors just above) always intercept a bare `None` before it
        // would reach generic lowerExpr (see docs/language/0052-optional.md).
        return -1;
    }

    if (const auto* tryExpr = dynamic_cast<const TryExpr*>(&expr))
    {
        // `<expr>?` (see docs/language/0052-optional.md) - lowered as an
        // IrBranch, the same "conditional + early-terminating side" shape
        // IfExpr uses just below, exploiting emitBranch's own existing
        // "a terminated side contributes no merge-block value" handling
        // (see LlvmIrEmitter::emitBranch) for the None side's `return`.
        const int optional = lowerExpr(*tryExpr->operand, scope, ctx);

        auto isSomeInst = std::make_unique<IrOptionalIsSome>();
        isSomeInst->object = optional;
        const int isSome = emit(ctx, std::move(isSomeInst));

        auto branch = std::make_unique<IrBranch>();
        branch->condition = isSome;

        IrScope thenScope(&scope, /*isBarrier=*/true);
        std::vector<int> thenStructLocals;
        Context thenCtx{&branch->thenBlock, ctx.registerCount, ctx.function, &thenStructLocals};
        auto unwrapInst = std::make_unique<IrOptionalUnwrap>();
        unwrapInst->object = optional;
        branch->thenValue = emit(thenCtx, std::move(unwrapInst));

        IrScope elseScope(&scope, /*isBarrier=*/true);
        std::vector<int> elseStructLocals;
        Context elseCtx{&branch->elseBlock, ctx.registerCount, ctx.function, &elseStructLocals};
        auto noneInst = std::make_unique<IrOptionalNew>();
        noneInst->value = -1;
        noneInst->payloadTypeName = optionalPayloadTypeName(*ctx.function->returnType);
        const int none = emit(elseCtx, std::move(noneInst));
        auto returnInst = std::make_unique<IrReturn>();
        returnInst->value = none;
        emitVoid(elseCtx, std::move(returnInst));
        branch->elseValue = -1; // elseBlock always terminates via `return` above

        return emit(ctx, std::move(branch));
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        // `print`/`write` (see docs/language/Axea_Printing_Formatting.md)
        // - checked before the ordinary IrCall lowering below, since
        // neither is a real callable Axea function/extern (TypeChecker's
        // own registerSignatures already guarantees no user declaration
        // can shadow either name).
        if (call->callee == "print" || call->callee == "write")
        {
            std::vector<int> printArgs;
            printArgs.reserve(call->arguments.size());
            for (const auto& argument : call->arguments)
            {
                printArgs.push_back(lowerExpr(*argument, scope, ctx));
            }
            auto printInst = std::make_unique<IrPrint>();
            printInst->args = std::move(printArgs);
            printInst->addNewline = call->callee == "print";
            return emit(ctx, std::move(printInst));
        }

        std::vector<int> args;
        args.reserve(call->arguments.size());
        for (const auto& argument : call->arguments)
        {
            args.push_back(lowerExpr(*argument, scope, ctx));
        }
        auto inst = std::make_unique<IrCall>();
        inst->callee = call->callee;
        inst->args = std::move(args);
        return emit(ctx, std::move(inst));
    }

    if (const auto* methodCall = dynamic_cast<const MethodCallExpr*>(&expr))
    {
        // Resolved from the AST, before lowering `object` below, since
        // isSetExpr/isStackExpr/isDequeExpr/isPriorityQueueExpr/
        // isSortedMapExpr all inspect the expression shape itself (see their
        // own doc comments) - needed only to disambiguate "contains"/
        // "remove" between Map, Set, and SortedMap and "set"/"get" between
        // Map and SortedMap (docs/language/0034-maps-and-sets.md,
        // docs/language/0040-sorted-maps.md), "push"/"pop"/"peek" between
        // List, Stack, and PriorityQueue (docs/language/0035-stacks.md,
        // docs/language/0039-priority-queues.md), and push_front/push_back/
        // pop_front/pop_back between LinkedList and Deque (see
        // docs/language/0037-deques.md); every other method name here is
        // unambiguous by itself.
        const std::optional<bool> setKind = isSetExpr(*methodCall->object, ctx.function, scope);
        const std::optional<bool> stackKind = isStackExpr(*methodCall->object, ctx.function, scope);
        const std::optional<bool> dequeKind = isDequeExpr(*methodCall->object, ctx.function, scope);
        const std::optional<bool> priorityQueueKind =
            isPriorityQueueExpr(*methodCall->object, ctx.function, scope);
        const std::optional<bool> sortedMapKind =
            isSortedMapExpr(*methodCall->object, ctx.function, scope);
        const std::optional<bool> sortedSetKind =
            isSortedSetExpr(*methodCall->object, ctx.function, scope);
        const std::optional<bool> bufferKind =
            isBufferExpr(*methodCall->object, ctx.function, scope);
        const int object = lowerExpr(*methodCall->object, scope, ctx);

        if (methodCall->method == "parse")
        {
            // Unambiguous by name alone - no disambiguation kind needed
            // (see docs/language/0046-generic-methods.md).
            auto inst = std::make_unique<IrParse>();
            inst->object = object;
            inst->targetType = methodCall->typeArgument;
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "to_cstr")
        {
            // Unambiguous by name alone, same reasoning as "parse" above
            // (see docs/language/0048-ffi.md).
            auto inst = std::make_unique<IrToCstr>();
            inst->object = object;
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "join")
        {
            // Unambiguous by name alone, same reasoning as "parse"/
            // "to_cstr" above (see
            // docs/language/0050-collection-join-and-slicing.md).
            auto inst = std::make_unique<IrJoin>();
            inst->object = object;
            inst->separator = lowerExpr(*methodCall->arguments.front(), scope, ctx);
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "is_some" || methodCall->method == "is_none")
        {
            // Unambiguous by name alone, same reasoning as "parse"/
            // "to_cstr"/"join" above (see docs/language/0052-optional.md).
            auto inst = std::make_unique<IrOptionalIsSome>();
            inst->object = object;
            inst->negate = methodCall->method == "is_none";
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "unwrap_or")
        {
            // `.unwrap_or(default)` (see docs/language/0052-optional.md) -
            // an ordinary two-sided IrBranch, exactly IfExpr's own shape
            // below (unlike TryExpr's early-returning branch, both sides
            // reach the merge block here, so emitBranch's usual phi path
            // handles it with no special-casing needed).
            auto isSomeInst = std::make_unique<IrOptionalIsSome>();
            isSomeInst->object = object;
            const int isSome = emit(ctx, std::move(isSomeInst));

            auto branch = std::make_unique<IrBranch>();
            branch->condition = isSome;

            IrScope thenScope(&scope, /*isBarrier=*/true);
            std::vector<int> thenStructLocals;
            Context thenCtx{&branch->thenBlock, ctx.registerCount, ctx.function, &thenStructLocals};
            auto unwrapInst = std::make_unique<IrOptionalUnwrap>();
            unwrapInst->object = object;
            branch->thenValue = emit(thenCtx, std::move(unwrapInst));

            IrScope elseScope(&scope, /*isBarrier=*/true);
            std::vector<int> elseStructLocals;
            Context elseCtx{&branch->elseBlock, ctx.registerCount, ctx.function, &elseStructLocals};
            branch->elseValue = lowerExpr(*methodCall->arguments.front(), elseScope, elseCtx);

            return emit(ctx, std::move(branch));
        }

        if (methodCall->method == "push")
        {
            // "push" is unit-typed (see docs/language/0033-lists.md), but
            // still gets a real dest register via emit() rather than -1 -
            // `x = numbers.push(4)` is legal (mirrors the established
            // `called = f()` idiom for any unit-returning call), and that
            // requires a real register to bind `x` to. Matches exactly how
            // a unit-returning IrCall already works: LlvmIrEmitter types this
            // register "void" and never calls ref() on it, only typeOf().
            // priorityQueueKind is checked before stackKind - the first
            // three-way method-name collision in this codebase (see
            // docs/language/0039-priority-queues.md).
            if (priorityQueueKind.value_or(false))
            {
                auto inst = std::make_unique<IrPriorityQueuePush>();
                inst->priorityQueue = object;
                inst->value = lowerExpr(*methodCall->arguments.front(), scope, ctx);
                return emit(ctx, std::move(inst));
            }
            if (stackKind.value_or(false))
            {
                auto inst = std::make_unique<IrStackPush>();
                inst->stack = object;
                inst->value = lowerExpr(*methodCall->arguments.front(), scope, ctx);
                return emit(ctx, std::move(inst));
            }
            auto inst = std::make_unique<IrListPush>();
            inst->list = object;
            inst->value = lowerExpr(*methodCall->arguments.front(), scope, ctx);
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "pop")
        {
            if (priorityQueueKind.value_or(false))
            {
                auto inst = std::make_unique<IrPriorityQueuePop>();
                inst->priorityQueue = object;
                return emit(ctx, std::move(inst));
            }
            if (stackKind.value_or(false))
            {
                auto inst = std::make_unique<IrStackPop>();
                inst->stack = object;
                return emit(ctx, std::move(inst));
            }
            auto inst = std::make_unique<IrListPop>();
            inst->list = object;
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "peek")
        {
            // No longer unambiguous now that PriorityQueue<T> also has
            // peek() (see docs/language/0039-priority-queues.md) -
            // priorityQueueKind disambiguates, mirroring push/pop above.
            if (priorityQueueKind.value_or(false))
            {
                auto inst = std::make_unique<IrPriorityQueuePeek>();
                inst->priorityQueue = object;
                return emit(ctx, std::move(inst));
            }
            auto inst = std::make_unique<IrStackPeek>();
            inst->stack = object;
            return emit(ctx, std::move(inst));
        }

        // push_front/push_back/pop_front/pop_back are shared between
        // LinkedList<T> (docs/language/0036-linked-lists.md) and Deque<T>
        // (docs/language/0037-deques.md) - dequeKind (computed above)
        // disambiguates, mirroring stackKind's own List-vs-Stack dispatch.
        if (methodCall->method == "push_front")
        {
            if (dequeKind.value_or(false))
            {
                auto inst = std::make_unique<IrDequePushFront>();
                inst->deque = object;
                inst->value = lowerExpr(*methodCall->arguments.front(), scope, ctx);
                return emit(ctx, std::move(inst));
            }
            auto inst = std::make_unique<IrLinkedListPushFront>();
            inst->list = object;
            inst->value = lowerExpr(*methodCall->arguments.front(), scope, ctx);
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "push_back")
        {
            if (dequeKind.value_or(false))
            {
                auto inst = std::make_unique<IrDequePushBack>();
                inst->deque = object;
                inst->value = lowerExpr(*methodCall->arguments.front(), scope, ctx);
                return emit(ctx, std::move(inst));
            }
            auto inst = std::make_unique<IrLinkedListPushBack>();
            inst->list = object;
            inst->value = lowerExpr(*methodCall->arguments.front(), scope, ctx);
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "pop_front")
        {
            if (dequeKind.value_or(false))
            {
                auto inst = std::make_unique<IrDequePopFront>();
                inst->deque = object;
                return emit(ctx, std::move(inst));
            }
            auto inst = std::make_unique<IrLinkedListPopFront>();
            inst->list = object;
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "pop_back")
        {
            if (dequeKind.value_or(false))
            {
                auto inst = std::make_unique<IrDequePopBack>();
                inst->deque = object;
                return emit(ctx, std::move(inst));
            }
            auto inst = std::make_unique<IrLinkedListPopBack>();
            inst->list = object;
            return emit(ctx, std::move(inst));
        }

        // "enqueue"/"dequeue" (see docs/language/0038-queues.md) are
        // brand-new method names nothing else in the language uses - unlike
        // "push_front"/"push_back"/"pop_front"/"pop_back" above, no
        // disambiguation resolver is needed at all.
        if (methodCall->method == "enqueue")
        {
            auto inst = std::make_unique<IrQueueEnqueue>();
            inst->queue = object;
            inst->value = lowerExpr(*methodCall->arguments.front(), scope, ctx);
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "dequeue")
        {
            auto inst = std::make_unique<IrQueueDequeue>();
            inst->queue = object;
            return emit(ctx, std::move(inst));
        }

        // "append" is shared between String (docs/language/0042-string.md)
        // and Buffer (docs/language/0043-buffer.md) - bufferKind
        // disambiguates. "append_line"/"clear"/"reserve"/"finish" are all
        // brand-new method names nothing else in the language uses - like
        // "enqueue"/"dequeue" before them, no disambiguation resolver is
        // needed for those.
        if (methodCall->method == "append")
        {
            if (bufferKind.value_or(false))
            {
                auto inst = std::make_unique<IrBufferAppend>();
                inst->buffer = object;
                inst->text = lowerExpr(*methodCall->arguments.front(), scope, ctx);
                return emit(ctx, std::move(inst));
            }
            auto inst = std::make_unique<IrStringAppend>();
            inst->string = object;
            inst->other = lowerExpr(*methodCall->arguments.front(), scope, ctx);
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "append_line")
        {
            auto inst = std::make_unique<IrBufferAppendLine>();
            inst->buffer = object;
            inst->text = lowerExpr(*methodCall->arguments.front(), scope, ctx);
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "clear")
        {
            auto inst = std::make_unique<IrBufferClear>();
            inst->buffer = object;
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "reserve")
        {
            auto inst = std::make_unique<IrBufferReserve>();
            inst->buffer = object;
            inst->capacity = lowerExpr(*methodCall->arguments.front(), scope, ctx);
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "finish")
        {
            auto inst = std::make_unique<IrBufferFinish>();
            inst->buffer = object;
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "set")
        {
            // sortedMapKind is checked first - SortedMap<K,V> and Map<K,V>
            // are the only two candidates for "set" (Set<T> has no "set"),
            // see docs/language/0040-sorted-maps.md.
            if (sortedMapKind.value_or(false))
            {
                auto inst = std::make_unique<IrSortedMapSet>();
                inst->sortedMap = object;
                inst->key = lowerExpr(*methodCall->arguments[0], scope, ctx);
                inst->value = lowerExpr(*methodCall->arguments[1], scope, ctx);
                return emit(ctx, std::move(inst));
            }
            auto inst = std::make_unique<IrMapSet>();
            inst->map = object;
            inst->key = lowerExpr(*methodCall->arguments[0], scope, ctx);
            inst->value = lowerExpr(*methodCall->arguments[1], scope, ctx);
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "get")
        {
            if (sortedMapKind.value_or(false))
            {
                auto inst = std::make_unique<IrSortedMapGet>();
                inst->sortedMap = object;
                inst->key = lowerExpr(*methodCall->arguments.front(), scope, ctx);
                return emit(ctx, std::move(inst));
            }
            auto inst = std::make_unique<IrMapGet>();
            inst->map = object;
            inst->key = lowerExpr(*methodCall->arguments.front(), scope, ctx);
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "add")
        {
            // sortedSetKind is checked first - Set<T> and SortedSet<T> are
            // the only two candidates for "add" (Map<K,V>/SortedMap<K,V>
            // have no "add"), see docs/language/0041-sorted-sets.md.
            if (sortedSetKind.value_or(false))
            {
                auto inst = std::make_unique<IrSortedSetAdd>();
                inst->sortedSet = object;
                inst->value = lowerExpr(*methodCall->arguments.front(), scope, ctx);
                return emit(ctx, std::move(inst));
            }
            auto inst = std::make_unique<IrSetAdd>();
            inst->set = object;
            inst->value = lowerExpr(*methodCall->arguments.front(), scope, ctx);
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "contains")
        {
            // Four-way now - sortedSetKind is checked before sortedMapKind
            // and setKind, mirroring push/pop/peek's own priorityQueueKind-
            // before-stackKind ordering (see
            // docs/language/0039-priority-queues.md's identical framing).
            const int argument = lowerExpr(*methodCall->arguments.front(), scope, ctx);
            if (sortedSetKind.value_or(false))
            {
                auto inst = std::make_unique<IrSortedSetContains>();
                inst->sortedSet = object;
                inst->value = argument;
                return emit(ctx, std::move(inst));
            }
            if (sortedMapKind.value_or(false))
            {
                auto inst = std::make_unique<IrSortedMapContains>();
                inst->sortedMap = object;
                inst->key = argument;
                return emit(ctx, std::move(inst));
            }
            if (setKind.value_or(false))
            {
                auto inst = std::make_unique<IrSetContains>();
                inst->set = object;
                inst->value = argument;
                return emit(ctx, std::move(inst));
            }
            auto inst = std::make_unique<IrMapContains>();
            inst->map = object;
            inst->key = argument;
            return emit(ctx, std::move(inst));
        }

        // TypeChecker already rejected anything but push/pop/set/get/
        // add/contains/remove here.
        const int argument = lowerExpr(*methodCall->arguments.front(), scope, ctx);
        if (sortedSetKind.value_or(false))
        {
            auto inst = std::make_unique<IrSortedSetRemove>();
            inst->sortedSet = object;
            inst->value = argument;
            return emit(ctx, std::move(inst));
        }
        if (sortedMapKind.value_or(false))
        {
            auto inst = std::make_unique<IrSortedMapRemove>();
            inst->sortedMap = object;
            inst->key = argument;
            return emit(ctx, std::move(inst));
        }
        if (setKind.value_or(false))
        {
            auto inst = std::make_unique<IrSetRemove>();
            inst->set = object;
            inst->value = argument;
            return emit(ctx, std::move(inst));
        }
        auto inst = std::make_unique<IrMapRemove>();
        inst->map = object;
        inst->key = argument;
        return emit(ctx, std::move(inst));
    }

    if (const auto* field = dynamic_cast<const FieldExpr*>(&expr))
    {
        // `.length` on a fixed array is always compile-time-known (see
        // docs/language/0031-arrays.md) - constant-fold it directly rather
        // than emitting a runtime IrFieldGet, so it's truly zero-cost. Falls
        // through to the normal struct-field path below for anything
        // arrayLengthOf can't resolve, including a genuine struct field that
        // happens to be named "length".
        if (field->field == "length")
        {
            if (const auto length = arrayLengthOf(*field->object, ctx.function, scope))
            {
                auto constInst = std::make_unique<IrConstInt>();
                constInst->value = *length;
                return emit(ctx, std::move(constInst));
            }
        }

        const int object = lowerExpr(*field->object, scope, ctx);
        auto inst = std::make_unique<IrFieldGet>();
        inst->object = object;
        inst->field = field->field;
        return emit(ctx, std::move(inst));
    }

    if (const auto* arrayLiteral = dynamic_cast<const ArrayLiteralExpr*>(&expr))
    {
        std::vector<int> elements;
        elements.reserve(arrayLiteral->elements.size());
        for (const auto& element : arrayLiteral->elements)
        {
            elements.push_back(lowerExpr(*element, scope, ctx));
        }
        auto inst = std::make_unique<IrArrayNew>();
        inst->elements = std::move(elements);
        return emit(ctx, std::move(inst));
    }

    if (const auto* listNew = dynamic_cast<const ListNewExpr*>(&expr))
    {
        auto inst = std::make_unique<IrListNew>();
        inst->elementTypeName = listNew->elementType;
        return emit(ctx, std::move(inst));
    }

    if (const auto* stackNew = dynamic_cast<const StackNewExpr*>(&expr))
    {
        auto inst = std::make_unique<IrStackNew>();
        inst->elementTypeName = stackNew->elementType;
        return emit(ctx, std::move(inst));
    }

    if (const auto* linkedListNew = dynamic_cast<const LinkedListNewExpr*>(&expr))
    {
        auto inst = std::make_unique<IrLinkedListNew>();
        inst->elementTypeName = linkedListNew->elementType;
        return emit(ctx, std::move(inst));
    }

    if (const auto* dequeNew = dynamic_cast<const DequeNewExpr*>(&expr))
    {
        auto inst = std::make_unique<IrDequeNew>();
        inst->elementTypeName = dequeNew->elementType;
        return emit(ctx, std::move(inst));
    }

    if (const auto* queueNew = dynamic_cast<const QueueNewExpr*>(&expr))
    {
        auto inst = std::make_unique<IrQueueNew>();
        inst->elementTypeName = queueNew->elementType;
        return emit(ctx, std::move(inst));
    }

    if (const auto* priorityQueueNew = dynamic_cast<const PriorityQueueNewExpr*>(&expr))
    {
        auto inst = std::make_unique<IrPriorityQueueNew>();
        inst->elementTypeName = priorityQueueNew->elementType;
        return emit(ctx, std::move(inst));
    }

    if (const auto* mapNew = dynamic_cast<const MapNewExpr*>(&expr))
    {
        auto inst = std::make_unique<IrMapNew>();
        inst->keyTypeName = mapNew->keyType;
        inst->valueTypeName = mapNew->valueType;
        return emit(ctx, std::move(inst));
    }

    if (const auto* setNew = dynamic_cast<const SetNewExpr*>(&expr))
    {
        auto inst = std::make_unique<IrSetNew>();
        inst->elementTypeName = setNew->elementType;
        return emit(ctx, std::move(inst));
    }

    if (const auto* sortedMapNew = dynamic_cast<const SortedMapNewExpr*>(&expr))
    {
        auto inst = std::make_unique<IrSortedMapNew>();
        inst->keyTypeName = sortedMapNew->keyType;
        inst->valueTypeName = sortedMapNew->valueType;
        return emit(ctx, std::move(inst));
    }

    if (const auto* sortedSetNew = dynamic_cast<const SortedSetNewExpr*>(&expr))
    {
        auto inst = std::make_unique<IrSortedSetNew>();
        inst->elementTypeName = sortedSetNew->elementType;
        return emit(ctx, std::move(inst));
    }

    if (const auto* stringNew = dynamic_cast<const StringNewExpr*>(&expr))
    {
        auto inst = std::make_unique<IrStringNew>();
        inst->text = lowerExpr(*stringNew->text, scope, ctx);
        return emit(ctx, std::move(inst));
    }

    if (dynamic_cast<const BufferNewExpr*>(&expr))
    {
        auto inst = std::make_unique<IrBufferNew>();
        return emit(ctx, std::move(inst));
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(&expr))
    {
        const int object = lowerExpr(*index->object, scope, ctx);
        const int indexReg = lowerExpr(*index->index, scope, ctx);
        auto inst = std::make_unique<IrIndexGet>();
        inst->object = object;
        inst->index = indexReg;
        return emit(ctx, std::move(inst));
    }

    if (const auto* strSlice = dynamic_cast<const StrSliceExpr*>(&expr))
    {
        const int object = lowerExpr(*strSlice->object, scope, ctx);
        auto inst = std::make_unique<IrStrSlice>();
        inst->object = object;
        inst->start = strSlice->start ? lowerExpr(*strSlice->start, scope, ctx) : -1;
        inst->end = strSlice->end ? lowerExpr(*strSlice->end, scope, ctx) : -1;
        return emit(ctx, std::move(inst));
    }

    if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(&expr))
    {
        std::vector<std::pair<std::string, int>> fields;
        fields.reserve(literal->fields.size());
        for (const auto& [fieldName, fieldExpr] : literal->fields)
        {
            fields.emplace_back(fieldName, lowerExpr(*fieldExpr, scope, ctx));
        }
        auto inst = std::make_unique<IrStructNew>();
        inst->typeName = literal->typeName;
        inst->fields = std::move(fields);
        return emit(ctx, std::move(inst));
    }

    if (const auto* ifExpr = dynamic_cast<const IfExpr*>(&expr))
    {
        const int condition = lowerExpr(*ifExpr->condition, scope, ctx);

        auto branch = std::make_unique<IrBranch>();
        branch->condition = condition;

        IrScope thenScope(&scope, /*isBarrier=*/true);
        std::vector<int> thenStructLocals;
        Context thenCtx{&branch->thenBlock, ctx.registerCount, ctx.function, &thenStructLocals};
        emitVoid(thenCtx, std::make_unique<IrRegionEnter>());
        branch->thenValue = lowerExpr(*ifExpr->thenBranch, thenScope, thenCtx);
        emitVoid(thenCtx, std::make_unique<IrRegionExit>());

        IrScope elseScope(&scope, /*isBarrier=*/true);
        std::vector<int> elseStructLocals;
        Context elseCtx{&branch->elseBlock, ctx.registerCount, ctx.function, &elseStructLocals};
        emitVoid(elseCtx, std::make_unique<IrRegionEnter>());
        branch->elseValue = lowerExpr(*ifExpr->elseBranch, elseScope, elseCtx);
        emitVoid(elseCtx, std::make_unique<IrRegionExit>());

        return emit(ctx, std::move(branch));
    }

    if (const auto* loopExpr = dynamic_cast<const LoopExpr*>(&expr))
    {
        return lowerLoop(nullptr, *loopExpr->body, scope, ctx);
    }

    if (const auto* block = dynamic_cast<const BlockExpr*>(&expr))
    {
        IrScope blockScope(&scope);
        std::vector<int> structLocals;
        Context blockCtx = ctx;
        blockCtx.structLocals = &structLocals;

        for (const auto& statement : block->statements)
        {
            lowerStmt(*statement, blockScope, blockCtx);
        }

        int result = -1;
        if (block->result)
        {
            result = lowerExpr(*block->result, blockScope, blockCtx);
        }

        for (auto it = structLocals.rbegin(); it != structLocals.rend(); ++it)
        {
            auto dropInst = std::make_unique<IrDrop>();
            dropInst->value = *it;
            emitVoid(blockCtx, std::move(dropInst));
        }

        return result;
    }

    return -1; // unreachable for a well-checked program
}

void IrGenerator::lowerStmt(const Stmt& stmt, IrScope& scope, Context& ctx)
{
    if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(&stmt))
    {
        // `x: Optional<T> = None` (see docs/language/0052-optional.md) -
        // built directly here, bypassing the generic lowerExpr(NoneExpr)
        // path (unreachable in a well-typed program), since only this call
        // site has the declared type text None's own payload type is read
        // from (mirrors TypeChecker::checkStmt's identical special case).
        const bool isNoneWithDeclaredType =
            dynamic_cast<const NoneExpr*>(assignment->value.get()) != nullptr &&
            assignment->declaredType.has_value();
        int value;
        if (isNoneWithDeclaredType)
        {
            auto inst = std::make_unique<IrOptionalNew>();
            inst->value = -1;
            inst->payloadTypeName = optionalPayloadTypeName(*assignment->declaredType);
            value = emit(ctx, std::move(inst));
        }
        else
        {
            value = lowerExpr(*assignment->value, scope, ctx);
        }
        // Mutates an already-existing binding (in this scope or any
        // enclosing one, subject to the same barrier rule as assign()
        // itself), the same as `++`/`--` already do; only a genuinely new
        // name defines a fresh local. Matters most for loops: a loop body's
        // `n = n + 1` needs to actually update the outer `n`, not shadow a
        // throwaway per-traversal copy (mirrors the same fix in
        // Interpreter::execute; see docs/language/0028-loops.md).
        if (!assignment->forceDefine && scope.contains(assignment->name))
        {
            scope.assign(assignment->name, value);
        }
        else
        {
            scope.define(assignment->name, value);
        }
        if (ctx.function && ctx.structLocals &&
            isObviouslyStructTyped(*assignment->value, *ctx.function))
        {
            ctx.structLocals->push_back(value);
        }
        // Records this name's array length, if known, so a later `.length`
        // on it can constant-fold (see arrayLengthOf and
        // docs/language/0031-arrays.md). Always local to this scope, mirroring
        // define() above - not assign()'s walk-up-and-mutate, since this is
        // read-only metadata about the binding, not the binding itself.
        if (const auto length = arrayLengthOf(*assignment->value, ctx.function, scope))
        {
            scope.defineArrayLength(assignment->name, *length);
        }
        // Records this name's Map-vs-Set kind, if known, so a later
        // `.contains`/`.remove` on it can be resolved unambiguously (see
        // isSetExpr and docs/language/0034-maps-and-sets.md) - mirrors
        // arrayLengthOf's own placement immediately above.
        if (const auto isSet = isSetExpr(*assignment->value, ctx.function, scope))
        {
            scope.defineIsSet(assignment->name, *isSet);
        }
        // Same reasoning, for List-vs-Stack (see isStackExpr and
        // docs/language/0035-stacks.md).
        if (const auto isStack = isStackExpr(*assignment->value, ctx.function, scope))
        {
            scope.defineIsStack(assignment->name, *isStack);
        }
        // Same reasoning again, for LinkedList-vs-Deque (see isDequeExpr and
        // docs/language/0037-deques.md).
        if (const auto isDeque = isDequeExpr(*assignment->value, ctx.function, scope))
        {
            scope.defineIsDeque(assignment->name, *isDeque);
        }
        // Same reasoning again, for List/Stack-vs-PriorityQueue (see
        // isPriorityQueueExpr and docs/language/0039-priority-queues.md).
        if (const auto isPriorityQueue =
                isPriorityQueueExpr(*assignment->value, ctx.function, scope))
        {
            scope.defineIsPriorityQueue(assignment->name, *isPriorityQueue);
        }
        // Same reasoning again, for Map/Set-vs-SortedMap (see
        // isSortedMapExpr and docs/language/0040-sorted-maps.md).
        if (const auto isSortedMap = isSortedMapExpr(*assignment->value, ctx.function, scope))
        {
            scope.defineIsSortedMap(assignment->name, *isSortedMap);
        }
        // Same reasoning again, for Set/Map/SortedMap-vs-SortedSet (see
        // isSortedSetExpr and docs/language/0041-sorted-sets.md).
        if (const auto isSortedSet = isSortedSetExpr(*assignment->value, ctx.function, scope))
        {
            scope.defineIsSortedSet(assignment->name, *isSortedSet);
        }
        // Same reasoning again, for String-vs-Buffer (see isBufferExpr and
        // docs/language/0043-buffer.md).
        if (const auto isBuffer = isBufferExpr(*assignment->value, ctx.function, scope))
        {
            scope.defineIsBuffer(assignment->name, *isBuffer);
        }
        return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt))
    {
        // `return None` (see docs/language/0052-optional.md) - same
        // reasoning as AssignmentStmt's own NoneExpr special case above,
        // using the enclosing function's own declared return type (always
        // Optional<T> here, in a well-typed program) as None's payload
        // type source instead of a declared local type.
        const bool isNoneReturn =
            returnStmt->value &&
            dynamic_cast<const NoneExpr*>(returnStmt->value.get()) != nullptr && ctx.function &&
            ctx.function->returnType;
        auto inst = std::make_unique<IrReturn>();
        if (isNoneReturn)
        {
            auto noneInst = std::make_unique<IrOptionalNew>();
            noneInst->value = -1;
            noneInst->payloadTypeName = optionalPayloadTypeName(*ctx.function->returnType);
            inst->value = emit(ctx, std::move(noneInst));
        }
        else
        {
            inst->value = returnStmt->value ? lowerExpr(*returnStmt->value, scope, ctx) : -1;
        }
        emitVoid(ctx, std::move(inst));
        return;
    }

    if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt))
    {
        lowerExpr(*exprStmt->expr, scope, ctx);
        return;
    }

    if (const auto* fieldAssign = dynamic_cast<const FieldAssignStmt*>(&stmt))
    {
        const int object = lowerExpr(*fieldAssign->object, scope, ctx);
        const int value = lowerExpr(*fieldAssign->value, scope, ctx);
        auto inst = std::make_unique<IrFieldSet>();
        inst->object = object;
        inst->field = fieldAssign->field;
        inst->value = value;
        emitVoid(ctx, std::move(inst));
        return;
    }

    if (const auto* indexAssign = dynamic_cast<const IndexAssignStmt*>(&stmt))
    {
        const int object = lowerExpr(*indexAssign->object, scope, ctx);
        const int indexReg = lowerExpr(*indexAssign->index, scope, ctx);
        const int value = lowerExpr(*indexAssign->value, scope, ctx);
        auto inst = std::make_unique<IrIndexSet>();
        inst->object = object;
        inst->index = indexReg;
        inst->value = value;
        emitVoid(ctx, std::move(inst));
        return;
    }

    if (const auto* incDec = dynamic_cast<const IncDecStmt*>(&stmt))
    {
        const std::int64_t delta = incDec->increment ? 1 : -1;

        if (const auto* name = dynamic_cast<const NameExpr*>(incDec->target.get()))
        {
            const int current = scope.find(name->name);

            auto constInst = std::make_unique<IrConstInt>();
            constInst->value = delta;
            const int deltaRegister = emit(ctx, std::move(constInst));

            auto binInst = std::make_unique<IrBinOp>();
            binInst->op = TokenKind::Plus;
            binInst->lhs = current;
            binInst->rhs = deltaRegister;
            const int newRegister = emit(ctx, std::move(binInst));

            scope.assign(name->name, newRegister);
            return;
        }

        if (const auto* field = dynamic_cast<const FieldExpr*>(incDec->target.get()))
        {
            const int object = lowerExpr(*field->object, scope, ctx);

            auto getInst = std::make_unique<IrFieldGet>();
            getInst->object = object;
            getInst->field = field->field;
            const int currentRegister = emit(ctx, std::move(getInst));

            auto constInst = std::make_unique<IrConstInt>();
            constInst->value = delta;
            const int deltaRegister = emit(ctx, std::move(constInst));

            auto binInst = std::make_unique<IrBinOp>();
            binInst->op = TokenKind::Plus;
            binInst->lhs = currentRegister;
            binInst->rhs = deltaRegister;
            const int newRegister = emit(ctx, std::move(binInst));

            auto setInst = std::make_unique<IrFieldSet>();
            setInst->object = object;
            setInst->field = field->field;
            setInst->value = newRegister;
            emitVoid(ctx, std::move(setInst));
        }
        return;
    }

    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt))
    {
        lowerLoop(whileStmt->condition.get(),
                  *whileStmt->body,
                  scope,
                  ctx); // dest discarded - `while` never produces a value
        return;
    }

    if (const auto* breakStmt = dynamic_cast<const BreakStmt*>(&stmt))
    {
        auto inst = std::make_unique<IrBreak>();
        // Order matters: the value expression is lowered (and may itself
        // reassign carried variables, e.g. `break n++`) before the carried
        // snapshot is taken, so the snapshot reflects everything up to and
        // including this statement.
        inst->value = breakStmt->value ? lowerExpr(*breakStmt->value, scope, ctx) : -1;
        inst->carried = currentLoopCarriedDiff(scope);
        emitVoid(ctx, std::move(inst));
        return;
    }

    if (dynamic_cast<const ContinueStmt*>(&stmt))
    {
        auto inst = std::make_unique<IrContinue>();
        inst->carried = currentLoopCarriedDiff(scope);
        emitVoid(ctx, std::move(inst));
        return;
    }
}

std::vector<std::pair<int, int>> IrGenerator::currentLoopCarriedDiff(IrScope& scope) const
{
    std::vector<std::pair<int, int>> result;
    if (loopPreSnapshotStack_.empty())
    {
        return result; // unreachable in a well-typed program - TypeChecker already
                       // rejects break/continue outside a loop
    }
    const auto& preSnapshot = loopPreSnapshotStack_.back();
    const auto currentSnapshot = scope.snapshot();
    for (const auto& [name, preReg] : preSnapshot)
    {
        const auto it = currentSnapshot.find(name);
        if (it != currentSnapshot.end() && it->second != preReg)
        {
            result.emplace_back(preReg, it->second);
        }
    }
    return result;
}

int IrGenerator::lowerLoop(const Expr* condition, const Expr& body, IrScope& scope, Context& ctx)
{
    auto loopInst = std::make_unique<IrLoop>();

    if (condition)
    {
        Context condCtx{
            &loopInst->conditionBlock, ctx.registerCount, ctx.function, ctx.structLocals};
        loopInst->conditionValue = lowerExpr(*condition, scope, condCtx);
    }

    // `body` is always a BlockExpr; lowerExpr's own BlockExpr handling
    // already creates its own nested (non-barrier) scope and drops its own
    // struct locals - nothing extra needed here beyond diffing what it did
    // to `scope` itself, since assign() walks all the way up through that
    // nested scope into `scope` for any name that already lived here.
    const auto preSnapshot = scope.snapshot();
    loopPreSnapshotStack_.push_back(
        preSnapshot); // innermost loop's snapshot, for nested break/continue
    Context bodyCtx{&loopInst->body, ctx.registerCount, ctx.function, ctx.structLocals};
    lowerExpr(body, scope, bodyCtx); // trailing block value discarded - only `break value` produces
                                     // the loop's own value
    loopPreSnapshotStack_.pop_back();
    const auto postSnapshot = scope.snapshot();

    for (const auto& [name, preReg] : preSnapshot)
    {
        const auto it = postSnapshot.find(name);
        if (it != postSnapshot.end() && it->second != preReg)
        {
            loopInst->carried.emplace_back(preReg, it->second);
        }
    }

    return emit(ctx, std::move(loopInst));
}

IrFunction IrGenerator::generateFunction(const FunctionDecl& function,
                                         const std::vector<Capability>& capabilities,
                                         const std::vector<Region>& regions)
{
    IrFunction irFunction;
    irFunction.name = function.name;
    irFunction.returnType = function.returnType;
    for (const auto& param : function.params)
    {
        irFunction.paramNames.push_back(param.name);
        irFunction.paramTypes.push_back(param.type);
    }

    IrScope scope;
    int registerCount = 0;
    Context ctx{&irFunction.body, &registerCount, &function, nullptr};

    emitVoid(ctx, std::make_unique<IrRegionEnter>());

    std::vector<int> paramRegisters;
    paramRegisters.reserve(function.params.size());
    for (std::size_t i = 0; i < function.params.size(); ++i)
    {
        const int paramRegister = freshRegister(ctx);
        scope.define(function.params[i].name, paramRegister);
        paramRegisters.push_back(paramRegister);

        if (regions[i] == Region::Owned)
        {
            auto inst = std::make_unique<IrMove>();
            inst->value = paramRegister;
            emitVoid(ctx, std::move(inst));
        }
        else if (capabilities[i] == Capability::Write)
        {
            auto inst = std::make_unique<IrBorrowWrite>();
            inst->value = paramRegister;
            emitVoid(ctx, std::move(inst));
        }
        else
        {
            auto inst = std::make_unique<IrBorrowRead>();
            inst->value = paramRegister;
            emitVoid(ctx, std::move(inst));
        }
    }

    lowerExpr(*function.body, scope, ctx);

    for (std::size_t i = 0; i < function.params.size(); ++i)
    {
        if (regions[i] == Region::Owned && structs_.contains(function.params[i].type))
        {
            auto inst = std::make_unique<IrDrop>();
            inst->value = paramRegisters[i];
            emitVoid(ctx, std::move(inst));
        }
    }

    // Only reachable for a unit-returning function in a well-typed program -
    // TypeChecker::definitelyReturns already guarantees any non-unit
    // function's body always hits an explicit `return` on every path.
    // Appending one here regardless would double-terminate an already fully
    // covered body (e.g. `if cond { return a } else { return b } `'s merge
    // block already ends in `unreachable`); when it does fire, it's always a
    // bare/unit return - `function.body`'s trailing value, if any, was a
    // discarded expression, not something to return.
    if (!alwaysTerminates(irFunction.body))
    {
        auto returnInst = std::make_unique<IrReturn>();
        returnInst->value = -1;
        emitVoid(ctx, std::move(returnInst));
    }

    emitVoid(ctx, std::make_unique<IrRegionExit>());

    irFunction.registerCount = registerCount;
    return irFunction;
}

IrProgram
IrGenerator::generate(const Program& program,
                      const std::unordered_map<std::string, std::vector<Capability>>& capabilities,
                      const std::unordered_map<std::string, std::vector<Region>>& regions)
{
    registerStructs(program);

    IrProgram irProgram;
    for (const auto& [name, structDecl] : structs_)
    {
        std::vector<std::pair<std::string, std::string>> fields;
        fields.reserve(structDecl->fields.size());
        for (const auto& field : structDecl->fields)
        {
            fields.emplace_back(field.name, field.type);
        }
        irProgram.structs[name] = std::move(fields);
    }

    IrScope topScope;
    int topRegisterCount = 0;
    Context topCtx{&irProgram.topLevel, &topRegisterCount, nullptr, nullptr};

    for (const auto& item : program.items)
    {
        if (const auto* function = dynamic_cast<const FunctionDecl*>(item.get()))
        {
            irProgram.functions.push_back(generateFunction(
                *function, capabilities.at(function->name), regions.at(function->name)));
        }
        else if (const auto* externDecl = dynamic_cast<const ExternDecl*>(item.get()))
        {
            // No body to lower (see docs/language/0048-ffi.md) - just
            // registered so LlvmIrEmitter can emit a `declare` for it;
            // call sites already lower identically to a regular
            // FunctionDecl call (IrCall doesn't distinguish the two).
            IrExtern irExtern;
            irExtern.name = externDecl->name;
            irExtern.paramTypes.reserve(externDecl->params.size());
            for (const auto& param : externDecl->params)
            {
                irExtern.paramTypes.push_back(param.type);
            }
            irExtern.returnType = externDecl->returnType;
            irProgram.externs.push_back(std::move(irExtern));
        }
        else if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(item.get()))
        {
            lowerStmt(*assignment, topScope, topCtx);
            irProgram.topLevelBindings.emplace_back(assignment->name,
                                                    topScope.find(assignment->name));
        }
        else if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(item.get()))
        {
            // A bare top-level call kept for its side effect
            // (`print("hi")`, `write("...")`) - see
            // Parser::looksLikeFunctionDecl's own doc comment. No
            // topLevelBindings entry (there's no name to bind), which is
            // exactly right - it should run once, not also be
            // auto-printed as if it were a binding.
            lowerStmt(*exprStmt, topScope, topCtx);
        }
    }

    return irProgram;
}
