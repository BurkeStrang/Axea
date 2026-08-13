#include "llvmir/LlvmIrEmitter.hpp"

namespace
{
    std::string llvmEscape(const std::string& text)
    {
        static const char* hex = "0123456789ABCDEF";
        std::string result;
        for (unsigned char c : text)
        {
            if (c == '"' || c == '\\' || c < 0x20 || c > 0x7e)
            {
                result += '\\';
                result += hex[(c >> 4) & 0xF];
                result += hex[c & 0xF];
            }
            else
            {
                result += static_cast<char>(c);
            }
        }
        return result;
    }
} // namespace

std::string LlvmIrEmitter::llvmType(const std::string& axeaTypeName) const
{
    if (axeaTypeName == "i32")
    {
        return "i32";
    }
    if (axeaTypeName == "bool")
    {
        return "i1";
    }
    if (axeaTypeName == "str")
    {
        return "i8*";
    }
    if (axeaTypeName == "unit")
    {
        return "void";
    }
    return "%" + axeaTypeName + "*"; // struct type: always by pointer
}

std::string LlvmIrEmitter::llvmReturnType(const std::optional<std::string>& returnType) const
{
    if (!returnType || *returnType == "unit")
    {
        return "void";
    }
    return llvmType(*returnType);
}

std::string LlvmIrEmitter::binOpMnemonic(TokenKind op) const
{
    switch (op)
    {
        case TokenKind::Plus: return "add";
        case TokenKind::Minus: return "sub";
        case TokenKind::Star: return "mul";
        case TokenKind::Slash: return "sdiv";
        case TokenKind::Less: return "icmp slt";
        case TokenKind::LessEqual: return "icmp sle";
        case TokenKind::Greater: return "icmp sgt";
        case TokenKind::GreaterEqual: return "icmp sge";
        case TokenKind::EqualEqual: return "icmp eq";
        case TokenKind::BangEqual: return "icmp ne";
        default: return "add"; // unreachable for a well-checked program
    }
}

int LlvmIrEmitter::allocateRegister(FunctionContext& fctx) const
{
    return fctx.nextLlvmRegister++;
}

int LlvmIrEmitter::defineRegister(int axeaReg, FunctionContext& fctx) const
{
    const int llvmReg = allocateRegister(fctx);
    fctx.llvmRegisterOf[axeaReg] = llvmReg;
    return llvmReg;
}

std::string LlvmIrEmitter::ref(int axeaReg, const FunctionContext& fctx) const
{
    return "%" + std::to_string(fctx.llvmRegisterOf.at(axeaReg));
}

bool LlvmIrEmitter::alwaysTerminates(const std::vector<std::unique_ptr<IrInst>>& instructions) const
{
    for (const auto& inst : instructions)
    {
        if (dynamic_cast<const IrReturn*>(inst.get()))
        {
            return true;
        }
        if (const auto* branch = dynamic_cast<const IrBranch*>(inst.get());
            branch && alwaysTerminates(branch->thenBlock) && alwaysTerminates(branch->elseBlock))
        {
            return true;
        }
    }
    return false;
}

std::pair<std::size_t, std::string>
LlvmIrEmitter::fieldIndexAndType(const std::string& structName, const std::string& fieldName) const
{
    const auto& declaredFields = structs_.at(structName);
    for (std::size_t index = 0; index < declaredFields.size(); ++index)
    {
        if (declaredFields[index].first == fieldName)
        {
            return {index, llvmType(declaredFields[index].second)};
        }
    }
    return {0, "i32"}; // unreachable for a well-checked program
}

std::string LlvmIrEmitter::structNameFromPointerType(const std::string& pointerType) const
{
    return pointerType.substr(1, pointerType.size() - 2); // strip leading '%' and trailing '*'
}

std::string LlvmIrEmitter::typeOf(int reg, const FunctionContext& fctx) const
{
    if (reg == -1)
    {
        return "void";
    }
    const auto it = fctx.registerTypes.find(reg);
    return it != fctx.registerTypes.end() ? it->second
                                          : "i32"; // fallback, unreachable post-checking
}

void LlvmIrEmitter::inferTypes(const IrFunction& function, FunctionContext& fctx)
{
    for (std::size_t i = 0; i < function.paramNames.size(); ++i)
    {
        fctx.registerTypes[static_cast<int>(i)] = llvmType(function.paramTypes[i]);
    }
    inferTypesInList(function.body, fctx);
}

void LlvmIrEmitter::inferTypesInList(const std::vector<std::unique_ptr<IrInst>>& instructions,
                                     FunctionContext& fctx)
{
    for (const auto& inst : instructions)
    {
        if (const auto* constInt = dynamic_cast<const IrConstInt*>(inst.get()))
        {
            fctx.registerTypes[constInt->dest] = "i32";
        }
        else if (const auto* constBool = dynamic_cast<const IrConstBool*>(inst.get()))
        {
            fctx.registerTypes[constBool->dest] = "i1";
        }
        else if (const auto* constString = dynamic_cast<const IrConstString*>(inst.get()))
        {
            fctx.registerTypes[constString->dest] = "i8*";
        }
        else if (const auto* binOp = dynamic_cast<const IrBinOp*>(inst.get()))
        {
            const bool isComparison =
                binOp->op == TokenKind::Less || binOp->op == TokenKind::LessEqual ||
                binOp->op == TokenKind::Greater || binOp->op == TokenKind::GreaterEqual ||
                binOp->op == TokenKind::EqualEqual || binOp->op == TokenKind::BangEqual;
            fctx.registerTypes[binOp->dest] = isComparison ? "i1" : "i32";
        }
        else if (const auto* call = dynamic_cast<const IrCall*>(inst.get()))
        {
            const auto it = functionReturnTypes_.find(call->callee);
            fctx.registerTypes[call->dest] =
                it != functionReturnTypes_.end() ? llvmReturnType(it->second) : "void";
        }
        else if (const auto* structNew = dynamic_cast<const IrStructNew*>(inst.get()))
        {
            fctx.registerTypes[structNew->dest] = "%" + structNew->typeName + "*";
        }
        else if (const auto* fieldGet = dynamic_cast<const IrFieldGet*>(inst.get()))
        {
            const std::string objectType = typeOf(fieldGet->object, fctx);
            const std::string structName = structNameFromPointerType(objectType);
            fctx.registerTypes[fieldGet->dest] =
                fieldIndexAndType(structName, fieldGet->field).second;
        }
        else if (const auto* branch = dynamic_cast<const IrBranch*>(inst.get()))
        {
            inferTypesInList(branch->thenBlock, fctx);
            inferTypesInList(branch->elseBlock, fctx);
            if (branch->thenValue != -1)
            {
                fctx.registerTypes[branch->dest] = typeOf(branch->thenValue, fctx);
            }
            else if (branch->elseValue != -1)
            {
                fctx.registerTypes[branch->dest] = typeOf(branch->elseValue, fctx);
            }
            else
            {
                fctx.registerTypes[branch->dest] = "void";
            }
        }
        // FieldSet, Return, BorrowRead/BorrowWrite, Move, RegionEnter/RegionExit, Drop: no dest.
    }
}

void LlvmIrEmitter::collectStrings(const std::vector<std::unique_ptr<IrInst>>& instructions)
{
    for (const auto& inst : instructions)
    {
        if (const auto* constString = dynamic_cast<const IrConstString*>(inst.get()))
        {
            if (!stringGlobalByLiteral_.contains(constString->value))
            {
                const std::string globalName = "@.str." + std::to_string(nextGlobal_++);
                stringGlobalByLiteral_[constString->value] = globalName;
                stringGlobals_.emplace_back(globalName, constString->value);
            }
        }
        else if (const auto* branch = dynamic_cast<const IrBranch*>(inst.get()))
        {
            collectStrings(branch->thenBlock);
            collectStrings(branch->elseBlock);
        }
    }
}

void LlvmIrEmitter::emitStringGlobals(std::ostringstream& out) const
{
    for (const auto& [globalName, literalText] : stringGlobals_)
    {
        const std::size_t length = literalText.size() + 1; // + null terminator
        out << globalName << " = private unnamed_addr constant [" << length << " x i8] c\""
            << llvmEscape(literalText) << "\\00\"\n";
    }
}

void LlvmIrEmitter::emitStructTypeDecls(std::ostringstream& out) const
{
    for (const auto& [name, fields] : structs_)
    {
        out << "%" << name << " = type { ";
        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            out << (i > 0 ? ", " : "") << llvmType(fields[i].second);
        }
        out << " }\n";
    }
}

void LlvmIrEmitter::emitStructNew(const IrStructNew& structNew, FunctionContext& fctx)
{
    const std::string llvmStructType = "%" + structNew.typeName;
    const std::string pointerType = llvmStructType + "*";

    // sizeof(%T) via the standard null-pointer GEP idiom, avoiding hand
    // computed byte sizes (padding/alignment stay LLVM's problem, not ours).
    const int sizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizePtrReg << " = getelementptr " << llvmStructType << ", " << pointerType
              << " null, i32 1\n";
    const int sizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizeIntReg << " = ptrtoint " << pointerType << " %" << sizePtrReg
              << " to i64\n";

    const int rawPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawPtrReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";

    const int destReg = defineRegister(structNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawPtrReg << " to " << pointerType
              << "\n";

    for (const auto& [fieldName, fieldValueReg] : structNew.fields)
    {
        const auto [index, fieldLlvmType] = fieldIndexAndType(structNew.typeName, fieldName);
        const int fieldPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << fieldPtrReg << " = getelementptr " << llvmStructType << ", "
                  << pointerType << " " << ref(structNew.dest, fctx) << ", i32 0, i32 " << index
                  << "\n";
        *fctx.out << "  store " << fieldLlvmType << " " << ref(fieldValueReg, fctx) << ", "
                  << fieldLlvmType << "* %" << fieldPtrReg << "\n";
    }
}

void LlvmIrEmitter::emitFieldGet(const IrFieldGet& fieldGet, FunctionContext& fctx)
{
    const std::string objectType = typeOf(fieldGet.object, fctx);
    const std::string structName = structNameFromPointerType(objectType);
    const auto [index, fieldLlvmType] = fieldIndexAndType(structName, fieldGet.field);

    const int fieldPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << fieldPtrReg << " = getelementptr %" << structName << ", " << objectType
              << " " << ref(fieldGet.object, fctx) << ", i32 0, i32 " << index << "\n";
    const int destReg = defineRegister(fieldGet.dest, fctx);
    *fctx.out << "  %" << destReg << " = load " << fieldLlvmType << ", " << fieldLlvmType << "* %"
              << fieldPtrReg << "\n";
}

void LlvmIrEmitter::emitFieldSet(const IrFieldSet& fieldSet, FunctionContext& fctx)
{
    const std::string objectType = typeOf(fieldSet.object, fctx);
    const std::string structName = structNameFromPointerType(objectType);
    const auto [index, fieldLlvmType] = fieldIndexAndType(structName, fieldSet.field);

    const int fieldPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << fieldPtrReg << " = getelementptr %" << structName << ", " << objectType
              << " " << ref(fieldSet.object, fctx) << ", i32 0, i32 " << index << "\n";
    *fctx.out << "  store " << fieldLlvmType << " " << ref(fieldSet.value, fctx) << ", "
              << fieldLlvmType << "* %" << fieldPtrReg << "\n";
}

void LlvmIrEmitter::emitBranch(const IrBranch& branch, FunctionContext& fctx)
{
    const int labelId = fctx.nextLabel++;
    const std::string thenLabel = "if.then" + std::to_string(labelId);
    const std::string elseLabel = "if.else" + std::to_string(labelId);
    const std::string mergeLabel = "if.merge" + std::to_string(labelId);

    *fctx.out << "  br i1 " << ref(branch.condition, fctx) << ", label %" << thenLabel
              << ", label %" << elseLabel << "\n";

    *fctx.out << thenLabel << ":\n";
    fctx.currentLabel = thenLabel;
    const bool thenTerminated = emitInstructions(branch.thenBlock, fctx);
    const std::string thenExitLabel =
        fctx.currentLabel; // may differ if thenBlock had its own nested branch
    if (!thenTerminated)
    {
        *fctx.out << "  br label %" << mergeLabel << "\n";
    }

    *fctx.out << elseLabel << ":\n";
    fctx.currentLabel = elseLabel;
    const bool elseTerminated = emitInstructions(branch.elseBlock, fctx);
    const std::string elseExitLabel = fctx.currentLabel;
    if (!elseTerminated)
    {
        *fctx.out << "  br label %" << mergeLabel << "\n";
    }

    if (thenTerminated && elseTerminated)
    {
        // Neither side falls through, so the merge block has no predecessors
        // and is unreachable. Still define it (every referenced label must
        // exist) with a trivial terminator so the module stays well-formed.
        *fctx.out << mergeLabel << ":\n";
        *fctx.out << "  unreachable\n";
        fctx.currentLabel = mergeLabel;
        return;
    }

    *fctx.out << mergeLabel << ":\n";
    fctx.currentLabel = mergeLabel;

    if (branch.dest != -1)
    {
        // A fallen-through side can still carry no value (branch.*Value ==
        // -1, e.g. an if-without-else's implicit unit else-branch) - only
        // sides that both reached the merge block *and* produced a real
        // register are valid phi predecessors.
        std::vector<std::pair<std::string, std::string>> incoming; // (value ref, predecessor label)
        if (!thenTerminated && branch.thenValue != -1)
        {
            incoming.emplace_back(ref(branch.thenValue, fctx), thenExitLabel);
        }
        if (!elseTerminated && branch.elseValue != -1)
        {
            incoming.emplace_back(ref(branch.elseValue, fctx), elseExitLabel);
        }

        if (!incoming.empty())
        {
            // A phi with a single incoming edge is valid LLVM IR, and unlike
            // `add`, phi works uniformly for pointer types too - so this
            // covers "both sides reach the merge" and "only one side does"
            // with the same code path.
            const int destReg = defineRegister(branch.dest, fctx);
            *fctx.out << "  %" << destReg << " = phi " << typeOf(branch.dest, fctx);
            for (std::size_t i = 0; i < incoming.size(); ++i)
            {
                *fctx.out << (i == 0 ? " [ " : ", [ ") << incoming[i].first << ", %"
                          << incoming[i].second << " ]";
            }
            *fctx.out << "\n";
        }
    }
}

bool LlvmIrEmitter::emitInstructions(const std::vector<std::unique_ptr<IrInst>>& instructions,
                                     FunctionContext& fctx)
{
    for (const auto& inst : instructions)
    {
        if (const auto* constInt = dynamic_cast<const IrConstInt*>(inst.get()))
        {
            // Constants are materialized as trivial SSA values (rather than
            // inlined at each use) so every Axea IR register stays uniformly
            // addressable as "%N" - real LLVM optimization passes fold these
            // away instantly (0022-llvm-backend.md: LLVM owns optimization).
            const int destReg = defineRegister(constInt->dest, fctx);
            *fctx.out << "  %" << destReg << " = add i32 0, " << constInt->value << "\n";
            continue;
        }
        if (const auto* constBool = dynamic_cast<const IrConstBool*>(inst.get()))
        {
            const int destReg = defineRegister(constBool->dest, fctx);
            *fctx.out << "  %" << destReg << " = add i1 0, " << (constBool->value ? 1 : 0) << "\n";
            continue;
        }
        if (const auto* constString = dynamic_cast<const IrConstString*>(inst.get()))
        {
            const std::string& globalName = stringGlobalByLiteral_.at(constString->value);
            const std::size_t length = constString->value.size() + 1;
            const int destReg = defineRegister(constString->dest, fctx);
            *fctx.out << "  %" << destReg << " = getelementptr [" << length << " x i8], [" << length
                      << " x i8]* " << globalName << ", i64 0, i64 0\n";
            continue;
        }
        if (const auto* binOp = dynamic_cast<const IrBinOp*>(inst.get()))
        {
            const std::string lhsType = typeOf(binOp->lhs, fctx);
            const std::string lhsRef = ref(binOp->lhs, fctx);
            const std::string rhsRef = ref(binOp->rhs, fctx);
            const int destReg = defineRegister(binOp->dest, fctx);
            *fctx.out << "  %" << destReg << " = " << binOpMnemonic(binOp->op) << " " << lhsType
                      << " " << lhsRef << ", " << rhsRef << "\n";
            continue;
        }
        if (const auto* call = dynamic_cast<const IrCall*>(inst.get()))
        {
            const std::string returnType = typeOf(call->dest, fctx);
            std::vector<std::string> argRefs;
            argRefs.reserve(call->args.size());
            for (int arg : call->args)
            {
                argRefs.push_back(ref(arg, fctx));
            }
            *fctx.out << "  ";
            if (returnType != "void")
            {
                const int destReg = defineRegister(call->dest, fctx);
                *fctx.out << "%" << destReg << " = ";
            }
            *fctx.out << "call " << returnType << " @" << call->callee << "(";
            for (std::size_t i = 0; i < call->args.size(); ++i)
            {
                *fctx.out << (i > 0 ? ", " : "") << typeOf(call->args[i], fctx) << " "
                          << argRefs[i];
            }
            *fctx.out << ")\n";
            continue;
        }
        if (const auto* structNew = dynamic_cast<const IrStructNew*>(inst.get()))
        {
            emitStructNew(*structNew, fctx);
            continue;
        }
        if (const auto* fieldGet = dynamic_cast<const IrFieldGet*>(inst.get()))
        {
            emitFieldGet(*fieldGet, fctx);
            continue;
        }
        if (const auto* fieldSet = dynamic_cast<const IrFieldSet*>(inst.get()))
        {
            emitFieldSet(*fieldSet, fctx);
            continue;
        }
        if (const auto* branch = dynamic_cast<const IrBranch*>(inst.get()))
        {
            emitBranch(*branch, fctx);
            if (alwaysTerminates(branch->thenBlock) && alwaysTerminates(branch->elseBlock))
            {
                // Both sides already terminated (the merge block is
                // `unreachable`, per emitBranch) - nothing after this in the
                // same list is reachable, and emitFunction must not append
                // its defensive trailing ret on top of it.
                return true;
            }
            continue;
        }
        if (const auto* returnInst = dynamic_cast<const IrReturn*>(inst.get()))
        {
            if (returnInst->value == -1)
            {
                *fctx.out << "  ret void\n";
            }
            else
            {
                *fctx.out << "  ret " << typeOf(returnInst->value, fctx) << " "
                          << ref(returnInst->value, fctx) << "\n";
            }
            return true;
        }
        // BorrowRead/BorrowWrite/Move/RegionEnter/RegionExit/Drop: informational
        // only in Axea IR (docs/language/0021-axea-ir.md) - no LLVM instruction.
    }
    return false;
}

void LlvmIrEmitter::emitFunction(const IrFunction& function, std::ostringstream& out)
{
    FunctionContext fctx;
    fctx.out = &out;

    inferTypes(function, fctx);

    // Parameters occupy the first N numbered SSA slots, in declaration
    // order, before anything in the body is emitted - matching how LLVM
    // itself numbers unnamed parameters.
    for (std::size_t i = 0; i < function.paramNames.size(); ++i)
    {
        defineRegister(static_cast<int>(i), fctx);
    }

    const std::string returnType = llvmReturnType(function.returnType);
    out << "define " << returnType << " @" << function.name << "(";
    for (std::size_t i = 0; i < function.paramNames.size(); ++i)
    {
        out << (i > 0 ? ", " : "") << llvmType(function.paramTypes[i]) << " "
            << ref(static_cast<int>(i), fctx);
    }
    out << ") {\n";
    out << "entry:\n";
    fctx.currentLabel = "entry";

    const bool terminated = emitInstructions(function.body, fctx);
    if (!terminated)
    {
        // Should not happen - IrGenerator always appends a trailing Return -
        // but stay well-formed defensively.
        out << (returnType == "void" ? "  ret void\n" : "  ret " + returnType + " undef\n");
    }

    out << "}\n\n";
}

std::string LlvmIrEmitter::emit(const IrProgram& program)
{
    structs_ = program.structs;
    for (const auto& function : program.functions)
    {
        functionReturnTypes_[function.name] = function.returnType;
    }

    for (const auto& function : program.functions)
    {
        collectStrings(function.body);
    }
    collectStrings(program.topLevel);

    std::ostringstream out;

    emitStructTypeDecls(out);
    out << "\ndeclare i8* @malloc(i64)\n\n";

    emitStringGlobals(out);
    if (!stringGlobals_.empty())
    {
        out << "\n";
    }

    for (const auto& function : program.functions)
    {
        emitFunction(function, out);
    }

    return out.str();
}
