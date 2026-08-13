#include "ir/IrGenerator.hpp"

#include <stdexcept>

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

    if (const auto* field = dynamic_cast<const FieldExpr*>(&expr))
    {
        const int object = lowerExpr(*field->object, scope, ctx);
        auto inst = std::make_unique<IrFieldGet>();
        inst->object = object;
        inst->field = field->field;
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
        scope.define(assignment->name, value);
        if (ctx.function && ctx.structLocals &&
            isObviouslyStructTyped(*assignment->value, *ctx.function))
        {
            ctx.structLocals->push_back(value);
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

    const int result = lowerExpr(*function.body, scope, ctx);

    for (std::size_t i = 0; i < function.params.size(); ++i)
    {
        if (regions[i] == Region::Owned && structs_.contains(function.params[i].type))
        {
            auto inst = std::make_unique<IrDrop>();
            inst->value = paramRegisters[i];
            emitVoid(ctx, std::move(inst));
        }
    }

    auto returnInst = std::make_unique<IrReturn>();
    returnInst->value = result;
    emitVoid(ctx, std::move(returnInst));

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
        }
    }

    return irProgram;
}
