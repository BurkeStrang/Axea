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

    // Finds the first *top-level* comma in `text` - i.e. one not nested
    // inside a further `<...>`/`[...]` type argument list (e.g. a Map value
    // type that's itself `Map<i32,i32>`, or `[i32;4]`'s own bracket pair).
    // Own copy, per this codebase's "each pass owns its own walk" convention
    // - TypeChecker and RegionChecker each have their own identical logic
    // for the same reason (see docs/language/0034-maps-and-sets.md's generic
    // rewrite).
    std::size_t findTopLevelComma(const std::string& text)
    {
        int depth = 0;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '<' || text[i] == '[')
            {
                ++depth;
            }
            else if (text[i] == '>' || text[i] == ']')
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
} // namespace

std::string LlvmIrEmitter::llvmType(const std::string& axeaTypeName)
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
    if (axeaTypeName.starts_with("slice<"))
    {
        // "slice<elem>" - the canonical form Parser::parseTypeName always
        // produces (see docs/language/0032-slices.md). A "fat pointer":
        // pointer + length, passed by value as an anonymous LLVM struct -
        // hand-verified against the real clang toolchain (at both -O0 and
        // -O1) before this design was committed to, since nothing in this
        // codebase had passed a struct by value before (structs are always
        // by pointer).
        const std::string elementName = axeaTypeName.substr(6, axeaTypeName.size() - 7);
        return "{" + llvmType(elementName) + "*, i32}";
    }
    if (axeaTypeName.starts_with("List<"))
    {
        // "List<elem>" - the canonical form Parser::parseTypeName always
        // produces (see docs/language/0033-lists.md). A *pointer* to an
        // anonymous 2-field heap record {length, data} - unlike slice<T>,
        // List<T> genuinely owns its storage (a stable heap pointer, mutated
        // in place on push/pop), so it follows struct/array's "always by
        // pointer" convention, not slice's by-value one.
        const std::string elementName = axeaTypeName.substr(5, axeaTypeName.size() - 6);
        return "{i32, " + llvmType(elementName) + "*}*";
    }
    if (axeaTypeName.starts_with("Stack<"))
    {
        // "Stack<elem>" - a LIFO collection backed internally by List<T>'s
        // own machinery (see docs/language/0035-stacks.md), so this
        // produces the *exact same text* the List<elem> branch above would
        // - genuinely the same LLVM type, not merely a similarly-shaped one.
        // The distinction between List<T> and Stack<T> lives entirely at
        // the Axea/IR level (TypeKind::Stack, IrStackNew/Push/Pop/Peek);
        // nothing downstream of a bare LLVM type string needs to (or can)
        // tell them apart, which is exactly why isListType's own existing
        // checks (`.length`, top-level printing) already handle a Stack<T>
        // value correctly with no changes at all.
        const std::string elementName = axeaTypeName.substr(6, axeaTypeName.size() - 7);
        return "{i32, " + llvmType(elementName) + "*}*";
    }
    if (axeaTypeName.starts_with("Map<"))
    {
        // Map<K,V> (see docs/language/0034-maps-and-sets.md's generic
        // rewrite): a pointer to a small anonymous 3-field heap header
        // {count, bucketCount, buckets}, mirroring List's own "always by
        // pointer, mutated in place" header. Each distinct (K,V) pair gets
        // its own named, numbered entry type (%axea.MapEntry.<id>, declared
        // by registerMapInstantiation) - named, not anonymous, because its
        // own self-reference (its `next` field points to another
        // %axea.MapEntry.<id>) can only be expressed in LLVM through a named
        // type, the one thing every other collection here avoided needing.
        const std::string args = axeaTypeName.substr(4, axeaTypeName.size() - 5);
        const auto comma = findTopLevelComma(args);
        return registerMapInstantiation(args.substr(0, comma), args.substr(comma + 1));
    }
    if (axeaTypeName.starts_with("Set<"))
    {
        // Set<T> - same reasoning as Map<K,V> above, with %axea.SetEntry.<id>
        // (key, next - no value field) in place of %axea.MapEntry.<id>.
        return registerSetInstantiation(axeaTypeName.substr(4, axeaTypeName.size() - 5));
    }
    return "%" + axeaTypeName + "*"; // struct type: always by pointer
}

std::string LlvmIrEmitter::llvmReturnType(const std::optional<std::string>& returnType)
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

std::pair<std::size_t, std::string> LlvmIrEmitter::fieldIndexAndType(const std::string& structName,
                                                                     const std::string& fieldName)
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

bool LlvmIrEmitter::isSliceType(const std::string& type) const
{
    // A slice is "{...}" passed *by value* - no trailing '*'. A List is
    // "{...}*", a *pointer* to a small heap record (see isListType) - the
    // trailing '*' is what tells the two apart, both starting with '{'.
    return !type.empty() && type.front() == '{' && type.back() != '*';
}

std::string LlvmIrEmitter::sliceElementType(const std::string& type) const
{
    // "{T*, i32}" - strip the leading '{' and trailing "*, i32}".
    return type.substr(1, type.size() - 1 - std::string("*, i32}").size());
}

bool LlvmIrEmitter::isListType(const std::string& type) const
{
    return !type.empty() && type.front() == '{' && type.back() == '*';
}

std::string LlvmIrEmitter::listElementType(const std::string& type) const
{
    // "{i32, T*}*" - strip the leading "{i32, " and trailing "*}*".
    return type.substr(6, type.size() - 6 - std::string("*}*").size());
}

bool LlvmIrEmitter::isMapType(const std::string& type) const
{
    return type.starts_with("{i32, i32, %axea.MapEntry.") && type.ends_with("**}*");
}

bool LlvmIrEmitter::isSetType(const std::string& type) const
{
    return type.starts_with("{i32, i32, %axea.SetEntry.") && type.ends_with("**}*");
}

std::string LlvmIrEmitter::mapSetInstantiationId(const std::string& type) const
{
    // "{i32, i32, %axea.MapEntry.<id>**}*"/"...SetEntry.<id>**}*" - the id
    // sits between the last '.' and the trailing "**}*".
    const auto lastDot = type.rfind('.');
    return type.substr(lastDot + 1, type.size() - lastDot - 1 - std::string("**}*").size());
}

std::string LlvmIrEmitter::mapValueLlvmType(const std::string& mapHeaderType) const
{
    return mapValueLlvmTypeById_.at(std::stoi(mapSetInstantiationId(mapHeaderType)));
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
            if (isSliceType(objectType) || isListType(objectType) || isMapType(objectType) ||
                isSetType(objectType))
            {
                // Only "length" ever reaches a slice, List, Map, or Set via
                // IrFieldGet - TypeChecker already guarantees this (see
                // docs/language/0032-slices.md, docs/language/0033-lists.md,
                // docs/language/0034-maps-and-sets.md).
                fctx.registerTypes[fieldGet->dest] = "i32";
            }
            else
            {
                const std::string structName = structNameFromPointerType(objectType);
                fctx.registerTypes[fieldGet->dest] =
                    fieldIndexAndType(structName, fieldGet->field).second;
            }
        }
        else if (const auto* listNew = dynamic_cast<const IrListNew*>(inst.get()))
        {
            fctx.registerTypes[listNew->dest] =
                "{i32, " + llvmType(listNew->elementTypeName) + "*}*";
        }
        else if (const auto* listPush = dynamic_cast<const IrListPush*>(inst.get()))
        {
            // Unit-typed (see docs/language/0033-lists.md) - mirrors how a
            // unit-returning IrCall is typed "void" here too.
            fctx.registerTypes[listPush->dest] = "void";
        }
        else if (const auto* listPop = dynamic_cast<const IrListPop*>(inst.get()))
        {
            fctx.registerTypes[listPop->dest] = listElementType(typeOf(listPop->list, fctx));
        }
        else if (const auto* stackNew = dynamic_cast<const IrStackNew*>(inst.get()))
        {
            // Same shape llvmType("Stack<T>") itself produces (see
            // docs/language/0035-stacks.md) - the exact same text
            // llvmType("List<T>") would, since Stack<T> is the same LLVM
            // type as List<T>.
            fctx.registerTypes[stackNew->dest] =
                "{i32, " + llvmType(stackNew->elementTypeName) + "*}*";
        }
        else if (const auto* stackPush = dynamic_cast<const IrStackPush*>(inst.get()))
        {
            fctx.registerTypes[stackPush->dest] = "void";
        }
        else if (const auto* stackPop = dynamic_cast<const IrStackPop*>(inst.get()))
        {
            fctx.registerTypes[stackPop->dest] = listElementType(typeOf(stackPop->stack, fctx));
        }
        else if (const auto* stackPeek = dynamic_cast<const IrStackPeek*>(inst.get()))
        {
            fctx.registerTypes[stackPeek->dest] = listElementType(typeOf(stackPeek->stack, fctx));
        }
        else if (const auto* mapNew = dynamic_cast<const IrMapNew*>(inst.get()))
        {
            // Drives registration (see registerMapInstantiation) - this is
            // what guarantees every instantiation is known by the time
            // emit() appends mapSetTypeDeclsText_/mapSetRuntimeText_.
            fctx.registerTypes[mapNew->dest] =
                llvmType("Map<" + mapNew->keyTypeName + "," + mapNew->valueTypeName + ">");
        }
        else if (const auto* setNew = dynamic_cast<const IrSetNew*>(inst.get()))
        {
            fctx.registerTypes[setNew->dest] = llvmType("Set<" + setNew->elementTypeName + ">");
        }
        else if (dynamic_cast<const IrMapSet*>(inst.get()) ||
                 dynamic_cast<const IrMapRemove*>(inst.get()) ||
                 dynamic_cast<const IrSetAdd*>(inst.get()) ||
                 dynamic_cast<const IrSetRemove*>(inst.get()))
        {
            // Unit-typed, same reasoning as IrListPush above.
            fctx.registerTypes[inst->dest] = "void";
        }
        else if (const auto* mapGet = dynamic_cast<const IrMapGet*>(inst.get()))
        {
            fctx.registerTypes[mapGet->dest] = mapValueLlvmType(typeOf(mapGet->map, fctx));
        }
        else if (dynamic_cast<const IrMapContains*>(inst.get()) ||
                 dynamic_cast<const IrSetContains*>(inst.get()))
        {
            fctx.registerTypes[inst->dest] = "i1";
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
            const std::string objectType = typeOf(indexGet->object, fctx);
            if (isSliceType(objectType))
            {
                fctx.registerTypes[indexGet->dest] = sliceElementType(objectType);
            }
            else if (isListType(objectType))
            {
                fctx.registerTypes[indexGet->dest] = listElementType(objectType);
            }
            else
            {
                fctx.registerTypes[indexGet->dest] = arrayElementType(objectType);
            }
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

void LlvmIrEmitter::emitStructTypeDecls(std::ostringstream& out)
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

namespace
{
    // Substitutes every occurrence of each `<<TOKEN>>` placeholder with its
    // replacement text, longest-token-first (defensive against one token
    // being a substring of another, though none of the tokens actually used
    // below collide this way). Shared by registerMapInstantiation/
    // registerSetInstantiation to turn the hand-verified template text (see
    // docs/language/0034-maps-and-sets.md) into a concrete instantiation's
    // runtime functions.
    std::string fillTemplate(std::string text,
                             const std::vector<std::pair<std::string, std::string>>& substitutions)
    {
        for (const auto& [token, value] : substitutions)
        {
            std::size_t pos = 0;
            while ((pos = text.find(token, pos)) != std::string::npos)
            {
                text.replace(pos, token.size(), value);
                pos += value.size();
            }
        }
        return text;
    }

    // The not-found sentinel Map<K,V>.get returns in compiled code (the
    // interpreter throws instead - see docs/language/0034-maps-and-sets.md).
    // Depends on V's own LLVM shape: i32's own min-int, i1's false, or null
    // for every pointer-shaped V (str/struct/array/List/Map/Set).
    std::string sentinelFor(const std::string& valueLlvmType)
    {
        if (valueLlvmType == "i32")
        {
            return "-2147483648";
        }
        if (valueLlvmType == "i1")
        {
            return "0";
        }
        return "null"; // every other valid V is pointer-shaped
    }

    const char* const kMapResizeTemplate = R"(
define void @axea.<<KIND>>.<<ID>>.resize(<<HEADERPTR>> %h) {
entry:
  %cptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 0
  %bcptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %bptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 2
  %oldBucketCount = load i32, i32* %bcptr
  %oldBuckets = load <<ENTRYPTRPTR>>, <<ENTRYPTRPTRPTR>> %bptr
  %newBucketCount = mul i32 %oldBucketCount, 2
  %sizePtr = getelementptr <<ENTRYPTR>>, <<ENTRYPTRPTR>> null, i32 1
  %elemSize = ptrtoint <<ENTRYPTRPTR>> %sizePtr to i64
  %newBucketCount64 = zext i32 %newBucketCount to i64
  %newBytes = mul i64 %newBucketCount64, %elemSize
  %rawNew = call i8* @malloc(i64 %newBytes)
  %newBuckets = bitcast i8* %rawNew to <<ENTRYPTRPTR>>
  %zi = alloca i32
  store i32 0, i32* %zi
  br label %zero.header
zero.header:
  %z0 = load i32, i32* %zi
  %z1 = icmp slt i32 %z0, %newBucketCount
  br i1 %z1, label %zero.body, label %zero.done
zero.body:
  %z2 = load i32, i32* %zi
  %z3 = getelementptr <<ENTRYPTR>>, <<ENTRYPTRPTR>> %newBuckets, i32 %z2
  store <<ENTRYPTR>> null, <<ENTRYPTRPTR>> %z3
  %z4 = add i32 %z2, 1
  store i32 %z4, i32* %zi
  br label %zero.header
zero.done:
  %oi = alloca i32
  store i32 0, i32* %oi
  br label %outer.header
outer.header:
  %o0 = load i32, i32* %oi
  %o1 = icmp slt i32 %o0, %oldBucketCount
  br i1 %o1, label %outer.body, label %outer.done
outer.body:
  %o2 = load i32, i32* %oi
  %o3 = getelementptr <<ENTRYPTR>>, <<ENTRYPTRPTR>> %oldBuckets, i32 %o2
  %o4 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %o3
  %cur = alloca <<ENTRYPTR>>
  store <<ENTRYPTR>> %o4, <<ENTRYPTRPTR>> %cur
  br label %inner.header
inner.header:
  %i0 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %cur
  %i1 = icmp eq <<ENTRYPTR>> %i0, null
  br i1 %i1, label %inner.done, label %inner.body
inner.body:
  %i2 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %i0, i32 0, i32 <<NEXTIDX>>
  %i3 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %i2
  %i4 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %i0, i32 0, i32 0
  %i5 = load <<KEYTYPE>>, <<KEYTYPEPTR>> %i4
  %i6 = call i32 <<HASHFN>>(<<KEYTYPE>> %i5)
  %i7 = sub i32 %newBucketCount, 1
  %i8 = and i32 %i6, %i7
  %i9 = getelementptr <<ENTRYPTR>>, <<ENTRYPTRPTR>> %newBuckets, i32 %i8
  %i10 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %i9
  store <<ENTRYPTR>> %i10, <<ENTRYPTRPTR>> %i2
  store <<ENTRYPTR>> %i0, <<ENTRYPTRPTR>> %i9
  store <<ENTRYPTR>> %i3, <<ENTRYPTRPTR>> %cur
  br label %inner.header
inner.done:
  %o5 = load i32, i32* %oi
  %o6 = add i32 %o5, 1
  store i32 %o6, i32* %oi
  br label %outer.header
outer.done:
  store i32 %newBucketCount, i32* %bcptr
  store <<ENTRYPTRPTR>> %newBuckets, <<ENTRYPTRPTRPTR>> %bptr
  ret void
}
)";

    const char* const kMapSetTemplate = R"(
define void @axea.map.<<ID>>.set(<<HEADERPTR>> %h, <<KEYTYPE>> %key, <<VALUETYPE>> %value) {
entry:
  %cptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 0
  %bcptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %bptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 2
  %bucketCount = load i32, i32* %bcptr
  %buckets = load <<ENTRYPTRPTR>>, <<ENTRYPTRPTRPTR>> %bptr
  %hv = call i32 <<HASHFN>>(<<KEYTYPE>> %key)
  %mask = sub i32 %bucketCount, 1
  %bucketIdx = and i32 %hv, %mask
  %bucketSlot = getelementptr <<ENTRYPTR>>, <<ENTRYPTRPTR>> %buckets, i32 %bucketIdx
  %head = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %bucketSlot
  %cur = alloca <<ENTRYPTR>>
  store <<ENTRYPTR>> %head, <<ENTRYPTRPTR>> %cur
  br label %search.header
search.header:
  %c0 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %cur
  %c1 = icmp eq <<ENTRYPTR>> %c0, null
  br i1 %c1, label %insert, label %search.check
search.check:
  %c2 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 0
  %c3 = load <<KEYTYPE>>, <<KEYTYPEPTR>> %c2
  %c4 = call i1 <<EQFN>>(<<KEYTYPE>> %c3, <<KEYTYPE>> %key)
  br i1 %c4, label %update, label %search.next
search.next:
  %c5 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 <<NEXTIDX>>
  %c6 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %c5
  store <<ENTRYPTR>> %c6, <<ENTRYPTRPTR>> %cur
  br label %search.header
update:
  %c7 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 1
  store <<VALUETYPE>> %value, <<VALUETYPEPTR>> %c7
  ret void
insert:
  %sizePtr2 = getelementptr <<ENTRY>>, <<ENTRYPTR>> null, i32 1
  %sizeInt = ptrtoint <<ENTRYPTR>> %sizePtr2 to i64
  %raw = call i8* @malloc(i64 %sizeInt)
  %newEntry = bitcast i8* %raw to <<ENTRYPTR>>
  %kp = getelementptr <<ENTRY>>, <<ENTRYPTR>> %newEntry, i32 0, i32 0
  store <<KEYTYPE>> %key, <<KEYTYPEPTR>> %kp
  %vp = getelementptr <<ENTRY>>, <<ENTRYPTR>> %newEntry, i32 0, i32 1
  store <<VALUETYPE>> %value, <<VALUETYPEPTR>> %vp
  %np = getelementptr <<ENTRY>>, <<ENTRYPTR>> %newEntry, i32 0, i32 <<NEXTIDX>>
  store <<ENTRYPTR>> %head, <<ENTRYPTRPTR>> %np
  store <<ENTRYPTR>> %newEntry, <<ENTRYPTRPTR>> %bucketSlot
  %oldCount = load i32, i32* %cptr
  %newCount = add i32 %oldCount, 1
  store i32 %newCount, i32* %cptr
  %lhs = mul i32 %newCount, 4
  %rhs = mul i32 %bucketCount, 3
  %needResize = icmp sgt i32 %lhs, %rhs
  br i1 %needResize, label %doresize, label %done
doresize:
  call void @axea.map.<<ID>>.resize(<<HEADERPTR>> %h)
  br label %done
done:
  ret void
}
)";

    const char* const kMapGetTemplate = R"(
define <<VALUETYPE>> @axea.map.<<ID>>.get(<<HEADERPTR>> %h, <<KEYTYPE>> %key) {
entry:
  %bcptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %bptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 2
  %bucketCount = load i32, i32* %bcptr
  %buckets = load <<ENTRYPTRPTR>>, <<ENTRYPTRPTRPTR>> %bptr
  %hv = call i32 <<HASHFN>>(<<KEYTYPE>> %key)
  %mask = sub i32 %bucketCount, 1
  %bucketIdx = and i32 %hv, %mask
  %bucketSlot = getelementptr <<ENTRYPTR>>, <<ENTRYPTRPTR>> %buckets, i32 %bucketIdx
  %head = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %bucketSlot
  %cur = alloca <<ENTRYPTR>>
  store <<ENTRYPTR>> %head, <<ENTRYPTRPTR>> %cur
  br label %header
header:
  %c0 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %cur
  %c1 = icmp eq <<ENTRYPTR>> %c0, null
  br i1 %c1, label %notfound, label %check
check:
  %c2 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 0
  %c3 = load <<KEYTYPE>>, <<KEYTYPEPTR>> %c2
  %c4 = call i1 <<EQFN>>(<<KEYTYPE>> %c3, <<KEYTYPE>> %key)
  br i1 %c4, label %found, label %next
next:
  %c5 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 <<NEXTIDX>>
  %c6 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %c5
  store <<ENTRYPTR>> %c6, <<ENTRYPTRPTR>> %cur
  br label %header
found:
  %c7 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 1
  %c8 = load <<VALUETYPE>>, <<VALUETYPEPTR>> %c7
  ret <<VALUETYPE>> %c8
notfound:
  ret <<VALUETYPE>> <<SENTINEL>>
}
)";

    const char* const kMapContainsTemplate = R"(
define i1 @axea.map.<<ID>>.contains(<<HEADERPTR>> %h, <<KEYTYPE>> %key) {
entry:
  %bcptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %bptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 2
  %bucketCount = load i32, i32* %bcptr
  %buckets = load <<ENTRYPTRPTR>>, <<ENTRYPTRPTRPTR>> %bptr
  %hv = call i32 <<HASHFN>>(<<KEYTYPE>> %key)
  %mask = sub i32 %bucketCount, 1
  %bucketIdx = and i32 %hv, %mask
  %bucketSlot = getelementptr <<ENTRYPTR>>, <<ENTRYPTRPTR>> %buckets, i32 %bucketIdx
  %head = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %bucketSlot
  %cur = alloca <<ENTRYPTR>>
  store <<ENTRYPTR>> %head, <<ENTRYPTRPTR>> %cur
  br label %header
header:
  %c0 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %cur
  %c1 = icmp eq <<ENTRYPTR>> %c0, null
  br i1 %c1, label %notfound, label %check
check:
  %c2 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 0
  %c3 = load <<KEYTYPE>>, <<KEYTYPEPTR>> %c2
  %c4 = call i1 <<EQFN>>(<<KEYTYPE>> %c3, <<KEYTYPE>> %key)
  br i1 %c4, label %found, label %next
next:
  %c5 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 <<NEXTIDX>>
  %c6 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %c5
  store <<ENTRYPTR>> %c6, <<ENTRYPTRPTR>> %cur
  br label %header
found:
  ret i1 1
notfound:
  ret i1 0
}
)";

    const char* const kMapRemoveTemplate = R"(
define void @axea.map.<<ID>>.remove(<<HEADERPTR>> %h, <<KEYTYPE>> %key) {
entry:
  %cptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 0
  %bcptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %bptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 2
  %bucketCount = load i32, i32* %bcptr
  %buckets = load <<ENTRYPTRPTR>>, <<ENTRYPTRPTRPTR>> %bptr
  %hv = call i32 <<HASHFN>>(<<KEYTYPE>> %key)
  %mask = sub i32 %bucketCount, 1
  %bucketIdx = and i32 %hv, %mask
  %bucketSlot = getelementptr <<ENTRYPTR>>, <<ENTRYPTRPTR>> %buckets, i32 %bucketIdx
  %head = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %bucketSlot
  %cur = alloca <<ENTRYPTR>>
  store <<ENTRYPTR>> %head, <<ENTRYPTRPTR>> %cur
  %prev = alloca <<ENTRYPTR>>
  store <<ENTRYPTR>> null, <<ENTRYPTRPTR>> %prev
  br label %header
header:
  %c0 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %cur
  %c1 = icmp eq <<ENTRYPTR>> %c0, null
  br i1 %c1, label %notfound, label %check
check:
  %c2 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 0
  %c3 = load <<KEYTYPE>>, <<KEYTYPEPTR>> %c2
  %c4 = call i1 <<EQFN>>(<<KEYTYPE>> %c3, <<KEYTYPE>> %key)
  br i1 %c4, label %found, label %next
next:
  %c5 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 <<NEXTIDX>>
  %c6 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %c5
  store <<ENTRYPTR>> %c0, <<ENTRYPTRPTR>> %prev
  store <<ENTRYPTR>> %c6, <<ENTRYPTRPTR>> %cur
  br label %header
found:
  %p0 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %prev
  %n0 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 <<NEXTIDX>>
  %n1 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %n0
  %hasPrev = icmp eq <<ENTRYPTR>> %p0, null
  br i1 %hasPrev, label %unlink.head, label %unlink.mid
unlink.head:
  store <<ENTRYPTR>> %n1, <<ENTRYPTRPTR>> %bucketSlot
  br label %unlink.done
unlink.mid:
  %pn = getelementptr <<ENTRY>>, <<ENTRYPTR>> %p0, i32 0, i32 <<NEXTIDX>>
  store <<ENTRYPTR>> %n1, <<ENTRYPTRPTR>> %pn
  br label %unlink.done
unlink.done:
  %oldCount = load i32, i32* %cptr
  %newCount = sub i32 %oldCount, 1
  store i32 %newCount, i32* %cptr
  ret void
notfound:
  ret void
}
)";

    // Set<T> add/contains/remove mirror Map<K,V>'s own set/contains/remove
    // exactly - just no value field, and "next" sits at field index 1
    // instead of 2 (handled via <<NEXTIDX>> above too, shared with the Map
    // templates - Set's own resize reuses kMapResizeTemplate directly, since
    // it never touches a value field either).
    const char* const kSetAddTemplate = R"(
define void @axea.set.<<ID>>.add(<<HEADERPTR>> %h, <<KEYTYPE>> %value) {
entry:
  %cptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 0
  %bcptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %bptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 2
  %bucketCount = load i32, i32* %bcptr
  %buckets = load <<ENTRYPTRPTR>>, <<ENTRYPTRPTRPTR>> %bptr
  %hv = call i32 <<HASHFN>>(<<KEYTYPE>> %value)
  %mask = sub i32 %bucketCount, 1
  %bucketIdx = and i32 %hv, %mask
  %bucketSlot = getelementptr <<ENTRYPTR>>, <<ENTRYPTRPTR>> %buckets, i32 %bucketIdx
  %head = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %bucketSlot
  %cur = alloca <<ENTRYPTR>>
  store <<ENTRYPTR>> %head, <<ENTRYPTRPTR>> %cur
  br label %search.header
search.header:
  %c0 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %cur
  %c1 = icmp eq <<ENTRYPTR>> %c0, null
  br i1 %c1, label %insert, label %search.check
search.check:
  %c2 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 0
  %c3 = load <<KEYTYPE>>, <<KEYTYPEPTR>> %c2
  %c4 = call i1 <<EQFN>>(<<KEYTYPE>> %c3, <<KEYTYPE>> %value)
  br i1 %c4, label %already, label %search.next
search.next:
  %c5 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 1
  %c6 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %c5
  store <<ENTRYPTR>> %c6, <<ENTRYPTRPTR>> %cur
  br label %search.header
already:
  ret void
insert:
  %sizePtr2 = getelementptr <<ENTRY>>, <<ENTRYPTR>> null, i32 1
  %sizeInt = ptrtoint <<ENTRYPTR>> %sizePtr2 to i64
  %raw = call i8* @malloc(i64 %sizeInt)
  %newEntry = bitcast i8* %raw to <<ENTRYPTR>>
  %kp = getelementptr <<ENTRY>>, <<ENTRYPTR>> %newEntry, i32 0, i32 0
  store <<KEYTYPE>> %value, <<KEYTYPEPTR>> %kp
  %np = getelementptr <<ENTRY>>, <<ENTRYPTR>> %newEntry, i32 0, i32 1
  store <<ENTRYPTR>> %head, <<ENTRYPTRPTR>> %np
  store <<ENTRYPTR>> %newEntry, <<ENTRYPTRPTR>> %bucketSlot
  %oldCount = load i32, i32* %cptr
  %newCount = add i32 %oldCount, 1
  store i32 %newCount, i32* %cptr
  %lhs = mul i32 %newCount, 4
  %rhs = mul i32 %bucketCount, 3
  %needResize = icmp sgt i32 %lhs, %rhs
  br i1 %needResize, label %doresize, label %done
doresize:
  call void @axea.set.<<ID>>.resize(<<HEADERPTR>> %h)
  br label %done
done:
  ret void
}
)";

    const char* const kSetContainsTemplate = R"(
define i1 @axea.set.<<ID>>.contains(<<HEADERPTR>> %h, <<KEYTYPE>> %value) {
entry:
  %bcptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %bptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 2
  %bucketCount = load i32, i32* %bcptr
  %buckets = load <<ENTRYPTRPTR>>, <<ENTRYPTRPTRPTR>> %bptr
  %hv = call i32 <<HASHFN>>(<<KEYTYPE>> %value)
  %mask = sub i32 %bucketCount, 1
  %bucketIdx = and i32 %hv, %mask
  %bucketSlot = getelementptr <<ENTRYPTR>>, <<ENTRYPTRPTR>> %buckets, i32 %bucketIdx
  %head = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %bucketSlot
  %cur = alloca <<ENTRYPTR>>
  store <<ENTRYPTR>> %head, <<ENTRYPTRPTR>> %cur
  br label %header
header:
  %c0 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %cur
  %c1 = icmp eq <<ENTRYPTR>> %c0, null
  br i1 %c1, label %notfound, label %check
check:
  %c2 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 0
  %c3 = load <<KEYTYPE>>, <<KEYTYPEPTR>> %c2
  %c4 = call i1 <<EQFN>>(<<KEYTYPE>> %c3, <<KEYTYPE>> %value)
  br i1 %c4, label %found, label %next
next:
  %c5 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 1
  %c6 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %c5
  store <<ENTRYPTR>> %c6, <<ENTRYPTRPTR>> %cur
  br label %header
found:
  ret i1 1
notfound:
  ret i1 0
}
)";

    const char* const kSetRemoveTemplate = R"(
define void @axea.set.<<ID>>.remove(<<HEADERPTR>> %h, <<KEYTYPE>> %value) {
entry:
  %cptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 0
  %bcptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %bptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 2
  %bucketCount = load i32, i32* %bcptr
  %buckets = load <<ENTRYPTRPTR>>, <<ENTRYPTRPTRPTR>> %bptr
  %hv = call i32 <<HASHFN>>(<<KEYTYPE>> %value)
  %mask = sub i32 %bucketCount, 1
  %bucketIdx = and i32 %hv, %mask
  %bucketSlot = getelementptr <<ENTRYPTR>>, <<ENTRYPTRPTR>> %buckets, i32 %bucketIdx
  %head = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %bucketSlot
  %cur = alloca <<ENTRYPTR>>
  store <<ENTRYPTR>> %head, <<ENTRYPTRPTR>> %cur
  %prev = alloca <<ENTRYPTR>>
  store <<ENTRYPTR>> null, <<ENTRYPTRPTR>> %prev
  br label %header
header:
  %c0 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %cur
  %c1 = icmp eq <<ENTRYPTR>> %c0, null
  br i1 %c1, label %notfound, label %check
check:
  %c2 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 0
  %c3 = load <<KEYTYPE>>, <<KEYTYPEPTR>> %c2
  %c4 = call i1 <<EQFN>>(<<KEYTYPE>> %c3, <<KEYTYPE>> %value)
  br i1 %c4, label %found, label %next
next:
  %c5 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 1
  %c6 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %c5
  store <<ENTRYPTR>> %c0, <<ENTRYPTRPTR>> %prev
  store <<ENTRYPTR>> %c6, <<ENTRYPTRPTR>> %cur
  br label %header
found:
  %p0 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %prev
  %n0 = getelementptr <<ENTRY>>, <<ENTRYPTR>> %c0, i32 0, i32 1
  %n1 = load <<ENTRYPTR>>, <<ENTRYPTRPTR>> %n0
  %hasPrev = icmp eq <<ENTRYPTR>> %p0, null
  br i1 %hasPrev, label %unlink.head, label %unlink.mid
unlink.head:
  store <<ENTRYPTR>> %n1, <<ENTRYPTRPTR>> %bucketSlot
  br label %unlink.done
unlink.mid:
  %pn = getelementptr <<ENTRY>>, <<ENTRYPTR>> %p0, i32 0, i32 1
  store <<ENTRYPTR>> %n1, <<ENTRYPTRPTR>> %pn
  br label %unlink.done
unlink.done:
  %oldCount = load i32, i32* %cptr
  %newCount = sub i32 %oldCount, 1
  store i32 %newCount, i32* %cptr
  ret void
notfound:
  ret void
}
)";
} // namespace

std::pair<std::string, std::string>
LlvmIrEmitter::registerKeyRuntime(const std::string& axeaKeyType)
{
    if (const auto it = keyRuntimeFns_.find(axeaKeyType); it != keyRuntimeFns_.end())
    {
        return it->second;
    }

    if (axeaKeyType == "i32")
    {
        mapSetRuntimeText_ << R"(
define i32 @axea.hash.i32(i32 %key) {
entry:
  %h = mul i32 %key, -1640531527
  ret i32 %h
}

define i1 @axea.eq.i32(i32 %a, i32 %b) {
entry:
  %e = icmp eq i32 %a, %b
  ret i1 %e
}
)";
        return keyRuntimeFns_[axeaKeyType] = {"@axea.hash.i32", "@axea.eq.i32"};
    }

    if (axeaKeyType == "bool")
    {
        mapSetRuntimeText_ << R"(
define i32 @axea.hash.bool(i1 %key) {
entry:
  %z = zext i1 %key to i32
  %h = mul i32 %z, -1640531527
  ret i32 %h
}

define i1 @axea.eq.bool(i1 %a, i1 %b) {
entry:
  %e = icmp eq i1 %a, %b
  ret i1 %e
}
)";
        return keyRuntimeFns_[axeaKeyType] = {"@axea.hash.bool", "@axea.eq.bool"};
    }

    if (axeaKeyType == "str")
    {
        // Hand-verified against the real clang toolchain (at both -O0 and
        // -O1) in this exact structure before being written here - see
        // docs/language/0034-maps-and-sets.md. Byte-walk hash/equality over
        // the null-terminated i8* - the one genuinely new primitive-key
        // mechanism this phase adds (i32/bool both reduce to a single
        // instruction each).
        mapSetRuntimeText_ << R"(
define i32 @axea.hash.str(i8* %s) {
entry:
  %i = alloca i32
  store i32 0, i32* %i
  %h = alloca i32
  store i32 0, i32* %h
  br label %header
header:
  %i0 = load i32, i32* %i
  %cptr = getelementptr i8, i8* %s, i32 %i0
  %c = load i8, i8* %cptr
  %iszero = icmp eq i8 %c, 0
  br i1 %iszero, label %done, label %body
body:
  %h0 = load i32, i32* %h
  %c32 = sext i8 %c to i32
  %h1 = mul i32 %h0, 31
  %h2 = add i32 %h1, %c32
  store i32 %h2, i32* %h
  %i1 = add i32 %i0, 1
  store i32 %i1, i32* %i
  br label %header
done:
  %hf = load i32, i32* %h
  ret i32 %hf
}

define i1 @axea.eq.str(i8* %a, i8* %b) {
entry:
  %i = alloca i32
  store i32 0, i32* %i
  br label %header
header:
  %i0 = load i32, i32* %i
  %aptr = getelementptr i8, i8* %a, i32 %i0
  %bptr = getelementptr i8, i8* %b, i32 %i0
  %ac = load i8, i8* %aptr
  %bc = load i8, i8* %bptr
  %ne = icmp ne i8 %ac, %bc
  br i1 %ne, label %notequal, label %check
check:
  %iszero = icmp eq i8 %ac, 0
  br i1 %iszero, label %equal, label %next
next:
  %i1 = add i32 %i0, 1
  store i32 %i1, i32* %i
  br label %header
equal:
  ret i1 1
notequal:
  ret i1 0
}
)";
        return keyRuntimeFns_[axeaKeyType] = {"@axea.hash.str", "@axea.eq.str"};
    }

    if (structs_.contains(axeaKeyType))
    {
        // Name-based, not numeric - struct names are already unique valid
        // identifiers, and a struct used as a key in several different
        // Map/Set instantiations should only ever get one hash/equality
        // implementation, generated once (mirrors registerKeyRuntime's own
        // memoization above). Recurses into each field's own
        // registerKeyRuntime - safe from infinite recursion on a self-/
        // mutually-recursive struct chain, since TypeChecker::isHashable
        // already rejected any key type where that's possible before this
        // function is ever reached. Hash: djb2/Java-style combine
        // (`acc = acc * 31 + fieldHash`) across fields in declared order.
        // Equality: AND together every field's own equality call - no
        // short-circuit branching, simpler and equally correct straight-line
        // code. See docs/language/0034-maps-and-sets.md.
        const std::string hashFn = "@axea.hash." + axeaKeyType;
        const std::string eqFn = "@axea.eq." + axeaKeyType;
        keyRuntimeFns_[axeaKeyType] = {hashFn, eqFn};

        const std::string structType = "%" + axeaKeyType;
        const std::string structPtrType = structType + "*";
        const auto& fields = structs_.at(axeaKeyType);

        std::ostringstream hashBody;
        hashBody << "\ndefine i32 " << hashFn << "(" << structPtrType << " %v) {\nentry:\n";
        std::string accReg = "0";
        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            const std::string idx = std::to_string(i);
            const std::string fieldLlvmType = llvmType(fields[i].second);
            const auto [fieldHashFn, fieldEqFn] = registerKeyRuntime(fields[i].second);
            hashBody << "  %fp" << idx << " = getelementptr " << structType << ", " << structPtrType
                     << " %v, i32 0, i32 " << idx << "\n";
            hashBody << "  %fv" << idx << " = load " << fieldLlvmType << ", " << fieldLlvmType
                     << "* %fp" << idx << "\n";
            hashBody << "  %fh" << idx << " = call i32 " << fieldHashFn << "(" << fieldLlvmType
                     << " %fv" << idx << ")\n";
            hashBody << "  %am" << idx << " = mul i32 " << accReg << ", 31\n";
            hashBody << "  %acc" << idx << " = add i32 %am" << idx << ", %fh" << idx << "\n";
            accReg = "%acc" + idx;
        }
        hashBody << "  ret i32 " << accReg << "\n}\n";

        std::ostringstream eqBody;
        eqBody << "\ndefine i1 " << eqFn << "(" << structPtrType << " %a, " << structPtrType
               << " %b) {\nentry:\n";
        std::string eqReg;
        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            const std::string idx = std::to_string(i);
            const std::string fieldLlvmType = llvmType(fields[i].second);
            const auto [fieldHashFn, fieldEqFn] = registerKeyRuntime(fields[i].second);
            eqBody << "  %ap" << idx << " = getelementptr " << structType << ", " << structPtrType
                   << " %a, i32 0, i32 " << idx << "\n";
            eqBody << "  %bp" << idx << " = getelementptr " << structType << ", " << structPtrType
                   << " %b, i32 0, i32 " << idx << "\n";
            eqBody << "  %av" << idx << " = load " << fieldLlvmType << ", " << fieldLlvmType
                   << "* %ap" << idx << "\n";
            eqBody << "  %bv" << idx << " = load " << fieldLlvmType << ", " << fieldLlvmType
                   << "* %bp" << idx << "\n";
            eqBody << "  %fe" << idx << " = call i1 " << fieldEqFn << "(" << fieldLlvmType << " %av"
                   << idx << ", " << fieldLlvmType << " %bv" << idx << ")\n";
            if (eqReg.empty())
            {
                eqReg = "%fe" + idx;
            }
            else
            {
                eqBody << "  %and" << idx << " = and i1 " << eqReg << ", %fe" << idx << "\n";
                eqReg = "%and" + idx;
            }
        }
        eqBody << "  ret i1 " << (eqReg.empty() ? "1" : eqReg) << "\n}\n";

        mapSetRuntimeText_ << hashBody.str() << eqBody.str();
        return keyRuntimeFns_[axeaKeyType];
    }

    if (!axeaKeyType.empty() && axeaKeyType.front() == '[')
    {
        // "[elem;N]" - a key *shape*, not a named type, so a synthetic
        // numeric ID (own ID space, separate from Map/Set's own
        // instantiation IDs - see nextArrayKeyId_). N is compile-time-known,
        // so - like emitMapNew's 8-slot bucket zero-init - this unrolls
        // rather than looping.
        const auto semicolon = axeaKeyType.find(';');
        const auto closeBracket = axeaKeyType.rfind(']');
        const std::string elementName = axeaKeyType.substr(1, semicolon - 1);
        const int n = std::stoi(axeaKeyType.substr(semicolon + 1, closeBracket - semicolon - 1));

        const std::string idStr = std::to_string(nextArrayKeyId_++);
        const std::string hashFn = "@axea.hash.arr." + idStr;
        const std::string eqFn = "@axea.eq.arr." + idStr;
        keyRuntimeFns_[axeaKeyType] = {hashFn, eqFn};

        const std::string elementType = llvmType(elementName);
        const auto [elemHashFn, elemEqFn] = registerKeyRuntime(elementName);
        const std::string arrType = "[" + std::to_string(n) + " x " + elementType + "]";
        const std::string arrPtrType = arrType + "*";

        std::ostringstream hashBody;
        hashBody << "\ndefine i32 " << hashFn << "(" << arrPtrType << " %v) {\nentry:\n";
        std::string accReg = "0";
        for (int i = 0; i < n; ++i)
        {
            const std::string idx = std::to_string(i);
            hashBody << "  %ep" << idx << " = getelementptr " << arrType << ", " << arrPtrType
                     << " %v, i32 0, i32 " << idx << "\n";
            hashBody << "  %ev" << idx << " = load " << elementType << ", " << elementType
                     << "* %ep" << idx << "\n";
            hashBody << "  %eh" << idx << " = call i32 " << elemHashFn << "(" << elementType
                     << " %ev" << idx << ")\n";
            hashBody << "  %am" << idx << " = mul i32 " << accReg << ", 31\n";
            hashBody << "  %acc" << idx << " = add i32 %am" << idx << ", %eh" << idx << "\n";
            accReg = "%acc" + idx;
        }
        hashBody << "  ret i32 " << accReg << "\n}\n";

        std::ostringstream eqBody;
        eqBody << "\ndefine i1 " << eqFn << "(" << arrPtrType << " %a, " << arrPtrType
               << " %b) {\nentry:\n";
        std::string eqReg;
        for (int i = 0; i < n; ++i)
        {
            const std::string idx = std::to_string(i);
            eqBody << "  %ap" << idx << " = getelementptr " << arrType << ", " << arrPtrType
                   << " %a, i32 0, i32 " << idx << "\n";
            eqBody << "  %bp" << idx << " = getelementptr " << arrType << ", " << arrPtrType
                   << " %b, i32 0, i32 " << idx << "\n";
            eqBody << "  %av" << idx << " = load " << elementType << ", " << elementType << "* %ap"
                   << idx << "\n";
            eqBody << "  %bv" << idx << " = load " << elementType << ", " << elementType << "* %bp"
                   << idx << "\n";
            eqBody << "  %fe" << idx << " = call i1 " << elemEqFn << "(" << elementType << " %av"
                   << idx << ", " << elementType << " %bv" << idx << ")\n";
            if (eqReg.empty())
            {
                eqReg = "%fe" + idx;
            }
            else
            {
                eqBody << "  %and" << idx << " = and i1 " << eqReg << ", %fe" << idx << "\n";
                eqReg = "%and" + idx;
            }
        }
        eqBody << "  ret i1 " << (eqReg.empty() ? "1" : eqReg) << "\n}\n";

        mapSetRuntimeText_ << hashBody.str() << eqBody.str();
        return keyRuntimeFns_[axeaKeyType];
    }

    if (axeaKeyType.starts_with("List<"))
    {
        // Unlike array above, a List<T>'s length isn't known until runtime -
        // the one genuinely new loop shape this phase, hand-verified against
        // the real clang toolchain (at both -O0 and -O1) in this exact
        // structure before being written here (see
        // docs/language/0034-maps-and-sets.md): compare lengths first (an
        // unequal length is an immediate mismatch, no need to walk
        // anything), then an alloca/load/store element-by-element walk
        // (same no-phi convention as every other loop in this backend),
        // combining hash / AND-ing equality per element via that element
        // type's own registerKeyRuntime call.
        const std::string elementName = axeaKeyType.substr(5, axeaKeyType.size() - 6);
        const std::string idStr = std::to_string(nextListKeyId_++);
        const std::string hashFn = "@axea.hash.list." + idStr;
        const std::string eqFn = "@axea.eq.list." + idStr;
        keyRuntimeFns_[axeaKeyType] = {hashFn, eqFn};

        const std::string elementType = llvmType(elementName);
        const auto [elemHashFn, elemEqFn] = registerKeyRuntime(elementName);
        const std::string headerType = "{i32, " + elementType + "*}";
        const std::string headerPtrType = headerType + "*";

        mapSetRuntimeText_ << "\ndefine i32 " << hashFn << "(" << headerPtrType
                           << " %v) {\nentry:\n"
                           << "  %lp = getelementptr " << headerType << ", " << headerPtrType
                           << " %v, i32 0, i32 0\n"
                           << "  %len = load i32, i32* %lp\n"
                           << "  %dp = getelementptr " << headerType << ", " << headerPtrType
                           << " %v, i32 0, i32 1\n"
                           << "  %data = load " << elementType << "*, " << elementType << "** %dp\n"
                           << "  %i = alloca i32\n"
                              "  store i32 0, i32* %i\n"
                              "  %acc = alloca i32\n"
                              "  store i32 0, i32* %acc\n"
                              "  br label %header\n"
                              "header:\n"
                              "  %i0 = load i32, i32* %i\n"
                              "  %cond = icmp slt i32 %i0, %len\n"
                              "  br i1 %cond, label %body, label %done\n"
                              "body:\n"
                           << "  %ep = getelementptr " << elementType << ", " << elementType
                           << "* %data, i32 %i0\n"
                           << "  %ev = load " << elementType << ", " << elementType << "* %ep\n"
                           << "  %eh = call i32 " << elemHashFn << "(" << elementType << " %ev)\n"
                           << "  %a0 = load i32, i32* %acc\n"
                              "  %am = mul i32 %a0, 31\n"
                              "  %a1 = add i32 %am, %eh\n"
                              "  store i32 %a1, i32* %acc\n"
                              "  %i1 = add i32 %i0, 1\n"
                              "  store i32 %i1, i32* %i\n"
                              "  br label %header\n"
                              "done:\n"
                              "  %af = load i32, i32* %acc\n"
                              "  ret i32 %af\n"
                              "}\n";

        mapSetRuntimeText_ << "\ndefine i1 " << eqFn << "(" << headerPtrType << " %a, "
                           << headerPtrType << " %b) {\nentry:\n"
                           << "  %alp = getelementptr " << headerType << ", " << headerPtrType
                           << " %a, i32 0, i32 0\n"
                           << "  %alen = load i32, i32* %alp\n"
                           << "  %blp = getelementptr " << headerType << ", " << headerPtrType
                           << " %b, i32 0, i32 0\n"
                           << "  %blen = load i32, i32* %blp\n"
                           << "  %lenEq = icmp eq i32 %alen, %blen\n"
                              "  br i1 %lenEq, label %walk, label %notequal\n"
                              "walk:\n"
                           << "  %adp = getelementptr " << headerType << ", " << headerPtrType
                           << " %a, i32 0, i32 1\n"
                           << "  %adata = load " << elementType << "*, " << elementType
                           << "** %adp\n"
                           << "  %bdp = getelementptr " << headerType << ", " << headerPtrType
                           << " %b, i32 0, i32 1\n"
                           << "  %bdata = load " << elementType << "*, " << elementType
                           << "** %bdp\n"
                           << "  %i = alloca i32\n"
                              "  store i32 0, i32* %i\n"
                              "  %eqacc = alloca i1\n"
                              "  store i1 1, i1* %eqacc\n"
                              "  br label %header\n"
                              "header:\n"
                              "  %i0 = load i32, i32* %i\n"
                              "  %cond = icmp slt i32 %i0, %alen\n"
                              "  br i1 %cond, label %body, label %done\n"
                              "body:\n"
                           << "  %aep = getelementptr " << elementType << ", " << elementType
                           << "* %adata, i32 %i0\n"
                           << "  %aev = load " << elementType << ", " << elementType << "* %aep\n"
                           << "  %bep = getelementptr " << elementType << ", " << elementType
                           << "* %bdata, i32 %i0\n"
                           << "  %bev = load " << elementType << ", " << elementType << "* %bep\n"
                           << "  %ee = call i1 " << elemEqFn << "(" << elementType << " %aev, "
                           << elementType << " %bev)\n"
                           << "  %e0 = load i1, i1* %eqacc\n"
                              "  %e1 = and i1 %e0, %ee\n"
                              "  store i1 %e1, i1* %eqacc\n"
                              "  %i1 = add i32 %i0, 1\n"
                              "  store i32 %i1, i32* %i\n"
                              "  br label %header\n"
                              "done:\n"
                              "  %ef = load i1, i1* %eqacc\n"
                              "  ret i1 %ef\n"
                              "notequal:\n"
                              "  ret i1 0\n"
                              "}\n";

        return keyRuntimeFns_[axeaKeyType];
    }

    // Unreachable for a well-typed program: TypeChecker::isHashable already
    // validated the key before this point.
    throw std::runtime_error("internal error: no key runtime registered for type " + axeaKeyType);
}

std::string LlvmIrEmitter::registerMapInstantiation(const std::string& keyAxeaType,
                                                    const std::string& valueAxeaType)
{
    const std::string canonical = "Map<" + keyAxeaType + "," + valueAxeaType + ">";
    if (const auto it = mapInstantiationIds_.find(canonical); it != mapInstantiationIds_.end())
    {
        return "{i32, i32, %axea.MapEntry." + std::to_string(it->second) + "**}*";
    }

    const int id = nextMapInstantiationId_++;
    mapInstantiationIds_[canonical] = id;
    const std::string idStr = std::to_string(id);

    const std::string entry = "%axea.MapEntry." + idStr;
    const std::string entryPtr = entry + "*";
    const std::string entryPtrPtr = entry + "**";
    const std::string entryPtrPtrPtr = entry + "***";
    const std::string headerType = "{i32, i32, " + entryPtrPtr + "}";
    const std::string headerPtr = headerType + "*";
    const std::string keyType = llvmType(keyAxeaType);
    const std::string valueType = llvmType(valueAxeaType);
    mapValueLlvmTypeById_[id] = valueType;
    const auto [hashFn, eqFn] = registerKeyRuntime(keyAxeaType);

    mapSetTypeDeclsText_ << entry << " = type { " << keyType << ", " << valueType << ", "
                         << entryPtr << " }\n";

    const std::vector<std::pair<std::string, std::string>> substitutions{
        {"<<ENTRYPTRPTRPTR>>", entryPtrPtrPtr},
        {"<<ENTRYPTRPTR>>", entryPtrPtr},
        {"<<ENTRYPTR>>", entryPtr},
        {"<<ENTRY>>", entry},
        {"<<HEADERPTR>>", headerPtr},
        {"<<HEADERTYPE>>", headerType},
        {"<<KEYTYPEPTR>>", keyType + "*"},
        {"<<KEYTYPE>>", keyType},
        {"<<VALUETYPEPTR>>", valueType + "*"},
        {"<<VALUETYPE>>", valueType},
        {"<<HASHFN>>", hashFn},
        {"<<EQFN>>", eqFn},
        {"<<SENTINEL>>", sentinelFor(valueType)},
        {"<<NEXTIDX>>", "2"},
        {"<<KIND>>", "map"},
        {"<<ID>>", idStr},
    };

    mapSetRuntimeText_ << fillTemplate(kMapResizeTemplate, substitutions);
    mapSetRuntimeText_ << fillTemplate(kMapSetTemplate, substitutions);
    mapSetRuntimeText_ << fillTemplate(kMapGetTemplate, substitutions);
    mapSetRuntimeText_ << fillTemplate(kMapContainsTemplate, substitutions);
    mapSetRuntimeText_ << fillTemplate(kMapRemoveTemplate, substitutions);

    return headerPtr;
}

std::string LlvmIrEmitter::registerSetInstantiation(const std::string& elementAxeaType)
{
    const std::string canonical = "Set<" + elementAxeaType + ">";
    if (const auto it = setInstantiationIds_.find(canonical); it != setInstantiationIds_.end())
    {
        return "{i32, i32, %axea.SetEntry." + std::to_string(it->second) + "**}*";
    }

    const int id = nextSetInstantiationId_++;
    setInstantiationIds_[canonical] = id;
    const std::string idStr = std::to_string(id);

    const std::string entry = "%axea.SetEntry." + idStr;
    const std::string entryPtr = entry + "*";
    const std::string entryPtrPtr = entry + "**";
    const std::string entryPtrPtrPtr = entry + "***";
    const std::string headerType = "{i32, i32, " + entryPtrPtr + "}";
    const std::string headerPtr = headerType + "*";
    const std::string keyType = llvmType(elementAxeaType);
    const auto [hashFn, eqFn] = registerKeyRuntime(elementAxeaType);

    mapSetTypeDeclsText_ << entry << " = type { " << keyType << ", " << entryPtr << " }\n";

    const std::vector<std::pair<std::string, std::string>> substitutions{
        {"<<ENTRYPTRPTRPTR>>", entryPtrPtrPtr},
        {"<<ENTRYPTRPTR>>", entryPtrPtr},
        {"<<ENTRYPTR>>", entryPtr},
        {"<<ENTRY>>", entry},
        {"<<HEADERPTR>>", headerPtr},
        {"<<HEADERTYPE>>", headerType},
        {"<<KEYTYPEPTR>>", keyType + "*"},
        {"<<KEYTYPE>>", keyType},
        {"<<HASHFN>>", hashFn},
        {"<<EQFN>>", eqFn},
        {"<<NEXTIDX>>", "1"},
        {"<<KIND>>", "set"},
        {"<<ID>>", idStr},
    };

    // Set<T>'s resize is structurally identical to Map<K,V>'s own (it never
    // touches a value field either) - reuses kMapResizeTemplate directly,
    // substituting <<NEXTIDX>> = "1" instead of "2".
    mapSetRuntimeText_ << fillTemplate(kMapResizeTemplate, substitutions);
    mapSetRuntimeText_ << fillTemplate(kSetAddTemplate, substitutions);
    mapSetRuntimeText_ << fillTemplate(kSetContainsTemplate, substitutions);
    mapSetRuntimeText_ << fillTemplate(kSetRemoveTemplate, substitutions);

    return headerPtr;
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

    if (isSliceType(objectType))
    {
        // A slice's ".length" is field 1 of the {T*, i32} fat pointer - a
        // genuine runtime read (unlike an array's own compile-time-constant
        // ".length" - see docs/language/0032-slices.md), but still no GEP or
        // load needed: it's already sitting directly in the by-value struct.
        const int destReg = defineRegister(fieldGet.dest, fctx);
        *fctx.out << "  %" << destReg << " = extractvalue " << objectType << " "
                  << ref(fieldGet.object, fctx) << ", 1\n";
        return;
    }

    if (isListType(objectType) || isMapType(objectType) || isSetType(objectType))
    {
        // A List's ".length" is field 0 of the {i32, T*} heap record; a
        // Map/Set's ".length" is field 0 (count) of its own 3-field header
        // {count, bucketCount, buckets} - same GEP index, same shape, so
        // this branch covers all three (see docs/language/0033-lists.md,
        // docs/language/0034-maps-and-sets.md). Stack<T> (docs/language/0035-stacks.md)
        // needs no branch of its own here at all: llvmType("Stack<T>")
        // produces the *exact same text* llvmType("List<T>") would, so
        // isListType's own test already matches it - not a coincidence the
        // way Map/Set's own count field briefly was, but a direct
        // consequence of Stack<T> and List<T> being the literal same LLVM
        // type. Unlike an array's compile-time-constant ".length", this is
        // always a genuine runtime read, and unlike a slice's by-value
        // extractvalue, these are all accessed by pointer, so it's GEP+load.
        const std::string headerType = objectType.substr(0, objectType.size() - 1);
        const int lenPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", " << objectType
                  << " " << ref(fieldGet.object, fctx) << ", i32 0, i32 0\n";
        const int destReg = defineRegister(fieldGet.dest, fctx);
        *fctx.out << "  %" << destReg << " = load i32, i32* %" << lenPtrReg << "\n";
        return;
    }

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

    if (isSliceType(objectType))
    {
        // {T*, i32} - extract the flat pointer, then a single-index GEP
        // (unlike an array's own "i32 0, i32 idx" two-index form: a flat
        // pointer isn't an aggregate, so there's no leading "0th element of
        // the pointee" index needed - see docs/language/0032-slices.md).
        const std::string elementType = sliceElementType(objectType);
        const int ptrReg = allocateRegister(fctx);
        *fctx.out << "  %" << ptrReg << " = extractvalue " << objectType << " "
                  << ref(indexGet.object, fctx) << ", 0\n";
        const int elementPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << elementPtrReg << " = getelementptr " << elementType << ", "
                  << elementType << "* %" << ptrReg << ", i32 " << ref(indexGet.index, fctx)
                  << "\n";
        const int destReg = defineRegister(indexGet.dest, fctx);
        *fctx.out << "  %" << destReg << " = load " << elementType << ", " << elementType << "* %"
                  << elementPtrReg << "\n";
        return;
    }

    if (isListType(objectType))
    {
        // {i32, T*}* - GEP+load the data pointer field (field 1) out of the
        // heap record, then the same flat-pointer single-index GEP slice
        // indexing already uses (see docs/language/0033-lists.md).
        const std::string headerType = objectType.substr(0, objectType.size() - 1);
        const std::string elementType = listElementType(objectType);
        const int dataPtrPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", "
                  << objectType << " " << ref(indexGet.object, fctx) << ", i32 0, i32 1\n";
        const int dataPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << dataPtrReg << " = load " << elementType << "*, " << elementType
                  << "** %" << dataPtrPtrReg << "\n";
        const int elementPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << elementPtrReg << " = getelementptr " << elementType << ", "
                  << elementType << "* %" << dataPtrReg << ", i32 " << ref(indexGet.index, fctx)
                  << "\n";
        const int destReg = defineRegister(indexGet.dest, fctx);
        *fctx.out << "  %" << destReg << " = load " << elementType << ", " << elementType << "* %"
                  << elementPtrReg << "\n";
        return;
    }

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

    if (isSliceType(objectType))
    {
        const std::string elementType = sliceElementType(objectType);
        const int ptrReg = allocateRegister(fctx);
        *fctx.out << "  %" << ptrReg << " = extractvalue " << objectType << " "
                  << ref(indexSet.object, fctx) << ", 0\n";
        const int elementPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << elementPtrReg << " = getelementptr " << elementType << ", "
                  << elementType << "* %" << ptrReg << ", i32 " << ref(indexSet.index, fctx)
                  << "\n";
        *fctx.out << "  store " << elementType << " " << ref(indexSet.value, fctx) << ", "
                  << elementType << "* %" << elementPtrReg << "\n";
        return;
    }

    if (isListType(objectType))
    {
        const std::string headerType = objectType.substr(0, objectType.size() - 1);
        const std::string elementType = listElementType(objectType);
        const int dataPtrPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", "
                  << objectType << " " << ref(indexSet.object, fctx) << ", i32 0, i32 1\n";
        const int dataPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << dataPtrReg << " = load " << elementType << "*, " << elementType
                  << "** %" << dataPtrPtrReg << "\n";
        const int elementPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << elementPtrReg << " = getelementptr " << elementType << ", "
                  << elementType << "* %" << dataPtrReg << ", i32 " << ref(indexSet.index, fctx)
                  << "\n";
        *fctx.out << "  store " << elementType << " " << ref(indexSet.value, fctx) << ", "
                  << elementType << "* %" << elementPtrReg << "\n";
        return;
    }

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

void LlvmIrEmitter::emitListNew(const IrListNew& listNew, FunctionContext& fctx)
{
    const std::string elementType = llvmType(listNew.elementTypeName);
    const std::string headerType = "{i32, " + elementType + "*}";
    const std::string pointerType = headerType + "*";

    // sizeof({i32, T*}) via the standard null-pointer GEP idiom - same idiom
    // as emitStructNew/emitArrayNew.
    const int sizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizePtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " null, i32 1\n";
    const int sizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizeIntReg << " = ptrtoint " << pointerType << " %" << sizePtrReg
              << " to i64\n";

    const int rawPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawPtrReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";

    const int destReg = defineRegister(listNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawPtrReg << " to " << pointerType
              << "\n";

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " " << ref(listNew.dest, fctx) << ", i32 0, i32 0\n";
    *fctx.out << "  store i32 0, i32* %" << lenPtrReg << "\n";

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " " << ref(listNew.dest, fctx) << ", i32 0, i32 1\n";
    *fctx.out << "  store " << elementType << "* null, " << elementType << "** %" << dataPtrPtrReg
              << "\n";
}

void LlvmIrEmitter::emitListPush(const IrListPush& listPush, FunctionContext& fctx)
{
    const std::string objectType = typeOf(listPush.list, fctx);
    const std::string headerType =
        objectType.substr(0, objectType.size() - 1); // strip trailing '*'
    const std::string elementType = listElementType(objectType);
    const std::string listRef = ref(listPush.list, fctx);

    // Current length.
    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << listRef << ", i32 0, i32 0\n";
    const int oldLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldLenReg << " = load i32, i32* %" << lenPtrReg << "\n";
    const int newLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << newLenReg << " = add i32 %" << oldLenReg << ", 1\n";

    // No amortized growth this phase (a deliberate simplification, not an
    // oversight - see docs/language/0033-lists.md): every push reallocates
    // to exactly newLen elements. sizeof(T) via the standard null-pointer GEP
    // idiom, same as every other heap allocation here.
    const int elemSizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << elemSizePtrReg << " = getelementptr " << elementType << ", "
              << elementType << "* null, i32 1\n";
    const int elemSizeReg = allocateRegister(fctx);
    *fctx.out << "  %" << elemSizeReg << " = ptrtoint " << elementType << "* %" << elemSizePtrReg
              << " to i64\n";
    const int newLen64Reg = allocateRegister(fctx);
    *fctx.out << "  %" << newLen64Reg << " = zext i32 %" << newLenReg << " to i64\n";
    const int newBytesReg = allocateRegister(fctx);
    *fctx.out << "  %" << newBytesReg << " = mul i64 %" << newLen64Reg << ", %" << elemSizeReg
              << "\n";
    const int rawNewDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawNewDataReg << " = call i8* @malloc(i64 %" << newBytesReg << ")\n";
    const int newDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << newDataReg << " = bitcast i8* %" << rawNewDataReg << " to " << elementType
              << "*\n";

    // Old data pointer, to copy from.
    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << listRef << ", i32 0, i32 1\n";
    const int oldDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldDataReg << " = load " << elementType << "*, " << elementType << "** %"
              << dataPtrPtrReg << "\n";

    // Copy loop over 0..oldLen. Uses alloca/load/store for the counter, not a
    // phi node: this codebase's unnamed SSA registers must appear in strictly
    // increasing textual order (see IrGenerator's own comment on this, and
    // docs/language/0028-loops.md for why loop-carried state already uses
    // this same alloca-based pattern instead of phi for exactly this reason)
    // - a phi here would need to forward-reference a not-yet-emitted register
    // from the loop body. Hand-verified against the real clang toolchain (at
    // both -O0 and -O1) in this exact unnamed-sequential-register shape
    // before this design was committed to.
    const int labelId = fctx.nextLabel++;
    const std::string headerLabel = "list.push.copy.header" + std::to_string(labelId);
    const std::string bodyLabel = "list.push.copy.body" + std::to_string(labelId);
    const std::string doneLabel = "list.push.copy.done" + std::to_string(labelId);

    const int counterSlotReg = allocateRegister(fctx);
    *fctx.out << "  %" << counterSlotReg << " = alloca i32\n";
    *fctx.out << "  store i32 0, i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << headerLabel << ":\n";
    fctx.currentLabel = headerLabel;
    const int iReg = allocateRegister(fctx);
    *fctx.out << "  %" << iReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int condReg = allocateRegister(fctx);
    *fctx.out << "  %" << condReg << " = icmp slt i32 %" << iReg << ", %" << oldLenReg << "\n";
    *fctx.out << "  br i1 %" << condReg << ", label %" << bodyLabel << ", label %" << doneLabel
              << "\n";

    *fctx.out << bodyLabel << ":\n";
    fctx.currentLabel = bodyLabel;
    const int iForSrcReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForSrcReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int srcPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << srcPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << oldDataReg << ", i32 %" << iForSrcReg << "\n";
    const int valReg = allocateRegister(fctx);
    *fctx.out << "  %" << valReg << " = load " << elementType << ", " << elementType << "* %"
              << srcPtrReg << "\n";
    const int iForDstReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForDstReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int dstPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dstPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << newDataReg << ", i32 %" << iForDstReg << "\n";
    *fctx.out << "  store " << elementType << " %" << valReg << ", " << elementType << "* %"
              << dstPtrReg << "\n";
    const int iForIncReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForIncReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int iNextReg = allocateRegister(fctx);
    *fctx.out << "  %" << iNextReg << " = add i32 %" << iForIncReg << ", 1\n";
    *fctx.out << "  store i32 %" << iNextReg << ", i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << doneLabel << ":\n";
    fctx.currentLabel = doneLabel;

    // Append the pushed value at the new buffer's last slot, then store the
    // new length/data back into the header's own fields in place - the
    // header pointer itself never changes, so every existing alias sees the
    // update, exactly like a struct field-assignment already does.
    const int newElementPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << newElementPtrReg << " = getelementptr " << elementType << ", "
              << elementType << "* %" << newDataReg << ", i32 %" << oldLenReg << "\n";
    *fctx.out << "  store " << elementType << " " << ref(listPush.value, fctx) << ", "
              << elementType << "* %" << newElementPtrReg << "\n";
    *fctx.out << "  store i32 %" << newLenReg << ", i32* %" << lenPtrReg << "\n";
    *fctx.out << "  store " << elementType << "* %" << newDataReg << ", " << elementType << "** %"
              << dataPtrPtrReg << "\n";

    // Unit-typed (see docs/language/0033-lists.md) - deliberately *not*
    // calling defineRegister here, exactly matching how a unit-returning
    // IrCall's dest is handled in emitInstructions: since inferTypesInList
    // already recorded this register's type as "void" (a separate map,
    // populated regardless), nothing downstream ever calls ref() on it -
    // only typeOf() - so it never needs an LLVM register number at all.
}

void LlvmIrEmitter::emitListPop(const IrListPop& listPop, FunctionContext& fctx)
{
    const std::string objectType = typeOf(listPop.list, fctx);
    const std::string headerType = objectType.substr(0, objectType.size() - 1);
    const std::string elementType = listElementType(objectType);
    const std::string listRef = ref(listPop.list, fctx);

    // No bounds check here (matches every other out-of-bounds case in this
    // backend - division, array/slice indexing: the interpreter checks,
    // compiled code does not - see docs/language/0033-lists.md).
    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << listRef << ", i32 0, i32 0\n";
    const int oldLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldLenReg << " = load i32, i32* %" << lenPtrReg << "\n";
    const int newLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << newLenReg << " = sub i32 %" << oldLenReg << ", 1\n";
    *fctx.out << "  store i32 %" << newLenReg << ", i32* %" << lenPtrReg << "\n";

    // No shrink/realloc (matches the "no capacity tracking" simplification -
    // the buffer just stays at its previous size, only the logical length
    // shrinks).
    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << listRef << ", i32 0, i32 1\n";
    const int dataReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataReg << " = load " << elementType << "*, " << elementType << "** %"
              << dataPtrPtrReg << "\n";
    const int elementPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << elementPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << dataReg << ", i32 %" << newLenReg << "\n";
    const int destReg = defineRegister(listPop.dest, fctx);
    *fctx.out << "  %" << destReg << " = load " << elementType << ", " << elementType << "* %"
              << elementPtrReg << "\n";
}

void LlvmIrEmitter::emitStackNew(const IrStackNew& stackNew, FunctionContext& fctx)
{
    // Structurally identical to emitListNew - see that function's own
    // comments for the sizeof idiom (see docs/language/0035-stacks.md).
    const std::string elementType = llvmType(stackNew.elementTypeName);
    const std::string headerType = "{i32, " + elementType + "*}";
    const std::string pointerType = headerType + "*";

    const int sizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizePtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " null, i32 1\n";
    const int sizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizeIntReg << " = ptrtoint " << pointerType << " %" << sizePtrReg
              << " to i64\n";

    const int rawPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawPtrReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";

    const int destReg = defineRegister(stackNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawPtrReg << " to " << pointerType
              << "\n";

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " " << ref(stackNew.dest, fctx) << ", i32 0, i32 0\n";
    *fctx.out << "  store i32 0, i32* %" << lenPtrReg << "\n";

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " " << ref(stackNew.dest, fctx) << ", i32 0, i32 1\n";
    *fctx.out << "  store " << elementType << "* null, " << elementType << "** %" << dataPtrPtrReg
              << "\n";
}

void LlvmIrEmitter::emitStackPush(const IrStackPush& stackPush, FunctionContext& fctx)
{
    // Structurally identical to emitListPush, including the hand-verified
    // alloca/load/store copy loop (no phi - see that function's own
    // comments; see docs/language/0035-stacks.md).
    const std::string objectType = typeOf(stackPush.stack, fctx);
    const std::string headerType =
        objectType.substr(0, objectType.size() - 1); // strip trailing '*'
    const std::string elementType = listElementType(objectType);
    const std::string stackRef = ref(stackPush.stack, fctx);

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << stackRef << ", i32 0, i32 0\n";
    const int oldLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldLenReg << " = load i32, i32* %" << lenPtrReg << "\n";
    const int newLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << newLenReg << " = add i32 %" << oldLenReg << ", 1\n";

    const int elemSizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << elemSizePtrReg << " = getelementptr " << elementType << ", "
              << elementType << "* null, i32 1\n";
    const int elemSizeReg = allocateRegister(fctx);
    *fctx.out << "  %" << elemSizeReg << " = ptrtoint " << elementType << "* %" << elemSizePtrReg
              << " to i64\n";
    const int newLen64Reg = allocateRegister(fctx);
    *fctx.out << "  %" << newLen64Reg << " = zext i32 %" << newLenReg << " to i64\n";
    const int newBytesReg = allocateRegister(fctx);
    *fctx.out << "  %" << newBytesReg << " = mul i64 %" << newLen64Reg << ", %" << elemSizeReg
              << "\n";
    const int rawNewDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawNewDataReg << " = call i8* @malloc(i64 %" << newBytesReg << ")\n";
    const int newDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << newDataReg << " = bitcast i8* %" << rawNewDataReg << " to " << elementType
              << "*\n";

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << stackRef << ", i32 0, i32 1\n";
    const int oldDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldDataReg << " = load " << elementType << "*, " << elementType << "** %"
              << dataPtrPtrReg << "\n";

    const int labelId = fctx.nextLabel++;
    const std::string headerLabel = "stack.push.copy.header" + std::to_string(labelId);
    const std::string bodyLabel = "stack.push.copy.body" + std::to_string(labelId);
    const std::string doneLabel = "stack.push.copy.done" + std::to_string(labelId);

    const int counterSlotReg = allocateRegister(fctx);
    *fctx.out << "  %" << counterSlotReg << " = alloca i32\n";
    *fctx.out << "  store i32 0, i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << headerLabel << ":\n";
    fctx.currentLabel = headerLabel;
    const int iReg = allocateRegister(fctx);
    *fctx.out << "  %" << iReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int condReg = allocateRegister(fctx);
    *fctx.out << "  %" << condReg << " = icmp slt i32 %" << iReg << ", %" << oldLenReg << "\n";
    *fctx.out << "  br i1 %" << condReg << ", label %" << bodyLabel << ", label %" << doneLabel
              << "\n";

    *fctx.out << bodyLabel << ":\n";
    fctx.currentLabel = bodyLabel;
    const int iForSrcReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForSrcReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int srcPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << srcPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << oldDataReg << ", i32 %" << iForSrcReg << "\n";
    const int valReg = allocateRegister(fctx);
    *fctx.out << "  %" << valReg << " = load " << elementType << ", " << elementType << "* %"
              << srcPtrReg << "\n";
    const int iForDstReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForDstReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int dstPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dstPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << newDataReg << ", i32 %" << iForDstReg << "\n";
    *fctx.out << "  store " << elementType << " %" << valReg << ", " << elementType << "* %"
              << dstPtrReg << "\n";
    const int iForIncReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForIncReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int iNextReg = allocateRegister(fctx);
    *fctx.out << "  %" << iNextReg << " = add i32 %" << iForIncReg << ", 1\n";
    *fctx.out << "  store i32 %" << iNextReg << ", i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << doneLabel << ":\n";
    fctx.currentLabel = doneLabel;

    const int newElementPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << newElementPtrReg << " = getelementptr " << elementType << ", "
              << elementType << "* %" << newDataReg << ", i32 %" << oldLenReg << "\n";
    *fctx.out << "  store " << elementType << " " << ref(stackPush.value, fctx) << ", "
              << elementType << "* %" << newElementPtrReg << "\n";
    *fctx.out << "  store i32 %" << newLenReg << ", i32* %" << lenPtrReg << "\n";
    *fctx.out << "  store " << elementType << "* %" << newDataReg << ", " << elementType << "** %"
              << dataPtrPtrReg << "\n";
}

void LlvmIrEmitter::emitStackPop(const IrStackPop& stackPop, FunctionContext& fctx)
{
    // Structurally identical to emitListPop - no bounds check (see that
    // function's own comments; see docs/language/0035-stacks.md).
    const std::string objectType = typeOf(stackPop.stack, fctx);
    const std::string headerType = objectType.substr(0, objectType.size() - 1);
    const std::string elementType = listElementType(objectType);
    const std::string stackRef = ref(stackPop.stack, fctx);

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << stackRef << ", i32 0, i32 0\n";
    const int oldLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldLenReg << " = load i32, i32* %" << lenPtrReg << "\n";
    const int newLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << newLenReg << " = sub i32 %" << oldLenReg << ", 1\n";
    *fctx.out << "  store i32 %" << newLenReg << ", i32* %" << lenPtrReg << "\n";

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << stackRef << ", i32 0, i32 1\n";
    const int dataReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataReg << " = load " << elementType << "*, " << elementType << "** %"
              << dataPtrPtrReg << "\n";
    const int elementPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << elementPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << dataReg << ", i32 %" << newLenReg << "\n";
    const int destReg = defineRegister(stackPop.dest, fctx);
    *fctx.out << "  %" << destReg << " = load " << elementType << ", " << elementType << "* %"
              << elementPtrReg << "\n";
}

void LlvmIrEmitter::emitStackPeek(const IrStackPeek& stackPeek, FunctionContext& fctx)
{
    // The one genuinely new operation: GEP+load the element at length-1,
    // *without* the decrement-and-store-back emitStackPop does above - the
    // stack still owns this element afterward (see docs/language/0035-stacks.md).
    // No bounds check, same reasoning as pop.
    const std::string objectType = typeOf(stackPeek.stack, fctx);
    const std::string headerType = objectType.substr(0, objectType.size() - 1);
    const std::string elementType = listElementType(objectType);
    const std::string stackRef = ref(stackPeek.stack, fctx);

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << stackRef << ", i32 0, i32 0\n";
    const int lenReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenReg << " = load i32, i32* %" << lenPtrReg << "\n";
    const int topIndexReg = allocateRegister(fctx);
    *fctx.out << "  %" << topIndexReg << " = sub i32 %" << lenReg << ", 1\n";

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << stackRef << ", i32 0, i32 1\n";
    const int dataReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataReg << " = load " << elementType << "*, " << elementType << "** %"
              << dataPtrPtrReg << "\n";
    const int elementPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << elementPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << dataReg << ", i32 %" << topIndexReg << "\n";
    const int destReg = defineRegister(stackPeek.dest, fctx);
    *fctx.out << "  %" << destReg << " = load " << elementType << ", " << elementType << "* %"
              << elementPtrReg << "\n";
}

void LlvmIrEmitter::emitMapNew(const IrMapNew& mapNew, FunctionContext& fctx)
{
    // Memoized (see registerMapInstantiation) - already registered by
    // inferTypesInList's own earlier pass over this same instruction, so
    // this just looks the instantiation's header type back up.
    const std::string pointerType =
        llvmType("Map<" + mapNew.keyTypeName + "," + mapNew.valueTypeName + ">");
    const std::string headerType = pointerType.substr(0, pointerType.size() - 1);
    const std::string entryPtrType = "%axea.MapEntry." + mapSetInstantiationId(pointerType) + "*";

    const int sizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizePtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " null, i32 1\n";
    const int sizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizeIntReg << " = ptrtoint " << pointerType << " %" << sizePtrReg
              << " to i64\n";
    const int rawHeaderReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawHeaderReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";
    const int destReg = defineRegister(mapNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawHeaderReg << " to " << pointerType
              << "\n";

    // Initial 8-slot bucket array (see docs/language/0034-maps-and-sets.md) -
    // sizeof(%axea.MapEntry*) via the same null-GEP idiom, times the
    // compile-time-constant 8.
    const int bucketSizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << bucketSizePtrReg << " = getelementptr " << entryPtrType << ", "
              << entryPtrType << "* null, i32 1\n";
    const int bucketElemSizeReg = allocateRegister(fctx);
    *fctx.out << "  %" << bucketElemSizeReg << " = ptrtoint " << entryPtrType << "* %"
              << bucketSizePtrReg << " to i64\n";
    const int bucketBytesReg = allocateRegister(fctx);
    *fctx.out << "  %" << bucketBytesReg << " = mul i64 8, %" << bucketElemSizeReg << "\n";
    const int rawBucketsReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawBucketsReg << " = call i8* @malloc(i64 %" << bucketBytesReg << ")\n";
    const int bucketsReg = allocateRegister(fctx);
    *fctx.out << "  %" << bucketsReg << " = bitcast i8* %" << rawBucketsReg << " to "
              << entryPtrType << "*\n";

    // 8 is a compile-time constant, so the zero-init is unrolled rather than
    // a real loop (same reasoning emitArrayNew-adjacent code elsewhere in
    // this file uses for other statically-known-size cases).
    for (int i = 0; i < 8; ++i)
    {
        const int slotPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << slotPtrReg << " = getelementptr " << entryPtrType << ", "
                  << entryPtrType << "* %" << bucketsReg << ", i32 " << i << "\n";
        *fctx.out << "  store " << entryPtrType << " null, " << entryPtrType << "* %" << slotPtrReg
                  << "\n";
    }

    const int countPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << countPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " %" << destReg << ", i32 0, i32 0\n";
    *fctx.out << "  store i32 0, i32* %" << countPtrReg << "\n";
    const int bucketCountPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << bucketCountPtrReg << " = getelementptr " << headerType << ", "
              << pointerType << " %" << destReg << ", i32 0, i32 1\n";
    *fctx.out << "  store i32 8, i32* %" << bucketCountPtrReg << "\n";
    const int bucketsFieldPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << bucketsFieldPtrReg << " = getelementptr " << headerType << ", "
              << pointerType << " %" << destReg << ", i32 0, i32 2\n";
    *fctx.out << "  store " << entryPtrType << "* %" << bucketsReg << ", " << entryPtrType << "** %"
              << bucketsFieldPtrReg << "\n";
}

void LlvmIrEmitter::emitSetNew(const IrSetNew& setNew, FunctionContext& fctx)
{
    // Memoized (see registerSetInstantiation) - already registered by
    // inferTypesInList's own earlier pass over this same instruction.
    const std::string pointerType = llvmType("Set<" + setNew.elementTypeName + ">");
    const std::string headerType = pointerType.substr(0, pointerType.size() - 1);
    const std::string entryPtrType = "%axea.SetEntry." + mapSetInstantiationId(pointerType) + "*";

    const int sizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizePtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " null, i32 1\n";
    const int sizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizeIntReg << " = ptrtoint " << pointerType << " %" << sizePtrReg
              << " to i64\n";
    const int rawHeaderReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawHeaderReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";
    const int destReg = defineRegister(setNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawHeaderReg << " to " << pointerType
              << "\n";

    const int bucketSizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << bucketSizePtrReg << " = getelementptr " << entryPtrType << ", "
              << entryPtrType << "* null, i32 1\n";
    const int bucketElemSizeReg = allocateRegister(fctx);
    *fctx.out << "  %" << bucketElemSizeReg << " = ptrtoint " << entryPtrType << "* %"
              << bucketSizePtrReg << " to i64\n";
    const int bucketBytesReg = allocateRegister(fctx);
    *fctx.out << "  %" << bucketBytesReg << " = mul i64 8, %" << bucketElemSizeReg << "\n";
    const int rawBucketsReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawBucketsReg << " = call i8* @malloc(i64 %" << bucketBytesReg << ")\n";
    const int bucketsReg = allocateRegister(fctx);
    *fctx.out << "  %" << bucketsReg << " = bitcast i8* %" << rawBucketsReg << " to "
              << entryPtrType << "*\n";

    for (int i = 0; i < 8; ++i)
    {
        const int slotPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << slotPtrReg << " = getelementptr " << entryPtrType << ", "
                  << entryPtrType << "* %" << bucketsReg << ", i32 " << i << "\n";
        *fctx.out << "  store " << entryPtrType << " null, " << entryPtrType << "* %" << slotPtrReg
                  << "\n";
    }

    const int countPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << countPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " %" << destReg << ", i32 0, i32 0\n";
    *fctx.out << "  store i32 0, i32* %" << countPtrReg << "\n";
    const int bucketCountPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << bucketCountPtrReg << " = getelementptr " << headerType << ", "
              << pointerType << " %" << destReg << ", i32 0, i32 1\n";
    *fctx.out << "  store i32 8, i32* %" << bucketCountPtrReg << "\n";
    const int bucketsFieldPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << bucketsFieldPtrReg << " = getelementptr " << headerType << ", "
              << pointerType << " %" << destReg << ", i32 0, i32 2\n";
    *fctx.out << "  store " << entryPtrType << "* %" << bucketsReg << ", " << entryPtrType << "** %"
              << bucketsFieldPtrReg << "\n";
}

void LlvmIrEmitter::emitMapSet(const IrMapSet& mapSet, FunctionContext& fctx)
{
    const std::string mapType = typeOf(mapSet.map, fctx);
    const std::string id = mapSetInstantiationId(mapType);
    const std::string keyType = typeOf(mapSet.key, fctx);
    const std::string valueType = typeOf(mapSet.value, fctx);
    // Unit-typed (see docs/language/0033-lists.md's identical reasoning for
    // push) - no defineRegister call, mirroring emitListPush.
    *fctx.out << "  call void @axea.map." << id << ".set(" << mapType << " "
              << ref(mapSet.map, fctx) << ", " << keyType << " " << ref(mapSet.key, fctx) << ", "
              << valueType << " " << ref(mapSet.value, fctx) << ")\n";
}

void LlvmIrEmitter::emitMapGet(const IrMapGet& mapGet, FunctionContext& fctx)
{
    const std::string mapType = typeOf(mapGet.map, fctx);
    const std::string id = mapSetInstantiationId(mapType);
    const std::string keyType = typeOf(mapGet.key, fctx);
    const std::string valueType = typeOf(mapGet.dest, fctx);
    const int destReg = defineRegister(mapGet.dest, fctx);
    *fctx.out << "  %" << destReg << " = call " << valueType << " @axea.map." << id << ".get("
              << mapType << " " << ref(mapGet.map, fctx) << ", " << keyType << " "
              << ref(mapGet.key, fctx) << ")\n";
}

void LlvmIrEmitter::emitMapContains(const IrMapContains& mapContains, FunctionContext& fctx)
{
    const std::string mapType = typeOf(mapContains.map, fctx);
    const std::string id = mapSetInstantiationId(mapType);
    const std::string keyType = typeOf(mapContains.key, fctx);
    const int destReg = defineRegister(mapContains.dest, fctx);
    *fctx.out << "  %" << destReg << " = call i1 @axea.map." << id << ".contains(" << mapType << " "
              << ref(mapContains.map, fctx) << ", " << keyType << " " << ref(mapContains.key, fctx)
              << ")\n";
}

void LlvmIrEmitter::emitMapRemove(const IrMapRemove& mapRemove, FunctionContext& fctx)
{
    const std::string mapType = typeOf(mapRemove.map, fctx);
    const std::string id = mapSetInstantiationId(mapType);
    const std::string keyType = typeOf(mapRemove.key, fctx);
    *fctx.out << "  call void @axea.map." << id << ".remove(" << mapType << " "
              << ref(mapRemove.map, fctx) << ", " << keyType << " " << ref(mapRemove.key, fctx)
              << ")\n";
}

void LlvmIrEmitter::emitSetAdd(const IrSetAdd& setAdd, FunctionContext& fctx)
{
    const std::string setType = typeOf(setAdd.set, fctx);
    const std::string id = mapSetInstantiationId(setType);
    const std::string elementType = typeOf(setAdd.value, fctx);
    *fctx.out << "  call void @axea.set." << id << ".add(" << setType << " "
              << ref(setAdd.set, fctx) << ", " << elementType << " " << ref(setAdd.value, fctx)
              << ")\n";
}

void LlvmIrEmitter::emitSetContains(const IrSetContains& setContains, FunctionContext& fctx)
{
    const std::string setType = typeOf(setContains.set, fctx);
    const std::string id = mapSetInstantiationId(setType);
    const std::string elementType = typeOf(setContains.value, fctx);
    const int destReg = defineRegister(setContains.dest, fctx);
    *fctx.out << "  %" << destReg << " = call i1 @axea.set." << id << ".contains(" << setType << " "
              << ref(setContains.set, fctx) << ", " << elementType << " "
              << ref(setContains.value, fctx) << ")\n";
}

void LlvmIrEmitter::emitSetRemove(const IrSetRemove& setRemove, FunctionContext& fctx)
{
    const std::string setType = typeOf(setRemove.set, fctx);
    const std::string id = mapSetInstantiationId(setType);
    const std::string elementType = typeOf(setRemove.value, fctx);
    *fctx.out << "  call void @axea.set." << id << ".remove(" << setType << " "
              << ref(setRemove.set, fctx) << ", " << elementType << " "
              << ref(setRemove.value, fctx) << ")\n";
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
            const auto paramTypesIt = functionParamTypes_.find(call->callee);

            // (LLVM type, value ref) per argument, resolved before the call
            // line itself, since an array->slice conversion needs its own
            // instructions emitted first (see docs/language/0032-slices.md).
            std::vector<std::pair<std::string, std::string>> args;
            args.reserve(call->args.size());
            for (std::size_t i = 0; i < call->args.size(); ++i)
            {
                const std::string argType = typeOf(call->args[i], fctx);
                const std::string argRef = ref(call->args[i], fctx);

                const bool needsSliceConversion = paramTypesIt != functionParamTypes_.end() &&
                                                  i < paramTypesIt->second.size() &&
                                                  paramTypesIt->second[i].starts_with("slice<") &&
                                                  !argType.empty() && argType.front() == '[';
                if (!needsSliceConversion)
                {
                    // Already the right shape - including an existing slice
                    // forwarded straight through to another slice parameter,
                    // which needs no conversion at all.
                    args.emplace_back(argType, argRef);
                    continue;
                }

                const std::string elementType = arrayElementType(argType);
                const std::string sliceType = "{" + elementType + "*, i32}";
                const std::string llvmArrayType = argType.substr(0, argType.size() - 1);

                const int flatPtrReg = allocateRegister(fctx);
                *fctx.out << "  %" << flatPtrReg << " = getelementptr " << llvmArrayType << ", "
                          << argType << " " << argRef << ", i32 0, i32 0\n";
                const int withPtrReg = allocateRegister(fctx);
                *fctx.out << "  %" << withPtrReg << " = insertvalue " << sliceType << " undef, "
                          << elementType << "* %" << flatPtrReg << ", 0\n";
                const int withLenReg = allocateRegister(fctx);
                *fctx.out << "  %" << withLenReg << " = insertvalue " << sliceType << " %"
                          << withPtrReg << ", i32 " << arraySizeFromPointerType(argType) << ", 1\n";

                args.emplace_back(sliceType, "%" + std::to_string(withLenReg));
            }

            *fctx.out << "  ";
            if (returnType != "void")
            {
                const int destReg = defineRegister(call->dest, fctx);
                *fctx.out << "%" << destReg << " = ";
            }
            *fctx.out << "call " << returnType << " @" << call->callee << "(";
            for (std::size_t i = 0; i < args.size(); ++i)
            {
                *fctx.out << (i > 0 ? ", " : "") << args[i].first << " " << args[i].second;
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
        if (const auto* listNew = dynamic_cast<const IrListNew*>(inst.get()))
        {
            emitListNew(*listNew, fctx);
            continue;
        }
        if (const auto* listPush = dynamic_cast<const IrListPush*>(inst.get()))
        {
            emitListPush(*listPush, fctx);
            continue;
        }
        if (const auto* listPop = dynamic_cast<const IrListPop*>(inst.get()))
        {
            emitListPop(*listPop, fctx);
            continue;
        }
        if (const auto* stackNew = dynamic_cast<const IrStackNew*>(inst.get()))
        {
            emitStackNew(*stackNew, fctx);
            continue;
        }
        if (const auto* stackPush = dynamic_cast<const IrStackPush*>(inst.get()))
        {
            emitStackPush(*stackPush, fctx);
            continue;
        }
        if (const auto* stackPop = dynamic_cast<const IrStackPop*>(inst.get()))
        {
            emitStackPop(*stackPop, fctx);
            continue;
        }
        if (const auto* stackPeek = dynamic_cast<const IrStackPeek*>(inst.get()))
        {
            emitStackPeek(*stackPeek, fctx);
            continue;
        }
        if (const auto* mapNew = dynamic_cast<const IrMapNew*>(inst.get()))
        {
            emitMapNew(*mapNew, fctx);
            continue;
        }
        if (const auto* setNew = dynamic_cast<const IrSetNew*>(inst.get()))
        {
            emitSetNew(*setNew, fctx);
            continue;
        }
        if (const auto* mapSet = dynamic_cast<const IrMapSet*>(inst.get()))
        {
            emitMapSet(*mapSet, fctx);
            continue;
        }
        if (const auto* mapGet = dynamic_cast<const IrMapGet*>(inst.get()))
        {
            emitMapGet(*mapGet, fctx);
            continue;
        }
        if (const auto* mapContains = dynamic_cast<const IrMapContains*>(inst.get()))
        {
            emitMapContains(*mapContains, fctx);
            continue;
        }
        if (const auto* mapRemove = dynamic_cast<const IrMapRemove*>(inst.get()))
        {
            emitMapRemove(*mapRemove, fctx);
            continue;
        }
        if (const auto* setAdd = dynamic_cast<const IrSetAdd*>(inst.get()))
        {
            emitSetAdd(*setAdd, fctx);
            continue;
        }
        if (const auto* setContains = dynamic_cast<const IrSetContains*>(inst.get()))
        {
            emitSetContains(*setContains, fctx);
            continue;
        }
        if (const auto* setRemove = dynamic_cast<const IrSetRemove*>(inst.get()))
        {
            emitSetRemove(*setRemove, fctx);
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
        else if (isMapType(llvmTypeStr) || isSetType(llvmTypeStr))
        {
            // No iteration this phase (see docs/language/0034-maps-and-sets.md),
            // so - unlike List's own runtime print loop just below - there's
            // no way to print contents; falls back to "Map(N entries)"/
            // "Set(N entries)" using the O(1) count field alone (field 0 of
            // the header, GEP+load - same shape List's own ".length" uses).
            const std::string headerType = llvmTypeStr.substr(0, llvmTypeStr.size() - 1);
            const std::string label = isMapType(llvmTypeStr) ? "Map(" : "Set(";
            const std::string prefix = stringPtrConstant(label);
            const std::string suffix = stringPtrConstant(" entries)");
            const std::string countFmt = stringPtrConstant("%s = %s%d%s\n");

            const int countPtrReg = allocateRegister(fctx);
            out << "  %" << countPtrReg << " = getelementptr " << headerType << ", " << llvmTypeStr
                << " " << ref(axeaReg, fctx) << ", i32 0, i32 0\n";
            const int countReg = allocateRegister(fctx);
            out << "  %" << countReg << " = load i32, i32* %" << countPtrReg << "\n";
            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                << countFmt << ", i8* " << namePtr << ", i8* " << prefix << ", i32 %" << countReg
                << ", i8* " << suffix << ")\n";
        }
        else if (isListType(llvmTypeStr))
        {
            // A List's length is only known at runtime (see
            // docs/language/0033-lists.md), unlike an array's compile-time-
            // constant size - so unlike the array branch above, this needs a
            // genuine runtime print loop, not a compile-time-unrolled one.
            // Structured as "print element 0 unconditionally if length > 0,
            // then loop i=1..length printing ', ' + element[i]" so only one
            // runtime branch (empty-vs-not) is needed up front, rather than a
            // per-iteration "is this the first element" check - same
            // alloca/load/store loop-counter idiom emitListPush already uses
            // (no phi, for the same unnamed-sequential-register reason).
            const std::string headerType = llvmTypeStr.substr(0, llvmTypeStr.size() - 1);
            const std::string elementType = listElementType(llvmTypeStr);
            const std::string openBracket = stringPtrConstant("[");
            const std::string closeBracket = stringPtrConstant("]");
            const std::string comma = stringPtrConstant(", ");
            const std::string bareIntFmt = stringPtrConstant("%d");
            const std::string bareStrFmt = stringPtrConstant("%s");

            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                << structPrefixFmt << ", i8* " << namePtr << ")\n";
            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                << openBracket << ")\n";

            const int lenPtrReg = allocateRegister(fctx);
            out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", " << llvmTypeStr
                << " " << ref(axeaReg, fctx) << ", i32 0, i32 0\n";
            const int lenReg = allocateRegister(fctx);
            out << "  %" << lenReg << " = load i32, i32* %" << lenPtrReg << "\n";
            const int dataPtrPtrReg = allocateRegister(fctx);
            out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", "
                << llvmTypeStr << " " << ref(axeaReg, fctx) << ", i32 0, i32 1\n";
            const int dataReg = allocateRegister(fctx);
            out << "  %" << dataReg << " = load " << elementType << "*, " << elementType << "** %"
                << dataPtrPtrReg << "\n";

            const int labelId = fctx.nextLabel++;
            const std::string firstLabel = "print.list.first" + std::to_string(labelId);
            const std::string headerLabel = "print.list.header" + std::to_string(labelId);
            const std::string bodyLabel = "print.list.body" + std::to_string(labelId);
            const std::string skipLabel = "print.list.skip" + std::to_string(labelId);
            const std::string doneLabel = "print.list.done" + std::to_string(labelId);

            const int isEmptyReg = allocateRegister(fctx);
            out << "  %" << isEmptyReg << " = icmp eq i32 %" << lenReg << ", 0\n";
            out << "  br i1 %" << isEmptyReg << ", label %" << skipLabel << ", label %"
                << firstLabel << "\n";

            out << firstLabel << ":\n";
            fctx.currentLabel = firstLabel;
            {
                const int elementPtrReg = allocateRegister(fctx);
                out << "  %" << elementPtrReg << " = getelementptr " << elementType << ", "
                    << elementType << "* %" << dataReg << ", i32 0\n";
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
            out << "  br label %" << skipLabel << "\n";

            out << skipLabel << ":\n";
            fctx.currentLabel = skipLabel;
            const int counterSlotReg = allocateRegister(fctx);
            out << "  %" << counterSlotReg << " = alloca i32\n";
            out << "  store i32 1, i32* %" << counterSlotReg << "\n";
            out << "  br label %" << headerLabel << "\n";

            out << headerLabel << ":\n";
            fctx.currentLabel = headerLabel;
            const int iReg = allocateRegister(fctx);
            out << "  %" << iReg << " = load i32, i32* %" << counterSlotReg << "\n";
            const int condReg = allocateRegister(fctx);
            out << "  %" << condReg << " = icmp slt i32 %" << iReg << ", %" << lenReg << "\n";
            out << "  br i1 %" << condReg << ", label %" << bodyLabel << ", label %" << doneLabel
                << "\n";

            out << bodyLabel << ":\n";
            fctx.currentLabel = bodyLabel;
            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* " << comma
                << ")\n";
            const int iForElemReg = allocateRegister(fctx);
            out << "  %" << iForElemReg << " = load i32, i32* %" << counterSlotReg << "\n";
            const int elementPtrReg = allocateRegister(fctx);
            out << "  %" << elementPtrReg << " = getelementptr " << elementType << ", "
                << elementType << "* %" << dataReg << ", i32 %" << iForElemReg << "\n";
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
                out << "  %" << selReg << " = select i1 %" << elementValReg << ", i8* " << trueStr
                    << ", i8* " << falseStr << "\n";
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
                out << "  call void @axea.print." << nestedStructName << "(" << elementType << " %"
                    << elementValReg << ")\n";
            }
            const int iForIncReg = allocateRegister(fctx);
            out << "  %" << iForIncReg << " = load i32, i32* %" << counterSlotReg << "\n";
            const int iNextReg = allocateRegister(fctx);
            out << "  %" << iNextReg << " = add i32 %" << iForIncReg << ", 1\n";
            out << "  store i32 %" << iNextReg << ", i32* %" << counterSlotReg << "\n";
            out << "  br label %" << headerLabel << "\n";

            out << doneLabel << ":\n";
            fctx.currentLabel = doneLabel;
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
        functionParamTypes_[function.name] = function.paramTypes;
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

    // Every function and topLevel (via mainOut, built above) has now been
    // through inferTypes/inferTypesInList - every Map<K,V>/Set<T>
    // instantiation actually used anywhere in the program is registered
    // (see registerMapInstantiation/registerSetInstantiation), so it's only
    // safe to snapshot mapSetTypeDeclsText_/mapSetRuntimeText_ now, not
    // earlier alongside emitStructTypeDecls above - LLVM doesn't care about
    // textual declaration order, so appending them here instead of at the
    // very top changes nothing about the module's meaning (see
    // docs/language/0034-maps-and-sets.md).
    out << mapSetTypeDeclsText_.str();
    out << mapSetRuntimeText_.str();
    out << helpers.str();
    out << mainOut.str();

    return out.str();
}
