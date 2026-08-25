#include "ir/IrGenerator.hpp"

#include <algorithm>
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

    // Finds the first *top-level* comma in `text` - i.e. one not nested
    // inside a further `<...>` type-argument list (e.g. Result<i32,
    // Map<i32,i32>>'s own inner comma). Own copy, per this codebase's
    // "each pass owns its own walk" convention - TypeChecker and
    // LlvmIrEmitter each already have their own identical logic for the
    // same reason (see docs/language/0034-maps-and-sets.md's generic
    // rewrite).
    std::size_t findTopLevelComma(const std::string& text)
    {
        int depth = 0;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '<')
            {
                ++depth;
            }
            else if (text[i] == '>')
            {
                --depth;
            }
            else if (text[i] == ',' && depth == 0)
            {
                return i;
            }
        }
        return std::string::npos;
    }

    // "Result<T,E>" -> {T, E} (see docs/language/0063-result.md) - the
    // Result-flavored analogue of optionalPayloadTypeName just above, used
    // for the identical reason: Ok(x)/Err(e) each need the *other* type
    // parameter's name from surrounding context (a declared type or the
    // enclosing function's own return type), since IrGenerator keeps no
    // real type table.
    std::pair<std::string, std::string> resultPayloadTypeNames(const std::string& resultTypeName)
    {
        const std::string args = resultTypeName.substr(7, resultTypeName.size() - 8);
        const auto comma = findTopLevelComma(args);
        return {args.substr(0, comma), args.substr(comma + 1)};
    }

    // A union's own canonical name ("i32|str" - see docs/language/0065-unions.md and
    // Parser::parseTypeName) is exactly the right *Axea-level* identity (readable, and what
    // enums_/IrScope::findEnumName key on), but '|' isn't a legal unquoted LLVM identifier
    // character - only [a-zA-Z$._][a-zA-Z$._0-9]* is. This is the one point that name ever turns
    // into actual LLVM text (an IrStructNew's own typeName, and irProgram.structs/enums's own
    // keys - both read directly by LlvmIrEmitter to build "%<name>"): '.' is a legal LLVM
    // identifier character no existing Axea type name ever contains, so substituting it for '|'
    // here is a deterministic, collision-free rename. A no-op for every real (never
    // '|'-containing) struct/enum name.
    std::string llvmSafeTypeName(std::string name)
    {
        std::replace(name.begin(), name.end(), '|', '.');
        return name;
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

void IrScope::defineEnumName(const std::string& name, std::string enumName)
{
    enumNames_[name] = std::move(enumName);
}

std::optional<std::string> IrScope::findEnumName(const std::string& name) const
{
    if (const auto it = enumNames_.find(name); it != enumNames_.end())
    {
        return it->second;
    }
    return parent_ ? parent_->findEnumName(name) : std::nullopt;
}

void IrScope::defineSimpleType(const std::string& name, std::string simpleType)
{
    simpleTypes_[name] = std::move(simpleType);
}

std::optional<std::string> IrScope::findSimpleType(const std::string& name) const
{
    if (const auto it = simpleTypes_.find(name); it != simpleTypes_.end())
    {
        return it->second;
    }
    return parent_ ? parent_->findSimpleType(name) : std::nullopt;
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
        else if (const auto* enumDecl = dynamic_cast<const EnumDecl*>(item.get()))
        {
            enums_[enumDecl->name] = enumDecl;
        }
        // Only the return type is needed here (see isSetExpr) - not a
        // general function table, so this stays folded into registerStructs
        // rather than becoming its own pass.
        if (const auto* function = dynamic_cast<const FunctionDecl*>(item.get()))
        {
            functions_[function->name] = function;
        }
        else if (const auto* implDecl = dynamic_cast<const ImplDecl*>(item.get()))
        {
            // See docs/language/0062-display-trait.md - each impl method
            // is registered exactly like a top-level FunctionDecl.
            for (const auto& method : implDecl->methods)
            {
                functions_[method->name] = method.get();
            }
        }
        else if (const auto* externDecl = dynamic_cast<const ExternDecl*>(item.get()))
        {
            // Modules (see docs/language/0066-modules.md) - see externModules_'s own comment.
            externModules_[externDecl->name] = externDecl->moduleName;
        }
    }

    // Modules (see docs/language/0066-modules.md) - see moduleNames_'s own comment.
    for (const auto& [name, function] : functions_)
    {
        if (const auto dot = name.rfind('.'); dot != std::string::npos)
        {
            moduleNames_.insert(name.substr(0, dot));
        }
    }
    for (const auto& [name, owningModule] : externModules_)
    {
        if (!owningModule.empty())
        {
            moduleNames_.insert(owningModule);
        }
    }

    // Anonymous union types (see docs/language/0065-unions.md) - every function/impl-method
    // signature is scanned up front for a param/return type string containing '|', registering
    // each one's synthetic EnumDecl eagerly. Needed so enumNameOfExpr's own
    // `enums_.contains(param.type)`/`enums_.contains(*returnType)` checks (used to resolve a
    // union-typed parameter or a union-returning call for `match`) see it as a real enum from
    // the very first reference - a param/return type is fixed at the signature, unlike a local's
    // declared type (which lowerStmt's AssignmentStmt case registers lazily, the first time that
    // specific assignment is lowered, since there's no equivalent "whole program" pass over
    // statement bodies here).
    for (const auto& [name, function] : functions_)
    {
        for (const auto& param : function->params)
        {
            if (param.type.find('|') != std::string::npos)
            {
                registerUnionType(param.type);
            }
        }
        if (function->returnType && function->returnType->find('|') != std::string::npos)
        {
            registerUnionType(*function->returnType);
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

std::optional<std::string> IrGenerator::enumNameOfExpr(const Expr& expr,
                                                       const FunctionDecl* function,
                                                       const IrScope& scope) const
{
    // A direct `EnumName.Variant(...)`/`EnumName.Variant` construction - the same "is `object`
    // a bare name matching a known enum" check MethodCallExpr's/FieldExpr's own lowerExpr cases
    // use (see docs/language/0064-enums.md).
    if (const auto* methodCall = dynamic_cast<const MethodCallExpr*>(&expr))
    {
        if (const auto* name = dynamic_cast<const NameExpr*>(methodCall->object.get());
            name && enums_.contains(name->name))
        {
            return name->name;
        }
    }
    if (const auto* field = dynamic_cast<const FieldExpr*>(&expr))
    {
        if (const auto* name = dynamic_cast<const NameExpr*>(field->object.get());
            name && enums_.contains(name->name))
        {
            return name->name;
        }
    }

    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        if (function)
        {
            for (const auto& param : function->params)
            {
                if (param.name == name->name && enums_.contains(param.type))
                {
                    return param.type;
                }
            }
        }
        return scope.findEnumName(name->name);
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        const auto it = functions_.find(call->callee);
        if (it != functions_.end() && it->second->returnType &&
            enums_.contains(*it->second->returnType))
        {
            return *it->second->returnType;
        }
    }

    return std::nullopt;
}

const EnumDecl& IrGenerator::registerUnionType(const std::string& canonicalName)
{
    if (const auto it = enums_.find(canonicalName); it != enums_.end())
    {
        return *it->second;
    }

    std::vector<EnumVariant> variants;
    std::size_t start = 0;
    while (true)
    {
        const auto bar = canonicalName.find('|', start);
        const std::string alternative =
            canonicalName.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        variants.push_back(EnumVariant{alternative, {alternative}});
        if (bar == std::string::npos)
        {
            break;
        }
        start = bar + 1;
    }
    auto decl = std::make_unique<EnumDecl>(canonicalName, std::move(variants));
    const EnumDecl* raw = decl.get();
    enums_[canonicalName] = raw;
    unionDecls_.push_back(std::move(decl));
    return *raw;
}

std::optional<std::string> IrGenerator::simpleTypeOfExpr(const Expr& expr,
                                                         const FunctionDecl* function,
                                                         const IrScope& scope) const
{
    if (dynamic_cast<const IntegerExpr*>(&expr))
    {
        return std::string("i32");
    }
    if (dynamic_cast<const Int64Expr*>(&expr))
    {
        return std::string("i64");
    }
    if (dynamic_cast<const FloatExpr*>(&expr))
    {
        return std::string("f64");
    }
    if (dynamic_cast<const BoolExpr*>(&expr))
    {
        return std::string("bool");
    }
    if (dynamic_cast<const CharExpr*>(&expr))
    {
        return std::string("char");
    }
    if (dynamic_cast<const StringExpr*>(&expr) ||
        dynamic_cast<const InterpolatedStringExpr*>(&expr))
    {
        return std::string("str");
    }
    if (const auto* structLiteral = dynamic_cast<const StructLiteralExpr*>(&expr))
    {
        return structLiteral->typeName;
    }
    if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
    {
        if (function)
        {
            for (const auto& param : function->params)
            {
                if (param.name == name->name)
                {
                    return param.type;
                }
            }
        }
        return scope.findSimpleType(name->name);
    }
    if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
    {
        const auto it = functions_.find(call->callee);
        if (it != functions_.end() && it->second->returnType)
        {
            return *it->second->returnType;
        }
    }
    return std::nullopt;
}

void IrGenerator::collectReferencedNames(const Expr& expr, std::unordered_set<std::string>& names)
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

void IrGenerator::collectReferencedNames(const Stmt& stmt, std::unordered_set<std::string>& names)
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

int IrGenerator::wrapForUnion(int valueReg,
                              const Expr& valueExpr,
                              const std::string& declaredTypeName,
                              IrScope& scope,
                              Context& ctx)
{
    if (declaredTypeName.find('|') == std::string::npos)
    {
        return valueReg;
    }

    const EnumDecl& unionDecl = registerUnionType(declaredTypeName);

    // Already exactly this union (e.g. forwarding an existing `i32 | str`
    // value through another `i32 | str`-typed boundary) - passed through
    // unchanged, not re-wrapped.
    if (const auto valueEnumName = enumNameOfExpr(valueExpr, ctx.function, scope);
        valueEnumName && *valueEnumName == declaredTypeName)
    {
        return valueReg;
    }

    const std::optional<std::string> valueType = simpleTypeOfExpr(valueExpr, ctx.function, scope);
    if (!valueType)
    {
        throw std::runtime_error("internal error: could not determine which alternative of union " +
                                 declaredTypeName + " to wrap a value as");
    }
    const auto variantIt =
        std::find_if(unionDecl.variants.begin(),
                     unionDecl.variants.end(),
                     [&](const EnumVariant& variant) { return variant.name == *valueType; });
    if (variantIt == unionDecl.variants.end())
    {
        throw std::runtime_error("internal error: '" + *valueType +
                                 "' is not an alternative of union " + declaredTypeName);
    }
    const int tagValue = static_cast<int>(variantIt - unionDecl.variants.begin());

    auto tagInst = std::make_unique<IrConstInt>();
    tagInst->value = tagValue;
    const int tagReg = emit(ctx, std::move(tagInst));

    std::vector<std::pair<std::string, int>> fields;
    fields.emplace_back("__tag", tagReg);
    fields.emplace_back(*valueType + "_0", valueReg);

    auto inst = std::make_unique<IrStructNew>();
    inst->typeName = llvmSafeTypeName(declaredTypeName);
    inst->fields = std::move(fields);
    return emit(ctx, std::move(inst));
}

int IrGenerator::lowerMatchArm(int scrutineeReg,
                               int tagReg,
                               const EnumDecl& enumDecl,
                               const std::vector<MatchArm>& arms,
                               std::size_t armIndex,
                               IrScope& scope,
                               Context& ctx)
{
    const MatchArm& arm = arms[armIndex];
    const bool isLast = armIndex + 1 == arms.size();

    // Binds this arm's own extracted payload fields (if any) and lowers its body - shared by
    // both the "tag matched" case and the (comparison-skipped) last-arm case below.
    auto lowerArmBody = [&](IrScope& bodyScope, Context& bodyCtx) -> int
    {
        IrScope armScope(&bodyScope, /*isBarrier=*/true);
        for (std::size_t i = 0; i < arm.bindingNames.size(); ++i)
        {
            auto fieldInst = std::make_unique<IrFieldGet>();
            fieldInst->object = scrutineeReg;
            fieldInst->field = arm.variantName + "_" + std::to_string(i);
            const int fieldReg = emit(bodyCtx, std::move(fieldInst));
            armScope.define(arm.bindingNames[i], fieldReg);
        }
        return lowerExpr(*arm.body, armScope, bodyCtx);
    };

    if (isLast)
    {
        return lowerArmBody(scope, ctx);
    }

    const auto variantIt =
        std::find_if(enumDecl.variants.begin(),
                     enumDecl.variants.end(),
                     [&](const EnumVariant& v) { return v.name == arm.variantName; });
    const int variantIndex = static_cast<int>(variantIt - enumDecl.variants.begin());

    auto tagConst = std::make_unique<IrConstInt>();
    tagConst->value = variantIndex;
    const int tagConstReg = emit(ctx, std::move(tagConst));

    auto eqInst = std::make_unique<IrBinOp>();
    eqInst->op = TokenKind::EqualEqual;
    eqInst->lhs = tagReg;
    eqInst->rhs = tagConstReg;
    const int condReg = emit(ctx, std::move(eqInst));

    auto branch = std::make_unique<IrBranch>();
    branch->condition = condReg;

    IrScope thenScope(&scope, /*isBarrier=*/true);
    std::vector<int> thenStructLocals;
    Context thenCtx{&branch->thenBlock, ctx.registerCount, ctx.function, &thenStructLocals};
    branch->thenValue = lowerArmBody(thenScope, thenCtx);

    IrScope elseScope(&scope, /*isBarrier=*/true);
    std::vector<int> elseStructLocals;
    Context elseCtx{&branch->elseBlock, ctx.registerCount, ctx.function, &elseStructLocals};
    branch->elseValue =
        lowerMatchArm(scrutineeReg, tagReg, enumDecl, arms, armIndex + 1, elseScope, elseCtx);

    return emit(ctx, std::move(branch));
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
                // `{expr=}` (see docs/language/0058-debug-formatting.md) -
                // lowered as an ordinary literal-text append (the exact
                // same IrConstString + IrBufferAppend pair a plain literal
                // piece already uses below), emitted right before the
                // expression's own value append, rather than threading a
                // prefix field through IrBufferAppendValue itself.
                if (!piece.selfDocPrefix.empty())
                {
                    auto prefixConst = std::make_unique<IrConstString>();
                    prefixConst->value = piece.selfDocPrefix + "=";
                    const int prefixReg = emit(ctx, std::move(prefixConst));
                    auto prefixAppend = std::make_unique<IrBufferAppend>();
                    prefixAppend->buffer = bufferReg;
                    prefixAppend->text = prefixReg;
                    emit(ctx, std::move(prefixAppend));
                }
                const int valueReg = lowerExpr(*piece.expr, scope, ctx);
                auto appendInst = std::make_unique<IrBufferAppendValue>();
                appendInst->buffer = bufferReg;
                appendInst->value = valueReg;
                appendInst->formatSpec = piece.formatSpec;
                appendInst->debug = piece.debug;
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

    if (dynamic_cast<const OkExpr*>(&expr) || dynamic_cast<const ErrExpr*>(&expr))
    {
        // Unreachable in a well-typed program - same reasoning as NoneExpr
        // just above (see docs/language/0063-result.md).
        return -1;
    }

    if (const auto* tryExpr = dynamic_cast<const TryExpr*>(&expr))
    {
        // `<expr>?` (see docs/language/0052-optional.md, generalized to
        // Result<T,E> by docs/language/0063-result.md) - lowered as an
        // IrBranch, the same "conditional + early-terminating side" shape
        // IfExpr uses just below, exploiting emitBranch's own existing
        // "a terminated side contributes no merge-block value" handling
        // (see LlvmIrEmitter::emitBranch) for the failure side's `return`.
        // The condition check (IrOptionalIsSome) and the then-branch
        // unwrap (IrOptionalUnwrap, field defaulted to 1) are shared
        // verbatim between Optional and Result - both layouts agree on
        // "field 0 is the positive tag, field 1 is the positive payload"
        // (see IrOptionalIsSome's own comment) - only the *else* branch's
        // own construction differs, decided by the enclosing function's
        // own declared return type (never the operand's own inferred
        // type, which IrGenerator has no table for - TypeChecker already
        // guarantees the two agree).
        const int operand = lowerExpr(*tryExpr->operand, scope, ctx);

        auto isSomeInst = std::make_unique<IrOptionalIsSome>();
        isSomeInst->object = operand;
        const int isSome = emit(ctx, std::move(isSomeInst));

        auto branch = std::make_unique<IrBranch>();
        branch->condition = isSome;

        IrScope thenScope(&scope, /*isBarrier=*/true);
        std::vector<int> thenStructLocals;
        Context thenCtx{&branch->thenBlock, ctx.registerCount, ctx.function, &thenStructLocals};
        auto unwrapInst = std::make_unique<IrOptionalUnwrap>();
        unwrapInst->object = operand;
        branch->thenValue = emit(thenCtx, std::move(unwrapInst));

        IrScope elseScope(&scope, /*isBarrier=*/true);
        std::vector<int> elseStructLocals;
        Context elseCtx{&branch->elseBlock, ctx.registerCount, ctx.function, &elseStructLocals};
        const std::string& returnTypeName = *ctx.function->returnType;
        int failureResult;
        if (returnTypeName.starts_with("Result<"))
        {
            // Preserves the operand's own Err payload (unlike None, which
            // carries nothing to preserve) - extracted via the same
            // IrOptionalUnwrap instruction, field 2 this time (see its own
            // comment for why that's the Err position in both layouts).
            auto unwrapErrInst = std::make_unique<IrOptionalUnwrap>();
            unwrapErrInst->object = operand;
            unwrapErrInst->field = 2;
            const int errValue = emit(elseCtx, std::move(unwrapErrInst));

            auto errNewInst = std::make_unique<IrResultNew>();
            errNewInst->isOk = false;
            errNewInst->value = errValue;
            errNewInst->otherPayloadTypeName = resultPayloadTypeNames(returnTypeName).first;
            failureResult = emit(elseCtx, std::move(errNewInst));
        }
        else
        {
            auto noneInst = std::make_unique<IrOptionalNew>();
            noneInst->value = -1;
            noneInst->payloadTypeName = optionalPayloadTypeName(returnTypeName);
            failureResult = emit(elseCtx, std::move(noneInst));
        }
        auto returnInst = std::make_unique<IrReturn>();
        returnInst->value = failureResult;
        emitVoid(elseCtx, std::move(returnInst));
        branch->elseValue = -1; // elseBlock always terminates via `return` above

        return emit(ctx, std::move(branch));
    }

    if (const auto* matchExpr = dynamic_cast<const MatchExpr*>(&expr))
    {
        // See docs/language/0064-enums.md and lowerMatchArm's own comment. The tag is read
        // once here, in the outer `ctx` - every nested branch built inside lowerMatchArm just
        // references this same register, exactly like IfExpr's own already-computed condition
        // register is referenced from inside its own thenBlock/elseBlock.
        const int scrutineeReg = lowerExpr(*matchExpr->scrutinee, scope, ctx);
        auto tagGet = std::make_unique<IrFieldGet>();
        tagGet->object = scrutineeReg;
        tagGet->field = "__tag";
        const int tagReg = emit(ctx, std::move(tagGet));

        const auto enumName = enumNameOfExpr(*matchExpr->scrutinee, ctx.function, scope);
        const EnumDecl& enumDecl = *enums_.at(*enumName);
        return lowerMatchArm(scrutineeReg, tagReg, enumDecl, matchExpr->arms, 0, scope, ctx);
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

        // `callback(x)` where `callback` is a closure-typed local/param (see
        // docs/language/0067-closures.md) - checked before the ordinary functions_ lookup below,
        // for the same "shares Identifier(args) syntax with a real call" reason
        // docs/language/0066-modules.md's own module-call interception is. Reuses
        // IrScope::findSimpleType (built for docs/language/0065-unions.md's own implicit-wrap
        // resolution) rather than a new tracker - a closure-typed local's own declared/inferred
        // type text always starts with "fn(", exactly the same signal simpleTypeOfExpr already
        // resolves for any other name.
        {
            std::optional<std::string> calleeType;
            if (ctx.function)
            {
                for (const auto& param : ctx.function->params)
                {
                    if (param.name == call->callee)
                    {
                        calleeType = param.type;
                        break;
                    }
                }
            }
            if (!calleeType)
            {
                calleeType = scope.findSimpleType(call->callee);
            }
            if (calleeType && calleeType->starts_with("fn("))
            {
                const int closureReg = scope.find(call->callee);
                std::vector<int> closureArgs;
                closureArgs.reserve(call->arguments.size());
                for (const auto& argument : call->arguments)
                {
                    closureArgs.push_back(lowerExpr(*argument, scope, ctx));
                }
                auto closureCall = std::make_unique<IrClosureCall>();
                closureCall->closureObject = closureReg;
                closureCall->args = std::move(closureArgs);
                return emit(ctx, std::move(closureCall));
            }
        }

        const auto calleeIt = functions_.find(call->callee);
        std::vector<int> args;
        args.reserve(call->arguments.size());
        for (std::size_t i = 0; i < call->arguments.size(); ++i)
        {
            int argReg = lowerExpr(*call->arguments[i], scope, ctx);
            // Implicit union wrapping (see docs/language/0065-unions.md) -
            // `f(5)`/`f("hi")` against `f(x: i32 | str)` need no wrapper
            // syntax.
            if (calleeIt != functions_.end() && i < calleeIt->second->params.size())
            {
                argReg = wrapForUnion(
                    argReg, *call->arguments[i], calleeIt->second->params[i].type, scope, ctx);
            }
            args.push_back(argReg);
        }
        auto inst = std::make_unique<IrCall>();
        inst->callee = call->callee;
        inst->args = std::move(args);
        return emit(ctx, std::move(inst));
    }

    if (const auto* methodCall = dynamic_cast<const MethodCallExpr*>(&expr))
    {
        // `EnumName.Variant(args)` construction (see docs/language/0064-enums.md) - checked
        // before lowering `methodCall->object` below, which would otherwise try to lower a bare
        // enum type name as if it were a real value expression. Builds an ordinary
        // IrStructNew (see IrGenerator::generate's own comment on why an enum is represented as
        // a flattened struct) - the tag field first, then each argument mapped to that
        // variant's own synthetic field name.
        if (const auto* name = dynamic_cast<const NameExpr*>(methodCall->object.get());
            name && enums_.contains(name->name))
        {
            const EnumDecl& enumDecl = *enums_.at(name->name);
            const auto variantIt =
                std::find_if(enumDecl.variants.begin(),
                             enumDecl.variants.end(),
                             [&](const EnumVariant& v) { return v.name == methodCall->method; });
            const int tagValue = static_cast<int>(variantIt - enumDecl.variants.begin());

            auto tagInst = std::make_unique<IrConstInt>();
            tagInst->value = tagValue;
            const int tagReg = emit(ctx, std::move(tagInst));

            std::vector<std::pair<std::string, int>> fields;
            fields.emplace_back("__tag", tagReg);
            for (std::size_t i = 0; i < methodCall->arguments.size(); ++i)
            {
                const int fieldReg = lowerExpr(*methodCall->arguments[i], scope, ctx);
                fields.emplace_back(methodCall->method + "_" + std::to_string(i), fieldReg);
            }

            auto inst = std::make_unique<IrStructNew>();
            inst->typeName = enumDecl.name;
            inst->fields = std::move(fields);
            return emit(ctx, std::move(inst));
        }

        // `math.sqrt(x)` module-qualified call (see docs/language/0066-modules.md) - checked
        // before lowering `methodCall->object` below, for the same "bare name that isn't a real
        // value" reason as the enum check just above. An IrCall is emitted either way - the only
        // question is *which* callee text: a function's own already-qualified name ("math.
        // helper"), or an extern's own bare, real, externally-linked symbol ("sqrt" - never
        // "math.sqrt", which would try to link against a symbol that doesn't exist; see
        // externModules_'s own comment).
        if (const auto* moduleName = dynamic_cast<const NameExpr*>(methodCall->object.get());
            moduleName && moduleNames_.contains(moduleName->name))
        {
            std::string callee = moduleName->name + "." + methodCall->method;
            if (const auto externIt = externModules_.find(methodCall->method);
                externIt != externModules_.end() && externIt->second == moduleName->name)
            {
                callee = methodCall->method;
            }

            std::vector<int> args;
            args.reserve(methodCall->arguments.size());
            for (const auto& argument : methodCall->arguments)
            {
                args.push_back(lowerExpr(*argument, scope, ctx));
            }
            auto callInst = std::make_unique<IrCall>();
            callInst->callee = callee;
            callInst->args = std::move(args);
            return emit(ctx, std::move(callInst));
        }

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

        if (methodCall->method == "is_some" || methodCall->method == "is_none" ||
            methodCall->method == "is_ok" || methodCall->method == "is_err")
        {
            // Unambiguous by name alone, same reasoning as "parse"/
            // "to_cstr"/"join" above (see docs/language/0052-optional.md,
            // docs/language/0063-result.md). is_ok/is_err reuse
            // IrOptionalIsSome verbatim - field 0 is "the positive tag" in
            // both Optional's and Result's layout (see that instruction's
            // own comment).
            auto inst = std::make_unique<IrOptionalIsSome>();
            inst->object = object;
            inst->negate = methodCall->method == "is_none" || methodCall->method == "is_err";
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

        // "write" (see docs/language/0061-buffer-write.md) is Buffer-only
        // (TypeChecker never allows it on String), so - unlike "append"
        // just above - it needs no bufferKind disambiguation: it always
        // lowers to the same IrBufferAppend "append" itself already lowers
        // to on that branch.
        if (methodCall->method == "write")
        {
            auto inst = std::make_unique<IrBufferAppend>();
            inst->buffer = object;
            inst->text = lowerExpr(*methodCall->arguments.front(), scope, ctx);
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
        // `EnumName.Variant` (no parens - a no-payload variant, see
        // docs/language/0064-enums.md and MethodCallExpr's own identical check above) -
        // constructs the same flattened struct, tag only, no payload fields to fill.
        if (const auto* name = dynamic_cast<const NameExpr*>(field->object.get());
            name && enums_.contains(name->name))
        {
            const EnumDecl& enumDecl = *enums_.at(name->name);
            const auto variantIt =
                std::find_if(enumDecl.variants.begin(),
                             enumDecl.variants.end(),
                             [&](const EnumVariant& v) { return v.name == field->field; });
            const int tagValue = static_cast<int>(variantIt - enumDecl.variants.begin());

            auto tagInst = std::make_unique<IrConstInt>();
            tagInst->value = tagValue;
            const int tagReg = emit(ctx, std::move(tagInst));

            auto inst = std::make_unique<IrStructNew>();
            inst->typeName = enumDecl.name;
            inst->fields.emplace_back("__tag", tagReg);
            return emit(ctx, std::move(inst));
        }

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

    if (const auto* closureExpr = dynamic_cast<const ClosureExpr*>(&expr))
    {
        // Move-only capture (see docs/language/0067-closures.md) - the same over-approximating
        // free-variable scan CapabilityChecker/Interpreter's own copies already use, minus the
        // closure's own param names.
        std::unordered_set<std::string> referenced;
        collectReferencedNames(*closureExpr->body, referenced);
        for (const auto& param : closureExpr->params)
        {
            referenced.erase(param.name);
        }
        // Deterministic order (insertion order of a sorted scan) so the same closure literal
        // always produces byte-identical IR across runs - an unordered_set's own iteration order
        // isn't guaranteed stable.
        std::vector<std::string> capturedNames(referenced.begin(), referenced.end());
        std::sort(capturedNames.begin(), capturedNames.end());

        std::vector<std::pair<std::string, int>> captureFields;
        std::vector<std::pair<std::string, std::string>> captureStructFields;
        for (const auto& capturedName : capturedNames)
        {
            const NameExpr capturedNameExpr(capturedName);
            const auto capturedType = simpleTypeOfExpr(capturedNameExpr, ctx.function, scope);
            if (!capturedType)
            {
                // Not actually a bound local/param (e.g. a top-level function's own name, also
                // picked up by the dumb, over-approximating scan) - resolved normally by name
                // when the closure body eventually calls it, not captured at all.
                continue;
            }
            const int capturedReg = lowerExpr(capturedNameExpr, scope, ctx);
            captureFields.emplace_back(capturedName, capturedReg);
            captureStructFields.emplace_back(capturedName, llvmSafeTypeName(*capturedType));
        }
        // Re-derive the actually-captured name list (some scanned names may have been dropped
        // just above) so the trampoline's own IrFieldGet list matches captureFields exactly.
        std::vector<std::string> actuallyCaptured;
        actuallyCaptured.reserve(captureFields.size());
        for (const auto& [name, reg] : captureFields)
        {
            actuallyCaptured.push_back(name);
        }

        const std::string capturesStructName =
            "closure.captures." + std::to_string(closureCounter_);
        const std::string trampolineName = "closure$" + std::to_string(closureCounter_);
        ++closureCounter_;
        closureCaptureStructs_[capturesStructName] = captureStructFields;

        auto capturesStructInst = std::make_unique<IrStructNew>();
        capturesStructInst->typeName = capturesStructName;
        capturesStructInst->fields = captureFields;
        const int capturesReg = emit(ctx, std::move(capturesStructInst));

        closureTrampolines_.push_back(generateClosureTrampoline(
            *closureExpr, trampolineName, capturesStructName, actuallyCaptured));

        std::vector<std::string> paramTypes;
        paramTypes.reserve(closureExpr->params.size());
        for (const auto& param : closureExpr->params)
        {
            paramTypes.push_back(param.type);
        }
        auto closureNewInst = std::make_unique<IrClosureNew>();
        closureNewInst->trampolineFunctionName = trampolineName;
        closureNewInst->capturesObject = capturesReg;
        closureNewInst->paramTypes = std::move(paramTypes);
        closureNewInst->returnType = closureExpr->returnType.value_or("unit");
        return emit(ctx, std::move(closureNewInst));
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
        // `x: Result<T,E> = Ok(value)`/`Err(value)` (see
        // docs/language/0063-result.md) - same "built directly here,
        // bypassing generic lowerExpr" treatment as None just above, since
        // only this call site has the declared type text the *other*
        // (not-supplied-by-`value`) type parameter is read from.
        const auto* okExpr = dynamic_cast<const OkExpr*>(assignment->value.get());
        const auto* errExpr = dynamic_cast<const ErrExpr*>(assignment->value.get());
        const bool isOkOrErrWithDeclaredType =
            (okExpr || errExpr) && assignment->declaredType.has_value();
        int value;
        if (isNoneWithDeclaredType)
        {
            auto inst = std::make_unique<IrOptionalNew>();
            inst->value = -1;
            inst->payloadTypeName = optionalPayloadTypeName(*assignment->declaredType);
            value = emit(ctx, std::move(inst));
        }
        else if (isOkOrErrWithDeclaredType)
        {
            const auto [okTypeName, errTypeName] =
                resultPayloadTypeNames(*assignment->declaredType);
            auto inst = std::make_unique<IrResultNew>();
            inst->isOk = okExpr != nullptr;
            inst->value = lowerExpr(okExpr ? *okExpr->value : *errExpr->value, scope, ctx);
            inst->otherPayloadTypeName = okExpr ? errTypeName : okTypeName;
            value = emit(ctx, std::move(inst));
        }
        else
        {
            value = lowerExpr(*assignment->value, scope, ctx);
        }
        // Implicit union wrapping (see docs/language/0065-unions.md) -
        // `x: i32 | str = 5` needs no wrapper syntax.
        if (assignment->declaredType)
        {
            value = wrapForUnion(value, *assignment->value, *assignment->declaredType, scope, ctx);
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
        // Same reasoning again, for which registered enum type a value has (see
        // enumNameOfExpr and docs/language/0064-enums.md) - needed so a later `match` on this
        // local can resolve its own scrutinee's enum type. A union-typed declared local (see
        // docs/language/0065-unions.md) is recorded straight from its own declared type text
        // instead - implicit wrapping means the RHS value expression's own natural type is one
        // of the union's alternatives, not the union itself, so enumNameOfExpr (which infers
        // from the *value* expression) would never recognize it.
        if (assignment->declaredType && assignment->declaredType->find('|') != std::string::npos)
        {
            scope.defineEnumName(assignment->name, *assignment->declaredType);
        }
        else if (const auto enumName = enumNameOfExpr(*assignment->value, ctx.function, scope))
        {
            scope.defineEnumName(assignment->name, *enumName);
        }
        // Records this name's own best-effort simple Axea type, if known (see
        // IrScope::findSimpleType/simpleTypeOfExpr) - needed only to resolve a plain local
        // through one level of assignment when later deciding which alternative of a union type
        // to wrap it as. Prefers the declared type text itself (more reliable than inferring
        // from the RHS) whenever it's present and isn't itself a union.
        if (assignment->declaredType && assignment->declaredType->find('|') == std::string::npos)
        {
            scope.defineSimpleType(assignment->name, *assignment->declaredType);
        }
        else if (const auto simpleType = simpleTypeOfExpr(*assignment->value, ctx.function, scope))
        {
            scope.defineSimpleType(assignment->name, *simpleType);
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
        // `return Ok(value)`/`Err(value)` (see docs/language/0063-result.md)
        // - same reasoning as `return None` just above.
        const auto* okExpr =
            returnStmt->value ? dynamic_cast<const OkExpr*>(returnStmt->value.get()) : nullptr;
        const auto* errExpr =
            returnStmt->value ? dynamic_cast<const ErrExpr*>(returnStmt->value.get()) : nullptr;
        const bool isOkOrErrReturn =
            (okExpr || errExpr) && ctx.function && ctx.function->returnType;
        auto inst = std::make_unique<IrReturn>();
        if (isNoneReturn)
        {
            auto noneInst = std::make_unique<IrOptionalNew>();
            noneInst->value = -1;
            noneInst->payloadTypeName = optionalPayloadTypeName(*ctx.function->returnType);
            inst->value = emit(ctx, std::move(noneInst));
        }
        else if (isOkOrErrReturn)
        {
            const auto [okTypeName, errTypeName] =
                resultPayloadTypeNames(*ctx.function->returnType);
            auto resultInst = std::make_unique<IrResultNew>();
            resultInst->isOk = okExpr != nullptr;
            resultInst->value = lowerExpr(okExpr ? *okExpr->value : *errExpr->value, scope, ctx);
            resultInst->otherPayloadTypeName = okExpr ? errTypeName : okTypeName;
            inst->value = emit(ctx, std::move(resultInst));
        }
        else
        {
            inst->value = returnStmt->value ? lowerExpr(*returnStmt->value, scope, ctx) : -1;
            // Implicit union wrapping (see docs/language/0065-unions.md) -
            // `return 5` from a function declared `-> i32 | str` needs no
            // wrapper syntax.
            if (returnStmt->value && ctx.function && ctx.function->returnType)
            {
                inst->value = wrapForUnion(
                    inst->value, *returnStmt->value, *ctx.function->returnType, scope, ctx);
            }
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
    // llvmSafeTypeName: a no-op for every ordinary type name - only rewrites an anonymous
    // union's own canonical "T1|T2" text (see docs/language/0065-unions.md) into the form
    // LlvmIrEmitter will actually emit as a struct type name.
    irFunction.returnType =
        function.returnType ? std::optional(llvmSafeTypeName(*function.returnType)) : std::nullopt;
    for (const auto& param : function.params)
    {
        irFunction.paramNames.push_back(param.name);
        irFunction.paramTypes.push_back(llvmSafeTypeName(param.type));
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

IrFunction IrGenerator::generateClosureTrampoline(const ClosureExpr& closureExpr,
                                                  const std::string& trampolineName,
                                                  const std::string& capturesStructName,
                                                  const std::vector<std::string>& capturedNames)
{
    IrFunction irFunction;
    irFunction.name = trampolineName;
    irFunction.returnType = closureExpr.returnType
                                ? std::optional(llvmSafeTypeName(*closureExpr.returnType))
                                : std::nullopt;
    irFunction.paramNames.push_back("__captures");
    irFunction.paramTypes.push_back(capturesStructName);
    for (const auto& param : closureExpr.params)
    {
        irFunction.paramNames.push_back(param.name);
        irFunction.paramTypes.push_back(llvmSafeTypeName(param.type));
    }

    IrScope scope;
    int registerCount = 0;
    // ctx.function is null - closures don't have an enclosing FunctionDecl of their own (a real,
    // if narrow, known imprecision: any ctx.function-dependent logic, like union-wrap self-
    // lookup, degrades to its own scope-only fallback inside a closure body - see
    // docs/language/0067-closures.md's own Known Imprecision).
    Context ctx{&irFunction.body, &registerCount, nullptr, nullptr};

    emitVoid(ctx, std::make_unique<IrRegionEnter>());

    // Every param's own register is allocated first, consecutively, with nothing else emitted in
    // between - mirrors generateFunction's own identical shape exactly, since LlvmIrEmitter's own
    // prologue maps the first N freshly allocated registers, in order, to the N actual LLVM
    // parameters. The captures-field IrFieldGets below must come *after* this, once every param
    // register already exists - emitting them interleaved would misalign that correspondence.
    const int capturesRegister = freshRegister(ctx);
    {
        auto inst = std::make_unique<IrBorrowRead>();
        inst->value = capturesRegister;
        emitVoid(ctx, std::move(inst));
    }
    std::vector<int> paramRegisters;
    paramRegisters.reserve(closureExpr.params.size());
    for (std::size_t i = 0; i < closureExpr.params.size(); ++i)
    {
        const int paramRegister = freshRegister(ctx);
        paramRegisters.push_back(paramRegister);
        // Always Owned/Move - never struct-typed (this phase's own scope cut), so this has no
        // observable effect at the LLVM level either way, mirroring generateFunction's own
        // identical treatment of a non-struct param.
        auto inst = std::make_unique<IrMove>();
        inst->value = paramRegister;
        emitVoid(ctx, std::move(inst));
    }

    for (std::size_t i = 0; i < closureExpr.params.size(); ++i)
    {
        scope.define(closureExpr.params[i].name, paramRegisters[i]);
    }
    for (const auto& capturedName : capturedNames)
    {
        auto fieldGet = std::make_unique<IrFieldGet>();
        fieldGet->object = capturesRegister;
        fieldGet->field = capturedName;
        const int fieldReg = emit(ctx, std::move(fieldGet));
        scope.define(capturedName, fieldReg);
    }

    lowerExpr(*closureExpr.body, scope, ctx);

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
        else if (const auto* implDecl = dynamic_cast<const ImplDecl*>(item.get()))
        {
            // Each method compiles exactly like a top-level FunctionDecl
            // (see docs/language/0062-display-trait.md). "Display"'s own
            // "format" method additionally gets registered into
            // displayImpls, the one thing LlvmIrEmitter's own struct-
            // stringify dispatch (registerStructToStringHelpers) actually
            // consults - every other trait/method compiles as a real,
            // callable-in-principle function but drives no runtime
            // dispatch yet, since nothing else consumes any other trait
            // name this phase.
            for (const auto& method : implDecl->methods)
            {
                irProgram.functions.push_back(generateFunction(
                    *method, capabilities.at(method->name), regions.at(method->name)));
                if (implDecl->traitName == "Display" &&
                    method->name == implDecl->typeName + ".format")
                {
                    irProgram.displayImpls[implDecl->typeName] = method->name;
                }
            }
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

    // `enum` declarations (see docs/language/0064-enums.md), *including* every anonymous union
    // type auto-registered along the way (see docs/language/0065-unions.md and
    // registerUnionType) - represented, at every layer below TypeChecker, as an ordinary struct:
    // a flattened `{i32 tag, <variant0's own fields>, <variant1's own fields>, ...}` layout, each
    // variant's own payload fields synthetically named "<VariantName>_<index>" (never visible to
    // real Axea source - see IrResultNew's own "internal name, unreachable from user syntax"
    // convention for the identical reasoning). This lets construction (IrStructNew), field access
    // (IrFieldGet), and every layer of LlvmIrEmitter's own struct machinery (type declarations,
    // malloc+GEP+store construction, GEP+load field reads) work completely unchanged - an enum
    // genuinely *is* a struct once it reaches this point, just one whose own fields happen to
    // come from several source variants concatenated rather than one flat field list.
    // `irProgram.enums` records enough (each variant's own name + field count, in order) for
    // LlvmIrEmitter to later generate a real, tag-aware print/tostring function instead of the
    // generic "print every field" one every ordinary struct gets (see
    // emitStructPrintHelpers/emitStructToStringHelpers's own new enum-skipping check). Flattened
    // here, *after* every function has been lowered above - not right after registerStructs, the
    // way a real user-declared enum's own registration is - because a union's own registration
    // (unlike a real enum's) happens lazily, discovered while lowering a function body (a local's
    // declared type, an argument being wrapped, ...), so enums_ isn't done growing until the
    // whole program has been lowered.
    for (const auto& [name, enumDecl] : enums_)
    {
        std::vector<std::pair<std::string, std::string>> fields;
        fields.emplace_back("__tag", "i32");
        std::vector<std::pair<std::string, int>> variantSummary;
        variantSummary.reserve(enumDecl->variants.size());
        for (const auto& variant : enumDecl->variants)
        {
            for (std::size_t i = 0; i < variant.fieldTypes.size(); ++i)
            {
                fields.emplace_back(variant.name + "_" + std::to_string(i), variant.fieldTypes[i]);
            }
            variantSummary.emplace_back(variant.name, static_cast<int>(variant.fieldTypes.size()));
        }
        const std::string llvmName = llvmSafeTypeName(name);
        irProgram.structs[llvmName] = std::move(fields);
        irProgram.enums[llvmName] = std::move(variantSummary);
    }

    // Closures (see docs/language/0067-closures.md) - every trampoline function and every
    // captures struct discovered while lowering function bodies above, flushed here for the same
    // "discovered lazily, not known up front the way a real top-level item is" reason unions are
    // flattened here rather than right after registerStructs.
    for (auto& trampoline : closureTrampolines_)
    {
        irProgram.functions.push_back(std::move(trampoline));
    }
    for (auto& [name, fields] : closureCaptureStructs_)
    {
        irProgram.structs[name] = std::move(fields);
    }

    return irProgram;
}
