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

int IrGenerator::lowerExpr(const Expr& expr, IrScope& scope, Context& ctx)
{
    if (const auto* integer = dynamic_cast<const IntegerExpr*>(&expr))
    {
        auto inst = std::make_unique<IrConstInt>();
        inst->value = integer->value;
        return emit(ctx, std::move(inst));
    }

    if (const auto* boolean = dynamic_cast<const BoolExpr*>(&expr))
    {
        auto inst = std::make_unique<IrConstBool>();
        inst->value = boolean->value;
        return emit(ctx, std::move(inst));
    }

    if (const auto* string = dynamic_cast<const StringExpr*>(&expr))
    {
        auto inst = std::make_unique<IrConstString>();
        inst->value = string->value;
        return emit(ctx, std::move(inst));
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

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
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
        // isSetExpr/isStackExpr both inspect the expression shape itself
        // (see their own doc comments) - needed only to disambiguate
        // "contains"/"remove" between Map and Set
        // (docs/language/0034-maps-and-sets.md) and "push"/"pop" between
        // List and Stack (docs/language/0035-stacks.md); every other method
        // name here is unambiguous by itself.
        const std::optional<bool> setKind = isSetExpr(*methodCall->object, ctx.function, scope);
        const std::optional<bool> stackKind = isStackExpr(*methodCall->object, ctx.function, scope);
        const int object = lowerExpr(*methodCall->object, scope, ctx);

        if (methodCall->method == "push")
        {
            // "push" is unit-typed (see docs/language/0033-lists.md), but
            // still gets a real dest register via emit() rather than -1 -
            // `x = numbers.push(4)` is legal (mirrors the established
            // `called = f()` idiom for any unit-returning call), and that
            // requires a real register to bind `x` to. Matches exactly how
            // a unit-returning IrCall already works: LlvmIrEmitter types this
            // register "void" and never calls ref() on it, only typeOf().
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
            // Unambiguous - only Stack<T> has peek.
            auto inst = std::make_unique<IrStackPeek>();
            inst->stack = object;
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "set")
        {
            auto inst = std::make_unique<IrMapSet>();
            inst->map = object;
            inst->key = lowerExpr(*methodCall->arguments[0], scope, ctx);
            inst->value = lowerExpr(*methodCall->arguments[1], scope, ctx);
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "get")
        {
            auto inst = std::make_unique<IrMapGet>();
            inst->map = object;
            inst->key = lowerExpr(*methodCall->arguments.front(), scope, ctx);
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "add")
        {
            auto inst = std::make_unique<IrSetAdd>();
            inst->set = object;
            inst->value = lowerExpr(*methodCall->arguments.front(), scope, ctx);
            return emit(ctx, std::move(inst));
        }

        if (methodCall->method == "contains")
        {
            const int argument = lowerExpr(*methodCall->arguments.front(), scope, ctx);
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

    if (const auto* index = dynamic_cast<const IndexExpr*>(&expr))
    {
        const int object = lowerExpr(*index->object, scope, ctx);
        const int indexReg = lowerExpr(*index->index, scope, ctx);
        auto inst = std::make_unique<IrIndexGet>();
        inst->object = object;
        inst->index = indexReg;
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
        const int value = lowerExpr(*assignment->value, scope, ctx);
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
        return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt))
    {
        auto inst = std::make_unique<IrReturn>();
        inst->value = returnStmt->value ? lowerExpr(*returnStmt->value, scope, ctx) : -1;
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
        else if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(item.get()))
        {
            lowerStmt(*assignment, topScope, topCtx);
            irProgram.topLevelBindings.emplace_back(assignment->name,
                                                    topScope.find(assignment->name));
        }
    }

    return irProgram;
}
