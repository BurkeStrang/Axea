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
    if (axeaTypeName == "char")
    {
        // A genuinely distinct integer width from "i32" (see
        // docs/language/0044-char.md) - not a stylistic choice: i32 and
        // char share every arithmetic/comparison opcode generically (LLVM
        // ops are width-agnostic text), so if char also emitted as plain
        // "i32" there would be no way for the top-level print dispatch (or
        // isCharType below) to tell a char register apart from a real i32
        // one. i24 comfortably covers the full Unicode range (0..0x10FFFF
        // needs 21 bits) while staying textually unmistakable.
        return "i24";
    }
    if (axeaTypeName == "str")
    {
        return "i8*";
    }
    if (axeaTypeName == "String")
    {
        // Axea's own owned, growable byte buffer (see
        // docs/language/0042-string.md) - a single concrete type, not
        // generic, so (unlike every collection above) this needs no
        // registerXInstantiation call: the same fixed text every time,
        // deliberately the exact same shape List<i8> would have.
        return "{i32, i8*}*";
    }
    if (axeaTypeName == "Buffer")
    {
        // Axea's own mutable, amortized-growth text-construction type
        // (see docs/language/0043-buffer.md) - not generic either, same
        // fixed-text treatment as "String" above, but with an extra i32
        // "capacity" field between length and data.
        return "{i32, i32, i8*}*";
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
    if (axeaTypeName.starts_with("SortedMap<"))
    {
        // SortedMap<K,V> (see docs/language/0040-sorted-maps.md): a pointer
        // to a small anonymous 2-field heap header {count, root}, mirroring
        // List<T>'s own "always by pointer, mutated in place" header - but
        // its second field is a *named* self-referential node pointer
        // (%axea.SortedMapNode.<id>*, declared by
        // registerSortedMapInstantiation), needed for the same reason
        // Map/Set's own entry type is: a node's own `left`/`right` fields
        // point at another node of the identical shape.
        const std::string args = axeaTypeName.substr(10, axeaTypeName.size() - 11);
        const auto comma = findTopLevelComma(args);
        return registerSortedMapInstantiation(args.substr(0, comma), args.substr(comma + 1));
    }
    if (axeaTypeName.starts_with("SortedSet<"))
    {
        // SortedSet<T> (see docs/language/0041-sorted-sets.md): same
        // reasoning as SortedMap<K,V> above, with %axea.SortedSetNode.<id>
        // (key, height, left, right - no value field) in place of
        // %axea.SortedMapNode.<id>.
        return registerSortedSetInstantiation(axeaTypeName.substr(10, axeaTypeName.size() - 11));
    }
    if (axeaTypeName.starts_with("LinkedList<"))
    {
        // LinkedList<T> (see docs/language/0036-linked-lists.md): a pointer
        // to a small anonymous 3-field heap header {length, head, tail},
        // mirroring List's own "always by pointer, mutated in place" header.
        // Each distinct element type gets its own named, numbered node type
        // (%axea.LLNode.<id>, declared by registerLinkedListInstantiation) -
        // named, not anonymous, because its own self-reference (`prev`/`next`
        // pointing at another node of the same type) can only be expressed
        // in LLVM through a named type, exactly like Map/Set's own entry
        // type above.
        return registerLinkedListInstantiation(axeaTypeName.substr(11, axeaTypeName.size() - 12));
    }
    if (axeaTypeName.starts_with("Deque<"))
    {
        // Deque<T> (see docs/language/0037-deques.md): a pointer to a small
        // anonymous 3-field heap header {count, start, data} - no named
        // type or monomorphization registration needed at all (unlike
        // Map/Set/LinkedList above): its third field is a plain T*, not a
        // self-referential entry pointer, so it can stay fully anonymous
        // exactly like List<T>'s own 2-field header does.
        const std::string elementName = axeaTypeName.substr(6, axeaTypeName.size() - 7);
        return "{i32, i32, " + llvmType(elementName) + "*}*";
    }
    if (axeaTypeName.starts_with("Queue<"))
    {
        // Queue<T> (see docs/language/0038-queues.md) - a FIFO collection
        // backed internally by Deque<T>'s own machinery, so this produces
        // the *exact same text* the Deque<elem> branch above would -
        // genuinely the same LLVM type, not merely a similarly-shaped one
        // (mirrors Stack<T>/List<T>'s own identical relationship). The
        // distinction between Deque<T> and Queue<T> lives entirely at the
        // Axea/IR level; isDequeType's own existing structural check already
        // handles a Queue<T> value correctly with no changes at all.
        const std::string elementName = axeaTypeName.substr(6, axeaTypeName.size() - 7);
        return "{i32, i32, " + llvmType(elementName) + "*}*";
    }
    if (axeaTypeName.starts_with("PriorityQueue<"))
    {
        // PriorityQueue<T> (see docs/language/0039-priority-queues.md): a
        // real binary heap, but stored in the *exact same* two-field header
        // List<T>/Stack<T> already use - "{i32, " + llvmType(elementName) +
        // "*}*" - genuinely the same LLVM type, not merely similarly shaped.
        // The heap-vs-list/stack distinction lives entirely at the Axea/IR
        // level (TypeKind::PriorityQueue,
        // IrPriorityQueueNew/Push/Pop/Peek); isListType's own existing
        // structural check already handles a PriorityQueue<T> value
        // correctly for `.length` and top-level printing, no new predicate
        // needed - the same free ride Stack<T> got before it.
        const std::string elementName = axeaTypeName.substr(14, axeaTypeName.size() - 15);
        return "{i32, " + llvmType(elementName) + "*}*";
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

bool LlvmIrEmitter::isLinkedListType(const std::string& type) const
{
    return type.starts_with("{i32, %axea.LLNode.");
}

std::string LlvmIrEmitter::linkedListInstantiationId(const std::string& type) const
{
    // "{i32, %axea.LLNode.<id>*, %axea.LLNode.<id>*}*" - the id sits between
    // the first "%axea.LLNode." and the '*' right after it.
    const auto start = type.find("%axea.LLNode.") + std::string("%axea.LLNode.").size();
    const auto starPos = type.find('*', start);
    return type.substr(start, starPos - start);
}

bool LlvmIrEmitter::isDequeType(const std::string& type) const
{
    // Excludes "%axea." explicitly, not just a trailing "**}*" check -
    // Deque<str>'s own data field is "i8**" (str is itself "i8*"), the same
    // double-star suffix Map/Set's own entry-pointer-pointer field has, so a
    // suffix-only check would misfire on that case.
    return type.starts_with("{i32, i32, ") && type.ends_with("}*") &&
           !type.starts_with("{i32, i32, %axea.");
}

std::string LlvmIrEmitter::dequeElementType(const std::string& type) const
{
    // "{i32, i32, T*}*" - strip the leading "{i32, i32, " and trailing "*}*".
    return type.substr(11, type.size() - 11 - std::string("*}*").size());
}

bool LlvmIrEmitter::isSortedMapType(const std::string& type) const
{
    return type.starts_with("{i32, %axea.SortedMapNode.");
}

std::string LlvmIrEmitter::sortedMapInstantiationId(const std::string& type) const
{
    // "{i32, %axea.SortedMapNode.<id>*}*" - the id sits between the last
    // '.' and the trailing "*}*".
    const auto lastDot = type.rfind('.');
    return type.substr(lastDot + 1, type.size() - lastDot - 1 - std::string("*}*").size());
}

std::string LlvmIrEmitter::sortedMapValueLlvmType(const std::string& sortedMapHeaderType) const
{
    return sortedMapValueLlvmTypeById_.at(std::stoi(sortedMapInstantiationId(sortedMapHeaderType)));
}

bool LlvmIrEmitter::isSortedSetType(const std::string& type) const
{
    return type.starts_with("{i32, %axea.SortedSetNode.");
}

std::string LlvmIrEmitter::sortedSetInstantiationId(const std::string& type) const
{
    // "{i32, %axea.SortedSetNode.<id>*}*" - the id sits between the last
    // '.' and the trailing "*}*".
    const auto lastDot = type.rfind('.');
    return type.substr(lastDot + 1, type.size() - lastDot - 1 - std::string("*}*").size());
}

bool LlvmIrEmitter::isStringType(const std::string& type) const
{
    return type == "{i32, i8*}*";
}

bool LlvmIrEmitter::isBufferType(const std::string& type) const
{
    return type == "{i32, i32, i8*}*";
}

bool LlvmIrEmitter::isCharType(const std::string& type) const
{
    return type == "i24";
}

std::string LlvmIrEmitter::mapValueLlvmType(const std::string& mapHeaderType) const
{
    return mapValueLlvmTypeById_.at(std::stoi(mapSetInstantiationId(mapHeaderType)));
}

std::string LlvmIrEmitter::linkedListElementLlvmType(const std::string& linkedListHeaderType) const
{
    return linkedListElementLlvmTypeById_.at(
        std::stoi(linkedListInstantiationId(linkedListHeaderType)));
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

std::string LlvmIrEmitter::resolveStrPtr(int reg, FunctionContext& fctx) const
{
    if (typeOf(reg, fctx) == "i8*")
    {
        return ref(reg, fctx);
    }
    // Must be a String header ({i32, i8*}*) - TypeChecker's own
    // isStrCoercible already guarantees no other type reaches here (see
    // docs/language/0042-string.md).
    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr {i32, i8*}, {i32, i8*}* "
              << ref(reg, fctx) << ", i32 0, i32 1\n";
    const int dataPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrReg << " = load i8*, i8** %" << dataPtrPtrReg << "\n";
    return "%" + std::to_string(dataPtrReg);
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
        else if (const auto* constChar = dynamic_cast<const IrConstChar*>(inst.get()))
        {
            fctx.registerTypes[constChar->dest] = "i24";
        }
        else if (const auto* strSlice = dynamic_cast<const IrStrSlice*>(inst.get()))
        {
            // Always a bare i8* (see docs/language/0045-str-slicing.md) -
            // the result is always a fresh str, never a String, regardless
            // of whether `object` itself was a str or a String.
            fctx.registerTypes[strSlice->dest] = "i8*";
        }
        else if (const auto* parse = dynamic_cast<const IrParse*>(inst.get()))
        {
            // See docs/language/0046-generic-methods.md - TypeChecker
            // already restricts targetType to "i32"/"bool".
            fctx.registerTypes[parse->dest] = parse->targetType == "bool" ? "i1" : "i32";
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
                isSetType(objectType) || isLinkedListType(objectType) || isDequeType(objectType) ||
                isSortedMapType(objectType) || isSortedSetType(objectType) || objectType == "i8*" ||
                isStringType(objectType) || isBufferType(objectType))
            {
                // Only "length"/"bytes"/"capacity" ever reach a slice,
                // List, Map, Set, LinkedList, Deque, SortedMap, SortedSet,
                // str, String, or Buffer via IrFieldGet, and all of them
                // are i32 - TypeChecker already guarantees this (see
                // docs/language/0032-slices.md, 0033-lists.md,
                // 0034-maps-and-sets.md, 0036-linked-lists.md,
                // 0037-deques.md, 0040-sorted-maps.md, 0041-sorted-sets.md,
                // 0047-unicode.md). str/String/Buffer are explicitly listed
                // (not left to fall into isDequeType's own coincidental
                // structural match the way Buffer's raw header shape
                // would) for the same "explicit, not accidental" reasoning
                // docs/language/0043-buffer.md already established.
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
        else if (const auto* priorityQueueNew = dynamic_cast<const IrPriorityQueueNew*>(inst.get()))
        {
            // Same shape llvmType("PriorityQueue<T>") itself produces (see
            // docs/language/0039-priority-queues.md) - the exact same text
            // llvmType("List<T>")/llvmType("Stack<T>") would.
            fctx.registerTypes[priorityQueueNew->dest] =
                "{i32, " + llvmType(priorityQueueNew->elementTypeName) + "*}*";
        }
        else if (const auto* priorityQueuePush =
                     dynamic_cast<const IrPriorityQueuePush*>(inst.get()))
        {
            fctx.registerTypes[priorityQueuePush->dest] = "void";
        }
        else if (const auto* priorityQueuePop = dynamic_cast<const IrPriorityQueuePop*>(inst.get()))
        {
            fctx.registerTypes[priorityQueuePop->dest] =
                listElementType(typeOf(priorityQueuePop->priorityQueue, fctx));
        }
        else if (const auto* priorityQueuePeek =
                     dynamic_cast<const IrPriorityQueuePeek*>(inst.get()))
        {
            fctx.registerTypes[priorityQueuePeek->dest] =
                listElementType(typeOf(priorityQueuePeek->priorityQueue, fctx));
        }
        else if (const auto* linkedListNew = dynamic_cast<const IrLinkedListNew*>(inst.get()))
        {
            // Drives registration (see registerLinkedListInstantiation) -
            // same reasoning as IrMapNew/IrSetNew above.
            fctx.registerTypes[linkedListNew->dest] =
                llvmType("LinkedList<" + linkedListNew->elementTypeName + ">");
        }
        else if (dynamic_cast<const IrLinkedListPushFront*>(inst.get()) ||
                 dynamic_cast<const IrLinkedListPushBack*>(inst.get()))
        {
            // Unit-typed, same reasoning as IrListPush above.
            fctx.registerTypes[inst->dest] = "void";
        }
        else if (const auto* popFront = dynamic_cast<const IrLinkedListPopFront*>(inst.get()))
        {
            fctx.registerTypes[popFront->dest] =
                linkedListElementLlvmType(typeOf(popFront->list, fctx));
        }
        else if (const auto* popBack = dynamic_cast<const IrLinkedListPopBack*>(inst.get()))
        {
            fctx.registerTypes[popBack->dest] =
                linkedListElementLlvmType(typeOf(popBack->list, fctx));
        }
        else if (const auto* dequeNew = dynamic_cast<const IrDequeNew*>(inst.get()))
        {
            // Drives registration only in the sense of resolving llvmType -
            // Deque<T> needs no actual registerXInstantiation call (see
            // llvmType's own Deque<T> branch: no monomorphization at all).
            fctx.registerTypes[dequeNew->dest] =
                llvmType("Deque<" + dequeNew->elementTypeName + ">");
        }
        else if (dynamic_cast<const IrDequePushFront*>(inst.get()) ||
                 dynamic_cast<const IrDequePushBack*>(inst.get()))
        {
            // Unit-typed, same reasoning as IrListPush above.
            fctx.registerTypes[inst->dest] = "void";
        }
        else if (const auto* dequePopFront = dynamic_cast<const IrDequePopFront*>(inst.get()))
        {
            // Unlike LinkedList's own linkedListElementLlvmType (which needs
            // a side-table, since a node's element type doesn't appear in
            // its own header text), Deque<T>'s element type sits directly in
            // its header string - dequeElementType reads it straight off,
            // no side-table needed at all.
            fctx.registerTypes[dequePopFront->dest] =
                dequeElementType(typeOf(dequePopFront->deque, fctx));
        }
        else if (const auto* dequePopBack = dynamic_cast<const IrDequePopBack*>(inst.get()))
        {
            fctx.registerTypes[dequePopBack->dest] =
                dequeElementType(typeOf(dequePopBack->deque, fctx));
        }
        else if (const auto* queueNew = dynamic_cast<const IrQueueNew*>(inst.get()))
        {
            // Same reasoning as IrDequeNew above - Queue<T> needs no
            // registerXInstantiation call either (see docs/language/0038-queues.md).
            fctx.registerTypes[queueNew->dest] =
                llvmType("Queue<" + queueNew->elementTypeName + ">");
        }
        else if (dynamic_cast<const IrQueueEnqueue*>(inst.get()))
        {
            // Unit-typed, same reasoning as IrListPush above.
            fctx.registerTypes[inst->dest] = "void";
        }
        else if (const auto* queueDequeue = dynamic_cast<const IrQueueDequeue*>(inst.get()))
        {
            // Same reasoning as IrDequePopFront above - dequeElementType
            // reads the element type straight off Queue<T>'s own header
            // string too (it's the literal same header Deque<T> has).
            fctx.registerTypes[queueDequeue->dest] =
                dequeElementType(typeOf(queueDequeue->queue, fctx));
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
        else if (const auto* sortedMapNew = dynamic_cast<const IrSortedMapNew*>(inst.get()))
        {
            // Drives registration (see registerSortedMapInstantiation) -
            // same reasoning as IrMapNew above.
            fctx.registerTypes[sortedMapNew->dest] = llvmType(
                "SortedMap<" + sortedMapNew->keyTypeName + "," + sortedMapNew->valueTypeName + ">");
        }
        else if (dynamic_cast<const IrSortedMapSet*>(inst.get()) ||
                 dynamic_cast<const IrSortedMapRemove*>(inst.get()))
        {
            // Unit-typed, same reasoning as IrMapSet/IrMapRemove above.
            fctx.registerTypes[inst->dest] = "void";
        }
        else if (const auto* sortedMapGet = dynamic_cast<const IrSortedMapGet*>(inst.get()))
        {
            fctx.registerTypes[sortedMapGet->dest] =
                sortedMapValueLlvmType(typeOf(sortedMapGet->sortedMap, fctx));
        }
        else if (dynamic_cast<const IrSortedMapContains*>(inst.get()))
        {
            fctx.registerTypes[inst->dest] = "i1";
        }
        else if (const auto* sortedSetNew = dynamic_cast<const IrSortedSetNew*>(inst.get()))
        {
            // Drives registration (see registerSortedSetInstantiation) -
            // same reasoning as IrSetNew above.
            fctx.registerTypes[sortedSetNew->dest] =
                llvmType("SortedSet<" + sortedSetNew->elementTypeName + ">");
        }
        else if (dynamic_cast<const IrSortedSetAdd*>(inst.get()) ||
                 dynamic_cast<const IrSortedSetRemove*>(inst.get()))
        {
            // Unit-typed, same reasoning as IrSetAdd/IrSetRemove above.
            fctx.registerTypes[inst->dest] = "void";
        }
        else if (dynamic_cast<const IrSortedSetContains*>(inst.get()))
        {
            fctx.registerTypes[inst->dest] = "i1";
        }
        else if (const auto* stringNew = dynamic_cast<const IrStringNew*>(inst.get()))
        {
            fctx.registerTypes[stringNew->dest] = llvmType("String");
        }
        else if (dynamic_cast<const IrStringAppend*>(inst.get()))
        {
            // Unit-typed, same reasoning as IrListPush above.
            fctx.registerTypes[inst->dest] = "void";
        }
        else if (const auto* bufferNew = dynamic_cast<const IrBufferNew*>(inst.get()))
        {
            fctx.registerTypes[bufferNew->dest] = llvmType("Buffer");
        }
        else if (dynamic_cast<const IrBufferAppend*>(inst.get()) ||
                 dynamic_cast<const IrBufferAppendLine*>(inst.get()) ||
                 dynamic_cast<const IrBufferClear*>(inst.get()) ||
                 dynamic_cast<const IrBufferReserve*>(inst.get()))
        {
            // Unit-typed, same reasoning as IrStringAppend above.
            fctx.registerTypes[inst->dest] = "void";
        }
        else if (const auto* bufferFinish = dynamic_cast<const IrBufferFinish*>(inst.get()))
        {
            fctx.registerTypes[bufferFinish->dest] = llvmType("String");
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
            else if (isDequeType(objectType))
            {
                // Checked before isListType, same "explicit, not accidental"
                // reasoning as every other 3-field collection here (see
                // docs/language/0037-deques.md) - a Deque header would
                // otherwise spuriously match isListType's own looser test.
                fctx.registerTypes[indexGet->dest] = dequeElementType(objectType);
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

    // LinkedList<T> (see docs/language/0036-linked-lists.md). Each of the
    // four templates below needs exactly one `br i1` to decide whether the
    // *opposite* end pointer also needs updating (is the list currently
    // empty, for push; does it become empty, for pop) - the reason these are
    // template-text runtime functions (named %registers, real control flow)
    // rather than inlined the way List<T>.push/.pop are: named LLVM
    // registers don't have this backend's own "strictly increasing anonymous
    // register" constraint, unlike the straight-line/loop-only code List's
    // own emitListPush/emitListPop hand-roll directly. Hand-verified against
    // real clang (-O0/-O1) in an isolated .ll file before being written here
    // - see the LinkedList<T> plan/task history.
    const char* const kLinkedListPushFrontTemplate = R"(
define void @axea.linkedlist.<<ID>>.push_front(<<HEADERPTR>> %h, <<VALUETYPE>> %value) {
entry:
  %sizePtr = getelementptr <<NODE>>, <<NODEPTR>> null, i32 1
  %sizeInt = ptrtoint <<NODEPTR>> %sizePtr to i64
  %raw = call i8* @malloc(i64 %sizeInt)
  %newNode = bitcast i8* %raw to <<NODEPTR>>
  %vp = getelementptr <<NODE>>, <<NODEPTR>> %newNode, i32 0, i32 0
  store <<VALUETYPE>> %value, <<VALUETYPEPTR>> %vp
  %hp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %oldHead = load <<NODEPTR>>, <<NODEPTRPTR>> %hp
  %pp = getelementptr <<NODE>>, <<NODEPTR>> %newNode, i32 0, i32 1
  store <<NODEPTR>> null, <<NODEPTRPTR>> %pp
  %np = getelementptr <<NODE>>, <<NODEPTR>> %newNode, i32 0, i32 2
  store <<NODEPTR>> %oldHead, <<NODEPTRPTR>> %np
  %isEmpty = icmp eq <<NODEPTR>> %oldHead, null
  br i1 %isEmpty, label %emptycase, label %nonemptycase
emptycase:
  %tp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 2
  store <<NODEPTR>> %newNode, <<NODEPTRPTR>> %tp
  br label %merge
nonemptycase:
  %ohpp = getelementptr <<NODE>>, <<NODEPTR>> %oldHead, i32 0, i32 1
  store <<NODEPTR>> %newNode, <<NODEPTRPTR>> %ohpp
  br label %merge
merge:
  store <<NODEPTR>> %newNode, <<NODEPTRPTR>> %hp
  %lp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 0
  %oldLen = load i32, i32* %lp
  %newLen = add i32 %oldLen, 1
  store i32 %newLen, i32* %lp
  ret void
}
)";

    const char* const kLinkedListPushBackTemplate = R"(
define void @axea.linkedlist.<<ID>>.push_back(<<HEADERPTR>> %h, <<VALUETYPE>> %value) {
entry:
  %sizePtr = getelementptr <<NODE>>, <<NODEPTR>> null, i32 1
  %sizeInt = ptrtoint <<NODEPTR>> %sizePtr to i64
  %raw = call i8* @malloc(i64 %sizeInt)
  %newNode = bitcast i8* %raw to <<NODEPTR>>
  %vp = getelementptr <<NODE>>, <<NODEPTR>> %newNode, i32 0, i32 0
  store <<VALUETYPE>> %value, <<VALUETYPEPTR>> %vp
  %tp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 2
  %oldTail = load <<NODEPTR>>, <<NODEPTRPTR>> %tp
  %np = getelementptr <<NODE>>, <<NODEPTR>> %newNode, i32 0, i32 2
  store <<NODEPTR>> null, <<NODEPTRPTR>> %np
  %pp = getelementptr <<NODE>>, <<NODEPTR>> %newNode, i32 0, i32 1
  store <<NODEPTR>> %oldTail, <<NODEPTRPTR>> %pp
  %isEmpty = icmp eq <<NODEPTR>> %oldTail, null
  br i1 %isEmpty, label %emptycase, label %nonemptycase
emptycase:
  %hp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  store <<NODEPTR>> %newNode, <<NODEPTRPTR>> %hp
  br label %merge
nonemptycase:
  %otnp = getelementptr <<NODE>>, <<NODEPTR>> %oldTail, i32 0, i32 2
  store <<NODEPTR>> %newNode, <<NODEPTRPTR>> %otnp
  br label %merge
merge:
  store <<NODEPTR>> %newNode, <<NODEPTRPTR>> %tp
  %lp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 0
  %oldLen = load i32, i32* %lp
  %newLen = add i32 %oldLen, 1
  store i32 %newLen, i32* %lp
  ret void
}
)";

    const char* const kLinkedListPopFrontTemplate = R"(
define <<VALUETYPE>> @axea.linkedlist.<<ID>>.pop_front(<<HEADERPTR>> %h) {
entry:
  %hp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %oldHead = load <<NODEPTR>>, <<NODEPTRPTR>> %hp
  %vp = getelementptr <<NODE>>, <<NODEPTR>> %oldHead, i32 0, i32 0
  %value = load <<VALUETYPE>>, <<VALUETYPEPTR>> %vp
  %np = getelementptr <<NODE>>, <<NODEPTR>> %oldHead, i32 0, i32 2
  %newHead = load <<NODEPTR>>, <<NODEPTRPTR>> %np
  %isNowEmpty = icmp eq <<NODEPTR>> %newHead, null
  br i1 %isNowEmpty, label %emptycase, label %nonemptycase
emptycase:
  %tp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 2
  store <<NODEPTR>> null, <<NODEPTRPTR>> %tp
  br label %merge
nonemptycase:
  %nhpp = getelementptr <<NODE>>, <<NODEPTR>> %newHead, i32 0, i32 1
  store <<NODEPTR>> null, <<NODEPTRPTR>> %nhpp
  br label %merge
merge:
  store <<NODEPTR>> %newHead, <<NODEPTRPTR>> %hp
  %lp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 0
  %oldLen = load i32, i32* %lp
  %newLen = sub i32 %oldLen, 1
  store i32 %newLen, i32* %lp
  ret <<VALUETYPE>> %value
}
)";

    const char* const kLinkedListPopBackTemplate = R"(
define <<VALUETYPE>> @axea.linkedlist.<<ID>>.pop_back(<<HEADERPTR>> %h) {
entry:
  %tp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 2
  %oldTail = load <<NODEPTR>>, <<NODEPTRPTR>> %tp
  %vp = getelementptr <<NODE>>, <<NODEPTR>> %oldTail, i32 0, i32 0
  %value = load <<VALUETYPE>>, <<VALUETYPEPTR>> %vp
  %pp = getelementptr <<NODE>>, <<NODEPTR>> %oldTail, i32 0, i32 1
  %newTail = load <<NODEPTR>>, <<NODEPTRPTR>> %pp
  %isNowEmpty = icmp eq <<NODEPTR>> %newTail, null
  br i1 %isNowEmpty, label %emptycase, label %nonemptycase
emptycase:
  %hp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  store <<NODEPTR>> null, <<NODEPTRPTR>> %hp
  br label %merge
nonemptycase:
  %ntnp = getelementptr <<NODE>>, <<NODEPTR>> %newTail, i32 0, i32 2
  store <<NODEPTR>> null, <<NODEPTRPTR>> %ntnp
  br label %merge
merge:
  store <<NODEPTR>> %newTail, <<NODEPTRPTR>> %tp
  %lp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 0
  %oldLen = load i32, i32* %lp
  %newLen = sub i32 %oldLen, 1
  store i32 %newLen, i32* %lp
  ret <<VALUETYPE>> %value
}
)";

    // SortedMap<K,V> (see docs/language/0040-sorted-maps.md) - a real AVL
    // tree. Node layout: { key, value, height, left, right } (field indices
    // 0-4). K is always i32 (the only orderable type this phase, enforced by
    // TypeChecker), so every comparison below is a direct `icmp slt`/`icmp
    // sgt i32`, not a generic comparator call the way Map<K,V>'s own
    // hash/equality is. Like every other loop in this backend, no `phi` -
    // mutable loop-carried state (minValueNode's own descent, the two
    // out-parameters insertNode/removeNode use to report back whether they
    // actually inserted/removed) lives in `alloca`d slots, read/written via
    // named (not numbered) registers - the same convention
    // kMapSetTemplate/kLinkedListPushFrontTemplate above already establish
    // for hand-written runtime-function text. Unlike every push/pop in this
    // backend, `remove` never calls `free` on the unlinked node - matches
    // this codebase's established "leak, don't free" policy (see
    // kMapRemoveTemplate above: Map<K,V>.remove doesn't free its entry
    // either).
    const char* const kSortedMapHeightTemplate = R"(
define i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %n) {
entry:
  %isnull = icmp eq <<NODEPTR>> %n, null
  br i1 %isnull, label %isnullcase, label %notnullcase
isnullcase:
  ret i32 0
notnullcase:
  %hp = getelementptr <<NODE>>, <<NODEPTR>> %n, i32 0, i32 2
  %h = load i32, i32* %hp
  ret i32 %h
}
)";

    // Classic single right rotation: y is the unbalanced node, x = y.left
    // becomes the new subtree root.
    const char* const kSortedMapRotateRightTemplate = R"(
define <<NODEPTR>> @axea.sortedmap.<<ID>>.rotateRight(<<NODEPTR>> %y) {
entry:
  %ylp = getelementptr <<NODE>>, <<NODEPTR>> %y, i32 0, i32 3
  %x = load <<NODEPTR>>, <<NODEPTR>>* %ylp
  %xlp = getelementptr <<NODE>>, <<NODEPTR>> %x, i32 0, i32 3
  %xl = load <<NODEPTR>>, <<NODEPTR>>* %xlp
  %xrp = getelementptr <<NODE>>, <<NODEPTR>> %x, i32 0, i32 4
  %t2 = load <<NODEPTR>>, <<NODEPTR>>* %xrp
  %yrp = getelementptr <<NODE>>, <<NODEPTR>> %y, i32 0, i32 4
  %yr = load <<NODEPTR>>, <<NODEPTR>>* %yrp
  store <<NODEPTR>> %y, <<NODEPTR>>* %xrp
  store <<NODEPTR>> %t2, <<NODEPTR>>* %ylp
  %ylh = call i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %t2)
  %yrh = call i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %yr)
  %ycmp = icmp sgt i32 %ylh, %yrh
  %ymax = select i1 %ycmp, i32 %ylh, i32 %yrh
  %ynewh = add i32 %ymax, 1
  %yhp = getelementptr <<NODE>>, <<NODEPTR>> %y, i32 0, i32 2
  store i32 %ynewh, i32* %yhp
  %xlh = call i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %xl)
  %xrh = call i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %y)
  %xcmp = icmp sgt i32 %xlh, %xrh
  %xmax = select i1 %xcmp, i32 %xlh, i32 %xrh
  %xnewh = add i32 %xmax, 1
  %xhp = getelementptr <<NODE>>, <<NODEPTR>> %x, i32 0, i32 2
  store i32 %xnewh, i32* %xhp
  ret <<NODEPTR>> %x
}
)";

    // Mirror image of rotateRight above: x is the unbalanced node, y = x.right
    // becomes the new subtree root.
    const char* const kSortedMapRotateLeftTemplate = R"(
define <<NODEPTR>> @axea.sortedmap.<<ID>>.rotateLeft(<<NODEPTR>> %x) {
entry:
  %xrp = getelementptr <<NODE>>, <<NODEPTR>> %x, i32 0, i32 4
  %y = load <<NODEPTR>>, <<NODEPTR>>* %xrp
  %yrp = getelementptr <<NODE>>, <<NODEPTR>> %y, i32 0, i32 4
  %yr = load <<NODEPTR>>, <<NODEPTR>>* %yrp
  %ylp = getelementptr <<NODE>>, <<NODEPTR>> %y, i32 0, i32 3
  %t2 = load <<NODEPTR>>, <<NODEPTR>>* %ylp
  %xlp = getelementptr <<NODE>>, <<NODEPTR>> %x, i32 0, i32 3
  %xl = load <<NODEPTR>>, <<NODEPTR>>* %xlp
  store <<NODEPTR>> %x, <<NODEPTR>>* %ylp
  store <<NODEPTR>> %t2, <<NODEPTR>>* %xrp
  %xlh = call i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %xl)
  %xrh = call i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %t2)
  %xcmp = icmp sgt i32 %xlh, %xrh
  %xmax = select i1 %xcmp, i32 %xlh, i32 %xrh
  %xnewh = add i32 %xmax, 1
  %xhp = getelementptr <<NODE>>, <<NODEPTR>> %x, i32 0, i32 2
  store i32 %xnewh, i32* %xhp
  %ylh = call i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %x)
  %yrh = call i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %yr)
  %ycmp = icmp sgt i32 %ylh, %yrh
  %ymax = select i1 %ycmp, i32 %ylh, i32 %yrh
  %ynewh = add i32 %ymax, 1
  %yhp = getelementptr <<NODE>>, <<NODEPTR>> %y, i32 0, i32 2
  store i32 %ynewh, i32* %yhp
  ret <<NODEPTR>> %y
}
)";

    // Recursive AVL insert: descends by key, updates (rather than
    // duplicating) an existing key, then rebalances on the way back up.
    // *isNewOut is set exactly once, at whichever base case is reached
    // (fresh node -> 1, update-in-place -> 0), and threaded unchanged
    // through every recursive call above it.
    const char* const kSortedMapInsertNodeTemplate = R"(
define <<NODEPTR>> @axea.sortedmap.<<ID>>.insertNode(<<NODEPTR>> %node, <<KEYTYPE>> %key, <<VALUETYPE>> %value, i1* %isNewOut) {
entry:
  %isnull = icmp eq <<NODEPTR>> %node, null
  br i1 %isnull, label %createNew, label %compare
createNew:
  %sizePtr = getelementptr <<NODE>>, <<NODEPTR>> null, i32 1
  %sizeInt = ptrtoint <<NODEPTR>> %sizePtr to i64
  %raw = call i8* @malloc(i64 %sizeInt)
  %newNode = bitcast i8* %raw to <<NODEPTR>>
  %kp = getelementptr <<NODE>>, <<NODEPTR>> %newNode, i32 0, i32 0
  store <<KEYTYPE>> %key, <<KEYTYPEPTR>> %kp
  %vp = getelementptr <<NODE>>, <<NODEPTR>> %newNode, i32 0, i32 1
  store <<VALUETYPE>> %value, <<VALUETYPEPTR>> %vp
  %hp0 = getelementptr <<NODE>>, <<NODEPTR>> %newNode, i32 0, i32 2
  store i32 1, i32* %hp0
  %lp0 = getelementptr <<NODE>>, <<NODEPTR>> %newNode, i32 0, i32 3
  store <<NODEPTR>> null, <<NODEPTR>>* %lp0
  %rp0 = getelementptr <<NODE>>, <<NODEPTR>> %newNode, i32 0, i32 4
  store <<NODEPTR>> null, <<NODEPTR>>* %rp0
  store i1 1, i1* %isNewOut
  ret <<NODEPTR>> %newNode
compare:
  %kp1 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 0
  %nodeKey = load <<KEYTYPE>>, <<KEYTYPEPTR>> %kp1
  %lessThan = icmp slt <<KEYTYPE>> %key, %nodeKey
  br i1 %lessThan, label %goLeft, label %checkGreater
checkGreater:
  %greaterThan = icmp sgt <<KEYTYPE>> %key, %nodeKey
  br i1 %greaterThan, label %goRight, label %updateValue
updateValue:
  %vp1 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 1
  store <<VALUETYPE>> %value, <<VALUETYPEPTR>> %vp1
  store i1 0, i1* %isNewOut
  ret <<NODEPTR>> %node
goLeft:
  %lp1 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 3
  %oldLeft = load <<NODEPTR>>, <<NODEPTR>>* %lp1
  %newLeft = call <<NODEPTR>> @axea.sortedmap.<<ID>>.insertNode(<<NODEPTR>> %oldLeft, <<KEYTYPE>> %key, <<VALUETYPE>> %value, i1* %isNewOut)
  store <<NODEPTR>> %newLeft, <<NODEPTR>>* %lp1
  br label %rebalance
goRight:
  %rp1 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 4
  %oldRight = load <<NODEPTR>>, <<NODEPTR>>* %rp1
  %newRight = call <<NODEPTR>> @axea.sortedmap.<<ID>>.insertNode(<<NODEPTR>> %oldRight, <<KEYTYPE>> %key, <<VALUETYPE>> %value, i1* %isNewOut)
  store <<NODEPTR>> %newRight, <<NODEPTR>>* %rp1
  br label %rebalance
rebalance:
  %lp2 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 3
  %curLeft = load <<NODEPTR>>, <<NODEPTR>>* %lp2
  %rp2 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 4
  %curRight = load <<NODEPTR>>, <<NODEPTR>>* %rp2
  %lh = call i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %curLeft)
  %rh = call i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %curRight)
  %hcmp = icmp sgt i32 %lh, %rh
  %hmax = select i1 %hcmp, i32 %lh, i32 %rh
  %newHeight = add i32 %hmax, 1
  %hp1 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 2
  store i32 %newHeight, i32* %hp1
  %balance = sub i32 %lh, %rh
  %leftHeavy = icmp sgt i32 %balance, 1
  br i1 %leftHeavy, label %checkLL, label %checkRightHeavy
checkLL:
  %llkp = getelementptr <<NODE>>, <<NODEPTR>> %curLeft, i32 0, i32 0
  %llkey = load <<KEYTYPE>>, <<KEYTYPEPTR>> %llkp
  %isLL = icmp slt <<KEYTYPE>> %key, %llkey
  br i1 %isLL, label %doLL, label %doLR
doLL:
  %resultLL = call <<NODEPTR>> @axea.sortedmap.<<ID>>.rotateRight(<<NODEPTR>> %node)
  ret <<NODEPTR>> %resultLL
doLR:
  %rotatedLeft = call <<NODEPTR>> @axea.sortedmap.<<ID>>.rotateLeft(<<NODEPTR>> %curLeft)
  store <<NODEPTR>> %rotatedLeft, <<NODEPTR>>* %lp2
  %resultLR = call <<NODEPTR>> @axea.sortedmap.<<ID>>.rotateRight(<<NODEPTR>> %node)
  ret <<NODEPTR>> %resultLR
checkRightHeavy:
  %rightHeavy = icmp slt i32 %balance, -1
  br i1 %rightHeavy, label %checkRR, label %noRebalance
checkRR:
  %rrkp = getelementptr <<NODE>>, <<NODEPTR>> %curRight, i32 0, i32 0
  %rrkey = load <<KEYTYPE>>, <<KEYTYPEPTR>> %rrkp
  %isRR = icmp sgt <<KEYTYPE>> %key, %rrkey
  br i1 %isRR, label %doRR, label %doRL
doRR:
  %resultRR = call <<NODEPTR>> @axea.sortedmap.<<ID>>.rotateLeft(<<NODEPTR>> %node)
  ret <<NODEPTR>> %resultRR
doRL:
  %rotatedRight = call <<NODEPTR>> @axea.sortedmap.<<ID>>.rotateRight(<<NODEPTR>> %curRight)
  store <<NODEPTR>> %rotatedRight, <<NODEPTR>>* %rp2
  %resultRL = call <<NODEPTR>> @axea.sortedmap.<<ID>>.rotateLeft(<<NODEPTR>> %node)
  ret <<NODEPTR>> %resultRL
noRebalance:
  ret <<NODEPTR>> %node
}
)";

    // Iterative descent to the leftmost (smallest-key) node of a subtree -
    // only ever called with a non-null argument (removeNode's own
    // two-children case, on a subtree it already knows is non-empty).
    const char* const kSortedMapMinValueNodeTemplate = R"(
define <<NODEPTR>> @axea.sortedmap.<<ID>>.minValueNode(<<NODEPTR>> %node) {
entry:
  %cur = alloca <<NODEPTR>>
  store <<NODEPTR>> %node, <<NODEPTR>>* %cur
  br label %header
header:
  %c0 = load <<NODEPTR>>, <<NODEPTR>>* %cur
  %lp = getelementptr <<NODE>>, <<NODEPTR>> %c0, i32 0, i32 3
  %l = load <<NODEPTR>>, <<NODEPTR>>* %lp
  %hasLeft = icmp ne <<NODEPTR>> %l, null
  br i1 %hasLeft, label %body, label %done
body:
  store <<NODEPTR>> %l, <<NODEPTR>>* %cur
  br label %header
done:
  ret <<NODEPTR>> %c0
}
)";

    // Recursive AVL delete: descends by key; on the found node, splices it
    // out (0 or 1 child) or replaces its key/value with its in-order
    // successor and recursively removes that successor from the right
    // subtree instead (2 children) - then rebalances on the way back up,
    // the same shape insertNode's own rebalance uses but checking the
    // *child's own* balance (balanceOf) rather than comparing against a
    // just-inserted key, since deletion has no such key to compare against.
    // *isRemovedOut is set only at the "found" point - never touched on a
    // not-found (null) path or by the internal successor-removal call
    // (which passes its own throwaway `i1* %dummy` instead).
    const char* const kSortedMapRemoveNodeTemplate = R"(
define <<NODEPTR>> @axea.sortedmap.<<ID>>.removeNode(<<NODEPTR>> %node, <<KEYTYPE>> %key, i1* %isRemovedOut) {
entry:
  %isnull = icmp eq <<NODEPTR>> %node, null
  br i1 %isnull, label %notfound, label %compare
notfound:
  ret <<NODEPTR>> null
compare:
  %kp0 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 0
  %nodeKey = load <<KEYTYPE>>, <<KEYTYPEPTR>> %kp0
  %lessThan = icmp slt <<KEYTYPE>> %key, %nodeKey
  br i1 %lessThan, label %goLeft, label %checkGreater
checkGreater:
  %greaterThan = icmp sgt <<KEYTYPE>> %key, %nodeKey
  br i1 %greaterThan, label %goRight, label %foundHere
goLeft:
  %lp0 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 3
  %oldLeft = load <<NODEPTR>>, <<NODEPTR>>* %lp0
  %newLeft = call <<NODEPTR>> @axea.sortedmap.<<ID>>.removeNode(<<NODEPTR>> %oldLeft, <<KEYTYPE>> %key, i1* %isRemovedOut)
  store <<NODEPTR>> %newLeft, <<NODEPTR>>* %lp0
  br label %rebalance
goRight:
  %rp0 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 4
  %oldRight = load <<NODEPTR>>, <<NODEPTR>>* %rp0
  %newRight = call <<NODEPTR>> @axea.sortedmap.<<ID>>.removeNode(<<NODEPTR>> %oldRight, <<KEYTYPE>> %key, i1* %isRemovedOut)
  store <<NODEPTR>> %newRight, <<NODEPTR>>* %rp0
  br label %rebalance
foundHere:
  store i1 1, i1* %isRemovedOut
  %lp1 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 3
  %left = load <<NODEPTR>>, <<NODEPTR>>* %lp1
  %rp1 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 4
  %right = load <<NODEPTR>>, <<NODEPTR>>* %rp1
  %leftNull = icmp eq <<NODEPTR>> %left, null
  br i1 %leftNull, label %noLeft, label %hasLeft
noLeft:
  ret <<NODEPTR>> %right
hasLeft:
  %rightNull = icmp eq <<NODEPTR>> %right, null
  br i1 %rightNull, label %onlyLeft, label %twoChildren
onlyLeft:
  ret <<NODEPTR>> %left
twoChildren:
  %successor = call <<NODEPTR>> @axea.sortedmap.<<ID>>.minValueNode(<<NODEPTR>> %right)
  %skp = getelementptr <<NODE>>, <<NODEPTR>> %successor, i32 0, i32 0
  %successorKey = load <<KEYTYPE>>, <<KEYTYPEPTR>> %skp
  %svp = getelementptr <<NODE>>, <<NODEPTR>> %successor, i32 0, i32 1
  %successorValue = load <<VALUETYPE>>, <<VALUETYPEPTR>> %svp
  store <<KEYTYPE>> %successorKey, <<KEYTYPEPTR>> %kp0
  %vp0 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 1
  store <<VALUETYPE>> %successorValue, <<VALUETYPEPTR>> %vp0
  %dummy = alloca i1
  %newRight2 = call <<NODEPTR>> @axea.sortedmap.<<ID>>.removeNode(<<NODEPTR>> %right, <<KEYTYPE>> %successorKey, i1* %dummy)
  store <<NODEPTR>> %newRight2, <<NODEPTR>>* %rp1
  br label %rebalance
rebalance:
  %lp2 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 3
  %curLeft = load <<NODEPTR>>, <<NODEPTR>>* %lp2
  %rp2 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 4
  %curRight = load <<NODEPTR>>, <<NODEPTR>>* %rp2
  %lh = call i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %curLeft)
  %rh = call i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %curRight)
  %hcmp = icmp sgt i32 %lh, %rh
  %hmax = select i1 %hcmp, i32 %lh, i32 %rh
  %newHeight = add i32 %hmax, 1
  %hp1 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 2
  store i32 %newHeight, i32* %hp1
  %balance = sub i32 %lh, %rh
  %leftHeavy = icmp sgt i32 %balance, 1
  br i1 %leftHeavy, label %checkLeftBalance, label %checkRightHeavy
checkLeftBalance:
  %cllp = getelementptr <<NODE>>, <<NODEPTR>> %curLeft, i32 0, i32 3
  %cll = load <<NODEPTR>>, <<NODEPTR>>* %cllp
  %clrp = getelementptr <<NODE>>, <<NODEPTR>> %curLeft, i32 0, i32 4
  %clr = load <<NODEPTR>>, <<NODEPTR>>* %clrp
  %cllh = call i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %cll)
  %clrh = call i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %clr)
  %leftBalance = sub i32 %cllh, %clrh
  %isLL2 = icmp sge i32 %leftBalance, 0
  br i1 %isLL2, label %doLL2, label %doLR2
doLL2:
  %resultLL2 = call <<NODEPTR>> @axea.sortedmap.<<ID>>.rotateRight(<<NODEPTR>> %node)
  ret <<NODEPTR>> %resultLL2
doLR2:
  %rotatedLeft2 = call <<NODEPTR>> @axea.sortedmap.<<ID>>.rotateLeft(<<NODEPTR>> %curLeft)
  store <<NODEPTR>> %rotatedLeft2, <<NODEPTR>>* %lp2
  %resultLR2 = call <<NODEPTR>> @axea.sortedmap.<<ID>>.rotateRight(<<NODEPTR>> %node)
  ret <<NODEPTR>> %resultLR2
checkRightHeavy:
  %rightHeavy = icmp slt i32 %balance, -1
  br i1 %rightHeavy, label %checkRightBalance, label %noRebalance2
checkRightBalance:
  %crlp = getelementptr <<NODE>>, <<NODEPTR>> %curRight, i32 0, i32 3
  %crl = load <<NODEPTR>>, <<NODEPTR>>* %crlp
  %crrp = getelementptr <<NODE>>, <<NODEPTR>> %curRight, i32 0, i32 4
  %crr = load <<NODEPTR>>, <<NODEPTR>>* %crrp
  %crlh = call i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %crl)
  %crrh = call i32 @axea.sortedmap.<<ID>>.height(<<NODEPTR>> %crr)
  %rightBalance = sub i32 %crlh, %crrh
  %isRR2 = icmp sle i32 %rightBalance, 0
  br i1 %isRR2, label %doRR2, label %doRL2
doRR2:
  %resultRR2 = call <<NODEPTR>> @axea.sortedmap.<<ID>>.rotateLeft(<<NODEPTR>> %node)
  ret <<NODEPTR>> %resultRR2
doRL2:
  %rotatedRight2 = call <<NODEPTR>> @axea.sortedmap.<<ID>>.rotateRight(<<NODEPTR>> %curRight)
  store <<NODEPTR>> %rotatedRight2, <<NODEPTR>>* %rp2
  %resultRL2 = call <<NODEPTR>> @axea.sortedmap.<<ID>>.rotateLeft(<<NODEPTR>> %node)
  ret <<NODEPTR>> %resultRL2
noRebalance2:
  ret <<NODEPTR>> %node
}
)";

    const char* const kSortedMapSetTemplate = R"(
define void @axea.sortedmap.<<ID>>.set(<<HEADERPTR>> %h, <<KEYTYPE>> %key, <<VALUETYPE>> %value) {
entry:
  %rootp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %root = load <<NODEPTR>>, <<NODEPTR>>* %rootp
  %isNew = alloca i1
  store i1 0, i1* %isNew
  %newRoot = call <<NODEPTR>> @axea.sortedmap.<<ID>>.insertNode(<<NODEPTR>> %root, <<KEYTYPE>> %key, <<VALUETYPE>> %value, i1* %isNew)
  store <<NODEPTR>> %newRoot, <<NODEPTR>>* %rootp
  %wasNew = load i1, i1* %isNew
  br i1 %wasNew, label %incCount, label %done
incCount:
  %cptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 0
  %oldCount = load i32, i32* %cptr
  %newCount = add i32 %oldCount, 1
  store i32 %newCount, i32* %cptr
  br label %done
done:
  ret void
}
)";

    const char* const kSortedMapGetTemplate = R"(
define <<VALUETYPE>> @axea.sortedmap.<<ID>>.get(<<HEADERPTR>> %h, <<KEYTYPE>> %key) {
entry:
  %rootp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %root = load <<NODEPTR>>, <<NODEPTR>>* %rootp
  %cur = alloca <<NODEPTR>>
  store <<NODEPTR>> %root, <<NODEPTR>>* %cur
  br label %header
header:
  %c0 = load <<NODEPTR>>, <<NODEPTR>>* %cur
  %isnull = icmp eq <<NODEPTR>> %c0, null
  br i1 %isnull, label %notfound, label %check
check:
  %kp = getelementptr <<NODE>>, <<NODEPTR>> %c0, i32 0, i32 0
  %nodeKey = load <<KEYTYPE>>, <<KEYTYPEPTR>> %kp
  %lessThan = icmp slt <<KEYTYPE>> %key, %nodeKey
  br i1 %lessThan, label %goLeft, label %checkGreater
checkGreater:
  %greaterThan = icmp sgt <<KEYTYPE>> %key, %nodeKey
  br i1 %greaterThan, label %goRight, label %found
goLeft:
  %lp = getelementptr <<NODE>>, <<NODEPTR>> %c0, i32 0, i32 3
  %l = load <<NODEPTR>>, <<NODEPTR>>* %lp
  store <<NODEPTR>> %l, <<NODEPTR>>* %cur
  br label %header
goRight:
  %rp = getelementptr <<NODE>>, <<NODEPTR>> %c0, i32 0, i32 4
  %r = load <<NODEPTR>>, <<NODEPTR>>* %rp
  store <<NODEPTR>> %r, <<NODEPTR>>* %cur
  br label %header
found:
  %vp = getelementptr <<NODE>>, <<NODEPTR>> %c0, i32 0, i32 1
  %v = load <<VALUETYPE>>, <<VALUETYPEPTR>> %vp
  ret <<VALUETYPE>> %v
notfound:
  ret <<VALUETYPE>> <<SENTINEL>>
}
)";

    const char* const kSortedMapContainsTemplate = R"(
define i1 @axea.sortedmap.<<ID>>.contains(<<HEADERPTR>> %h, <<KEYTYPE>> %key) {
entry:
  %rootp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %root = load <<NODEPTR>>, <<NODEPTR>>* %rootp
  %cur = alloca <<NODEPTR>>
  store <<NODEPTR>> %root, <<NODEPTR>>* %cur
  br label %header
header:
  %c0 = load <<NODEPTR>>, <<NODEPTR>>* %cur
  %isnull = icmp eq <<NODEPTR>> %c0, null
  br i1 %isnull, label %notfound, label %check
check:
  %kp = getelementptr <<NODE>>, <<NODEPTR>> %c0, i32 0, i32 0
  %nodeKey = load <<KEYTYPE>>, <<KEYTYPEPTR>> %kp
  %lessThan = icmp slt <<KEYTYPE>> %key, %nodeKey
  br i1 %lessThan, label %goLeft, label %checkGreater
checkGreater:
  %greaterThan = icmp sgt <<KEYTYPE>> %key, %nodeKey
  br i1 %greaterThan, label %goRight, label %found
goLeft:
  %lp = getelementptr <<NODE>>, <<NODEPTR>> %c0, i32 0, i32 3
  %l = load <<NODEPTR>>, <<NODEPTR>>* %lp
  store <<NODEPTR>> %l, <<NODEPTR>>* %cur
  br label %header
goRight:
  %rp = getelementptr <<NODE>>, <<NODEPTR>> %c0, i32 0, i32 4
  %r = load <<NODEPTR>>, <<NODEPTR>>* %rp
  store <<NODEPTR>> %r, <<NODEPTR>>* %cur
  br label %header
found:
  ret i1 1
notfound:
  ret i1 0
}
)";

    const char* const kSortedMapRemoveTemplate = R"(
define void @axea.sortedmap.<<ID>>.remove(<<HEADERPTR>> %h, <<KEYTYPE>> %key) {
entry:
  %rootp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %root = load <<NODEPTR>>, <<NODEPTR>>* %rootp
  %isRemoved = alloca i1
  store i1 0, i1* %isRemoved
  %newRoot = call <<NODEPTR>> @axea.sortedmap.<<ID>>.removeNode(<<NODEPTR>> %root, <<KEYTYPE>> %key, i1* %isRemoved)
  store <<NODEPTR>> %newRoot, <<NODEPTR>>* %rootp
  %wasRemoved = load i1, i1* %isRemoved
  br i1 %wasRemoved, label %decCount, label %done
decCount:
  %cptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 0
  %oldCount = load i32, i32* %cptr
  %newCount = sub i32 %oldCount, 1
  store i32 %newCount, i32* %cptr
  br label %done
done:
  ret void
}
)";

    // SortedSet<T> (see docs/language/0041-sorted-sets.md) - the same real
    // AVL tree SortedMap<K,V> uses above, minus the value field: node
    // layout is { key, height, left, right } (field indices 0-3, shifted
    // down by one from SortedMap<K,V>'s own 0-1-3-4 since there's no value
    // at index 1). `insertNode`'s "already present" base case needs no
    // update-in-place step (there's no value to update) - it just reports
    // "not new" and returns the node unchanged. Every other shape (rotation
    // primitives, rebalance logic, successor-splicing two-children removal,
    // no `free`, no `phi`) is identical to SortedMap<K,V>'s own; see that
    // type's own Design section for the full explanation.
    const char* const kSortedSetHeightTemplate = R"(
define i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %n) {
entry:
  %isnull = icmp eq <<NODEPTR>> %n, null
  br i1 %isnull, label %isnullcase, label %notnullcase
isnullcase:
  ret i32 0
notnullcase:
  %hp = getelementptr <<NODE>>, <<NODEPTR>> %n, i32 0, i32 1
  %h = load i32, i32* %hp
  ret i32 %h
}
)";

    const char* const kSortedSetRotateRightTemplate = R"(
define <<NODEPTR>> @axea.sortedset.<<ID>>.rotateRight(<<NODEPTR>> %y) {
entry:
  %ylp = getelementptr <<NODE>>, <<NODEPTR>> %y, i32 0, i32 2
  %x = load <<NODEPTR>>, <<NODEPTR>>* %ylp
  %xlp = getelementptr <<NODE>>, <<NODEPTR>> %x, i32 0, i32 2
  %xl = load <<NODEPTR>>, <<NODEPTR>>* %xlp
  %xrp = getelementptr <<NODE>>, <<NODEPTR>> %x, i32 0, i32 3
  %t2 = load <<NODEPTR>>, <<NODEPTR>>* %xrp
  %yrp = getelementptr <<NODE>>, <<NODEPTR>> %y, i32 0, i32 3
  %yr = load <<NODEPTR>>, <<NODEPTR>>* %yrp
  store <<NODEPTR>> %y, <<NODEPTR>>* %xrp
  store <<NODEPTR>> %t2, <<NODEPTR>>* %ylp
  %ylh = call i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %t2)
  %yrh = call i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %yr)
  %ycmp = icmp sgt i32 %ylh, %yrh
  %ymax = select i1 %ycmp, i32 %ylh, i32 %yrh
  %ynewh = add i32 %ymax, 1
  %yhp = getelementptr <<NODE>>, <<NODEPTR>> %y, i32 0, i32 1
  store i32 %ynewh, i32* %yhp
  %xlh = call i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %xl)
  %xrh = call i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %y)
  %xcmp = icmp sgt i32 %xlh, %xrh
  %xmax = select i1 %xcmp, i32 %xlh, i32 %xrh
  %xnewh = add i32 %xmax, 1
  %xhp = getelementptr <<NODE>>, <<NODEPTR>> %x, i32 0, i32 1
  store i32 %xnewh, i32* %xhp
  ret <<NODEPTR>> %x
}
)";

    const char* const kSortedSetRotateLeftTemplate = R"(
define <<NODEPTR>> @axea.sortedset.<<ID>>.rotateLeft(<<NODEPTR>> %x) {
entry:
  %xrp = getelementptr <<NODE>>, <<NODEPTR>> %x, i32 0, i32 3
  %y = load <<NODEPTR>>, <<NODEPTR>>* %xrp
  %yrp = getelementptr <<NODE>>, <<NODEPTR>> %y, i32 0, i32 3
  %yr = load <<NODEPTR>>, <<NODEPTR>>* %yrp
  %ylp = getelementptr <<NODE>>, <<NODEPTR>> %y, i32 0, i32 2
  %t2 = load <<NODEPTR>>, <<NODEPTR>>* %ylp
  %xlp = getelementptr <<NODE>>, <<NODEPTR>> %x, i32 0, i32 2
  %xl = load <<NODEPTR>>, <<NODEPTR>>* %xlp
  store <<NODEPTR>> %x, <<NODEPTR>>* %ylp
  store <<NODEPTR>> %t2, <<NODEPTR>>* %xrp
  %xlh = call i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %xl)
  %xrh = call i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %t2)
  %xcmp = icmp sgt i32 %xlh, %xrh
  %xmax = select i1 %xcmp, i32 %xlh, i32 %xrh
  %xnewh = add i32 %xmax, 1
  %xhp = getelementptr <<NODE>>, <<NODEPTR>> %x, i32 0, i32 1
  store i32 %xnewh, i32* %xhp
  %ylh = call i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %x)
  %yrh = call i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %yr)
  %ycmp = icmp sgt i32 %ylh, %yrh
  %ymax = select i1 %ycmp, i32 %ylh, i32 %yrh
  %ynewh = add i32 %ymax, 1
  %yhp = getelementptr <<NODE>>, <<NODEPTR>> %y, i32 0, i32 1
  store i32 %ynewh, i32* %yhp
  ret <<NODEPTR>> %y
}
)";

    const char* const kSortedSetInsertNodeTemplate = R"(
define <<NODEPTR>> @axea.sortedset.<<ID>>.insertNode(<<NODEPTR>> %node, <<KEYTYPE>> %key, i1* %isNewOut) {
entry:
  %isnull = icmp eq <<NODEPTR>> %node, null
  br i1 %isnull, label %createNew, label %compare
createNew:
  %sizePtr = getelementptr <<NODE>>, <<NODEPTR>> null, i32 1
  %sizeInt = ptrtoint <<NODEPTR>> %sizePtr to i64
  %raw = call i8* @malloc(i64 %sizeInt)
  %newNode = bitcast i8* %raw to <<NODEPTR>>
  %kp = getelementptr <<NODE>>, <<NODEPTR>> %newNode, i32 0, i32 0
  store <<KEYTYPE>> %key, <<KEYTYPEPTR>> %kp
  %hp0 = getelementptr <<NODE>>, <<NODEPTR>> %newNode, i32 0, i32 1
  store i32 1, i32* %hp0
  %lp0 = getelementptr <<NODE>>, <<NODEPTR>> %newNode, i32 0, i32 2
  store <<NODEPTR>> null, <<NODEPTR>>* %lp0
  %rp0 = getelementptr <<NODE>>, <<NODEPTR>> %newNode, i32 0, i32 3
  store <<NODEPTR>> null, <<NODEPTR>>* %rp0
  store i1 1, i1* %isNewOut
  ret <<NODEPTR>> %newNode
compare:
  %kp1 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 0
  %nodeKey = load <<KEYTYPE>>, <<KEYTYPEPTR>> %kp1
  %lessThan = icmp slt <<KEYTYPE>> %key, %nodeKey
  br i1 %lessThan, label %goLeft, label %checkGreater
checkGreater:
  %greaterThan = icmp sgt <<KEYTYPE>> %key, %nodeKey
  br i1 %greaterThan, label %goRight, label %alreadyPresent
alreadyPresent:
  store i1 0, i1* %isNewOut
  ret <<NODEPTR>> %node
goLeft:
  %lp1 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 2
  %oldLeft = load <<NODEPTR>>, <<NODEPTR>>* %lp1
  %newLeft = call <<NODEPTR>> @axea.sortedset.<<ID>>.insertNode(<<NODEPTR>> %oldLeft, <<KEYTYPE>> %key, i1* %isNewOut)
  store <<NODEPTR>> %newLeft, <<NODEPTR>>* %lp1
  br label %rebalance
goRight:
  %rp1 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 3
  %oldRight = load <<NODEPTR>>, <<NODEPTR>>* %rp1
  %newRight = call <<NODEPTR>> @axea.sortedset.<<ID>>.insertNode(<<NODEPTR>> %oldRight, <<KEYTYPE>> %key, i1* %isNewOut)
  store <<NODEPTR>> %newRight, <<NODEPTR>>* %rp1
  br label %rebalance
rebalance:
  %lp2 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 2
  %curLeft = load <<NODEPTR>>, <<NODEPTR>>* %lp2
  %rp2 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 3
  %curRight = load <<NODEPTR>>, <<NODEPTR>>* %rp2
  %lh = call i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %curLeft)
  %rh = call i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %curRight)
  %hcmp = icmp sgt i32 %lh, %rh
  %hmax = select i1 %hcmp, i32 %lh, i32 %rh
  %newHeight = add i32 %hmax, 1
  %hp1 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 1
  store i32 %newHeight, i32* %hp1
  %balance = sub i32 %lh, %rh
  %leftHeavy = icmp sgt i32 %balance, 1
  br i1 %leftHeavy, label %checkLL, label %checkRightHeavy
checkLL:
  %llkp = getelementptr <<NODE>>, <<NODEPTR>> %curLeft, i32 0, i32 0
  %llkey = load <<KEYTYPE>>, <<KEYTYPEPTR>> %llkp
  %isLL = icmp slt <<KEYTYPE>> %key, %llkey
  br i1 %isLL, label %doLL, label %doLR
doLL:
  %resultLL = call <<NODEPTR>> @axea.sortedset.<<ID>>.rotateRight(<<NODEPTR>> %node)
  ret <<NODEPTR>> %resultLL
doLR:
  %rotatedLeft = call <<NODEPTR>> @axea.sortedset.<<ID>>.rotateLeft(<<NODEPTR>> %curLeft)
  store <<NODEPTR>> %rotatedLeft, <<NODEPTR>>* %lp2
  %resultLR = call <<NODEPTR>> @axea.sortedset.<<ID>>.rotateRight(<<NODEPTR>> %node)
  ret <<NODEPTR>> %resultLR
checkRightHeavy:
  %rightHeavy = icmp slt i32 %balance, -1
  br i1 %rightHeavy, label %checkRR, label %noRebalance
checkRR:
  %rrkp = getelementptr <<NODE>>, <<NODEPTR>> %curRight, i32 0, i32 0
  %rrkey = load <<KEYTYPE>>, <<KEYTYPEPTR>> %rrkp
  %isRR = icmp sgt <<KEYTYPE>> %key, %rrkey
  br i1 %isRR, label %doRR, label %doRL
doRR:
  %resultRR = call <<NODEPTR>> @axea.sortedset.<<ID>>.rotateLeft(<<NODEPTR>> %node)
  ret <<NODEPTR>> %resultRR
doRL:
  %rotatedRight = call <<NODEPTR>> @axea.sortedset.<<ID>>.rotateRight(<<NODEPTR>> %curRight)
  store <<NODEPTR>> %rotatedRight, <<NODEPTR>>* %rp2
  %resultRL = call <<NODEPTR>> @axea.sortedset.<<ID>>.rotateLeft(<<NODEPTR>> %node)
  ret <<NODEPTR>> %resultRL
noRebalance:
  ret <<NODEPTR>> %node
}
)";

    const char* const kSortedSetMinValueNodeTemplate = R"(
define <<NODEPTR>> @axea.sortedset.<<ID>>.minValueNode(<<NODEPTR>> %node) {
entry:
  %cur = alloca <<NODEPTR>>
  store <<NODEPTR>> %node, <<NODEPTR>>* %cur
  br label %header
header:
  %c0 = load <<NODEPTR>>, <<NODEPTR>>* %cur
  %lp = getelementptr <<NODE>>, <<NODEPTR>> %c0, i32 0, i32 2
  %l = load <<NODEPTR>>, <<NODEPTR>>* %lp
  %hasLeft = icmp ne <<NODEPTR>> %l, null
  br i1 %hasLeft, label %body, label %done
body:
  store <<NODEPTR>> %l, <<NODEPTR>>* %cur
  br label %header
done:
  ret <<NODEPTR>> %c0
}
)";

    const char* const kSortedSetRemoveNodeTemplate = R"(
define <<NODEPTR>> @axea.sortedset.<<ID>>.removeNode(<<NODEPTR>> %node, <<KEYTYPE>> %key, i1* %isRemovedOut) {
entry:
  %isnull = icmp eq <<NODEPTR>> %node, null
  br i1 %isnull, label %notfound, label %compare
notfound:
  ret <<NODEPTR>> null
compare:
  %kp0 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 0
  %nodeKey = load <<KEYTYPE>>, <<KEYTYPEPTR>> %kp0
  %lessThan = icmp slt <<KEYTYPE>> %key, %nodeKey
  br i1 %lessThan, label %goLeft, label %checkGreater
checkGreater:
  %greaterThan = icmp sgt <<KEYTYPE>> %key, %nodeKey
  br i1 %greaterThan, label %goRight, label %foundHere
goLeft:
  %lp0 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 2
  %oldLeft = load <<NODEPTR>>, <<NODEPTR>>* %lp0
  %newLeft = call <<NODEPTR>> @axea.sortedset.<<ID>>.removeNode(<<NODEPTR>> %oldLeft, <<KEYTYPE>> %key, i1* %isRemovedOut)
  store <<NODEPTR>> %newLeft, <<NODEPTR>>* %lp0
  br label %rebalance
goRight:
  %rp0 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 3
  %oldRight = load <<NODEPTR>>, <<NODEPTR>>* %rp0
  %newRight = call <<NODEPTR>> @axea.sortedset.<<ID>>.removeNode(<<NODEPTR>> %oldRight, <<KEYTYPE>> %key, i1* %isRemovedOut)
  store <<NODEPTR>> %newRight, <<NODEPTR>>* %rp0
  br label %rebalance
foundHere:
  store i1 1, i1* %isRemovedOut
  %lp1 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 2
  %left = load <<NODEPTR>>, <<NODEPTR>>* %lp1
  %rp1 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 3
  %right = load <<NODEPTR>>, <<NODEPTR>>* %rp1
  %leftNull = icmp eq <<NODEPTR>> %left, null
  br i1 %leftNull, label %noLeft, label %hasLeft
noLeft:
  ret <<NODEPTR>> %right
hasLeft:
  %rightNull = icmp eq <<NODEPTR>> %right, null
  br i1 %rightNull, label %onlyLeft, label %twoChildren
onlyLeft:
  ret <<NODEPTR>> %left
twoChildren:
  %successor = call <<NODEPTR>> @axea.sortedset.<<ID>>.minValueNode(<<NODEPTR>> %right)
  %skp = getelementptr <<NODE>>, <<NODEPTR>> %successor, i32 0, i32 0
  %successorKey = load <<KEYTYPE>>, <<KEYTYPEPTR>> %skp
  store <<KEYTYPE>> %successorKey, <<KEYTYPEPTR>> %kp0
  %dummy = alloca i1
  %newRight2 = call <<NODEPTR>> @axea.sortedset.<<ID>>.removeNode(<<NODEPTR>> %right, <<KEYTYPE>> %successorKey, i1* %dummy)
  store <<NODEPTR>> %newRight2, <<NODEPTR>>* %rp1
  br label %rebalance
rebalance:
  %lp2 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 2
  %curLeft = load <<NODEPTR>>, <<NODEPTR>>* %lp2
  %rp2 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 3
  %curRight = load <<NODEPTR>>, <<NODEPTR>>* %rp2
  %lh = call i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %curLeft)
  %rh = call i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %curRight)
  %hcmp = icmp sgt i32 %lh, %rh
  %hmax = select i1 %hcmp, i32 %lh, i32 %rh
  %newHeight = add i32 %hmax, 1
  %hp1 = getelementptr <<NODE>>, <<NODEPTR>> %node, i32 0, i32 1
  store i32 %newHeight, i32* %hp1
  %balance = sub i32 %lh, %rh
  %leftHeavy = icmp sgt i32 %balance, 1
  br i1 %leftHeavy, label %checkLeftBalance, label %checkRightHeavy
checkLeftBalance:
  %cllp = getelementptr <<NODE>>, <<NODEPTR>> %curLeft, i32 0, i32 2
  %cll = load <<NODEPTR>>, <<NODEPTR>>* %cllp
  %clrp = getelementptr <<NODE>>, <<NODEPTR>> %curLeft, i32 0, i32 3
  %clr = load <<NODEPTR>>, <<NODEPTR>>* %clrp
  %cllh = call i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %cll)
  %clrh = call i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %clr)
  %leftBalance = sub i32 %cllh, %clrh
  %isLL2 = icmp sge i32 %leftBalance, 0
  br i1 %isLL2, label %doLL2, label %doLR2
doLL2:
  %resultLL2 = call <<NODEPTR>> @axea.sortedset.<<ID>>.rotateRight(<<NODEPTR>> %node)
  ret <<NODEPTR>> %resultLL2
doLR2:
  %rotatedLeft2 = call <<NODEPTR>> @axea.sortedset.<<ID>>.rotateLeft(<<NODEPTR>> %curLeft)
  store <<NODEPTR>> %rotatedLeft2, <<NODEPTR>>* %lp2
  %resultLR2 = call <<NODEPTR>> @axea.sortedset.<<ID>>.rotateRight(<<NODEPTR>> %node)
  ret <<NODEPTR>> %resultLR2
checkRightHeavy:
  %rightHeavy = icmp slt i32 %balance, -1
  br i1 %rightHeavy, label %checkRightBalance, label %noRebalance2
checkRightBalance:
  %crlp = getelementptr <<NODE>>, <<NODEPTR>> %curRight, i32 0, i32 2
  %crl = load <<NODEPTR>>, <<NODEPTR>>* %crlp
  %crrp = getelementptr <<NODE>>, <<NODEPTR>> %curRight, i32 0, i32 3
  %crr = load <<NODEPTR>>, <<NODEPTR>>* %crrp
  %crlh = call i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %crl)
  %crrh = call i32 @axea.sortedset.<<ID>>.height(<<NODEPTR>> %crr)
  %rightBalance = sub i32 %crlh, %crrh
  %isRR2 = icmp sle i32 %rightBalance, 0
  br i1 %isRR2, label %doRR2, label %doRL2
doRR2:
  %resultRR2 = call <<NODEPTR>> @axea.sortedset.<<ID>>.rotateLeft(<<NODEPTR>> %node)
  ret <<NODEPTR>> %resultRR2
doRL2:
  %rotatedRight2 = call <<NODEPTR>> @axea.sortedset.<<ID>>.rotateRight(<<NODEPTR>> %curRight)
  store <<NODEPTR>> %rotatedRight2, <<NODEPTR>>* %rp2
  %resultRL2 = call <<NODEPTR>> @axea.sortedset.<<ID>>.rotateLeft(<<NODEPTR>> %node)
  ret <<NODEPTR>> %resultRL2
noRebalance2:
  ret <<NODEPTR>> %node
}
)";

    const char* const kSortedSetAddTemplate = R"(
define void @axea.sortedset.<<ID>>.add(<<HEADERPTR>> %h, <<KEYTYPE>> %key) {
entry:
  %rootp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %root = load <<NODEPTR>>, <<NODEPTR>>* %rootp
  %isNew = alloca i1
  store i1 0, i1* %isNew
  %newRoot = call <<NODEPTR>> @axea.sortedset.<<ID>>.insertNode(<<NODEPTR>> %root, <<KEYTYPE>> %key, i1* %isNew)
  store <<NODEPTR>> %newRoot, <<NODEPTR>>* %rootp
  %wasNew = load i1, i1* %isNew
  br i1 %wasNew, label %incCount, label %done
incCount:
  %cptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 0
  %oldCount = load i32, i32* %cptr
  %newCount = add i32 %oldCount, 1
  store i32 %newCount, i32* %cptr
  br label %done
done:
  ret void
}
)";

    const char* const kSortedSetContainsTemplate = R"(
define i1 @axea.sortedset.<<ID>>.contains(<<HEADERPTR>> %h, <<KEYTYPE>> %key) {
entry:
  %rootp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %root = load <<NODEPTR>>, <<NODEPTR>>* %rootp
  %cur = alloca <<NODEPTR>>
  store <<NODEPTR>> %root, <<NODEPTR>>* %cur
  br label %header
header:
  %c0 = load <<NODEPTR>>, <<NODEPTR>>* %cur
  %isnull = icmp eq <<NODEPTR>> %c0, null
  br i1 %isnull, label %notfound, label %check
check:
  %kp = getelementptr <<NODE>>, <<NODEPTR>> %c0, i32 0, i32 0
  %nodeKey = load <<KEYTYPE>>, <<KEYTYPEPTR>> %kp
  %lessThan = icmp slt <<KEYTYPE>> %key, %nodeKey
  br i1 %lessThan, label %goLeft, label %checkGreater
checkGreater:
  %greaterThan = icmp sgt <<KEYTYPE>> %key, %nodeKey
  br i1 %greaterThan, label %goRight, label %found
goLeft:
  %lp = getelementptr <<NODE>>, <<NODEPTR>> %c0, i32 0, i32 2
  %l = load <<NODEPTR>>, <<NODEPTR>>* %lp
  store <<NODEPTR>> %l, <<NODEPTR>>* %cur
  br label %header
goRight:
  %rp = getelementptr <<NODE>>, <<NODEPTR>> %c0, i32 0, i32 3
  %r = load <<NODEPTR>>, <<NODEPTR>>* %rp
  store <<NODEPTR>> %r, <<NODEPTR>>* %cur
  br label %header
found:
  ret i1 1
notfound:
  ret i1 0
}
)";

    const char* const kSortedSetRemoveTemplate = R"(
define void @axea.sortedset.<<ID>>.remove(<<HEADERPTR>> %h, <<KEYTYPE>> %key) {
entry:
  %rootp = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 1
  %root = load <<NODEPTR>>, <<NODEPTR>>* %rootp
  %isRemoved = alloca i1
  store i1 0, i1* %isRemoved
  %newRoot = call <<NODEPTR>> @axea.sortedset.<<ID>>.removeNode(<<NODEPTR>> %root, <<KEYTYPE>> %key, i1* %isRemoved)
  store <<NODEPTR>> %newRoot, <<NODEPTR>>* %rootp
  %wasRemoved = load i1, i1* %isRemoved
  br i1 %wasRemoved, label %decCount, label %done
decCount:
  %cptr = getelementptr <<HEADERTYPE>>, <<HEADERPTR>> %h, i32 0, i32 0
  %oldCount = load i32, i32* %cptr
  %newCount = sub i32 %oldCount, 1
  store i32 %newCount, i32* %cptr
  br label %done
done:
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

std::string LlvmIrEmitter::registerLinkedListInstantiation(const std::string& elementAxeaType)
{
    const std::string canonical = "LinkedList<" + elementAxeaType + ">";
    if (const auto it = linkedListInstantiationIds_.find(canonical);
        it != linkedListInstantiationIds_.end())
    {
        const std::string node = "%axea.LLNode." + std::to_string(it->second);
        return "{i32, " + node + "*, " + node + "*}*";
    }

    const int id = nextLinkedListInstantiationId_++;
    linkedListInstantiationIds_[canonical] = id;
    const std::string idStr = std::to_string(id);

    const std::string node = "%axea.LLNode." + idStr;
    const std::string nodePtr = node + "*";
    const std::string nodePtrPtr = node + "**";
    const std::string headerType = "{i32, " + nodePtr + ", " + nodePtr + "}";
    const std::string headerPtr = headerType + "*";
    const std::string valueType = llvmType(elementAxeaType);
    linkedListElementLlvmTypeById_[id] = valueType;

    linkedListTypeDeclsText_ << node << " = type { " << valueType << ", " << nodePtr << ", "
                             << nodePtr << " }\n";

    const std::vector<std::pair<std::string, std::string>> substitutions{
        {"<<NODEPTRPTR>>", nodePtrPtr},
        {"<<NODEPTR>>", nodePtr},
        {"<<NODE>>", node},
        {"<<HEADERPTR>>", headerPtr},
        {"<<HEADERTYPE>>", headerType},
        {"<<VALUETYPEPTR>>", valueType + "*"},
        {"<<VALUETYPE>>", valueType},
        {"<<ID>>", idStr},
    };

    linkedListRuntimeText_ << fillTemplate(kLinkedListPushFrontTemplate, substitutions);
    linkedListRuntimeText_ << fillTemplate(kLinkedListPushBackTemplate, substitutions);
    linkedListRuntimeText_ << fillTemplate(kLinkedListPopFrontTemplate, substitutions);
    linkedListRuntimeText_ << fillTemplate(kLinkedListPopBackTemplate, substitutions);

    return headerPtr;
}

std::string LlvmIrEmitter::registerSortedMapInstantiation(const std::string& keyAxeaType,
                                                          const std::string& valueAxeaType)
{
    const std::string canonical = "SortedMap<" + keyAxeaType + "," + valueAxeaType + ">";
    if (const auto it = sortedMapInstantiationIds_.find(canonical);
        it != sortedMapInstantiationIds_.end())
    {
        return "{i32, %axea.SortedMapNode." + std::to_string(it->second) + "*}*";
    }

    const int id = nextSortedMapInstantiationId_++;
    sortedMapInstantiationIds_[canonical] = id;
    const std::string idStr = std::to_string(id);

    const std::string node = "%axea.SortedMapNode." + idStr;
    const std::string nodePtr = node + "*";
    const std::string headerType = "{i32, " + nodePtr + "}";
    const std::string headerPtr = headerType + "*";
    // K is always i32 by the time this is reached (TypeChecker's own
    // orderability restriction - see docs/language/0040-sorted-maps.md), so
    // no registerKeyRuntime call is needed the way Map<K,V>'s own hashable-
    // and-arbitrary K requires.
    const std::string keyType = llvmType(keyAxeaType);
    const std::string valueType = llvmType(valueAxeaType);
    sortedMapValueLlvmTypeById_[id] = valueType;

    sortedMapTypeDeclsText_ << node << " = type { " << keyType << ", " << valueType << ", i32, "
                            << nodePtr << ", " << nodePtr << " }\n";

    const std::vector<std::pair<std::string, std::string>> substitutions{
        {"<<NODEPTR>>", nodePtr},
        {"<<NODE>>", node},
        {"<<HEADERPTR>>", headerPtr},
        {"<<HEADERTYPE>>", headerType},
        {"<<KEYTYPEPTR>>", keyType + "*"},
        {"<<KEYTYPE>>", keyType},
        {"<<VALUETYPEPTR>>", valueType + "*"},
        {"<<VALUETYPE>>", valueType},
        {"<<SENTINEL>>", sentinelFor(valueType)},
        {"<<ID>>", idStr},
    };

    sortedMapRuntimeText_ << fillTemplate(kSortedMapHeightTemplate, substitutions);
    sortedMapRuntimeText_ << fillTemplate(kSortedMapRotateRightTemplate, substitutions);
    sortedMapRuntimeText_ << fillTemplate(kSortedMapRotateLeftTemplate, substitutions);
    sortedMapRuntimeText_ << fillTemplate(kSortedMapInsertNodeTemplate, substitutions);
    sortedMapRuntimeText_ << fillTemplate(kSortedMapMinValueNodeTemplate, substitutions);
    sortedMapRuntimeText_ << fillTemplate(kSortedMapRemoveNodeTemplate, substitutions);
    sortedMapRuntimeText_ << fillTemplate(kSortedMapSetTemplate, substitutions);
    sortedMapRuntimeText_ << fillTemplate(kSortedMapGetTemplate, substitutions);
    sortedMapRuntimeText_ << fillTemplate(kSortedMapContainsTemplate, substitutions);
    sortedMapRuntimeText_ << fillTemplate(kSortedMapRemoveTemplate, substitutions);

    return headerPtr;
}

std::string LlvmIrEmitter::registerSortedSetInstantiation(const std::string& elementAxeaType)
{
    const std::string canonical = "SortedSet<" + elementAxeaType + ">";
    if (const auto it = sortedSetInstantiationIds_.find(canonical);
        it != sortedSetInstantiationIds_.end())
    {
        return "{i32, %axea.SortedSetNode." + std::to_string(it->second) + "*}*";
    }

    const int id = nextSortedSetInstantiationId_++;
    sortedSetInstantiationIds_[canonical] = id;
    const std::string idStr = std::to_string(id);

    const std::string node = "%axea.SortedSetNode." + idStr;
    const std::string nodePtr = node + "*";
    const std::string headerType = "{i32, " + nodePtr + "}";
    const std::string headerPtr = headerType + "*";
    // T is always i32 (TypeChecker's own orderability restriction - see
    // docs/language/0041-sorted-sets.md), so no registerKeyRuntime call is
    // needed, same reasoning as SortedMap<K,V>'s own K.
    const std::string keyType = llvmType(elementAxeaType);

    // 4 fields - key, height, left, right - no value field, unlike
    // SortedMap<K,V>'s own 5-field node.
    sortedSetTypeDeclsText_ << node << " = type { " << keyType << ", i32, " << nodePtr << ", "
                            << nodePtr << " }\n";

    const std::vector<std::pair<std::string, std::string>> substitutions{
        {"<<NODEPTR>>", nodePtr},
        {"<<NODE>>", node},
        {"<<HEADERPTR>>", headerPtr},
        {"<<HEADERTYPE>>", headerType},
        {"<<KEYTYPEPTR>>", keyType + "*"},
        {"<<KEYTYPE>>", keyType},
        {"<<ID>>", idStr},
    };

    sortedSetRuntimeText_ << fillTemplate(kSortedSetHeightTemplate, substitutions);
    sortedSetRuntimeText_ << fillTemplate(kSortedSetRotateRightTemplate, substitutions);
    sortedSetRuntimeText_ << fillTemplate(kSortedSetRotateLeftTemplate, substitutions);
    sortedSetRuntimeText_ << fillTemplate(kSortedSetInsertNodeTemplate, substitutions);
    sortedSetRuntimeText_ << fillTemplate(kSortedSetMinValueNodeTemplate, substitutions);
    sortedSetRuntimeText_ << fillTemplate(kSortedSetRemoveNodeTemplate, substitutions);
    sortedSetRuntimeText_ << fillTemplate(kSortedSetAddTemplate, substitutions);
    sortedSetRuntimeText_ << fillTemplate(kSortedSetContainsTemplate, substitutions);
    sortedSetRuntimeText_ << fillTemplate(kSortedSetRemoveTemplate, substitutions);

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

    if (objectType == "i8*")
    {
        // A bare str's ".length"/".bytes" (see docs/language/0047-unicode.md) -
        // previously unreachable here at all (TypeChecker rejected every
        // str field access). ".bytes" is the exact same runtime @strlen
        // call String/Buffer's own construction/append already use for an
        // operand's byte length; ".length" calls the new shared
        // @axea.utf8.count runtime directly on the bare pointer - no data
        // extraction needed, since str already *is* the data pointer.
        if (fieldGet.field == "bytes")
        {
            const int len64Reg = allocateRegister(fctx);
            *fctx.out << "  %" << len64Reg << " = call i64 @strlen(i8* "
                      << ref(fieldGet.object, fctx) << ")\n";
            const int destReg = defineRegister(fieldGet.dest, fctx);
            *fctx.out << "  %" << destReg << " = trunc i64 %" << len64Reg << " to i32\n";
        }
        else
        {
            const std::string fnName = registerUtf8CountRuntime();
            const int destReg = defineRegister(fieldGet.dest, fctx);
            *fctx.out << "  %" << destReg << " = call i32 " << fnName << "(i8* "
                      << ref(fieldGet.object, fctx) << ")\n";
        }
        return;
    }

    if (isStringType(objectType))
    {
        // Checked *before* the shared chain below - String's own header
        // is List<i8>-shaped, so ".bytes" (the raw stored count - what
        // ".length" itself used to mean, see docs/language/0042-string.md)
        // still rides that shared field-0 shape, but ".length" no longer
        // can: it now means "count codepoints", a genuine runtime scan
        // over the data pointer, not a stored field read at all (see
        // docs/language/0047-unicode.md). A dedicated branch is required
        // the moment even *one* of a type's own fields needs behavior the
        // shared chain can't express - the same reasoning Buffer's own
        // dedicated branch below was already built on.
        const std::string headerType = objectType.substr(0, objectType.size() - 1);
        if (fieldGet.field == "bytes")
        {
            const int lenPtrReg = allocateRegister(fctx);
            *fctx.out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", "
                      << objectType << " " << ref(fieldGet.object, fctx) << ", i32 0, i32 0\n";
            const int destReg = defineRegister(fieldGet.dest, fctx);
            *fctx.out << "  %" << destReg << " = load i32, i32* %" << lenPtrReg << "\n";
        }
        else
        {
            const int dataPtrPtrReg = allocateRegister(fctx);
            *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", "
                      << objectType << " " << ref(fieldGet.object, fctx) << ", i32 0, i32 1\n";
            const int dataPtrReg = allocateRegister(fctx);
            *fctx.out << "  %" << dataPtrReg << " = load i8*, i8** %" << dataPtrPtrReg << "\n";
            const std::string fnName = registerUtf8CountRuntime();
            const int destReg = defineRegister(fieldGet.dest, fctx);
            *fctx.out << "  %" << destReg << " = call i32 " << fnName << "(i8* %" << dataPtrReg
                      << ")\n";
        }
        return;
    }

    if (isBufferType(objectType))
    {
        // Checked *before* the shared chain below - Buffer's own 3-field
        // header ({i32 length, i32 capacity, i8* data}*) is structurally
        // identical to Deque<T>'s own ("{i32, i32, ...}*"), which would
        // otherwise silently match isDequeType's own looser test there and
        // always read field 0 regardless of which field was actually
        // requested (see docs/language/0043-buffer.md). ".bytes" (the raw
        // stored byte count - what ".length" itself used to mean) and
        // ".capacity" are still plain field reads; ".length" now means
        // "count codepoints", a runtime scan over the extracted data
        // pointer (field 2), same as String's own identical split above
        // (see docs/language/0047-unicode.md).
        const std::string headerType = objectType.substr(0, objectType.size() - 1);
        if (fieldGet.field == "length")
        {
            const int dataPtrPtrReg = allocateRegister(fctx);
            *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", "
                      << objectType << " " << ref(fieldGet.object, fctx) << ", i32 0, i32 2\n";
            const int dataPtrReg = allocateRegister(fctx);
            *fctx.out << "  %" << dataPtrReg << " = load i8*, i8** %" << dataPtrPtrReg << "\n";
            const std::string fnName = registerUtf8CountRuntime();
            const int destReg = defineRegister(fieldGet.dest, fctx);
            *fctx.out << "  %" << destReg << " = call i32 " << fnName << "(i8* %" << dataPtrReg
                      << ")\n";
            return;
        }
        const int fieldIndex = fieldGet.field == "capacity" ? 1 : 0;
        const int fieldPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << fieldPtrReg << " = getelementptr " << headerType << ", " << objectType
                  << " " << ref(fieldGet.object, fctx) << ", i32 0, i32 " << fieldIndex << "\n";
        const int destReg = defineRegister(fieldGet.dest, fctx);
        *fctx.out << "  %" << destReg << " = load i32, i32* %" << fieldPtrReg << "\n";
        return;
    }

    if (isListType(objectType) || isMapType(objectType) || isSetType(objectType) ||
        isLinkedListType(objectType) || isDequeType(objectType) || isSortedMapType(objectType) ||
        isSortedSetType(objectType))
    {
        // A List's ".length" is field 0 of the {i32, T*} heap record; a
        // Map/Set's ".length" is field 0 (count) of its own 3-field header
        // {count, bucketCount, buckets}; a LinkedList's ".length" is field 0
        // of its own 3-field header {length, head, tail}; a Deque's
        // ".length" is field 0 (count) of its own 3-field header {count,
        // start, data}; a SortedMap/SortedSet's ".length" is field 0
        // (count) of its own 2-field header {count, root} - same GEP
        // index, same shape, so this branch covers all seven (see
        // docs/language/0033-lists.md, docs/language/0034-maps-and-sets.md,
        // docs/language/0036-linked-lists.md, docs/language/0037-deques.md,
        // docs/language/0040-sorted-maps.md, docs/language/0041-sorted-sets.md).
        // Stack<T> (docs/language/0035-stacks.md)
        // needs no branch of its own here at all: llvmType("Stack<T>")
        // produces the *exact same text* llvmType("List<T>") would, so
        // isListType's own test already matches it - not a coincidence the
        // way Map/Set's own count field briefly was, but a direct
        // consequence of Stack<T> and List<T> being the literal same LLVM
        // type. isLinkedListType/isDequeType *are* still checked explicitly,
        // even though this shared field-0-i32 code would coincidentally
        // handle either header too (unlike Stack<T>, their header shapes
        // genuinely differ from List<T>'s) - same "explicit, not accidental"
        // choice Map/Set's own count field made. Unlike an array's
        // compile-time-constant ".length", this is always a genuine runtime
        // read, and unlike a slice's by-value extractvalue, these are all
        // accessed by pointer, so it's GEP+load.
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

    if (isDequeType(objectType))
    {
        // {i32, i32, T*}* - checked before isListType, same "explicit, not
        // accidental" reasoning as every other 3-field collection (see
        // docs/language/0037-deques.md). GEP+load start (field 1) and data
        // (field 2), add start to the given index (start only ever
        // increases via pop_front and resets to 0 on the next push - see
        // Design - so this is a plain add, never a modular/wraparound one),
        // then the same flat-pointer single-index GEP List's own branch uses.
        const std::string headerType = objectType.substr(0, objectType.size() - 1);
        const std::string elementType = dequeElementType(objectType);
        const int startPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << startPtrReg << " = getelementptr " << headerType << ", " << objectType
                  << " " << ref(indexGet.object, fctx) << ", i32 0, i32 1\n";
        const int startReg = allocateRegister(fctx);
        *fctx.out << "  %" << startReg << " = load i32, i32* %" << startPtrReg << "\n";
        const int dataPtrPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", "
                  << objectType << " " << ref(indexGet.object, fctx) << ", i32 0, i32 2\n";
        const int dataPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << dataPtrReg << " = load " << elementType << "*, " << elementType
                  << "** %" << dataPtrPtrReg << "\n";
        const int actualIdxReg = allocateRegister(fctx);
        *fctx.out << "  %" << actualIdxReg << " = add i32 %" << startReg << ", "
                  << ref(indexGet.index, fctx) << "\n";
        const int elementPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << elementPtrReg << " = getelementptr " << elementType << ", "
                  << elementType << "* %" << dataPtrReg << ", i32 %" << actualIdxReg << "\n";
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

    if (isDequeType(objectType))
    {
        // Mirrors emitIndexGet's own Deque branch exactly, plus a store
        // instead of a load - see docs/language/0037-deques.md.
        const std::string headerType = objectType.substr(0, objectType.size() - 1);
        const std::string elementType = dequeElementType(objectType);
        const int startPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << startPtrReg << " = getelementptr " << headerType << ", " << objectType
                  << " " << ref(indexSet.object, fctx) << ", i32 0, i32 1\n";
        const int startReg = allocateRegister(fctx);
        *fctx.out << "  %" << startReg << " = load i32, i32* %" << startPtrReg << "\n";
        const int dataPtrPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", "
                  << objectType << " " << ref(indexSet.object, fctx) << ", i32 0, i32 2\n";
        const int dataPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << dataPtrReg << " = load " << elementType << "*, " << elementType
                  << "** %" << dataPtrPtrReg << "\n";
        const int actualIdxReg = allocateRegister(fctx);
        *fctx.out << "  %" << actualIdxReg << " = add i32 %" << startReg << ", "
                  << ref(indexSet.index, fctx) << "\n";
        const int elementPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << elementPtrReg << " = getelementptr " << elementType << ", "
                  << elementType << "* %" << dataPtrReg << ", i32 %" << actualIdxReg << "\n";
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

void LlvmIrEmitter::emitPriorityQueueNew(const IrPriorityQueueNew& priorityQueueNew,
                                         FunctionContext& fctx)
{
    // Structurally identical to emitStackNew/emitListNew - see that
    // function's own comments for the sizeof idiom (see
    // docs/language/0039-priority-queues.md).
    const std::string elementType = llvmType(priorityQueueNew.elementTypeName);
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

    const int destReg = defineRegister(priorityQueueNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawPtrReg << " to " << pointerType
              << "\n";

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " " << ref(priorityQueueNew.dest, fctx) << ", i32 0, i32 0\n";
    *fctx.out << "  store i32 0, i32* %" << lenPtrReg << "\n";

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " " << ref(priorityQueueNew.dest, fctx) << ", i32 0, i32 1\n";
    *fctx.out << "  store " << elementType << "* null, " << elementType << "** %" << dataPtrPtrReg
              << "\n";
}

void LlvmIrEmitter::emitPriorityQueuePush(const IrPriorityQueuePush& priorityQueuePush,
                                          FunctionContext& fctx)
{
    // The grow-and-copy-and-append prefix is a direct copy of
    // emitStackPush's own (see that function's own comments for the
    // hand-verified alloca/load/store copy-loop idiom) - see
    // docs/language/0039-priority-queues.md.
    const std::string objectType = typeOf(priorityQueuePush.priorityQueue, fctx);
    const std::string headerType = objectType.substr(0, objectType.size() - 1);
    const std::string elementType = listElementType(objectType);
    const std::string pqRef = ref(priorityQueuePush.priorityQueue, fctx);

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << pqRef << ", i32 0, i32 0\n";
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
              << " " << pqRef << ", i32 0, i32 1\n";
    const int oldDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldDataReg << " = load " << elementType << "*, " << elementType << "** %"
              << dataPtrPtrReg << "\n";

    const int copyLabelId = fctx.nextLabel++;
    const std::string copyHeaderLabel =
        "priorityqueue.push.copy.header" + std::to_string(copyLabelId);
    const std::string copyBodyLabel = "priorityqueue.push.copy.body" + std::to_string(copyLabelId);
    const std::string copyDoneLabel = "priorityqueue.push.copy.done" + std::to_string(copyLabelId);

    const int counterSlotReg = allocateRegister(fctx);
    *fctx.out << "  %" << counterSlotReg << " = alloca i32\n";
    *fctx.out << "  store i32 0, i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << copyHeaderLabel << "\n";

    *fctx.out << copyHeaderLabel << ":\n";
    fctx.currentLabel = copyHeaderLabel;
    const int iReg = allocateRegister(fctx);
    *fctx.out << "  %" << iReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int condReg = allocateRegister(fctx);
    *fctx.out << "  %" << condReg << " = icmp slt i32 %" << iReg << ", %" << oldLenReg << "\n";
    *fctx.out << "  br i1 %" << condReg << ", label %" << copyBodyLabel << ", label %"
              << copyDoneLabel << "\n";

    *fctx.out << copyBodyLabel << ":\n";
    fctx.currentLabel = copyBodyLabel;
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
    *fctx.out << "  br label %" << copyHeaderLabel << "\n";

    *fctx.out << copyDoneLabel << ":\n";
    fctx.currentLabel = copyDoneLabel;

    const int newElementPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << newElementPtrReg << " = getelementptr " << elementType << ", "
              << elementType << "* %" << newDataReg << ", i32 %" << oldLenReg << "\n";
    *fctx.out << "  store " << elementType << " " << ref(priorityQueuePush.value, fctx) << ", "
              << elementType << "* %" << newElementPtrReg << "\n";

    // Sift-up: the just-appended element (index oldLen, in newData) moves
    // toward the root while it's smaller than its parent - see
    // docs/language/0039-priority-queues.md's own Design section. Hand-rolled
    // alloca/load/store-counter blocks, the same no-phi convention the copy
    // loop above uses, since a phi here would need to forward-reference a
    // not-yet-emitted register from doSwap on the loop's own back-edge.
    const int siftLabelId = fctx.nextLabel++;
    const std::string siftHeaderLabel =
        "priorityqueue.push.siftup.header" + std::to_string(siftLabelId);
    const std::string siftCheckLabel =
        "priorityqueue.push.siftup.check" + std::to_string(siftLabelId);
    const std::string siftSwapLabel =
        "priorityqueue.push.siftup.swap" + std::to_string(siftLabelId);
    const std::string siftDoneLabel =
        "priorityqueue.push.siftup.done" + std::to_string(siftLabelId);

    const int idxSlotReg = allocateRegister(fctx);
    *fctx.out << "  %" << idxSlotReg << " = alloca i32\n";
    *fctx.out << "  store i32 %" << oldLenReg << ", i32* %" << idxSlotReg << "\n";
    *fctx.out << "  br label %" << siftHeaderLabel << "\n";

    *fctx.out << siftHeaderLabel << ":\n";
    fctx.currentLabel = siftHeaderLabel;
    const int idxReg = allocateRegister(fctx);
    *fctx.out << "  %" << idxReg << " = load i32, i32* %" << idxSlotReg << "\n";
    const int isRootReg = allocateRegister(fctx);
    *fctx.out << "  %" << isRootReg << " = icmp eq i32 %" << idxReg << ", 0\n";
    *fctx.out << "  br i1 %" << isRootReg << ", label %" << siftDoneLabel << ", label %"
              << siftCheckLabel << "\n";

    *fctx.out << siftCheckLabel << ":\n";
    fctx.currentLabel = siftCheckLabel;
    const int idxMinus1Reg = allocateRegister(fctx);
    *fctx.out << "  %" << idxMinus1Reg << " = sub i32 %" << idxReg << ", 1\n";
    const int parentReg = allocateRegister(fctx);
    *fctx.out << "  %" << parentReg << " = sdiv i32 %" << idxMinus1Reg << ", 2\n";
    const int parentPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << parentPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << newDataReg << ", i32 %" << parentReg << "\n";
    const int parentValReg = allocateRegister(fctx);
    *fctx.out << "  %" << parentValReg << " = load " << elementType << ", " << elementType << "* %"
              << parentPtrReg << "\n";
    const int curPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << curPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << newDataReg << ", i32 %" << idxReg << "\n";
    const int curValReg = allocateRegister(fctx);
    *fctx.out << "  %" << curValReg << " = load " << elementType << ", " << elementType << "* %"
              << curPtrReg << "\n";
    const int okReg = allocateRegister(fctx);
    *fctx.out << "  %" << okReg << " = icmp sle i32 %" << parentValReg << ", %" << curValReg
              << "\n";
    *fctx.out << "  br i1 %" << okReg << ", label %" << siftDoneLabel << ", label %"
              << siftSwapLabel << "\n";

    *fctx.out << siftSwapLabel << ":\n";
    fctx.currentLabel = siftSwapLabel;
    *fctx.out << "  store " << elementType << " %" << curValReg << ", " << elementType << "* %"
              << parentPtrReg << "\n";
    *fctx.out << "  store " << elementType << " %" << parentValReg << ", " << elementType << "* %"
              << curPtrReg << "\n";
    *fctx.out << "  store i32 %" << parentReg << ", i32* %" << idxSlotReg << "\n";
    *fctx.out << "  br label %" << siftHeaderLabel << "\n";

    *fctx.out << siftDoneLabel << ":\n";
    fctx.currentLabel = siftDoneLabel;

    *fctx.out << "  store i32 %" << newLenReg << ", i32* %" << lenPtrReg << "\n";
    *fctx.out << "  store " << elementType << "* %" << newDataReg << ", " << elementType << "** %"
              << dataPtrPtrReg << "\n";
}

void LlvmIrEmitter::emitPriorityQueuePop(const IrPriorityQueuePop& priorityQueuePop,
                                         FunctionContext& fctx)
{
    // No bounds check (matches every other pop in this backend). Moves the
    // last element into the vacated root slot, shrinks the length, then
    // sifts that element down - see docs/language/0039-priority-queues.md's
    // own Design section.
    const std::string objectType = typeOf(priorityQueuePop.priorityQueue, fctx);
    const std::string headerType = objectType.substr(0, objectType.size() - 1);
    const std::string elementType = listElementType(objectType);
    const std::string pqRef = ref(priorityQueuePop.priorityQueue, fctx);

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << pqRef << ", i32 0, i32 0\n";
    const int oldLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldLenReg << " = load i32, i32* %" << lenPtrReg << "\n";
    const int newLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << newLenReg << " = sub i32 %" << oldLenReg << ", 1\n";

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << pqRef << ", i32 0, i32 1\n";
    const int dataReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataReg << " = load " << elementType << "*, " << elementType << "** %"
              << dataPtrPtrReg << "\n";

    const int rootPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << rootPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << dataReg << ", i32 0\n";
    // The value ultimately returned - captured now, before the root slot is
    // overwritten below.
    const int destReg = defineRegister(priorityQueuePop.dest, fctx);
    *fctx.out << "  %" << destReg << " = load " << elementType << ", " << elementType << "* %"
              << rootPtrReg << "\n";

    const int lastPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lastPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << dataReg << ", i32 %" << newLenReg << "\n";
    const int lastValReg = allocateRegister(fctx);
    *fctx.out << "  %" << lastValReg << " = load " << elementType << ", " << elementType << "* %"
              << lastPtrReg << "\n";
    *fctx.out << "  store " << elementType << " %" << lastValReg << ", " << elementType << "* %"
              << rootPtrReg << "\n";
    *fctx.out << "  store i32 %" << newLenReg << ", i32* %" << lenPtrReg << "\n";

    // Sift-down: the element now at the root moves toward the leaves,
    // swapping with whichever child is smaller, until the heap property
    // holds again. Same hand-rolled alloca/load/store, no-phi convention as
    // push's own sift-up loop above - one more block than sift-up needs,
    // to choose the smaller of two children before comparing against the
    // parent.
    const int siftLabelId = fctx.nextLabel++;
    const std::string headerLabel =
        "priorityqueue.pop.siftdown.header" + std::to_string(siftLabelId);
    const std::string checkRightLabel =
        "priorityqueue.pop.siftdown.checkright" + std::to_string(siftLabelId);
    const std::string compareRightLabel =
        "priorityqueue.pop.siftdown.compareright" + std::to_string(siftLabelId);
    const std::string setRightLabel =
        "priorityqueue.pop.siftdown.setright" + std::to_string(siftLabelId);
    const std::string compareSmallestLabel =
        "priorityqueue.pop.siftdown.comparesmallest" + std::to_string(siftLabelId);
    const std::string swapLabel = "priorityqueue.pop.siftdown.swap" + std::to_string(siftLabelId);
    const std::string doneLabel = "priorityqueue.pop.siftdown.done" + std::to_string(siftLabelId);

    const int idxSlotReg = allocateRegister(fctx);
    *fctx.out << "  %" << idxSlotReg << " = alloca i32\n";
    *fctx.out << "  store i32 0, i32* %" << idxSlotReg << "\n";
    const int smallestSlotReg = allocateRegister(fctx);
    *fctx.out << "  %" << smallestSlotReg << " = alloca i32\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << headerLabel << ":\n";
    fctx.currentLabel = headerLabel;
    const int idxReg = allocateRegister(fctx);
    *fctx.out << "  %" << idxReg << " = load i32, i32* %" << idxSlotReg << "\n";
    const int idxTimes2Reg = allocateRegister(fctx);
    *fctx.out << "  %" << idxTimes2Reg << " = mul i32 %" << idxReg << ", 2\n";
    const int leftReg = allocateRegister(fctx);
    *fctx.out << "  %" << leftReg << " = add i32 %" << idxTimes2Reg << ", 1\n";
    const int hasLeftReg = allocateRegister(fctx);
    *fctx.out << "  %" << hasLeftReg << " = icmp slt i32 %" << leftReg << ", %" << newLenReg
              << "\n";
    *fctx.out << "  br i1 %" << hasLeftReg << ", label %" << checkRightLabel << ", label %"
              << doneLabel << "\n";

    *fctx.out << checkRightLabel << ":\n";
    fctx.currentLabel = checkRightLabel;
    *fctx.out << "  store i32 %" << leftReg << ", i32* %" << smallestSlotReg << "\n";
    const int rightReg = allocateRegister(fctx);
    *fctx.out << "  %" << rightReg << " = add i32 %" << leftReg << ", 1\n";
    const int hasRightReg = allocateRegister(fctx);
    *fctx.out << "  %" << hasRightReg << " = icmp slt i32 %" << rightReg << ", %" << newLenReg
              << "\n";
    *fctx.out << "  br i1 %" << hasRightReg << ", label %" << compareRightLabel << ", label %"
              << compareSmallestLabel << "\n";

    *fctx.out << compareRightLabel << ":\n";
    fctx.currentLabel = compareRightLabel;
    const int rightPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << rightPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << dataReg << ", i32 %" << rightReg << "\n";
    const int rightValReg = allocateRegister(fctx);
    *fctx.out << "  %" << rightValReg << " = load " << elementType << ", " << elementType << "* %"
              << rightPtrReg << "\n";
    const int leftPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << leftPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << dataReg << ", i32 %" << leftReg << "\n";
    const int leftValReg = allocateRegister(fctx);
    *fctx.out << "  %" << leftValReg << " = load " << elementType << ", " << elementType << "* %"
              << leftPtrReg << "\n";
    const int rightSmallerReg = allocateRegister(fctx);
    *fctx.out << "  %" << rightSmallerReg << " = icmp slt i32 %" << rightValReg << ", %"
              << leftValReg << "\n";
    *fctx.out << "  br i1 %" << rightSmallerReg << ", label %" << setRightLabel << ", label %"
              << compareSmallestLabel << "\n";

    *fctx.out << setRightLabel << ":\n";
    fctx.currentLabel = setRightLabel;
    *fctx.out << "  store i32 %" << rightReg << ", i32* %" << smallestSlotReg << "\n";
    *fctx.out << "  br label %" << compareSmallestLabel << "\n";

    *fctx.out << compareSmallestLabel << ":\n";
    fctx.currentLabel = compareSmallestLabel;
    const int smallestReg = allocateRegister(fctx);
    *fctx.out << "  %" << smallestReg << " = load i32, i32* %" << smallestSlotReg << "\n";
    const int smallestPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << smallestPtrReg << " = getelementptr " << elementType << ", "
              << elementType << "* %" << dataReg << ", i32 %" << smallestReg << "\n";
    const int smallestValReg = allocateRegister(fctx);
    *fctx.out << "  %" << smallestValReg << " = load " << elementType << ", " << elementType
              << "* %" << smallestPtrReg << "\n";
    const int curPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << curPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << dataReg << ", i32 %" << idxReg << "\n";
    const int curValReg = allocateRegister(fctx);
    *fctx.out << "  %" << curValReg << " = load " << elementType << ", " << elementType << "* %"
              << curPtrReg << "\n";
    const int needSwapReg = allocateRegister(fctx);
    *fctx.out << "  %" << needSwapReg << " = icmp slt i32 %" << smallestValReg << ", %" << curValReg
              << "\n";
    *fctx.out << "  br i1 %" << needSwapReg << ", label %" << swapLabel << ", label %" << doneLabel
              << "\n";

    *fctx.out << swapLabel << ":\n";
    fctx.currentLabel = swapLabel;
    *fctx.out << "  store " << elementType << " %" << curValReg << ", " << elementType << "* %"
              << smallestPtrReg << "\n";
    *fctx.out << "  store " << elementType << " %" << smallestValReg << ", " << elementType << "* %"
              << curPtrReg << "\n";
    *fctx.out << "  store i32 %" << smallestReg << ", i32* %" << idxSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << doneLabel << ":\n";
    fctx.currentLabel = doneLabel;
}

void LlvmIrEmitter::emitPriorityQueuePeek(const IrPriorityQueuePeek& priorityQueuePeek,
                                          FunctionContext& fctx)
{
    // The minimum always sits at index 0 by the heap invariant - unlike
    // emitStackPeek, no length read/arithmetic is needed at all. No bounds
    // check, same reasoning as every other peek/pop here.
    const std::string objectType = typeOf(priorityQueuePeek.priorityQueue, fctx);
    const std::string headerType = objectType.substr(0, objectType.size() - 1);
    const std::string elementType = listElementType(objectType);
    const std::string pqRef = ref(priorityQueuePeek.priorityQueue, fctx);

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << pqRef << ", i32 0, i32 1\n";
    const int dataReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataReg << " = load " << elementType << "*, " << elementType << "** %"
              << dataPtrPtrReg << "\n";
    const int rootPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << rootPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << dataReg << ", i32 0\n";
    const int destReg = defineRegister(priorityQueuePeek.dest, fctx);
    *fctx.out << "  %" << destReg << " = load " << elementType << ", " << elementType << "* %"
              << rootPtrReg << "\n";
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

void LlvmIrEmitter::emitSortedMapNew(const IrSortedMapNew& sortedMapNew, FunctionContext& fctx)
{
    // A fresh {count: 0, root: null} heap header - same malloc + null-GEP
    // sizeof idiom as emitListNew/emitStackNew (2 fields, no bucket array
    // unlike emitMapNew/emitSetNew - a tree needs no initial bucket
    // allocation, see docs/language/0040-sorted-maps.md).
    const std::string pointerType =
        llvmType("SortedMap<" + sortedMapNew.keyTypeName + "," + sortedMapNew.valueTypeName + ">");
    const std::string headerType = pointerType.substr(0, pointerType.size() - 1);

    const int sizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizePtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " null, i32 1\n";
    const int sizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizeIntReg << " = ptrtoint " << pointerType << " %" << sizePtrReg
              << " to i64\n";
    const int rawHeaderReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawHeaderReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";
    const int destReg = defineRegister(sortedMapNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawHeaderReg << " to " << pointerType
              << "\n";

    const int countPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << countPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " %" << destReg << ", i32 0, i32 0\n";
    *fctx.out << "  store i32 0, i32* %" << countPtrReg << "\n";
    const std::string nodePtrType =
        "%axea.SortedMapNode." + sortedMapInstantiationId(pointerType) + "*";
    const int rootPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << rootPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " %" << destReg << ", i32 0, i32 1\n";
    *fctx.out << "  store " << nodePtrType << " null, " << nodePtrType << "* %" << rootPtrReg
              << "\n";
}

void LlvmIrEmitter::emitSortedMapSet(const IrSortedMapSet& sortedMapSet, FunctionContext& fctx)
{
    const std::string sortedMapType = typeOf(sortedMapSet.sortedMap, fctx);
    const std::string id = sortedMapInstantiationId(sortedMapType);
    const std::string keyType = typeOf(sortedMapSet.key, fctx);
    const std::string valueType = typeOf(sortedMapSet.value, fctx);
    *fctx.out << "  call void @axea.sortedmap." << id << ".set(" << sortedMapType << " "
              << ref(sortedMapSet.sortedMap, fctx) << ", " << keyType << " "
              << ref(sortedMapSet.key, fctx) << ", " << valueType << " "
              << ref(sortedMapSet.value, fctx) << ")\n";
}

void LlvmIrEmitter::emitSortedMapGet(const IrSortedMapGet& sortedMapGet, FunctionContext& fctx)
{
    const std::string sortedMapType = typeOf(sortedMapGet.sortedMap, fctx);
    const std::string id = sortedMapInstantiationId(sortedMapType);
    const std::string keyType = typeOf(sortedMapGet.key, fctx);
    const std::string valueType = typeOf(sortedMapGet.dest, fctx);
    const int destReg = defineRegister(sortedMapGet.dest, fctx);
    *fctx.out << "  %" << destReg << " = call " << valueType << " @axea.sortedmap." << id << ".get("
              << sortedMapType << " " << ref(sortedMapGet.sortedMap, fctx) << ", " << keyType << " "
              << ref(sortedMapGet.key, fctx) << ")\n";
}

void LlvmIrEmitter::emitSortedMapContains(const IrSortedMapContains& sortedMapContains,
                                          FunctionContext& fctx)
{
    const std::string sortedMapType = typeOf(sortedMapContains.sortedMap, fctx);
    const std::string id = sortedMapInstantiationId(sortedMapType);
    const std::string keyType = typeOf(sortedMapContains.key, fctx);
    const int destReg = defineRegister(sortedMapContains.dest, fctx);
    *fctx.out << "  %" << destReg << " = call i1 @axea.sortedmap." << id << ".contains("
              << sortedMapType << " " << ref(sortedMapContains.sortedMap, fctx) << ", " << keyType
              << " " << ref(sortedMapContains.key, fctx) << ")\n";
}

void LlvmIrEmitter::emitSortedMapRemove(const IrSortedMapRemove& sortedMapRemove,
                                        FunctionContext& fctx)
{
    const std::string sortedMapType = typeOf(sortedMapRemove.sortedMap, fctx);
    const std::string id = sortedMapInstantiationId(sortedMapType);
    const std::string keyType = typeOf(sortedMapRemove.key, fctx);
    *fctx.out << "  call void @axea.sortedmap." << id << ".remove(" << sortedMapType << " "
              << ref(sortedMapRemove.sortedMap, fctx) << ", " << keyType << " "
              << ref(sortedMapRemove.key, fctx) << ")\n";
}

void LlvmIrEmitter::emitSortedSetNew(const IrSortedSetNew& sortedSetNew, FunctionContext& fctx)
{
    // Mirrors emitSortedMapNew exactly - a fresh {count: 0, root: null}
    // heap header, no bucket array (see docs/language/0041-sorted-sets.md).
    const std::string pointerType = llvmType("SortedSet<" + sortedSetNew.elementTypeName + ">");
    const std::string headerType = pointerType.substr(0, pointerType.size() - 1);

    const int sizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizePtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " null, i32 1\n";
    const int sizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizeIntReg << " = ptrtoint " << pointerType << " %" << sizePtrReg
              << " to i64\n";
    const int rawHeaderReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawHeaderReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";
    const int destReg = defineRegister(sortedSetNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawHeaderReg << " to " << pointerType
              << "\n";

    const int countPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << countPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " %" << destReg << ", i32 0, i32 0\n";
    *fctx.out << "  store i32 0, i32* %" << countPtrReg << "\n";
    const std::string nodePtrType =
        "%axea.SortedSetNode." + sortedSetInstantiationId(pointerType) + "*";
    const int rootPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << rootPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " %" << destReg << ", i32 0, i32 1\n";
    *fctx.out << "  store " << nodePtrType << " null, " << nodePtrType << "* %" << rootPtrReg
              << "\n";
}

void LlvmIrEmitter::emitSortedSetAdd(const IrSortedSetAdd& sortedSetAdd, FunctionContext& fctx)
{
    const std::string sortedSetType = typeOf(sortedSetAdd.sortedSet, fctx);
    const std::string id = sortedSetInstantiationId(sortedSetType);
    const std::string elementType = typeOf(sortedSetAdd.value, fctx);
    *fctx.out << "  call void @axea.sortedset." << id << ".add(" << sortedSetType << " "
              << ref(sortedSetAdd.sortedSet, fctx) << ", " << elementType << " "
              << ref(sortedSetAdd.value, fctx) << ")\n";
}

void LlvmIrEmitter::emitSortedSetContains(const IrSortedSetContains& sortedSetContains,
                                          FunctionContext& fctx)
{
    const std::string sortedSetType = typeOf(sortedSetContains.sortedSet, fctx);
    const std::string id = sortedSetInstantiationId(sortedSetType);
    const std::string elementType = typeOf(sortedSetContains.value, fctx);
    const int destReg = defineRegister(sortedSetContains.dest, fctx);
    *fctx.out << "  %" << destReg << " = call i1 @axea.sortedset." << id << ".contains("
              << sortedSetType << " " << ref(sortedSetContains.sortedSet, fctx) << ", "
              << elementType << " " << ref(sortedSetContains.value, fctx) << ")\n";
}

void LlvmIrEmitter::emitSortedSetRemove(const IrSortedSetRemove& sortedSetRemove,
                                        FunctionContext& fctx)
{
    const std::string sortedSetType = typeOf(sortedSetRemove.sortedSet, fctx);
    const std::string id = sortedSetInstantiationId(sortedSetType);
    const std::string elementType = typeOf(sortedSetRemove.value, fctx);
    *fctx.out << "  call void @axea.sortedset." << id << ".remove(" << sortedSetType << " "
              << ref(sortedSetRemove.sortedSet, fctx) << ", " << elementType << " "
              << ref(sortedSetRemove.value, fctx) << ")\n";
}

void LlvmIrEmitter::emitStringNew(const IrStringNew& stringNew, FunctionContext& fctx)
{
    // The first collection here whose copy length is a *runtime* @strlen
    // call rather than a field already sitting in a header (see
    // docs/language/0042-string.md) - str has no length of its own, unlike
    // every element type every other collection's own push/new copies.
    const std::string srcPtr = resolveStrPtr(stringNew.text, fctx);

    const int srcLen64Reg = allocateRegister(fctx);
    *fctx.out << "  %" << srcLen64Reg << " = call i64 @strlen(i8* " << srcPtr << ")\n";
    const int srcLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << srcLenReg << " = trunc i64 %" << srcLen64Reg << " to i32\n";

    // Header malloc (sizeof {i32, i8*}) - same null-GEP idiom as
    // emitListNew.
    const int sizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizePtrReg << " = getelementptr {i32, i8*}, {i32, i8*}* null, i32 1\n";
    const int sizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizeIntReg << " = ptrtoint {i32, i8*}* %" << sizePtrReg << " to i64\n";
    const int rawHeaderReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawHeaderReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";
    const int destReg = defineRegister(stringNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawHeaderReg << " to {i32, i8*}*\n";

    // Data buffer: srcLen + 1 bytes, room for the null terminator that
    // keeps this backend's own top-level `%s` printing (and any future
    // FFI use as a plain cstr) correct with no extra bookkeeping.
    const int srcLen64bReg = allocateRegister(fctx);
    *fctx.out << "  %" << srcLen64bReg << " = zext i32 %" << srcLenReg << " to i64\n";
    const int bufBytesReg = allocateRegister(fctx);
    *fctx.out << "  %" << bufBytesReg << " = add i64 %" << srcLen64bReg << ", 1\n";
    const int newDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << newDataReg << " = call i8* @malloc(i64 %" << bufBytesReg << ")\n";

    // Copy loop: srcLen bytes from srcPtr into newDataReg - same hand-
    // verified alloca/load/store counter idiom emitListPush's own copy
    // loop uses (no phi - see that function's own comments).
    const int labelId = fctx.nextLabel++;
    const std::string headerLabel = "string.new.copy.header" + std::to_string(labelId);
    const std::string bodyLabel = "string.new.copy.body" + std::to_string(labelId);
    const std::string doneLabel = "string.new.copy.done" + std::to_string(labelId);

    const int counterSlotReg = allocateRegister(fctx);
    *fctx.out << "  %" << counterSlotReg << " = alloca i32\n";
    *fctx.out << "  store i32 0, i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << headerLabel << ":\n";
    fctx.currentLabel = headerLabel;
    const int iReg = allocateRegister(fctx);
    *fctx.out << "  %" << iReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int condReg = allocateRegister(fctx);
    *fctx.out << "  %" << condReg << " = icmp slt i32 %" << iReg << ", %" << srcLenReg << "\n";
    *fctx.out << "  br i1 %" << condReg << ", label %" << bodyLabel << ", label %" << doneLabel
              << "\n";

    *fctx.out << bodyLabel << ":\n";
    fctx.currentLabel = bodyLabel;
    const int iForSrcReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForSrcReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int srcElemPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << srcElemPtrReg << " = getelementptr i8, i8* " << srcPtr << ", i32 %"
              << iForSrcReg << "\n";
    const int byteValReg = allocateRegister(fctx);
    *fctx.out << "  %" << byteValReg << " = load i8, i8* %" << srcElemPtrReg << "\n";
    const int iForDstReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForDstReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int dstElemPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dstElemPtrReg << " = getelementptr i8, i8* %" << newDataReg << ", i32 %"
              << iForDstReg << "\n";
    *fctx.out << "  store i8 %" << byteValReg << ", i8* %" << dstElemPtrReg << "\n";
    const int iForIncReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForIncReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int iNextReg = allocateRegister(fctx);
    *fctx.out << "  %" << iNextReg << " = add i32 %" << iForIncReg << ", 1\n";
    *fctx.out << "  store i32 %" << iNextReg << ", i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << doneLabel << ":\n";
    fctx.currentLabel = doneLabel;

    const int nullPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << nullPtrReg << " = getelementptr i8, i8* %" << newDataReg << ", i32 %"
              << srcLenReg << "\n";
    *fctx.out << "  store i8 0, i8* %" << nullPtrReg << "\n";

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr {i32, i8*}, {i32, i8*}* "
              << ref(stringNew.dest, fctx) << ", i32 0, i32 0\n";
    *fctx.out << "  store i32 %" << srcLenReg << ", i32* %" << lenPtrReg << "\n";
    const int dataFieldPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataFieldPtrReg << " = getelementptr {i32, i8*}, {i32, i8*}* "
              << ref(stringNew.dest, fctx) << ", i32 0, i32 1\n";
    *fctx.out << "  store i8* %" << newDataReg << ", i8** %" << dataFieldPtrReg << "\n";
}

void LlvmIrEmitter::emitStringAppend(const IrStringAppend& stringAppend, FunctionContext& fctx)
{
    const std::string stringRef = ref(stringAppend.string, fctx);
    const std::string otherPtr = resolveStrPtr(stringAppend.other, fctx);

    const int otherLen64Reg = allocateRegister(fctx);
    *fctx.out << "  %" << otherLen64Reg << " = call i64 @strlen(i8* " << otherPtr << ")\n";
    const int otherLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << otherLenReg << " = trunc i64 %" << otherLen64Reg << " to i32\n";

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr {i32, i8*}, {i32, i8*}* " << stringRef
              << ", i32 0, i32 0\n";
    const int oldLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldLenReg << " = load i32, i32* %" << lenPtrReg << "\n";
    const int newLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << newLenReg << " = add i32 %" << oldLenReg << ", %" << otherLenReg << "\n";

    // New buffer, sized to the combined length + 1 (null terminator) - no
    // amortized growth, matching every push-based collection's own
    // identical "reallocate every time" simplification (see
    // docs/language/0033-lists.md).
    const int newLen64Reg = allocateRegister(fctx);
    *fctx.out << "  %" << newLen64Reg << " = zext i32 %" << newLenReg << " to i64\n";
    const int newBytesReg = allocateRegister(fctx);
    *fctx.out << "  %" << newBytesReg << " = add i64 %" << newLen64Reg << ", 1\n";
    const int newDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << newDataReg << " = call i8* @malloc(i64 %" << newBytesReg << ")\n";

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr {i32, i8*}, {i32, i8*}* " << stringRef
              << ", i32 0, i32 1\n";
    const int oldDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldDataReg << " = load i8*, i8** %" << dataPtrPtrReg << "\n";

    // Copy loop 1: the string's own existing oldLen bytes into
    // newDataReg[0..oldLen).
    const int labelId1 = fctx.nextLabel++;
    const std::string oldHeaderLabel = "string.append.copyold.header" + std::to_string(labelId1);
    const std::string oldBodyLabel = "string.append.copyold.body" + std::to_string(labelId1);
    const std::string oldDoneLabel = "string.append.copyold.done" + std::to_string(labelId1);

    const int oldCounterSlotReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldCounterSlotReg << " = alloca i32\n";
    *fctx.out << "  store i32 0, i32* %" << oldCounterSlotReg << "\n";
    *fctx.out << "  br label %" << oldHeaderLabel << "\n";

    *fctx.out << oldHeaderLabel << ":\n";
    fctx.currentLabel = oldHeaderLabel;
    const int oi0Reg = allocateRegister(fctx);
    *fctx.out << "  %" << oi0Reg << " = load i32, i32* %" << oldCounterSlotReg << "\n";
    const int oCondReg = allocateRegister(fctx);
    *fctx.out << "  %" << oCondReg << " = icmp slt i32 %" << oi0Reg << ", %" << oldLenReg << "\n";
    *fctx.out << "  br i1 %" << oCondReg << ", label %" << oldBodyLabel << ", label %"
              << oldDoneLabel << "\n";

    *fctx.out << oldBodyLabel << ":\n";
    fctx.currentLabel = oldBodyLabel;
    const int oiSrcReg = allocateRegister(fctx);
    *fctx.out << "  %" << oiSrcReg << " = load i32, i32* %" << oldCounterSlotReg << "\n";
    const int oSrcPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << oSrcPtrReg << " = getelementptr i8, i8* %" << oldDataReg << ", i32 %"
              << oiSrcReg << "\n";
    const int oByteValReg = allocateRegister(fctx);
    *fctx.out << "  %" << oByteValReg << " = load i8, i8* %" << oSrcPtrReg << "\n";
    const int oiDstReg = allocateRegister(fctx);
    *fctx.out << "  %" << oiDstReg << " = load i32, i32* %" << oldCounterSlotReg << "\n";
    const int oDstPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << oDstPtrReg << " = getelementptr i8, i8* %" << newDataReg << ", i32 %"
              << oiDstReg << "\n";
    *fctx.out << "  store i8 %" << oByteValReg << ", i8* %" << oDstPtrReg << "\n";
    const int oiIncReg = allocateRegister(fctx);
    *fctx.out << "  %" << oiIncReg << " = load i32, i32* %" << oldCounterSlotReg << "\n";
    const int oiNextReg = allocateRegister(fctx);
    *fctx.out << "  %" << oiNextReg << " = add i32 %" << oiIncReg << ", 1\n";
    *fctx.out << "  store i32 %" << oiNextReg << ", i32* %" << oldCounterSlotReg << "\n";
    *fctx.out << "  br label %" << oldHeaderLabel << "\n";

    *fctx.out << oldDoneLabel << ":\n";
    fctx.currentLabel = oldDoneLabel;

    // Copy loop 2: other's own otherLen bytes into
    // newDataReg[oldLen..oldLen+otherLen).
    const int labelId2 = fctx.nextLabel++;
    const std::string newHeaderLabel = "string.append.copynew.header" + std::to_string(labelId2);
    const std::string newBodyLabel = "string.append.copynew.body" + std::to_string(labelId2);
    const std::string newDoneLabel = "string.append.copynew.done" + std::to_string(labelId2);

    const int newCounterSlotReg = allocateRegister(fctx);
    *fctx.out << "  %" << newCounterSlotReg << " = alloca i32\n";
    *fctx.out << "  store i32 0, i32* %" << newCounterSlotReg << "\n";
    *fctx.out << "  br label %" << newHeaderLabel << "\n";

    *fctx.out << newHeaderLabel << ":\n";
    fctx.currentLabel = newHeaderLabel;
    const int ni0Reg = allocateRegister(fctx);
    *fctx.out << "  %" << ni0Reg << " = load i32, i32* %" << newCounterSlotReg << "\n";
    const int nCondReg = allocateRegister(fctx);
    *fctx.out << "  %" << nCondReg << " = icmp slt i32 %" << ni0Reg << ", %" << otherLenReg << "\n";
    *fctx.out << "  br i1 %" << nCondReg << ", label %" << newBodyLabel << ", label %"
              << newDoneLabel << "\n";

    *fctx.out << newBodyLabel << ":\n";
    fctx.currentLabel = newBodyLabel;
    const int niSrcReg = allocateRegister(fctx);
    *fctx.out << "  %" << niSrcReg << " = load i32, i32* %" << newCounterSlotReg << "\n";
    const int nSrcPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << nSrcPtrReg << " = getelementptr i8, i8* " << otherPtr << ", i32 %"
              << niSrcReg << "\n";
    const int nByteValReg = allocateRegister(fctx);
    *fctx.out << "  %" << nByteValReg << " = load i8, i8* %" << nSrcPtrReg << "\n";
    const int niDstIdxReg = allocateRegister(fctx);
    *fctx.out << "  %" << niDstIdxReg << " = load i32, i32* %" << newCounterSlotReg << "\n";
    const int niDstOffReg = allocateRegister(fctx);
    *fctx.out << "  %" << niDstOffReg << " = add i32 %" << oldLenReg << ", %" << niDstIdxReg
              << "\n";
    const int nDstPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << nDstPtrReg << " = getelementptr i8, i8* %" << newDataReg << ", i32 %"
              << niDstOffReg << "\n";
    *fctx.out << "  store i8 %" << nByteValReg << ", i8* %" << nDstPtrReg << "\n";
    const int niIncReg = allocateRegister(fctx);
    *fctx.out << "  %" << niIncReg << " = load i32, i32* %" << newCounterSlotReg << "\n";
    const int niNextReg = allocateRegister(fctx);
    *fctx.out << "  %" << niNextReg << " = add i32 %" << niIncReg << ", 1\n";
    *fctx.out << "  store i32 %" << niNextReg << ", i32* %" << newCounterSlotReg << "\n";
    *fctx.out << "  br label %" << newHeaderLabel << "\n";

    *fctx.out << newDoneLabel << ":\n";
    fctx.currentLabel = newDoneLabel;

    const int nullPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << nullPtrReg << " = getelementptr i8, i8* %" << newDataReg << ", i32 %"
              << newLenReg << "\n";
    *fctx.out << "  store i8 0, i8* %" << nullPtrReg << "\n";

    // Store new length/data back into the header's own fields in place -
    // the header pointer itself never changes, so every existing alias
    // sees the update, exactly like emitListPush's own identical closing
    // step.
    *fctx.out << "  store i32 %" << newLenReg << ", i32* %" << lenPtrReg << "\n";
    *fctx.out << "  store i8* %" << newDataReg << ", i8** %" << dataPtrPtrReg << "\n";
}

void LlvmIrEmitter::emitStrSlice(const IrStrSlice& strSlice, FunctionContext& fctx)
{
    const std::string objectPtr = resolveStrPtr(strSlice.object, fctx);

    std::string startRef = "0";
    if (strSlice.start != -1)
    {
        startRef = ref(strSlice.start, fctx);
    }

    std::string endRef;
    if (strSlice.end != -1)
    {
        endRef = ref(strSlice.end, fctx);
    }
    else
    {
        const int objLen64Reg = allocateRegister(fctx);
        *fctx.out << "  %" << objLen64Reg << " = call i64 @strlen(i8* " << objectPtr << ")\n";
        const int objLenReg = allocateRegister(fctx);
        *fctx.out << "  %" << objLenReg << " = trunc i64 %" << objLen64Reg << " to i32\n";
        endRef = "%" + std::to_string(objLenReg);
    }

    const int lengthReg = allocateRegister(fctx);
    *fctx.out << "  %" << lengthReg << " = sub i32 " << endRef << ", " << startRef << "\n";

    const int length64Reg = allocateRegister(fctx);
    *fctx.out << "  %" << length64Reg << " = zext i32 %" << lengthReg << " to i64\n";
    const int bufBytesReg = allocateRegister(fctx);
    *fctx.out << "  %" << bufBytesReg << " = add i64 %" << length64Reg << ", 1\n";
    const int destReg = defineRegister(strSlice.dest, fctx);
    *fctx.out << "  %" << destReg << " = call i8* @malloc(i64 %" << bufBytesReg << ")\n";

    // Copy loop: `length` bytes from objectPtr[start..start+length) into
    // the new buffer[0..length) - same hand-verified alloca/load/store
    // counter idiom every other copy loop in this backend uses, no phi.
    const int labelId = fctx.nextLabel++;
    const std::string headerLabel = "strslice.copy.header" + std::to_string(labelId);
    const std::string bodyLabel = "strslice.copy.body" + std::to_string(labelId);
    const std::string doneLabel = "strslice.copy.done" + std::to_string(labelId);

    const int counterSlotReg = allocateRegister(fctx);
    *fctx.out << "  %" << counterSlotReg << " = alloca i32\n";
    *fctx.out << "  store i32 0, i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << headerLabel << ":\n";
    fctx.currentLabel = headerLabel;
    const int iReg = allocateRegister(fctx);
    *fctx.out << "  %" << iReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int condReg = allocateRegister(fctx);
    *fctx.out << "  %" << condReg << " = icmp slt i32 %" << iReg << ", %" << lengthReg << "\n";
    *fctx.out << "  br i1 %" << condReg << ", label %" << bodyLabel << ", label %" << doneLabel
              << "\n";

    *fctx.out << bodyLabel << ":\n";
    fctx.currentLabel = bodyLabel;
    const int iForSrcReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForSrcReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int srcOffReg = allocateRegister(fctx);
    *fctx.out << "  %" << srcOffReg << " = add i32 " << startRef << ", %" << iForSrcReg << "\n";
    const int srcPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << srcPtrReg << " = getelementptr i8, i8* " << objectPtr << ", i32 %"
              << srcOffReg << "\n";
    const int byteValReg = allocateRegister(fctx);
    *fctx.out << "  %" << byteValReg << " = load i8, i8* %" << srcPtrReg << "\n";
    const int iForDstReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForDstReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int dstPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dstPtrReg << " = getelementptr i8, i8* %" << destReg << ", i32 %"
              << iForDstReg << "\n";
    *fctx.out << "  store i8 %" << byteValReg << ", i8* %" << dstPtrReg << "\n";
    const int iForIncReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForIncReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int iNextReg = allocateRegister(fctx);
    *fctx.out << "  %" << iNextReg << " = add i32 %" << iForIncReg << ", 1\n";
    *fctx.out << "  store i32 %" << iNextReg << ", i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << doneLabel << ":\n";
    fctx.currentLabel = doneLabel;

    const int nullPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << nullPtrReg << " = getelementptr i8, i8* %" << destReg << ", i32 %"
              << lengthReg << "\n";
    *fctx.out << "  store i8 0, i8* %" << nullPtrReg << "\n";
}

void LlvmIrEmitter::ensureBufferCapacity(const std::string& bufferRef,
                                         const std::string& neededRef,
                                         FunctionContext& fctx)
{
    const int capPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << capPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 1\n";
    const int capReg = allocateRegister(fctx);
    *fctx.out << "  %" << capReg << " = load i32, i32* %" << capPtrReg << "\n";
    const int needGrowReg = allocateRegister(fctx);
    *fctx.out << "  %" << needGrowReg << " = icmp sgt i32 " << neededRef << ", %" << capReg << "\n";

    const int labelId = fctx.nextLabel++;
    const std::string growLabel = "buffer.grow" + std::to_string(labelId);
    const std::string doneLabel = "buffer.grow.done" + std::to_string(labelId);
    *fctx.out << "  br i1 %" << needGrowReg << ", label %" << growLabel << ", label %" << doneLabel
              << "\n";

    *fctx.out << growLabel << ":\n";
    fctx.currentLabel = growLabel;
    const int doubledReg = allocateRegister(fctx);
    *fctx.out << "  %" << doubledReg << " = mul i32 %" << capReg << ", 2\n";
    const int needsMoreReg = allocateRegister(fctx);
    *fctx.out << "  %" << needsMoreReg << " = icmp sgt i32 " << neededRef << ", %" << doubledReg
              << "\n";
    const int newCapReg = allocateRegister(fctx);
    *fctx.out << "  %" << newCapReg << " = select i1 %" << needsMoreReg << ", i32 " << neededRef
              << ", i32 %" << doubledReg << "\n";
    const int newCap64Reg = allocateRegister(fctx);
    *fctx.out << "  %" << newCap64Reg << " = zext i32 %" << newCapReg << " to i64\n";
    const int newDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << newDataReg << " = call i8* @malloc(i64 %" << newCap64Reg << ")\n";

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 0\n";
    const int lenReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenReg << " = load i32, i32* %" << lenPtrReg << "\n";
    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 2\n";
    const int oldDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldDataReg << " = load i8*, i8** %" << dataPtrPtrReg << "\n";

    // Copy loop: the buffer's own existing `length` bytes into newDataReg -
    // same hand-verified alloca/load/store counter idiom emitStringNew's
    // own copy loop uses (no phi).
    const std::string headerLabel = "buffer.grow.copy.header" + std::to_string(labelId);
    const std::string bodyLabel = "buffer.grow.copy.body" + std::to_string(labelId);
    const std::string copyDoneLabel = "buffer.grow.copy.done" + std::to_string(labelId);

    const int counterSlotReg = allocateRegister(fctx);
    *fctx.out << "  %" << counterSlotReg << " = alloca i32\n";
    *fctx.out << "  store i32 0, i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << headerLabel << ":\n";
    fctx.currentLabel = headerLabel;
    const int iReg = allocateRegister(fctx);
    *fctx.out << "  %" << iReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int condReg = allocateRegister(fctx);
    *fctx.out << "  %" << condReg << " = icmp slt i32 %" << iReg << ", %" << lenReg << "\n";
    *fctx.out << "  br i1 %" << condReg << ", label %" << bodyLabel << ", label %" << copyDoneLabel
              << "\n";

    *fctx.out << bodyLabel << ":\n";
    fctx.currentLabel = bodyLabel;
    const int iForSrcReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForSrcReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int srcElemPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << srcElemPtrReg << " = getelementptr i8, i8* %" << oldDataReg << ", i32 %"
              << iForSrcReg << "\n";
    const int byteValReg = allocateRegister(fctx);
    *fctx.out << "  %" << byteValReg << " = load i8, i8* %" << srcElemPtrReg << "\n";
    const int iForDstReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForDstReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int dstElemPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dstElemPtrReg << " = getelementptr i8, i8* %" << newDataReg << ", i32 %"
              << iForDstReg << "\n";
    *fctx.out << "  store i8 %" << byteValReg << ", i8* %" << dstElemPtrReg << "\n";
    const int iForIncReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForIncReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int iNextReg = allocateRegister(fctx);
    *fctx.out << "  %" << iNextReg << " = add i32 %" << iForIncReg << ", 1\n";
    *fctx.out << "  store i32 %" << iNextReg << ", i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << copyDoneLabel << ":\n";
    fctx.currentLabel = copyDoneLabel;

    *fctx.out << "  store i32 %" << newCapReg << ", i32* %" << capPtrReg << "\n";
    *fctx.out << "  store i8* %" << newDataReg << ", i8** %" << dataPtrPtrReg << "\n";
    *fctx.out << "  br label %" << doneLabel << "\n";

    *fctx.out << doneLabel << ":\n";
    fctx.currentLabel = doneLabel;
}

std::string LlvmIrEmitter::encodeCharUtf8(const std::string& codepointRef, FunctionContext& fctx)
{
    // Always malloc a fixed 5 bytes (max 4 UTF-8 bytes + null terminator) -
    // every branch below writes its own null terminator at the correct
    // offset for however many bytes it actually used, so the unused tail
    // bytes (if any) are simply never read. `ule`, not `sle`, throughout:
    // a codepoint is conceptually an unsigned magnitude (see
    // docs/language/0044-char.md) even though it's stored in i24, and
    // every valid codepoint (0..0x10FFFF) fits well within i24's signed
    // range anyway, so this is a clarity choice, not a correctness one.
    const int bufReg = allocateRegister(fctx);
    *fctx.out << "  %" << bufReg << " = call i8* @malloc(i64 5)\n";

    const int labelId = fctx.nextLabel++;
    const std::string oneByteLabel = "char.utf8.len1." + std::to_string(labelId);
    const std::string checkTwoLabel = "char.utf8.check2." + std::to_string(labelId);
    const std::string twoByteLabel = "char.utf8.len2." + std::to_string(labelId);
    const std::string checkThreeLabel = "char.utf8.check3." + std::to_string(labelId);
    const std::string threeByteLabel = "char.utf8.len3." + std::to_string(labelId);
    const std::string fourByteLabel = "char.utf8.len4." + std::to_string(labelId);
    const std::string doneLabel = "char.utf8.done." + std::to_string(labelId);

    const auto storeByte = [&](int byteReg, int offset)
    {
        const int ptrReg = allocateRegister(fctx);
        *fctx.out << "  %" << ptrReg << " = getelementptr i8, i8* %" << bufReg << ", i32 " << offset
                  << "\n";
        *fctx.out << "  store i8 %" << byteReg << ", i8* %" << ptrReg << "\n";
    };
    const auto storeNull = [&](int offset)
    {
        const int ptrReg = allocateRegister(fctx);
        *fctx.out << "  %" << ptrReg << " = getelementptr i8, i8* %" << bufReg << ", i32 " << offset
                  << "\n";
        *fctx.out << "  store i8 0, i8* %" << ptrReg << "\n";
    };
    // byte = (codepointRef >> shift) & mask, OR'd with `tag` (the leading
    // bits identifying this byte's position in the sequence), truncated
    // to i8 - shift/mask of 0 skips the shift/mask step (used for the
    // trailing byte of every multi-byte sequence, and for the whole value
    // in the 1-byte case).
    const auto encodeByte = [&](int shift, int mask, int tag) -> int
    {
        std::string valueRef = codepointRef;
        if (shift != 0)
        {
            const int shifted = allocateRegister(fctx);
            *fctx.out << "  %" << shifted << " = lshr i24 " << valueRef << ", " << shift << "\n";
            valueRef = "%" + std::to_string(shifted);
        }
        if (mask != 0)
        {
            const int masked = allocateRegister(fctx);
            *fctx.out << "  %" << masked << " = and i24 " << valueRef << ", " << mask << "\n";
            valueRef = "%" + std::to_string(masked);
        }
        const int tagged = allocateRegister(fctx);
        *fctx.out << "  %" << tagged << " = or i24 " << valueRef << ", " << tag << "\n";
        const int byte8 = allocateRegister(fctx);
        *fctx.out << "  %" << byte8 << " = trunc i24 %" << tagged << " to i8\n";
        return byte8;
    };

    const int cond1 = allocateRegister(fctx);
    *fctx.out << "  %" << cond1 << " = icmp ule i24 " << codepointRef << ", 127\n";
    *fctx.out << "  br i1 %" << cond1 << ", label %" << oneByteLabel << ", label %" << checkTwoLabel
              << "\n";

    *fctx.out << oneByteLabel << ":\n";
    fctx.currentLabel = oneByteLabel;
    storeByte(encodeByte(0, 0, 0), 0);
    storeNull(1);
    *fctx.out << "  br label %" << doneLabel << "\n";

    *fctx.out << checkTwoLabel << ":\n";
    fctx.currentLabel = checkTwoLabel;
    const int cond2 = allocateRegister(fctx);
    *fctx.out << "  %" << cond2 << " = icmp ule i24 " << codepointRef << ", 2047\n";
    *fctx.out << "  br i1 %" << cond2 << ", label %" << twoByteLabel << ", label %"
              << checkThreeLabel << "\n";

    *fctx.out << twoByteLabel << ":\n";
    fctx.currentLabel = twoByteLabel;
    storeByte(encodeByte(6, 0, 0xC0), 0);
    storeByte(encodeByte(0, 0x3F, 0x80), 1);
    storeNull(2);
    *fctx.out << "  br label %" << doneLabel << "\n";

    *fctx.out << checkThreeLabel << ":\n";
    fctx.currentLabel = checkThreeLabel;
    const int cond3 = allocateRegister(fctx);
    *fctx.out << "  %" << cond3 << " = icmp ule i24 " << codepointRef << ", 65535\n";
    *fctx.out << "  br i1 %" << cond3 << ", label %" << threeByteLabel << ", label %"
              << fourByteLabel << "\n";

    *fctx.out << threeByteLabel << ":\n";
    fctx.currentLabel = threeByteLabel;
    storeByte(encodeByte(12, 0, 0xE0), 0);
    storeByte(encodeByte(6, 0x3F, 0x80), 1);
    storeByte(encodeByte(0, 0x3F, 0x80), 2);
    storeNull(3);
    *fctx.out << "  br label %" << doneLabel << "\n";

    *fctx.out << fourByteLabel << ":\n";
    fctx.currentLabel = fourByteLabel;
    storeByte(encodeByte(18, 0, 0xF0), 0);
    storeByte(encodeByte(12, 0x3F, 0x80), 1);
    storeByte(encodeByte(6, 0x3F, 0x80), 2);
    storeByte(encodeByte(0, 0x3F, 0x80), 3);
    storeNull(4);
    *fctx.out << "  br label %" << doneLabel << "\n";

    *fctx.out << doneLabel << ":\n";
    fctx.currentLabel = doneLabel;

    return "%" + std::to_string(bufReg);
}

std::string LlvmIrEmitter::registerParseRuntime(const std::string& targetType)
{
    if (targetType == "i32")
    {
        const std::string fnName = "@axea.parse.i32";
        if (parseI32Registered_)
        {
            return fnName;
        }
        parseI32Registered_ = true;

        // Hand-rolled digit loop (see docs/language/0046-generic-methods.md)
        // - optional leading '-', then decimal digits until a
        // non-digit/null terminator. No overflow checking, and invalid
        // input (no digits at all) parses as 0 - a defined, documented
        // fallback, matching every other "no bounds checking in compiled
        // code" simplification in this backend. alloca/load/store for
        // every loop-carried value (idx/acc/sign), no phi, `select` to
        // apply the sign at the end - the same idioms
        // ensureBufferCapacity/encodeCharUtf8 already established.
        parseRuntimeText_ << R"(
define i32 @axea.parse.i32(i8* %s) {
entry:
  %idxSlot = alloca i32
  %accSlot = alloca i32
  %negSlot = alloca i32
  store i32 0, i32* %accSlot
  %c0p = getelementptr i8, i8* %s, i32 0
  %c0 = load i8, i8* %c0p
  %isNeg = icmp eq i8 %c0, 45
  br i1 %isNeg, label %neg, label %nonneg
neg:
  store i32 1, i32* %idxSlot
  store i32 1, i32* %negSlot
  br label %loophdr
nonneg:
  store i32 0, i32* %idxSlot
  store i32 0, i32* %negSlot
  br label %loophdr
loophdr:
  %idx = load i32, i32* %idxSlot
  %cp = getelementptr i8, i8* %s, i32 %idx
  %c = load i8, i8* %cp
  %ge0 = icmp sge i8 %c, 48
  %le9 = icmp sle i8 %c, 57
  %isDigit = and i1 %ge0, %le9
  br i1 %isDigit, label %loopbody, label %loopdone
loopbody:
  %acc0 = load i32, i32* %accSlot
  %acc1 = mul i32 %acc0, 10
  %dv8 = sub i8 %c, 48
  %dv32 = zext i8 %dv8 to i32
  %acc2 = add i32 %acc1, %dv32
  store i32 %acc2, i32* %accSlot
  %idx2 = load i32, i32* %idxSlot
  %idx3 = add i32 %idx2, 1
  store i32 %idx3, i32* %idxSlot
  br label %loophdr
loopdone:
  %finalAcc = load i32, i32* %accSlot
  %negFlag = load i32, i32* %negSlot
  %isNegBool = icmp ne i32 %negFlag, 0
  %negated = sub i32 0, %finalAcc
  %result = select i1 %isNegBool, i32 %negated, i32 %finalAcc
  ret i32 %result
}
)";
        return fnName;
    }

    // "bool"
    const std::string fnName = "@axea.parse.bool";
    if (parseBoolRegistered_)
    {
        return fnName;
    }
    parseBoolRegistered_ = true;

    // Hand-rolled short-circuit byte comparison against the fixed literal
    // "true" (see docs/language/0046-generic-methods.md) - branches, not
    // an unrolled straight-line chain, specifically so a short input
    // (e.g. "t") never reads past its own null terminator: each
    // subsequent byte is only read after confirming the previous one
    // matched a non-null expected character, guaranteeing the string
    // continues at least that far. Anything other than exactly "true"
    // (including "TRUE", "false", "", "trueish") parses as false - a
    // defined, lenient fallback, not an error, matching parse<i32>'s own
    // "invalid input yields a harmless default" choice.
    parseRuntimeText_ << R"(
define i1 @axea.parse.bool(i8* %s) {
entry:
  %c0p = getelementptr i8, i8* %s, i32 0
  %c0 = load i8, i8* %c0p
  %e0 = icmp eq i8 %c0, 116
  br i1 %e0, label %check1, label %isfalse
check1:
  %c1p = getelementptr i8, i8* %s, i32 1
  %c1 = load i8, i8* %c1p
  %e1 = icmp eq i8 %c1, 114
  br i1 %e1, label %check2, label %isfalse
check2:
  %c2p = getelementptr i8, i8* %s, i32 2
  %c2 = load i8, i8* %c2p
  %e2 = icmp eq i8 %c2, 117
  br i1 %e2, label %check3, label %isfalse
check3:
  %c3p = getelementptr i8, i8* %s, i32 3
  %c3 = load i8, i8* %c3p
  %e3 = icmp eq i8 %c3, 101
  br i1 %e3, label %check4, label %isfalse
check4:
  %c4p = getelementptr i8, i8* %s, i32 4
  %c4 = load i8, i8* %c4p
  %e4 = icmp eq i8 %c4, 0
  br i1 %e4, label %istrue, label %isfalse
istrue:
  ret i1 1
isfalse:
  ret i1 0
}
)";
    return fnName;
}

void LlvmIrEmitter::emitParse(const IrParse& parse, FunctionContext& fctx)
{
    const std::string objectPtr = resolveStrPtr(parse.object, fctx);
    const std::string fnName = registerParseRuntime(parse.targetType);
    const std::string retType = parse.targetType == "bool" ? "i1" : "i32";
    const int destReg = defineRegister(parse.dest, fctx);
    *fctx.out << "  %" << destReg << " = call " << retType << " " << fnName << "(i8* " << objectPtr
              << ")\n";
}

std::string LlvmIrEmitter::registerUtf8CountRuntime()
{
    const std::string fnName = "@axea.utf8.count";
    if (utf8CountRegistered_)
    {
        return fnName;
    }
    utf8CountRegistered_ = true;

    // Standard "count non-continuation bytes" UTF-8 codepoint count (see
    // docs/language/0047-unicode.md): every byte whose top two bits
    // aren't `10` (i.e. `byte & 0xC0 != 0x80`) starts a new codepoint.
    // alloca/load/store for idx/count, no phi - `select` (not a branch)
    // to conditionally bump the counter, since both the "count" and
    // "count + 1" values are cheap to compute unconditionally and this
    // avoids yet another basic block for what's otherwise a tight,
    // single-block loop body.
    utf8CountRuntimeText_ << R"(
define i32 @axea.utf8.count(i8* %s) {
entry:
  %idxSlot = alloca i32
  %cntSlot = alloca i32
  store i32 0, i32* %idxSlot
  store i32 0, i32* %cntSlot
  br label %loophdr
loophdr:
  %idx = load i32, i32* %idxSlot
  %cp = getelementptr i8, i8* %s, i32 %idx
  %c = load i8, i8* %cp
  %atEnd = icmp eq i8 %c, 0
  br i1 %atEnd, label %done, label %body
body:
  %masked = and i8 %c, 192
  %isCont = icmp eq i8 %masked, 128
  %cnt0 = load i32, i32* %cntSlot
  %cntPlus1 = add i32 %cnt0, 1
  %newCnt = select i1 %isCont, i32 %cnt0, i32 %cntPlus1
  store i32 %newCnt, i32* %cntSlot
  %idx2 = load i32, i32* %idxSlot
  %idx3 = add i32 %idx2, 1
  store i32 %idx3, i32* %idxSlot
  br label %loophdr
done:
  %final = load i32, i32* %cntSlot
  ret i32 %final
}
)";
    return fnName;
}

void LlvmIrEmitter::emitBufferNew(const IrBufferNew& bufferNew, FunctionContext& fctx)
{
    const int sizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizePtrReg
              << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* null, i32 1\n";
    const int sizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizeIntReg << " = ptrtoint {i32, i32, i8*}* %" << sizePtrReg
              << " to i64\n";
    const int rawHeaderReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawHeaderReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";
    const int destReg = defineRegister(bufferNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawHeaderReg << " to {i32, i32, i8*}*\n";

    // Minimal 1-byte data allocation, matching the exact "fresh, empty"
    // state emitBufferFinish resets a finished buffer back to.
    const int dataReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataReg << " = call i8* @malloc(i64 1)\n";
    *fctx.out << "  store i8 0, i8* %" << dataReg << "\n";

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << ref(bufferNew.dest, fctx) << ", i32 0, i32 0\n";
    *fctx.out << "  store i32 0, i32* %" << lenPtrReg << "\n";
    const int capPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << capPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << ref(bufferNew.dest, fctx) << ", i32 0, i32 1\n";
    *fctx.out << "  store i32 1, i32* %" << capPtrReg << "\n";
    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << ref(bufferNew.dest, fctx) << ", i32 0, i32 2\n";
    *fctx.out << "  store i8* %" << dataReg << ", i8** %" << dataPtrPtrReg << "\n";
}

void LlvmIrEmitter::emitBufferAppend(const IrBufferAppend& bufferAppend, FunctionContext& fctx)
{
    const std::string bufferRef = ref(bufferAppend.buffer, fctx);
    const std::string textPtr = resolveStrPtr(bufferAppend.text, fctx);

    const int textLen64Reg = allocateRegister(fctx);
    *fctx.out << "  %" << textLen64Reg << " = call i64 @strlen(i8* " << textPtr << ")\n";
    const int textLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << textLenReg << " = trunc i64 %" << textLen64Reg << " to i32\n";

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 0\n";
    const int oldLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldLenReg << " = load i32, i32* %" << lenPtrReg << "\n";
    const int newLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << newLenReg << " = add i32 %" << oldLenReg << ", %" << textLenReg << "\n";
    const int neededReg = allocateRegister(fctx);
    *fctx.out << "  %" << neededReg << " = add i32 %" << newLenReg << ", 1\n";

    ensureBufferCapacity(bufferRef, "%" + std::to_string(neededReg), fctx);

    // Re-read the data pointer fresh - ensureBufferCapacity may have just
    // replaced it (the "reload from memory instead of phi" trick, same as
    // every other multi-predecessor merge in this backend).
    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 2\n";
    const int dataReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataReg << " = load i8*, i8** %" << dataPtrPtrReg << "\n";

    const int labelId = fctx.nextLabel++;
    const std::string headerLabel = "buffer.append.copy.header" + std::to_string(labelId);
    const std::string bodyLabel = "buffer.append.copy.body" + std::to_string(labelId);
    const std::string doneLabel = "buffer.append.copy.done" + std::to_string(labelId);

    const int counterSlotReg = allocateRegister(fctx);
    *fctx.out << "  %" << counterSlotReg << " = alloca i32\n";
    *fctx.out << "  store i32 0, i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << headerLabel << ":\n";
    fctx.currentLabel = headerLabel;
    const int iReg = allocateRegister(fctx);
    *fctx.out << "  %" << iReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int condReg = allocateRegister(fctx);
    *fctx.out << "  %" << condReg << " = icmp slt i32 %" << iReg << ", %" << textLenReg << "\n";
    *fctx.out << "  br i1 %" << condReg << ", label %" << bodyLabel << ", label %" << doneLabel
              << "\n";

    *fctx.out << bodyLabel << ":\n";
    fctx.currentLabel = bodyLabel;
    const int iForSrcReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForSrcReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int srcElemPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << srcElemPtrReg << " = getelementptr i8, i8* " << textPtr << ", i32 %"
              << iForSrcReg << "\n";
    const int byteValReg = allocateRegister(fctx);
    *fctx.out << "  %" << byteValReg << " = load i8, i8* %" << srcElemPtrReg << "\n";
    const int iForDstReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForDstReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int dstOffReg = allocateRegister(fctx);
    *fctx.out << "  %" << dstOffReg << " = add i32 %" << oldLenReg << ", %" << iForDstReg << "\n";
    const int dstElemPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dstElemPtrReg << " = getelementptr i8, i8* %" << dataReg << ", i32 %"
              << dstOffReg << "\n";
    *fctx.out << "  store i8 %" << byteValReg << ", i8* %" << dstElemPtrReg << "\n";
    const int iForIncReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForIncReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int iNextReg = allocateRegister(fctx);
    *fctx.out << "  %" << iNextReg << " = add i32 %" << iForIncReg << ", 1\n";
    *fctx.out << "  store i32 %" << iNextReg << ", i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << doneLabel << ":\n";
    fctx.currentLabel = doneLabel;

    const int nullPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << nullPtrReg << " = getelementptr i8, i8* %" << dataReg << ", i32 %"
              << newLenReg << "\n";
    *fctx.out << "  store i8 0, i8* %" << nullPtrReg << "\n";
    *fctx.out << "  store i32 %" << newLenReg << ", i32* %" << lenPtrReg << "\n";
}

void LlvmIrEmitter::emitBufferAppendLine(const IrBufferAppendLine& bufferAppendLine,
                                         FunctionContext& fctx)
{
    const std::string bufferRef = ref(bufferAppendLine.buffer, fctx);
    const std::string textPtr = resolveStrPtr(bufferAppendLine.text, fctx);

    const int textLen64Reg = allocateRegister(fctx);
    *fctx.out << "  %" << textLen64Reg << " = call i64 @strlen(i8* " << textPtr << ")\n";
    const int textLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << textLenReg << " = trunc i64 %" << textLen64Reg << " to i32\n";

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 0\n";
    const int oldLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldLenReg << " = load i32, i32* %" << lenPtrReg << "\n";
    // newLen accounts for the appended text plus one '\n' byte - the
    // trailing null terminator is not itself counted in length (matches
    // every other buffer/string here: length is content length, the null
    // terminator lives at data[length]).
    const int sumReg = allocateRegister(fctx);
    *fctx.out << "  %" << sumReg << " = add i32 %" << oldLenReg << ", %" << textLenReg << "\n";
    const int newLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << newLenReg << " = add i32 %" << sumReg << ", 1\n";
    const int neededReg = allocateRegister(fctx);
    *fctx.out << "  %" << neededReg << " = add i32 %" << newLenReg << ", 1\n";

    ensureBufferCapacity(bufferRef, "%" + std::to_string(neededReg), fctx);

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 2\n";
    const int dataReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataReg << " = load i8*, i8** %" << dataPtrPtrReg << "\n";

    const int labelId = fctx.nextLabel++;
    const std::string headerLabel = "buffer.appendline.copy.header" + std::to_string(labelId);
    const std::string bodyLabel = "buffer.appendline.copy.body" + std::to_string(labelId);
    const std::string doneLabel = "buffer.appendline.copy.done" + std::to_string(labelId);

    const int counterSlotReg = allocateRegister(fctx);
    *fctx.out << "  %" << counterSlotReg << " = alloca i32\n";
    *fctx.out << "  store i32 0, i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << headerLabel << ":\n";
    fctx.currentLabel = headerLabel;
    const int iReg = allocateRegister(fctx);
    *fctx.out << "  %" << iReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int condReg = allocateRegister(fctx);
    *fctx.out << "  %" << condReg << " = icmp slt i32 %" << iReg << ", %" << textLenReg << "\n";
    *fctx.out << "  br i1 %" << condReg << ", label %" << bodyLabel << ", label %" << doneLabel
              << "\n";

    *fctx.out << bodyLabel << ":\n";
    fctx.currentLabel = bodyLabel;
    const int iForSrcReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForSrcReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int srcElemPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << srcElemPtrReg << " = getelementptr i8, i8* " << textPtr << ", i32 %"
              << iForSrcReg << "\n";
    const int byteValReg = allocateRegister(fctx);
    *fctx.out << "  %" << byteValReg << " = load i8, i8* %" << srcElemPtrReg << "\n";
    const int iForDstReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForDstReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int dstOffReg = allocateRegister(fctx);
    *fctx.out << "  %" << dstOffReg << " = add i32 %" << oldLenReg << ", %" << iForDstReg << "\n";
    const int dstElemPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dstElemPtrReg << " = getelementptr i8, i8* %" << dataReg << ", i32 %"
              << dstOffReg << "\n";
    *fctx.out << "  store i8 %" << byteValReg << ", i8* %" << dstElemPtrReg << "\n";
    const int iForIncReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForIncReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int iNextReg = allocateRegister(fctx);
    *fctx.out << "  %" << iNextReg << " = add i32 %" << iForIncReg << ", 1\n";
    *fctx.out << "  store i32 %" << iNextReg << ", i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << doneLabel << ":\n";
    fctx.currentLabel = doneLabel;

    const int newlinePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << newlinePtrReg << " = getelementptr i8, i8* %" << dataReg << ", i32 %"
              << sumReg << "\n";
    *fctx.out << "  store i8 10, i8* %" << newlinePtrReg << "\n";
    const int nullPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << nullPtrReg << " = getelementptr i8, i8* %" << dataReg << ", i32 %"
              << newLenReg << "\n";
    *fctx.out << "  store i8 0, i8* %" << nullPtrReg << "\n";
    *fctx.out << "  store i32 %" << newLenReg << ", i32* %" << lenPtrReg << "\n";
}

void LlvmIrEmitter::emitBufferClear(const IrBufferClear& bufferClear, FunctionContext& fctx)
{
    const std::string bufferRef = ref(bufferClear.buffer, fctx);

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 0\n";
    *fctx.out << "  store i32 0, i32* %" << lenPtrReg << "\n";

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 2\n";
    const int dataReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataReg << " = load i8*, i8** %" << dataPtrPtrReg << "\n";
    *fctx.out << "  store i8 0, i8* %" << dataReg << "\n";
}

void LlvmIrEmitter::emitBufferReserve(const IrBufferReserve& bufferReserve, FunctionContext& fctx)
{
    const std::string bufferRef = ref(bufferReserve.buffer, fctx);
    const std::string neededRef = ref(bufferReserve.capacity, fctx);
    ensureBufferCapacity(bufferRef, neededRef, fctx);
}

void LlvmIrEmitter::emitBufferFinish(const IrBufferFinish& bufferFinish, FunctionContext& fctx)
{
    const std::string bufferRef = ref(bufferFinish.buffer, fctx);

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 0\n";
    const int oldLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldLenReg << " = load i32, i32* %" << lenPtrReg << "\n";
    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 2\n";
    const int oldDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldDataReg << " = load i8*, i8** %" << dataPtrPtrReg << "\n";

    // Fresh String header, stealing the buffer's own length/data field
    // *values* directly - no byte copy at all.
    const int sizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizePtrReg << " = getelementptr {i32, i8*}, {i32, i8*}* null, i32 1\n";
    const int sizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizeIntReg << " = ptrtoint {i32, i8*}* %" << sizePtrReg << " to i64\n";
    const int rawHeaderReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawHeaderReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";
    const int destReg = defineRegister(bufferFinish.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawHeaderReg << " to {i32, i8*}*\n";

    const int newLenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << newLenPtrReg << " = getelementptr {i32, i8*}, {i32, i8*}* "
              << ref(bufferFinish.dest, fctx) << ", i32 0, i32 0\n";
    *fctx.out << "  store i32 %" << oldLenReg << ", i32* %" << newLenPtrReg << "\n";
    const int newDataFieldPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << newDataFieldPtrReg << " = getelementptr {i32, i8*}, {i32, i8*}* "
              << ref(bufferFinish.dest, fctx) << ", i32 0, i32 1\n";
    *fctx.out << "  store i8* %" << oldDataReg << ", i8** %" << newDataFieldPtrReg << "\n";

    // Reset the original buffer back to the exact same fresh, minimal
    // state emitBufferNew produces - never left null/dangling, safely
    // reusable (see docs/language/0043-buffer.md).
    *fctx.out << "  store i32 0, i32* %" << lenPtrReg << "\n";
    const int capPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << capPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 1\n";
    *fctx.out << "  store i32 1, i32* %" << capPtrReg << "\n";
    const int freshDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << freshDataReg << " = call i8* @malloc(i64 1)\n";
    *fctx.out << "  store i8 0, i8* %" << freshDataReg << "\n";
    *fctx.out << "  store i8* %" << freshDataReg << ", i8** %" << dataPtrPtrReg << "\n";
}

void LlvmIrEmitter::emitLinkedListNew(const IrLinkedListNew& linkedListNew, FunctionContext& fctx)
{
    // Already registered (see registerLinkedListInstantiation) by
    // inferTypesInList's own earlier pass over this same instruction, so
    // this just looks the instantiation's header type back up - mirrors
    // emitMapNew's identical reasoning.
    const std::string pointerType = llvmType("LinkedList<" + linkedListNew.elementTypeName + ">");
    const std::string headerType = pointerType.substr(0, pointerType.size() - 1);
    const std::string nodePtrType = "%axea.LLNode." + linkedListInstantiationId(pointerType) + "*";

    // sizeof({i32, Node*, Node*}) via the standard null-pointer GEP idiom -
    // same idiom as emitListNew/emitMapNew.
    const int sizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizePtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " null, i32 1\n";
    const int sizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizeIntReg << " = ptrtoint " << pointerType << " %" << sizePtrReg
              << " to i64\n";

    const int rawPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawPtrReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";

    const int destReg = defineRegister(linkedListNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawPtrReg << " to " << pointerType
              << "\n";

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " " << ref(linkedListNew.dest, fctx) << ", i32 0, i32 0\n";
    *fctx.out << "  store i32 0, i32* %" << lenPtrReg << "\n";

    const int headPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << headPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " " << ref(linkedListNew.dest, fctx) << ", i32 0, i32 1\n";
    *fctx.out << "  store " << nodePtrType << " null, " << nodePtrType << "* %" << headPtrReg
              << "\n";

    const int tailPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << tailPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " " << ref(linkedListNew.dest, fctx) << ", i32 0, i32 2\n";
    *fctx.out << "  store " << nodePtrType << " null, " << nodePtrType << "* %" << tailPtrReg
              << "\n";
}

void LlvmIrEmitter::emitLinkedListPushFront(const IrLinkedListPushFront& pushFront,
                                            FunctionContext& fctx)
{
    const std::string listType = typeOf(pushFront.list, fctx);
    const std::string id = linkedListInstantiationId(listType);
    const std::string valueType = typeOf(pushFront.value, fctx);
    // Unit-typed (see docs/language/0033-lists.md's identical reasoning for
    // List<T>.push) - no defineRegister call.
    *fctx.out << "  call void @axea.linkedlist." << id << ".push_front(" << listType << " "
              << ref(pushFront.list, fctx) << ", " << valueType << " " << ref(pushFront.value, fctx)
              << ")\n";
}

void LlvmIrEmitter::emitLinkedListPushBack(const IrLinkedListPushBack& pushBack,
                                           FunctionContext& fctx)
{
    const std::string listType = typeOf(pushBack.list, fctx);
    const std::string id = linkedListInstantiationId(listType);
    const std::string valueType = typeOf(pushBack.value, fctx);
    *fctx.out << "  call void @axea.linkedlist." << id << ".push_back(" << listType << " "
              << ref(pushBack.list, fctx) << ", " << valueType << " " << ref(pushBack.value, fctx)
              << ")\n";
}

void LlvmIrEmitter::emitLinkedListPopFront(const IrLinkedListPopFront& popFront,
                                           FunctionContext& fctx)
{
    const std::string listType = typeOf(popFront.list, fctx);
    const std::string id = linkedListInstantiationId(listType);
    const std::string valueType = linkedListElementLlvmType(listType);
    const int destReg = defineRegister(popFront.dest, fctx);
    *fctx.out << "  %" << destReg << " = call " << valueType << " @axea.linkedlist." << id
              << ".pop_front(" << listType << " " << ref(popFront.list, fctx) << ")\n";
}

void LlvmIrEmitter::emitLinkedListPopBack(const IrLinkedListPopBack& popBack, FunctionContext& fctx)
{
    const std::string listType = typeOf(popBack.list, fctx);
    const std::string id = linkedListInstantiationId(listType);
    const std::string valueType = linkedListElementLlvmType(listType);
    const int destReg = defineRegister(popBack.dest, fctx);
    *fctx.out << "  %" << destReg << " = call " << valueType << " @axea.linkedlist." << id
              << ".pop_back(" << listType << " " << ref(popBack.list, fctx) << ")\n";
}

void LlvmIrEmitter::emitDequeNew(const IrDequeNew& dequeNew, FunctionContext& fctx)
{
    // Already registered (resolving llvmType is enough - unlike LinkedList/
    // Map/Set, Deque<T> has no registerXInstantiation call at all, see
    // llvmType's own Deque<T> branch) by inferTypesInList's earlier pass
    // over this same instruction, so this just looks the header type back
    // up - mirrors emitMapNew/emitLinkedListNew's identical reasoning.
    const std::string pointerType = llvmType("Deque<" + dequeNew.elementTypeName + ">");
    const std::string headerType = pointerType.substr(0, pointerType.size() - 1);
    const std::string elementType = dequeElementType(pointerType);

    // sizeof({i32, i32, T*}) via the standard null-pointer GEP idiom - same
    // idiom as emitListNew/emitLinkedListNew.
    const int sizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizePtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " null, i32 1\n";
    const int sizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizeIntReg << " = ptrtoint " << pointerType << " %" << sizePtrReg
              << " to i64\n";
    const int rawPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawPtrReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";
    const int destReg = defineRegister(dequeNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawPtrReg << " to " << pointerType
              << "\n";

    const int countPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << countPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " " << ref(dequeNew.dest, fctx) << ", i32 0, i32 0\n";
    *fctx.out << "  store i32 0, i32* %" << countPtrReg << "\n";

    const int startPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << startPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " " << ref(dequeNew.dest, fctx) << ", i32 0, i32 1\n";
    *fctx.out << "  store i32 0, i32* %" << startPtrReg << "\n";

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " " << ref(dequeNew.dest, fctx) << ", i32 0, i32 2\n";
    *fctx.out << "  store " << elementType << "* null, " << elementType << "** %" << dataPtrPtrReg
              << "\n";
}

// Shared by emitDequePushFront/emitDequePushBack below: mallocs a fresh
// buffer sized to `oldCount + 1` and copies the existing `oldCount` elements
// out, offset by the *old* start (reading data[start+i], not data[i] - see
// docs/language/0037-deques.md), writing each copied element to
// `newData[i + destOffset]` (destOffset is 0 for push_back, 1 for
// push_front, so push_front's copy lands one slot further in, leaving room
// for the new element at index 0). Returns the new data register; the
// caller still needs to write the pushed value itself and store the new
// count/start(=0)/data back into the header. Mirrors emitListPush's own
// malloc + hand-rolled copy loop (alloca/load/store counter, no phi - same
// unnamed-sequential-register reason) almost exactly, generalized by one
// destination offset parameter.
int LlvmIrEmitter::emitDequeCopyForPush(const std::string& elementType,
                                        const std::string& oldDataRef,
                                        int oldCountReg,
                                        int oldStartReg,
                                        int newDataReg,
                                        int destOffset,
                                        FunctionContext& fctx)
{
    const int labelId = fctx.nextLabel++;
    const std::string headerLabel = "deque.push.copy.header" + std::to_string(labelId);
    const std::string bodyLabel = "deque.push.copy.body" + std::to_string(labelId);
    const std::string doneLabel = "deque.push.copy.done" + std::to_string(labelId);

    const int counterSlotReg = allocateRegister(fctx);
    *fctx.out << "  %" << counterSlotReg << " = alloca i32\n";
    *fctx.out << "  store i32 0, i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << headerLabel << ":\n";
    fctx.currentLabel = headerLabel;
    const int iReg = allocateRegister(fctx);
    *fctx.out << "  %" << iReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int condReg = allocateRegister(fctx);
    *fctx.out << "  %" << condReg << " = icmp slt i32 %" << iReg << ", %" << oldCountReg << "\n";
    *fctx.out << "  br i1 %" << condReg << ", label %" << bodyLabel << ", label %" << doneLabel
              << "\n";

    *fctx.out << bodyLabel << ":\n";
    fctx.currentLabel = bodyLabel;
    const int iForSrcReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForSrcReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int srcIdxReg = allocateRegister(fctx);
    *fctx.out << "  %" << srcIdxReg << " = add i32 %" << oldStartReg << ", %" << iForSrcReg << "\n";
    const int srcPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << srcPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* " << oldDataRef << ", i32 %" << srcIdxReg << "\n";
    const int valReg = allocateRegister(fctx);
    *fctx.out << "  %" << valReg << " = load " << elementType << ", " << elementType << "* %"
              << srcPtrReg << "\n";
    const int iForDstReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForDstReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int dstIdxReg = allocateRegister(fctx);
    *fctx.out << "  %" << dstIdxReg << " = add i32 %" << iForDstReg << ", " << destOffset << "\n";
    const int dstPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dstPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << newDataReg << ", i32 %" << dstIdxReg << "\n";
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
    return newDataReg;
}

void LlvmIrEmitter::emitDequePushFront(const IrDequePushFront& pushFront, FunctionContext& fctx)
{
    const std::string objectType = typeOf(pushFront.deque, fctx);
    const std::string headerType = objectType.substr(0, objectType.size() - 1);
    const std::string elementType = dequeElementType(objectType);
    const std::string dequeRef = ref(pushFront.deque, fctx);

    const int countPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << countPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << dequeRef << ", i32 0, i32 0\n";
    const int oldCountReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldCountReg << " = load i32, i32* %" << countPtrReg << "\n";
    const int startPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << startPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << dequeRef << ", i32 0, i32 1\n";
    const int oldStartReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldStartReg << " = load i32, i32* %" << startPtrReg << "\n";
    const int newCountReg = allocateRegister(fctx);
    *fctx.out << "  %" << newCountReg << " = add i32 %" << oldCountReg << ", 1\n";

    const int elemSizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << elemSizePtrReg << " = getelementptr " << elementType << ", "
              << elementType << "* null, i32 1\n";
    const int elemSizeReg = allocateRegister(fctx);
    *fctx.out << "  %" << elemSizeReg << " = ptrtoint " << elementType << "* %" << elemSizePtrReg
              << " to i64\n";
    const int newCount64Reg = allocateRegister(fctx);
    *fctx.out << "  %" << newCount64Reg << " = zext i32 %" << newCountReg << " to i64\n";
    const int newBytesReg = allocateRegister(fctx);
    *fctx.out << "  %" << newBytesReg << " = mul i64 %" << newCount64Reg << ", %" << elemSizeReg
              << "\n";
    const int rawNewDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawNewDataReg << " = call i8* @malloc(i64 %" << newBytesReg << ")\n";
    const int newDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << newDataReg << " = bitcast i8* %" << rawNewDataReg << " to " << elementType
              << "*\n";

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << dequeRef << ", i32 0, i32 2\n";
    const int oldDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldDataReg << " = load " << elementType << "*, " << elementType << "** %"
              << dataPtrPtrReg << "\n";

    // Copy the existing elements one slot further in (destOffset 1), leaving
    // index 0 free for the new front element.
    emitDequeCopyForPush(elementType,
                         "%" + std::to_string(oldDataReg),
                         oldCountReg,
                         oldStartReg,
                         newDataReg,
                         1,
                         fctx);

    const int newElementPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << newElementPtrReg << " = getelementptr " << elementType << ", "
              << elementType << "* %" << newDataReg << ", i32 0\n";
    *fctx.out << "  store " << elementType << " " << ref(pushFront.value, fctx) << ", "
              << elementType << "* %" << newElementPtrReg << "\n";
    *fctx.out << "  store i32 %" << newCountReg << ", i32* %" << countPtrReg << "\n";
    *fctx.out << "  store i32 0, i32* %" << startPtrReg << "\n";
    *fctx.out << "  store " << elementType << "* %" << newDataReg << ", " << elementType << "** %"
              << dataPtrPtrReg << "\n";
}

void LlvmIrEmitter::emitDequePushBack(const IrDequePushBack& pushBack, FunctionContext& fctx)
{
    const std::string objectType = typeOf(pushBack.deque, fctx);
    const std::string headerType = objectType.substr(0, objectType.size() - 1);
    const std::string elementType = dequeElementType(objectType);
    const std::string dequeRef = ref(pushBack.deque, fctx);

    const int countPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << countPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << dequeRef << ", i32 0, i32 0\n";
    const int oldCountReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldCountReg << " = load i32, i32* %" << countPtrReg << "\n";
    const int startPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << startPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << dequeRef << ", i32 0, i32 1\n";
    const int oldStartReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldStartReg << " = load i32, i32* %" << startPtrReg << "\n";
    const int newCountReg = allocateRegister(fctx);
    *fctx.out << "  %" << newCountReg << " = add i32 %" << oldCountReg << ", 1\n";

    const int elemSizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << elemSizePtrReg << " = getelementptr " << elementType << ", "
              << elementType << "* null, i32 1\n";
    const int elemSizeReg = allocateRegister(fctx);
    *fctx.out << "  %" << elemSizeReg << " = ptrtoint " << elementType << "* %" << elemSizePtrReg
              << " to i64\n";
    const int newCount64Reg = allocateRegister(fctx);
    *fctx.out << "  %" << newCount64Reg << " = zext i32 %" << newCountReg << " to i64\n";
    const int newBytesReg = allocateRegister(fctx);
    *fctx.out << "  %" << newBytesReg << " = mul i64 %" << newCount64Reg << ", %" << elemSizeReg
              << "\n";
    const int rawNewDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawNewDataReg << " = call i8* @malloc(i64 %" << newBytesReg << ")\n";
    const int newDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << newDataReg << " = bitcast i8* %" << rawNewDataReg << " to " << elementType
              << "*\n";

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << dequeRef << ", i32 0, i32 2\n";
    const int oldDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldDataReg << " = load " << elementType << "*, " << elementType << "** %"
              << dataPtrPtrReg << "\n";

    // Copy the existing elements straight across (destOffset 0); the new
    // back element goes at index oldCount, right after them.
    emitDequeCopyForPush(elementType,
                         "%" + std::to_string(oldDataReg),
                         oldCountReg,
                         oldStartReg,
                         newDataReg,
                         0,
                         fctx);

    const int newElementPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << newElementPtrReg << " = getelementptr " << elementType << ", "
              << elementType << "* %" << newDataReg << ", i32 %" << oldCountReg << "\n";
    *fctx.out << "  store " << elementType << " " << ref(pushBack.value, fctx) << ", "
              << elementType << "* %" << newElementPtrReg << "\n";
    *fctx.out << "  store i32 %" << newCountReg << ", i32* %" << countPtrReg << "\n";
    *fctx.out << "  store i32 0, i32* %" << startPtrReg << "\n";
    *fctx.out << "  store " << elementType << "* %" << newDataReg << ", " << elementType << "** %"
              << dataPtrPtrReg << "\n";
}

void LlvmIrEmitter::emitDequePopFront(const IrDequePopFront& popFront, FunctionContext& fctx)
{
    const std::string objectType = typeOf(popFront.deque, fctx);
    const std::string headerType = objectType.substr(0, objectType.size() - 1);
    const std::string elementType = dequeElementType(objectType);
    const std::string dequeRef = ref(popFront.deque, fctx);

    const int startPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << startPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << dequeRef << ", i32 0, i32 1\n";
    const int startReg = allocateRegister(fctx);
    *fctx.out << "  %" << startReg << " = load i32, i32* %" << startPtrReg << "\n";
    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << dequeRef << ", i32 0, i32 2\n";
    const int dataReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataReg << " = load " << elementType << "*, " << elementType << "** %"
              << dataPtrPtrReg << "\n";
    const int elementPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << elementPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << dataReg << ", i32 %" << startReg << "\n";
    const int destReg = defineRegister(popFront.dest, fctx);
    *fctx.out << "  %" << destReg << " = load " << elementType << ", " << elementType << "* %"
              << elementPtrReg << "\n";

    const int newStartReg = allocateRegister(fctx);
    *fctx.out << "  %" << newStartReg << " = add i32 %" << startReg << ", 1\n";
    *fctx.out << "  store i32 %" << newStartReg << ", i32* %" << startPtrReg << "\n";
    const int countPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << countPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << dequeRef << ", i32 0, i32 0\n";
    const int oldCountReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldCountReg << " = load i32, i32* %" << countPtrReg << "\n";
    const int newCountReg = allocateRegister(fctx);
    *fctx.out << "  %" << newCountReg << " = sub i32 %" << oldCountReg << ", 1\n";
    *fctx.out << "  store i32 %" << newCountReg << ", i32* %" << countPtrReg << "\n";
}

void LlvmIrEmitter::emitDequePopBack(const IrDequePopBack& popBack, FunctionContext& fctx)
{
    const std::string objectType = typeOf(popBack.deque, fctx);
    const std::string headerType = objectType.substr(0, objectType.size() - 1);
    const std::string elementType = dequeElementType(objectType);
    const std::string dequeRef = ref(popBack.deque, fctx);

    const int countPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << countPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << dequeRef << ", i32 0, i32 0\n";
    const int oldCountReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldCountReg << " = load i32, i32* %" << countPtrReg << "\n";
    const int startPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << startPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << dequeRef << ", i32 0, i32 1\n";
    const int startReg = allocateRegister(fctx);
    *fctx.out << "  %" << startReg << " = load i32, i32* %" << startPtrReg << "\n";
    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << dequeRef << ", i32 0, i32 2\n";
    const int dataReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataReg << " = load " << elementType << "*, " << elementType << "** %"
              << dataPtrPtrReg << "\n";

    const int lastRelReg = allocateRegister(fctx);
    *fctx.out << "  %" << lastRelReg << " = sub i32 %" << oldCountReg << ", 1\n";
    const int lastIdxReg = allocateRegister(fctx);
    *fctx.out << "  %" << lastIdxReg << " = add i32 %" << startReg << ", %" << lastRelReg << "\n";
    const int elementPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << elementPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << dataReg << ", i32 %" << lastIdxReg << "\n";
    const int destReg = defineRegister(popBack.dest, fctx);
    *fctx.out << "  %" << destReg << " = load " << elementType << ", " << elementType << "* %"
              << elementPtrReg << "\n";

    const int newCountReg = allocateRegister(fctx);
    *fctx.out << "  %" << newCountReg << " = sub i32 %" << oldCountReg << ", 1\n";
    *fctx.out << "  store i32 %" << newCountReg << ", i32* %" << countPtrReg << "\n";
}

void LlvmIrEmitter::emitQueueNew(const IrQueueNew& queueNew, FunctionContext& fctx)
{
    // Structurally identical to emitDequeNew - see its own doc comment
    // (see docs/language/0038-queues.md).
    const std::string pointerType = llvmType("Queue<" + queueNew.elementTypeName + ">");
    const std::string headerType = pointerType.substr(0, pointerType.size() - 1);
    const std::string elementType = dequeElementType(pointerType);

    const int sizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizePtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " null, i32 1\n";
    const int sizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizeIntReg << " = ptrtoint " << pointerType << " %" << sizePtrReg
              << " to i64\n";
    const int rawPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawPtrReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";
    const int destReg = defineRegister(queueNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawPtrReg << " to " << pointerType
              << "\n";

    const int countPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << countPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " " << ref(queueNew.dest, fctx) << ", i32 0, i32 0\n";
    *fctx.out << "  store i32 0, i32* %" << countPtrReg << "\n";

    const int startPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << startPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " " << ref(queueNew.dest, fctx) << ", i32 0, i32 1\n";
    *fctx.out << "  store i32 0, i32* %" << startPtrReg << "\n";

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << pointerType
              << " " << ref(queueNew.dest, fctx) << ", i32 0, i32 2\n";
    *fctx.out << "  store " << elementType << "* null, " << elementType << "** %" << dataPtrPtrReg
              << "\n";
}

void LlvmIrEmitter::emitQueueEnqueue(const IrQueueEnqueue& enqueue, FunctionContext& fctx)
{
    // Structurally identical to emitDequePushBack (enqueue maps onto
    // push_back - add at the back) - see its own doc comment (see
    // docs/language/0038-queues.md).
    const std::string objectType = typeOf(enqueue.queue, fctx);
    const std::string headerType = objectType.substr(0, objectType.size() - 1);
    const std::string elementType = dequeElementType(objectType);
    const std::string queueRef = ref(enqueue.queue, fctx);

    const int countPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << countPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << queueRef << ", i32 0, i32 0\n";
    const int oldCountReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldCountReg << " = load i32, i32* %" << countPtrReg << "\n";
    const int startPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << startPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << queueRef << ", i32 0, i32 1\n";
    const int oldStartReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldStartReg << " = load i32, i32* %" << startPtrReg << "\n";
    const int newCountReg = allocateRegister(fctx);
    *fctx.out << "  %" << newCountReg << " = add i32 %" << oldCountReg << ", 1\n";

    const int elemSizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << elemSizePtrReg << " = getelementptr " << elementType << ", "
              << elementType << "* null, i32 1\n";
    const int elemSizeReg = allocateRegister(fctx);
    *fctx.out << "  %" << elemSizeReg << " = ptrtoint " << elementType << "* %" << elemSizePtrReg
              << " to i64\n";
    const int newCount64Reg = allocateRegister(fctx);
    *fctx.out << "  %" << newCount64Reg << " = zext i32 %" << newCountReg << " to i64\n";
    const int newBytesReg = allocateRegister(fctx);
    *fctx.out << "  %" << newBytesReg << " = mul i64 %" << newCount64Reg << ", %" << elemSizeReg
              << "\n";
    const int rawNewDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawNewDataReg << " = call i8* @malloc(i64 %" << newBytesReg << ")\n";
    const int newDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << newDataReg << " = bitcast i8* %" << rawNewDataReg << " to " << elementType
              << "*\n";

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << queueRef << ", i32 0, i32 2\n";
    const int oldDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldDataReg << " = load " << elementType << "*, " << elementType << "** %"
              << dataPtrPtrReg << "\n";

    emitDequeCopyForPush(elementType,
                         "%" + std::to_string(oldDataReg),
                         oldCountReg,
                         oldStartReg,
                         newDataReg,
                         0,
                         fctx);

    const int newElementPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << newElementPtrReg << " = getelementptr " << elementType << ", "
              << elementType << "* %" << newDataReg << ", i32 %" << oldCountReg << "\n";
    *fctx.out << "  store " << elementType << " " << ref(enqueue.value, fctx) << ", " << elementType
              << "* %" << newElementPtrReg << "\n";
    *fctx.out << "  store i32 %" << newCountReg << ", i32* %" << countPtrReg << "\n";
    *fctx.out << "  store i32 0, i32* %" << startPtrReg << "\n";
    *fctx.out << "  store " << elementType << "* %" << newDataReg << ", " << elementType << "** %"
              << dataPtrPtrReg << "\n";
}

void LlvmIrEmitter::emitQueueDequeue(const IrQueueDequeue& dequeue, FunctionContext& fctx)
{
    // Structurally identical to emitDequePopFront (dequeue maps onto
    // pop_front - remove from the front, classic FIFO) - see its own doc
    // comment (see docs/language/0038-queues.md).
    const std::string objectType = typeOf(dequeue.queue, fctx);
    const std::string headerType = objectType.substr(0, objectType.size() - 1);
    const std::string elementType = dequeElementType(objectType);
    const std::string queueRef = ref(dequeue.queue, fctx);

    const int startPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << startPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << queueRef << ", i32 0, i32 1\n";
    const int startReg = allocateRegister(fctx);
    *fctx.out << "  %" << startReg << " = load i32, i32* %" << startPtrReg << "\n";
    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << queueRef << ", i32 0, i32 2\n";
    const int dataReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataReg << " = load " << elementType << "*, " << elementType << "** %"
              << dataPtrPtrReg << "\n";
    const int elementPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << elementPtrReg << " = getelementptr " << elementType << ", " << elementType
              << "* %" << dataReg << ", i32 %" << startReg << "\n";
    const int destReg = defineRegister(dequeue.dest, fctx);
    *fctx.out << "  %" << destReg << " = load " << elementType << ", " << elementType << "* %"
              << elementPtrReg << "\n";

    const int newStartReg = allocateRegister(fctx);
    *fctx.out << "  %" << newStartReg << " = add i32 %" << startReg << ", 1\n";
    *fctx.out << "  store i32 %" << newStartReg << ", i32* %" << startPtrReg << "\n";
    const int countPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << countPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << queueRef << ", i32 0, i32 0\n";
    const int oldCountReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldCountReg << " = load i32, i32* %" << countPtrReg << "\n";
    const int newCountReg = allocateRegister(fctx);
    *fctx.out << "  %" << newCountReg << " = sub i32 %" << oldCountReg << ", 1\n";
    *fctx.out << "  store i32 %" << newCountReg << ", i32* %" << countPtrReg << "\n";
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
        if (const auto* constChar = dynamic_cast<const IrConstChar*>(inst.get()))
        {
            const int destReg = defineRegister(constChar->dest, fctx);
            *fctx.out << "  %" << destReg << " = add i24 0, " << constChar->codepoint << "\n";
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
                // String implicitly lends a str at a call boundary (see
                // docs/language/0042-string.md and
                // TypeChecker::isStrCoercible's identical rule) - mirrors
                // needsSliceConversion's own shape for a different pair of
                // types.
                const bool needsStringToStrConversion =
                    paramTypesIt != functionParamTypes_.end() && i < paramTypesIt->second.size() &&
                    paramTypesIt->second[i] == "str" && isStringType(argType);
                if (needsStringToStrConversion)
                {
                    args.emplace_back("i8*", resolveStrPtr(call->args[i], fctx));
                    continue;
                }
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
        if (const auto* strSlice = dynamic_cast<const IrStrSlice*>(inst.get()))
        {
            emitStrSlice(*strSlice, fctx);
            continue;
        }
        if (const auto* parse = dynamic_cast<const IrParse*>(inst.get()))
        {
            emitParse(*parse, fctx);
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
        if (const auto* priorityQueueNew = dynamic_cast<const IrPriorityQueueNew*>(inst.get()))
        {
            emitPriorityQueueNew(*priorityQueueNew, fctx);
            continue;
        }
        if (const auto* priorityQueuePush = dynamic_cast<const IrPriorityQueuePush*>(inst.get()))
        {
            emitPriorityQueuePush(*priorityQueuePush, fctx);
            continue;
        }
        if (const auto* priorityQueuePop = dynamic_cast<const IrPriorityQueuePop*>(inst.get()))
        {
            emitPriorityQueuePop(*priorityQueuePop, fctx);
            continue;
        }
        if (const auto* priorityQueuePeek = dynamic_cast<const IrPriorityQueuePeek*>(inst.get()))
        {
            emitPriorityQueuePeek(*priorityQueuePeek, fctx);
            continue;
        }
        if (const auto* linkedListNew = dynamic_cast<const IrLinkedListNew*>(inst.get()))
        {
            emitLinkedListNew(*linkedListNew, fctx);
            continue;
        }
        if (const auto* pushFront = dynamic_cast<const IrLinkedListPushFront*>(inst.get()))
        {
            emitLinkedListPushFront(*pushFront, fctx);
            continue;
        }
        if (const auto* pushBack = dynamic_cast<const IrLinkedListPushBack*>(inst.get()))
        {
            emitLinkedListPushBack(*pushBack, fctx);
            continue;
        }
        if (const auto* popFront = dynamic_cast<const IrLinkedListPopFront*>(inst.get()))
        {
            emitLinkedListPopFront(*popFront, fctx);
            continue;
        }
        if (const auto* popBack = dynamic_cast<const IrLinkedListPopBack*>(inst.get()))
        {
            emitLinkedListPopBack(*popBack, fctx);
            continue;
        }
        if (const auto* dequeNew = dynamic_cast<const IrDequeNew*>(inst.get()))
        {
            emitDequeNew(*dequeNew, fctx);
            continue;
        }
        if (const auto* dequePushFront = dynamic_cast<const IrDequePushFront*>(inst.get()))
        {
            emitDequePushFront(*dequePushFront, fctx);
            continue;
        }
        if (const auto* dequePushBack = dynamic_cast<const IrDequePushBack*>(inst.get()))
        {
            emitDequePushBack(*dequePushBack, fctx);
            continue;
        }
        if (const auto* dequePopFront = dynamic_cast<const IrDequePopFront*>(inst.get()))
        {
            emitDequePopFront(*dequePopFront, fctx);
            continue;
        }
        if (const auto* dequePopBack = dynamic_cast<const IrDequePopBack*>(inst.get()))
        {
            emitDequePopBack(*dequePopBack, fctx);
            continue;
        }
        if (const auto* queueNew = dynamic_cast<const IrQueueNew*>(inst.get()))
        {
            emitQueueNew(*queueNew, fctx);
            continue;
        }
        if (const auto* queueEnqueue = dynamic_cast<const IrQueueEnqueue*>(inst.get()))
        {
            emitQueueEnqueue(*queueEnqueue, fctx);
            continue;
        }
        if (const auto* queueDequeue = dynamic_cast<const IrQueueDequeue*>(inst.get()))
        {
            emitQueueDequeue(*queueDequeue, fctx);
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
        if (const auto* sortedMapNew = dynamic_cast<const IrSortedMapNew*>(inst.get()))
        {
            emitSortedMapNew(*sortedMapNew, fctx);
            continue;
        }
        if (const auto* sortedMapSet = dynamic_cast<const IrSortedMapSet*>(inst.get()))
        {
            emitSortedMapSet(*sortedMapSet, fctx);
            continue;
        }
        if (const auto* sortedMapGet = dynamic_cast<const IrSortedMapGet*>(inst.get()))
        {
            emitSortedMapGet(*sortedMapGet, fctx);
            continue;
        }
        if (const auto* sortedMapContains = dynamic_cast<const IrSortedMapContains*>(inst.get()))
        {
            emitSortedMapContains(*sortedMapContains, fctx);
            continue;
        }
        if (const auto* sortedMapRemove = dynamic_cast<const IrSortedMapRemove*>(inst.get()))
        {
            emitSortedMapRemove(*sortedMapRemove, fctx);
            continue;
        }
        if (const auto* sortedSetNew = dynamic_cast<const IrSortedSetNew*>(inst.get()))
        {
            emitSortedSetNew(*sortedSetNew, fctx);
            continue;
        }
        if (const auto* sortedSetAdd = dynamic_cast<const IrSortedSetAdd*>(inst.get()))
        {
            emitSortedSetAdd(*sortedSetAdd, fctx);
            continue;
        }
        if (const auto* sortedSetContains = dynamic_cast<const IrSortedSetContains*>(inst.get()))
        {
            emitSortedSetContains(*sortedSetContains, fctx);
            continue;
        }
        if (const auto* sortedSetRemove = dynamic_cast<const IrSortedSetRemove*>(inst.get()))
        {
            emitSortedSetRemove(*sortedSetRemove, fctx);
            continue;
        }
        if (const auto* stringNew = dynamic_cast<const IrStringNew*>(inst.get()))
        {
            emitStringNew(*stringNew, fctx);
            continue;
        }
        if (const auto* stringAppend = dynamic_cast<const IrStringAppend*>(inst.get()))
        {
            emitStringAppend(*stringAppend, fctx);
            continue;
        }
        if (const auto* bufferNew = dynamic_cast<const IrBufferNew*>(inst.get()))
        {
            emitBufferNew(*bufferNew, fctx);
            continue;
        }
        if (const auto* bufferAppend = dynamic_cast<const IrBufferAppend*>(inst.get()))
        {
            emitBufferAppend(*bufferAppend, fctx);
            continue;
        }
        if (const auto* bufferAppendLine = dynamic_cast<const IrBufferAppendLine*>(inst.get()))
        {
            emitBufferAppendLine(*bufferAppendLine, fctx);
            continue;
        }
        if (const auto* bufferClear = dynamic_cast<const IrBufferClear*>(inst.get()))
        {
            emitBufferClear(*bufferClear, fctx);
            continue;
        }
        if (const auto* bufferReserve = dynamic_cast<const IrBufferReserve*>(inst.get()))
        {
            emitBufferReserve(*bufferReserve, fctx);
            continue;
        }
        if (const auto* bufferFinish = dynamic_cast<const IrBufferFinish*>(inst.get()))
        {
            emitBufferFinish(*bufferFinish, fctx);
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

    // A char field's own UTF-8 encoding (see below) needs encodeCharUtf8,
    // which - like every other emit helper - takes a real FunctionContext,
    // not the hand-tracked bare `nextReg` int this whole function otherwise
    // uses. Declared once here (not per-struct/per-field) purely so its own
    // `nextLabel` counter keeps incrementing uniquely across every char
    // field in every struct - guarantees no label-name collision even
    // between two char fields in the *same* struct, which would otherwise
    // both start from label 0 and collide within that one shared
    // `@axea.print.<Name>` function body. Its own `out`/`nextLlvmRegister`
    // are kept in sync with this function's own `out`/`nextReg` at each use.
    FunctionContext charHelperFctx;
    charHelperFctx.out = &out;

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
            else if (isCharType(fieldLlvmType))
            {
                // A char field prints as its own Unicode character, same
                // reasoning as the top-level print dispatch's own char
                // branch (see docs/language/0044-char.md) - checked
                // *before* the generic "must be a nested struct pointer"
                // fallback below, which would otherwise misinterpret
                // "i24" as a garbled struct pointer type and try to print
                // a non-existent struct.
                charHelperFctx.nextLlvmRegister = nextReg;
                const std::string utf8Ptr =
                    encodeCharUtf8("%" + std::to_string(valReg), charHelperFctx);
                nextReg = charHelperFctx.nextLlvmRegister;
                out << "  %" << nextReg++ << " = call i32 (i8*, ...) @printf(i8* " << bareStrFmt
                    << ", i8* " << utf8Ptr << ")\n";
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
        else if (isCharType(llvmTypeStr))
        {
            // A char prints as its own Unicode character, not its numeric
            // codepoint (see docs/language/0044-char.md) - encodeCharUtf8
            // does the actual codepoint -> UTF-8-bytes work; this is then
            // just the same %s-of-an-i8*-pointer printf every other
            // string-shaped top-level binding above already uses.
            const std::string utf8Ptr = encodeCharUtf8(ref(axeaReg, fctx), fctx);
            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                << strFmt << ", i8* " << namePtr << ", i8* " << utf8Ptr << ")\n";
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
                else if (isCharType(elementType))
                {
                    // Checked *before* the generic "must be a nested
                    // struct pointer" fallback below, same reasoning as
                    // emitStructPrintHelpers' own char branch (see
                    // docs/language/0044-char.md).
                    const std::string utf8Ptr =
                        encodeCharUtf8("%" + std::to_string(elementValReg), fctx);
                    out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                        << bareStrFmt << ", i8* " << utf8Ptr << ")\n";
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
        else if (isLinkedListType(llvmTypeStr))
        {
            // Checked *before* isListType, same reasoning as Map/Set above -
            // a LinkedList header is also "{i32, ...}*"-shaped, so it would
            // otherwise spuriously match isListType's own looser test (and,
            // unlike Stack<T>, genuinely produce garbage: LinkedList's header
            // isn't shaped like List's own {length, data} record, so List's
            // print loop would misread the head/tail node pointers as a
            // T* data pointer). Same count-only fallback as Map/Set, for a
            // related but distinct reason (see docs/language/0036-linked-lists.md):
            // walking node pointers is possible in principle, but the "for
            // x in expr" desugaring that would give this a natural user-
            // facing analog needs `[i]` indexing, which a LinkedList doesn't
            // support - so this stays deliberately minimal rather than
            // hand-rolling a print-only traversal nothing else in the
            // language exercises.
            const std::string headerType = llvmTypeStr.substr(0, llvmTypeStr.size() - 1);
            const std::string prefix = stringPtrConstant("LinkedList(");
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
        else if (isSortedMapType(llvmTypeStr))
        {
            // Checked *before* isListType, same reasoning as LinkedList
            // above - a SortedMap header is also "{i32, ...}*"-shaped
            // (and would genuinely produce garbage via List's own print
            // loop below: the second field is a node pointer, not a T*
            // data pointer). Same count-only fallback as Map/Set/
            // LinkedList (see docs/language/0040-sorted-maps.md): an
            // in-order tree walk is possible in principle, but - like
            // LinkedList's own node-pointer walk - has no `for`-in
            // desugaring to hang off yet, so this stays deliberately
            // minimal.
            const std::string headerType = llvmTypeStr.substr(0, llvmTypeStr.size() - 1);
            const std::string prefix = stringPtrConstant("SortedMap(");
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
        else if (isSortedSetType(llvmTypeStr))
        {
            // Checked *before* isListType, same reasoning as SortedMap
            // above - a SortedSet header is also "{i32, ...}*"-shaped. Same
            // count-only fallback (see docs/language/0041-sorted-sets.md).
            const std::string headerType = llvmTypeStr.substr(0, llvmTypeStr.size() - 1);
            const std::string prefix = stringPtrConstant("SortedSet(");
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
        else if (isBufferType(llvmTypeStr))
        {
            // Checked *before* isDequeType (whose own "{i32, i32, ...}*"
            // structural check would otherwise silently match Buffer's own
            // identically-shaped header first) and before isListType -
            // Buffer's own header is structurally identical to Deque<T>'s
            // own shape (see docs/language/0043-buffer.md), and its data
            // field sits at index 2, not index 1 like String's own 2-field
            // header - so this mirrors isStringType's own dedicated print
            // branch below, with the field index adjusted. Always
            // null-terminated (see emitBufferNew/Append/Clear/Finish), so
            // a direct %s of the data pointer is always safe.
            const std::string headerType = llvmTypeStr.substr(0, llvmTypeStr.size() - 1);
            const int dataPtrPtrReg = allocateRegister(fctx);
            out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", "
                << llvmTypeStr << " " << ref(axeaReg, fctx) << ", i32 0, i32 2\n";
            const int dataPtrReg = allocateRegister(fctx);
            out << "  %" << dataPtrReg << " = load i8*, i8** %" << dataPtrPtrReg << "\n";
            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                << strFmt << ", i8* " << namePtr << ", i8* %" << dataPtrReg << ")\n";
        }
        else if (isDequeType(llvmTypeStr))
        {
            // Checked *before* isListType, same reasoning as Map/Set/
            // LinkedList above - a Deque header is also "{i32, ...}*"-shaped
            // (and would genuinely produce garbage via List's own print
            // loop below: the data pointer sits at field 2, not field 1, and
            // every element read needs a `+start` offset). Unlike Map/Set/
            // LinkedList, Deque<T> gets full bracket-format printing, not a
            // count-only fallback - its growable-array-with-a-start-offset
            // representation directly supports it, the same reasoning that
            // makes `[i]` cheap (see docs/language/0037-deques.md). Adapts
            // List's own runtime print loop directly below, with every
            // data-pointer read offset by `+start`.
            const std::string headerType = llvmTypeStr.substr(0, llvmTypeStr.size() - 1);
            const std::string elementType = dequeElementType(llvmTypeStr);
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
            const int startPtrReg = allocateRegister(fctx);
            out << "  %" << startPtrReg << " = getelementptr " << headerType << ", " << llvmTypeStr
                << " " << ref(axeaReg, fctx) << ", i32 0, i32 1\n";
            const int startReg = allocateRegister(fctx);
            out << "  %" << startReg << " = load i32, i32* %" << startPtrReg << "\n";
            const int dataPtrPtrReg = allocateRegister(fctx);
            out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", "
                << llvmTypeStr << " " << ref(axeaReg, fctx) << ", i32 0, i32 2\n";
            const int dataReg = allocateRegister(fctx);
            out << "  %" << dataReg << " = load " << elementType << "*, " << elementType << "** %"
                << dataPtrPtrReg << "\n";

            const int labelId = fctx.nextLabel++;
            const std::string firstLabel = "print.deque.first" + std::to_string(labelId);
            const std::string headerLabel = "print.deque.header" + std::to_string(labelId);
            const std::string bodyLabel = "print.deque.body" + std::to_string(labelId);
            const std::string skipLabel = "print.deque.skip" + std::to_string(labelId);
            const std::string doneLabel = "print.deque.done" + std::to_string(labelId);

            const int isEmptyReg = allocateRegister(fctx);
            out << "  %" << isEmptyReg << " = icmp eq i32 %" << lenReg << ", 0\n";
            out << "  br i1 %" << isEmptyReg << ", label %" << skipLabel << ", label %"
                << firstLabel << "\n";

            out << firstLabel << ":\n";
            fctx.currentLabel = firstLabel;
            {
                const int elementPtrReg = allocateRegister(fctx);
                out << "  %" << elementPtrReg << " = getelementptr " << elementType << ", "
                    << elementType << "* %" << dataReg << ", i32 %" << startReg << "\n";
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
                else if (isCharType(elementType))
                {
                    // Checked *before* the generic "must be a nested
                    // struct pointer" fallback below, same reasoning as
                    // the fixed-size array branch above (see
                    // docs/language/0044-char.md).
                    const std::string utf8Ptr =
                        encodeCharUtf8("%" + std::to_string(elementValReg), fctx);
                    out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                        << bareStrFmt << ", i8* " << utf8Ptr << ")\n";
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
            const int actualIdxReg = allocateRegister(fctx);
            out << "  %" << actualIdxReg << " = add i32 %" << startReg << ", %" << iForElemReg
                << "\n";
            const int elementPtrReg = allocateRegister(fctx);
            out << "  %" << elementPtrReg << " = getelementptr " << elementType << ", "
                << elementType << "* %" << dataReg << ", i32 %" << actualIdxReg << "\n";
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
            else if (isCharType(elementType))
            {
                // Checked *before* the generic "must be a nested struct
                // pointer" fallback below, same reasoning as the
                // fixed-size array branch above (see
                // docs/language/0044-char.md).
                const std::string utf8Ptr =
                    encodeCharUtf8("%" + std::to_string(elementValReg), fctx);
                out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                    << bareStrFmt << ", i8* " << utf8Ptr << ")\n";
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
        else if (isStringType(llvmTypeStr))
        {
            // Checked *before* isListType - String's own header
            // {i32, i8*}* is deliberately the exact same LLVM type
            // List<i8> would have (see docs/language/0042-string.md's own
            // Design section), so without this dedicated branch it would
            // fall into the generic byte-print loop below and misprint
            // every byte as if it were a nested struct pointer
            // (listElementType would read "i8", which the generic loop's
            // own element-type dispatch has no case for). String's data
            // is always kept null-terminated (see emitStringNew/
            // emitStringAppend), so printing is just the existing bare
            // GEP+load-the-data-pointer, reusing the exact %s format a
            // plain top-level `str` binding already uses.
            const std::string headerType = llvmTypeStr.substr(0, llvmTypeStr.size() - 1);
            const int dataPtrPtrReg = allocateRegister(fctx);
            out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", "
                << llvmTypeStr << " " << ref(axeaReg, fctx) << ", i32 0, i32 1\n";
            const int dataPtrReg = allocateRegister(fctx);
            out << "  %" << dataPtrReg << " = load i8*, i8** %" << dataPtrPtrReg << "\n";
            out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                << strFmt << ", i8* " << namePtr << ", i8* %" << dataPtrReg << ")\n";
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
                else if (isCharType(elementType))
                {
                    // Checked *before* the generic "must be a nested
                    // struct pointer" fallback below, same reasoning as
                    // the fixed-size array branch above (see
                    // docs/language/0044-char.md).
                    const std::string utf8Ptr =
                        encodeCharUtf8("%" + std::to_string(elementValReg), fctx);
                    out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                        << bareStrFmt << ", i8* " << utf8Ptr << ")\n";
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
            else if (isCharType(elementType))
            {
                // Checked *before* the generic "must be a nested struct
                // pointer" fallback below, same reasoning as the
                // fixed-size array branch above (see
                // docs/language/0044-char.md).
                const std::string utf8Ptr =
                    encodeCharUtf8("%" + std::to_string(elementValReg), fctx);
                out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                    << bareStrFmt << ", i8* " << utf8Ptr << ")\n";
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
    out << "declare i32 @printf(i8*, ...)\n";
    // String<>'s own construction/append need a runtime str length (see
    // docs/language/0042-string.md) - str (i8*) has no length field of its
    // own, unlike every other collection here, so this is the one place a
    // third libc function beyond malloc/printf gets declared.
    out << "declare i64 @strlen(i8*)\n\n";

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
    // Same reasoning as above, for LinkedList<T> (see
    // docs/language/0036-linked-lists.md) - safe to snapshot only once every
    // function/topLevel has been through inferTypes (registerLinkedListInstantiation
    // is driven by llvmType, same as Map/Set's own registration).
    out << linkedListTypeDeclsText_.str();
    out << linkedListRuntimeText_.str();
    // Same reasoning again, for SortedMap<K,V> (see
    // docs/language/0040-sorted-maps.md).
    out << sortedMapTypeDeclsText_.str();
    out << sortedMapRuntimeText_.str();
    // Same reasoning again, for SortedSet<T> (see
    // docs/language/0041-sorted-sets.md).
    out << sortedSetTypeDeclsText_.str();
    out << sortedSetRuntimeText_.str();
    // Same reasoning again, for `.parse<T>()` (see
    // docs/language/0046-generic-methods.md) - registerParseRuntime is
    // driven by emitParse itself (called during emitFunction/emitMain
    // above), not by inferTypes/llvmType like every registration above,
    // but the ordering guarantee is identical: every function and
    // topLevel has already been fully emitted by this point, so
    // parseRuntimeText_ is complete.
    out << parseRuntimeText_.str();
    // Same reasoning again, for `.length`'s own codepoint-counting
    // runtime (see docs/language/0047-unicode.md) - registerUtf8CountRuntime
    // is driven by emitFieldGet itself, same lazy-during-emission timing
    // as registerParseRuntime above.
    out << utf8CountRuntimeText_.str();
    out << helpers.str();
    out << mainOut.str();

    return out.str();
}
