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
    if (!axeaTypeName.empty() && axeaTypeName.front() == '[')
    {
        // "[elem;N]" - the canonical, no-spaces form Parser::parseTypeName
        // always produces (see docs/language/0031-arrays.md). No named type
        // declaration needed (unlike struct) - LLVM's anonymous array type is
        // used directly at every reference site.
        const auto semicolon = axeaTypeName.find(';');
        const auto closeBracket = axeaTypeName.rfind(']');
        const std::string elementName = axeaTypeName.substr(1, semicolon - 1);
        const std::string sizeText =
            axeaTypeName.substr(semicolon + 1, closeBracket - semicolon - 1);
        return "[" + sizeText + " x " + llvmType(elementName) + "]*";
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
        // IrBreak/IrContinue are terminators exactly like IrReturn here -
        // emitInstructions treats all three the same way (early "return
        // true"), so this static predicate must agree, or emitLoop's "did
        // the body fall through naturally" check disagrees with what
        // emitBranch/emitInstructions actually did dynamically, which can
        // leave a block with two terminators (e.g. `unreachable` from a
        // both-sides-break IrBranch, followed by emitLoop's own fallback
        // store+branch appended right after it).
        if (dynamic_cast<const IrReturn*>(inst.get()) || dynamic_cast<const IrBreak*>(inst.get()) ||
            dynamic_cast<const IrContinue*>(inst.get()))
        {
            return true;
        }
        if (const auto* branch = dynamic_cast<const IrBranch*>(inst.get());
            branch && alwaysTerminates(branch->thenBlock) && alwaysTerminates(branch->elseBlock))
        {
            return true;
        }
        if (const auto* loop = dynamic_cast<const IrLoop*>(inst.get());
            loop && loop->conditionBlock.empty() && loop->conditionValue == -1 &&
            !instructionsContainBreak(loop->body))
        {
            // Infinite loop with no way out via break - everything after it
            // in this list is unreachable, same as a Branch where both
            // sides return.
            return true;
        }
    }
    return false;
}

bool LlvmIrEmitter::instructionsContainBreak(
    const std::vector<std::unique_ptr<IrInst>>& instructions) const
{
    for (const auto& inst : instructions)
    {
        if (dynamic_cast<const IrBreak*>(inst.get()))
        {
            return true;
        }
        if (const auto* branch = dynamic_cast<const IrBranch*>(inst.get()))
        {
            if (instructionsContainBreak(branch->thenBlock) ||
                instructionsContainBreak(branch->elseBlock))
            {
                return true;
            }
        }
        // Deliberately not recursing into a nested IrLoop's own body - a
        // break there targets that inner loop, not this one.
    }
    return false;
}

int LlvmIrEmitter::findFirstBreakValue(
    const std::vector<std::unique_ptr<IrInst>>& instructions) const
{
    for (const auto& inst : instructions)
    {
        if (const auto* breakInst = dynamic_cast<const IrBreak*>(inst.get());
            breakInst && breakInst->value != -1)
        {
            return breakInst->value;
        }
        if (const auto* branch = dynamic_cast<const IrBranch*>(inst.get()))
        {
            const int thenResult = findFirstBreakValue(branch->thenBlock);
            if (thenResult != -1)
            {
                return thenResult;
            }
            const int elseResult = findFirstBreakValue(branch->elseBlock);
            if (elseResult != -1)
            {
                return elseResult;
            }
        }
    }
    return -1;
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

std::string LlvmIrEmitter::arrayElementType(const std::string& pointerType) const
{
    // "[N x T]*" - strip the leading "[N x " and trailing "]*".
    const auto xPos = pointerType.find(" x ");
    return pointerType.substr(xPos + 3, pointerType.size() - (xPos + 3) - 2);
}

int LlvmIrEmitter::arraySizeFromPointerType(const std::string& pointerType) const
{
    // "[N x T]*" - N is everything between the leading '[' and the first ' '.
    return std::stoi(pointerType.substr(1, pointerType.find(' ') - 1));
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
        else if (const auto* arrayNew = dynamic_cast<const IrArrayNew*>(inst.get()))
        {
            // Every element register is already type-inferred by this point
            // (elements are always lowered, and therefore appear earlier in
            // this same list, before the IrArrayNew that references them) -
            // TypeChecker already guarantees every element agrees, so the
            // first one is representative (same pattern as IrLoop's break
            // values below). See docs/language/0031-arrays.md.
            const std::string elementType = typeOf(arrayNew->elements.front(), fctx);
            fctx.registerTypes[arrayNew->dest] =
                "[" + std::to_string(arrayNew->elements.size()) + " x " + elementType + "]*";
        }
        else if (const auto* indexGet = dynamic_cast<const IrIndexGet*>(inst.get()))
        {
            fctx.registerTypes[indexGet->dest] = arrayElementType(typeOf(indexGet->object, fctx));
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
        else if (const auto* loop = dynamic_cast<const IrLoop*>(inst.get()))
        {
            inferTypesInList(loop->conditionBlock, fctx);
            inferTypesInList(loop->body, fctx);
            // The loop's own produced type comes from whatever its break
            // values produce - every reachable break within this loop is
            // already known, by TypeChecker's own unification rule, to
            // agree on type, so the first one found is representative. No
            // break anywhere => void (mirrors TypeChecker's own "no break =>
            // unit" imprecision).
            const int firstBreakValue = findFirstBreakValue(loop->body);
            fctx.registerTypes[loop->dest] =
                firstBreakValue != -1 ? typeOf(firstBreakValue, fctx) : "void";
        }
        // FieldSet, Return, Break, Continue, BorrowRead/BorrowWrite, Move,
        // RegionEnter/RegionExit, Drop: no dest.
    }
}

void LlvmIrEmitter::collectStrings(const std::vector<std::unique_ptr<IrInst>>& instructions)
{
    for (const auto& inst : instructions)
    {
        if (const auto* constString = dynamic_cast<const IrConstString*>(inst.get()))
        {
            hoistString(constString->value);
        }
        else if (const auto* branch = dynamic_cast<const IrBranch*>(inst.get()))
        {
            collectStrings(branch->thenBlock);
            collectStrings(branch->elseBlock);
        }
    }
}

std::string LlvmIrEmitter::hoistString(const std::string& text)
{
    if (!stringGlobalByLiteral_.contains(text))
    {
        const std::string globalName = "@.str." + std::to_string(nextGlobal_++);
        stringGlobalByLiteral_[text] = globalName;
        stringGlobals_.emplace_back(globalName, text);
    }
    return stringGlobalByLiteral_.at(text);
}

std::string LlvmIrEmitter::stringPtrConstant(const std::string& text)
{
    const std::string globalName = hoistString(text);
    const std::size_t length = text.size() + 1;
    return "getelementptr ([" + std::to_string(length) + " x i8], [" + std::to_string(length) +
           " x i8]* " + globalName + ", i64 0, i64 0)";
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

void LlvmIrEmitter::emitArrayNew(const IrArrayNew& arrayNew, FunctionContext& fctx)
{
    const std::string elementType = typeOf(arrayNew.elements.front(), fctx);
    const std::string llvmArrayType =
        "[" + std::to_string(arrayNew.elements.size()) + " x " + elementType + "]";
    const std::string pointerType = llvmArrayType + "*";

    // sizeof([N x T]) via the standard null-pointer GEP idiom - same idiom as
    // emitStructNew, avoiding hand-computed byte sizes.
    const int sizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizePtrReg << " = getelementptr " << llvmArrayType << ", " << pointerType
              << " null, i32 1\n";
    const int sizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizeIntReg << " = ptrtoint " << pointerType << " %" << sizePtrReg
              << " to i64\n";

    const int rawPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawPtrReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";

    const int destReg = defineRegister(arrayNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawPtrReg << " to " << pointerType
              << "\n";

    for (std::size_t i = 0; i < arrayNew.elements.size(); ++i)
    {
        const int elementPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << elementPtrReg << " = getelementptr " << llvmArrayType << ", "
                  << pointerType << " " << ref(arrayNew.dest, fctx) << ", i32 0, i32 " << i << "\n";
        *fctx.out << "  store " << elementType << " " << ref(arrayNew.elements[i], fctx) << ", "
                  << elementType << "* %" << elementPtrReg << "\n";
    }
}

void LlvmIrEmitter::emitIndexGet(const IrIndexGet& indexGet, FunctionContext& fctx)
{
    const std::string objectType = typeOf(indexGet.object, fctx);
    const std::string llvmArrayType =
        objectType.substr(0, objectType.size() - 1); // strip trailing '*'
    const std::string elementType = arrayElementType(objectType);

    const int elementPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << elementPtrReg << " = getelementptr " << llvmArrayType << ", "
              << objectType << " " << ref(indexGet.object, fctx) << ", i32 0, i32 "
              << ref(indexGet.index, fctx) << "\n";
    const int destReg = defineRegister(indexGet.dest, fctx);
    *fctx.out << "  %" << destReg << " = load " << elementType << ", " << elementType << "* %"
              << elementPtrReg << "\n";
}

void LlvmIrEmitter::emitIndexSet(const IrIndexSet& indexSet, FunctionContext& fctx)
{
    const std::string objectType = typeOf(indexSet.object, fctx);
    const std::string llvmArrayType =
        objectType.substr(0, objectType.size() - 1); // strip trailing '*'
    const std::string elementType = arrayElementType(objectType);

    const int elementPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << elementPtrReg << " = getelementptr " << llvmArrayType << ", "
              << objectType << " " << ref(indexSet.object, fctx) << ", i32 0, i32 "
              << ref(indexSet.index, fctx) << "\n";
    *fctx.out << "  store " << elementType << " " << ref(indexSet.value, fctx) << ", "
              << elementType << "* %" << elementPtrReg << "\n";
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

void LlvmIrEmitter::storeCarriedValues(const std::vector<std::pair<int, int>>& carried,
                                       const FunctionContext::LoopEmitContext& loopCtx,
                                       FunctionContext& fctx)
{
    for (const auto& [preLoopReg, currentReg] : carried)
    {
        for (const auto& [slotPreLoopReg, slotBodyEndReg, slotReg] : loopCtx.carriedSlots)
        {
            if (slotPreLoopReg == preLoopReg)
            {
                const std::string llvmTypeStr = typeOf(preLoopReg, fctx);
                *fctx.out << "  store " << llvmTypeStr << " " << ref(currentReg, fctx) << ", "
                          << llvmTypeStr << "* %" << slotReg << "\n";
                break;
            }
        }
    }
}

void LlvmIrEmitter::emitLoop(const IrLoop& loop, FunctionContext& fctx)
{
    const int labelId = fctx.nextLabel++;
    const std::string headerLabel = "loop.header" + std::to_string(labelId);
    const std::string bodyLabel = "loop.body" + std::to_string(labelId);
    const std::string exitLabel = "loop.exit" + std::to_string(labelId);
    const bool hasCondition = !loop.conditionBlock.empty() || loop.conditionValue != -1;

    FunctionContext::LoopEmitContext loopCtx;
    loopCtx.headerLabel = headerLabel;
    loopCtx.exitLabel = exitLabel;

    // Allocate + initialize a stack slot for each carried variable, storing
    // its pre-loop value, before ever entering the loop.
    for (const auto& [preLoopReg, bodyEndReg] : loop.carried)
    {
        const std::string llvmTypeStr = typeOf(preLoopReg, fctx);
        const int slotReg = allocateRegister(fctx);
        *fctx.out << "  %" << slotReg << " = alloca " << llvmTypeStr << "\n";
        *fctx.out << "  store " << llvmTypeStr << " " << ref(preLoopReg, fctx) << ", "
                  << llvmTypeStr << "* %" << slotReg << "\n";
        loopCtx.carriedSlots.emplace_back(preLoopReg, bodyEndReg, slotReg);
    }

    *fctx.out << "  br label %" << headerLabel << "\n";
    *fctx.out << headerLabel << ":\n";
    fctx.currentLabel = headerLabel;

    // Re-read every iteration (this block re-executes each time around) and
    // rebind the pre-loop register to see it, so any reference within
    // conditionBlock/body transparently sees the current value via the
    // existing ref()/llvmRegisterOf machinery.
    for (const auto& [preLoopReg, bodyEndReg, slotReg] : loopCtx.carriedSlots)
    {
        const std::string llvmTypeStr = typeOf(preLoopReg, fctx);
        const int loadReg = allocateRegister(fctx);
        *fctx.out << "  %" << loadReg << " = load " << llvmTypeStr << ", " << llvmTypeStr << "* %"
                  << slotReg << "\n";
        fctx.llvmRegisterOf[preLoopReg] = loadReg;
    }

    fctx.loopStack.push_back(loopCtx);

    if (hasCondition)
    {
        emitInstructions(loop.conditionBlock, fctx);
        *fctx.out << "  br i1 " << ref(loop.conditionValue, fctx) << ", label %" << bodyLabel
                  << ", label %" << exitLabel << "\n";
    }
    else
    {
        *fctx.out << "  br label %" << bodyLabel << "\n";
    }

    *fctx.out << bodyLabel << ":\n";
    fctx.currentLabel = bodyLabel;
    const bool bodyTerminated = emitInstructions(loop.body, fctx);
    if (!bodyTerminated)
    {
        // Fell off the natural end of the body (no break/continue/return
        // fired) - store each carried variable's end-of-body value and loop
        // back to re-check the header.
        storeCarriedValues(loop.carried, fctx.loopStack.back(), fctx);
        *fctx.out << "  br label %" << headerLabel << "\n";
    }

    const FunctionContext::LoopEmitContext loopCtxFinal = fctx.loopStack.back();
    fctx.loopStack.pop_back();

    *fctx.out << exitLabel << ":\n";
    fctx.currentLabel = exitLabel;

    if (!hasCondition && !loopCtxFinal.anyBreakSeen)
    {
        // Infinite loop, never broken out of - the exit block has no
        // predecessors at all. Still define it (every referenced label must
        // exist) with a trivial terminator, same as emitBranch's "both sides
        // terminate" case.
        *fctx.out << "  unreachable\n";
        return;
    }

    // Re-read every carried slot once more here so code *after* the loop
    // (which references each carried variable via its body-end Axea
    // register) sees the correct final value, regardless of which path
    // reached the exit (natural condition-false, or any break) - the slot is
    // always current by the time control gets here.
    for (const auto& [preLoopReg, bodyEndReg, slotReg] : loopCtxFinal.carriedSlots)
    {
        const std::string llvmTypeStr = typeOf(preLoopReg, fctx);
        const int loadReg = allocateRegister(fctx);
        *fctx.out << "  %" << loadReg << " = load " << llvmTypeStr << ", " << llvmTypeStr << "* %"
                  << slotReg << "\n";
        fctx.llvmRegisterOf[bodyEndReg] = loadReg;
    }

    if (loop.dest != -1 && !loopCtxFinal.breakValues.empty())
    {
        // Generalizes emitBranch's merge-phi to however many break sites
        // exist (a `loop` can have any number, not just two).
        const int destReg = defineRegister(loop.dest, fctx);
        *fctx.out << "  %" << destReg << " = phi " << typeOf(loop.dest, fctx);
        for (std::size_t i = 0; i < loopCtxFinal.breakValues.size(); ++i)
        {
            *fctx.out << (i == 0 ? " [ " : ", [ ") << loopCtxFinal.breakValues[i].first << ", %"
                      << loopCtxFinal.breakValues[i].second << " ]";
        }
        *fctx.out << "\n";
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
        if (const auto* arrayNew = dynamic_cast<const IrArrayNew*>(inst.get()))
        {
            emitArrayNew(*arrayNew, fctx);
            continue;
        }
        if (const auto* indexGet = dynamic_cast<const IrIndexGet*>(inst.get()))
        {
            emitIndexGet(*indexGet, fctx);
            continue;
        }
        if (const auto* indexSet = dynamic_cast<const IrIndexSet*>(inst.get()))
        {
            emitIndexSet(*indexSet, fctx);
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
        if (const auto* loop = dynamic_cast<const IrLoop*>(inst.get()))
        {
            emitLoop(*loop, fctx);
            const bool hasCondition = !loop->conditionBlock.empty() || loop->conditionValue != -1;
            if (!hasCondition && !instructionsContainBreak(loop->body))
            {
                // Infinite loop, no way out - nothing after it in the same
                // list is reachable (mirrors the IrBranch case above).
                return true;
            }
            continue;
        }
        if (const auto* breakInst = dynamic_cast<const IrBreak*>(inst.get()))
        {
            FunctionContext::LoopEmitContext& loopCtx = fctx.loopStack.back();
            storeCarriedValues(breakInst->carried, loopCtx, fctx);
            loopCtx.anyBreakSeen = true;
            if (breakInst->value != -1)
            {
                loopCtx.breakValues.emplace_back(ref(breakInst->value, fctx), fctx.currentLabel);
            }
            *fctx.out << "  br label %" << loopCtx.exitLabel << "\n";
            return true;
        }
        if (const auto* continueInst = dynamic_cast<const IrContinue*>(inst.get()))
        {
            const FunctionContext::LoopEmitContext& loopCtx = fctx.loopStack.back();
            storeCarriedValues(continueInst->carried, loopCtx, fctx);
            *fctx.out << "  br label %" << loopCtx.headerLabel << "\n";
            return true;
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

void LlvmIrEmitter::emitStructPrintHelpers(const IrProgram& program, std::ostringstream& out)
{
    // Shared across every struct's helper - deduped by hoistString either way,
    // computed once up front purely to keep the loop body readable.
    const std::string separator = stringPtrConstant(", ");
    const std::string closeBrace = stringPtrConstant(" }");
    const std::string bareIntFmt = stringPtrConstant("%d");
    const std::string bareStrFmt = stringPtrConstant("%s");
    const std::string trueStr = stringPtrConstant("true");
    const std::string falseStr = stringPtrConstant("false");

    for (const auto& [name, fields] : program.structs)
    {
        const std::string pointerType = "%" + name + "*";
        out << "define void @axea.print." << name << "(" << pointerType << " %0) {\n";
        out << "entry:\n";
        int nextReg = 1; // %0 is the incoming pointer param

        // A `call` to a non-void function - @printf returns i32 - implicitly
        // consumes the next numbered SSA slot even when its result is
        // discarded, exactly like a named instruction would; LLVM's parser
        // rejects a later explicit "%N = ..." that doesn't account for it.
        // So every printf call here is captured into a (deliberately unused)
        // register, keeping `nextReg` the single source of truth.
        out << "  %" << nextReg++ << " = call i32 (i8*, ...) @printf(i8* "
            << stringPtrConstant(name + " { ") << ")\n";

        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            const auto& [fieldName, fieldTypeName] = fields[i];
            if (i > 0)
            {
                out << "  %" << nextReg++ << " = call i32 (i8*, ...) @printf(i8* " << separator
                    << ")\n";
            }
            out << "  %" << nextReg++ << " = call i32 (i8*, ...) @printf(i8* "
                << stringPtrConstant(fieldName + ": ") << ")\n";

            const std::string fieldLlvmType = llvmType(fieldTypeName);
            const int ptrReg = nextReg++;
            out << "  %" << ptrReg << " = getelementptr %" << name << ", " << pointerType << " %0, "
                << "i32 0, i32 " << i << "\n";
            const int valReg = nextReg++;
            out << "  %" << valReg << " = load " << fieldLlvmType << ", " << fieldLlvmType << "* %"
                << ptrReg << "\n";

            if (fieldLlvmType == "i32")
            {
                out << "  %" << nextReg++ << " = call i32 (i8*, ...) @printf(i8* " << bareIntFmt
                    << ", i32 %" << valReg << ")\n";
            }
            else if (fieldLlvmType == "i1")
            {
                const int selReg = nextReg++;
                out << "  %" << selReg << " = select i1 %" << valReg << ", i8* " << trueStr
                    << ", i8* " << falseStr << "\n";
                out << "  %" << nextReg++ << " = call i32 (i8*, ...) @printf(i8* " << bareStrFmt
                    << ", i8* %" << selReg << ")\n";
            }
            else if (fieldLlvmType == "i8*")
            {
                out << "  %" << nextReg++ << " = call i32 (i8*, ...) @printf(i8* " << bareStrFmt
                    << ", i8* %" << valReg << ")\n";
            }
            else // nested struct pointer
            {
                const std::string nestedStructName = structNameFromPointerType(fieldLlvmType);
                out << "  call void @axea.print." << nestedStructName << "(" << fieldLlvmType
                    << " %" << valReg << ")\n";
            }
        }

        out << "  %" << nextReg++ << " = call i32 (i8*, ...) @printf(i8* " << closeBrace << ")\n";
        out << "  ret void\n";
        out << "}\n\n";
    }
}

void LlvmIrEmitter::emitMain(const IrProgram& program, std::ostringstream& out)
{
    FunctionContext fctx;
    fctx.out = &out;
    inferTypesInList(program.topLevel, fctx);

    out << "define i32 @main() {\n";
    out << "entry:\n";
    fctx.currentLabel = "entry";

    emitInstructions(program.topLevel, fctx);

    const std::string intFmt = stringPtrConstant("%s = %d\n");
    const std::string strFmt = stringPtrConstant("%s = %s\n");
    const std::string structPrefixFmt = stringPtrConstant("%s = ");
    const std::string newline = stringPtrConstant("\n");
    const std::string trueStr = stringPtrConstant("true");
    const std::string falseStr = stringPtrConstant("false");
    const std::string unitStr = stringPtrConstant("()");

    for (const auto& [name, axeaReg] : program.topLevelBindings)
    {
        const std::string namePtr = stringPtrConstant(name);
        const std::string llvmTypeStr = typeOf(axeaReg, fctx);

        // See emitStructPrintHelpers: a discarded-result call to a non-void
        // function (printf returns i32) still consumes a numbered SSA slot,
        // so every printf call below is captured via allocateRegister even
        // though nothing ever reads it back.
        if (llvmTypeStr == "i32")
        {
            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                << intFmt << ", i8* " << namePtr << ", i32 " << ref(axeaReg, fctx) << ")\n";
        }
        else if (llvmTypeStr == "i1")
        {
            const int selReg = allocateRegister(fctx);
            out << "  %" << selReg << " = select i1 " << ref(axeaReg, fctx) << ", i8* " << trueStr
                << ", i8* " << falseStr << "\n";
            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                << strFmt << ", i8* " << namePtr << ", i8* %" << selReg << ")\n";
        }
        else if (llvmTypeStr == "i8*")
        {
            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                << strFmt << ", i8* " << namePtr << ", i8* " << ref(axeaReg, fctx) << ")\n";
        }
        else if (llvmTypeStr == "void")
        {
            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                << strFmt << ", i8* " << namePtr << ", i8* " << unitStr << ")\n";
        }
        else if (!llvmTypeStr.empty() && llvmTypeStr.front() == '[')
        {
            // Fixed array (see docs/language/0031-arrays.md): unlike struct,
            // there's no named per-shape helper function to call (arrays are
            // anonymous LLVM types) - the element count is statically known
            // right here, so the N element loads/prints are unrolled inline
            // instead, using the same per-element-type branching
            // emitStructPrintHelpers uses for struct fields.
            const std::string elementType = arrayElementType(llvmTypeStr);
            const int size = arraySizeFromPointerType(llvmTypeStr);
            const std::string llvmArrayType =
                llvmTypeStr.substr(0, llvmTypeStr.size() - 1); // strip trailing '*'
            const std::string openBracket = stringPtrConstant("[");
            const std::string closeBracket = stringPtrConstant("]");
            const std::string comma = stringPtrConstant(", ");
            const std::string bareIntFmt = stringPtrConstant("%d");
            const std::string bareStrFmt = stringPtrConstant("%s");

            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                << structPrefixFmt << ", i8* " << namePtr << ")\n";
            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                << openBracket << ")\n";

            for (int i = 0; i < size; ++i)
            {
                if (i > 0)
                {
                    out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                        << comma << ")\n";
                }

                const int elementPtrReg = allocateRegister(fctx);
                out << "  %" << elementPtrReg << " = getelementptr " << llvmArrayType << ", "
                    << llvmTypeStr << " " << ref(axeaReg, fctx) << ", i32 0, i32 " << i << "\n";
                const int elementValReg = allocateRegister(fctx);
                out << "  %" << elementValReg << " = load " << elementType << ", " << elementType
                    << "* %" << elementPtrReg << "\n";

                if (elementType == "i32")
                {
                    out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                        << bareIntFmt << ", i32 %" << elementValReg << ")\n";
                }
                else if (elementType == "i1")
                {
                    const int selReg = allocateRegister(fctx);
                    out << "  %" << selReg << " = select i1 %" << elementValReg << ", i8* "
                        << trueStr << ", i8* " << falseStr << "\n";
                    out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                        << bareStrFmt << ", i8* %" << selReg << ")\n";
                }
                else if (elementType == "i8*")
                {
                    out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                        << bareStrFmt << ", i8* %" << elementValReg << ")\n";
                }
                else // nested struct pointer
                {
                    const std::string nestedStructName = structNameFromPointerType(elementType);
                    out << "  call void @axea.print." << nestedStructName << "(" << elementType
                        << " %" << elementValReg << ")\n";
                }
            }

            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                << closeBracket << ")\n";
            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                << newline << ")\n";
        }
        else // struct pointer
        {
            const std::string structName = structNameFromPointerType(llvmTypeStr);
            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                << structPrefixFmt << ", i8* " << namePtr << ")\n";
            out << "  call void @axea.print." << structName << "(" << llvmTypeStr << " "
                << ref(axeaReg, fctx) << ")\n";
            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                << newline << ")\n";
        }
    }

    out << "  ret i32 0\n";
    out << "}\n";
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

    // Built into their own buffers first so any synthetic strings they need
    // (format strings, punctuation, binding/field names) get hoisted into
    // stringGlobals_ before the globals block below is emitted - LLVM
    // doesn't require forward-reference ordering for globals or calls, this
    // just keeps every string constant declared in one place for readability.
    std::ostringstream helpers;
    emitStructPrintHelpers(program, helpers);
    std::ostringstream mainOut;
    emitMain(program, mainOut);

    std::ostringstream out;

    emitStructTypeDecls(out);
    out << "\ndeclare i8* @malloc(i64)\n";
    out << "declare i32 @printf(i8*, ...)\n\n";

    emitStringGlobals(out);
    if (!stringGlobals_.empty())
    {
        out << "\n";
    }

    for (const auto& function : program.functions)
    {
        emitFunction(function, out);
    }

    out << helpers.str();
    out << mainOut.str();

    return out.str();
}
