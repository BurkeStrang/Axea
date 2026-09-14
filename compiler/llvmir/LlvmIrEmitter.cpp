#include "llvmir/LlvmIrEmitter.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>

namespace
{
    // LLVM IR's own hexadecimal float-constant form - 16 hex digits, the
    // IEEE 754 bit pattern of the double directly (see the LLVM Language
    // Reference's "Simple Constants" section) - used unconditionally
    // rather than plain decimal notation, since decimal notation is only
    // guaranteed exact for values LLVM's parser happens to round-trip
    // losslessly; the hex form is always exact, for any double.
    std::string formatDoubleLiteral(double value)
    {
        std::uint64_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        std::ostringstream oss;
        oss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(16) << bits;
        return oss.str();
    }

    // A union type's own canonical name ("i32|str") uses '|' as declared in Axea source, but a
    // struct/enum field's own stored type text (IrProgram::structs' second-of-pair) is never
    // re-mangled the way IrGenerator's own llvmSafeTypeName already mangles it before using it
    // as an actual `program.structs`/`program.enums` *key* (see IrGenerator.cpp's own comment on
    // this at the union-flattening site) - so a union-typed struct field's stored text still
    // reads "i32|str" even though the union's own registered entry lives under "i32.str". Own
    // copy of the identical rename, per this codebase's "each pass owns its own walk"
    // convention - used only by emitStructRefcountHelpers, to correctly recognize a union-typed
    // field as (if the union's own alternatives happen to include a struct/enum) potentially
    // needing recursive release.
    std::string mangleUnionTypeName(std::string name)
    {
        std::replace(name.begin(), name.end(), '|', '.');
        return name;
    }

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
    // rewrite). '('/')' tracked too, since docs/language/0067-closures.md's
    // own "fn(T1,T2)->R" closure type text can nest inside another type's
    // own argument list, exactly like a further Map<K,V> already can.
    std::size_t findTopLevelComma(const std::string& text)
    {
        int depth = 0;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '<' || text[i] == '[' || text[i] == '(')
            {
                ++depth;
            }
            else if (text[i] == '>' || text[i] == ']' || text[i] == ')')
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
    if (axeaTypeName == "i64")
    {
        return "i64";
    }
    if (axeaTypeName == "f64")
    {
        // LLVM's own name for a 64-bit IEEE 754 float (see
        // docs/language/0005-type-system.md) - "f64" only ever refers to
        // Axea source text; "double" is this type's actual LLVM spelling.
        return "double";
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
    if (axeaTypeName == "cstr")
    {
        // Representationally identical to str (see
        // docs/language/0048-ffi.md and docs/language/0042-string.md's own
        // Design section) - both a bare, null-terminated i8*. The two stay
        // distinct Axea types (no implicit coercion either direction) so
        // `.to_cstr()` remains a real, deliberate step at the type level,
        // even though it produces bit-identical output.
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
    if (!axeaTypeName.empty() && axeaTypeName.front() == '*')
    {
        // `*T` (see docs/language/0019-unsafe.md) - a bare LLVM pointer, one level deeper than
        // llvmType(T)'s own representation. Note: for a struct pointee (already itself
        // represented as "%StructName*" - see this backend's own by-reference struct
        // convention), this produces "%StructName**" - a genuinely correct double-pointer, but
        // this milestone's own codegen helpers (isPointerType/pointerElementType below) only
        // recognize a *primitive*-pointee pointer; a struct-pointee `*T` type-checks fine (any
        // concrete T) but its deref/arithmetic codegen is untested and out of scope this phase.
        return llvmType(axeaTypeName.substr(1)) + "*";
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
    if (axeaTypeName.starts_with("Optional<"))
    {
        // "Optional<elem>" - the canonical form Parser::parseTypeName
        // always produces (see docs/language/0052-optional.md).
        const std::string elementName = axeaTypeName.substr(9, axeaTypeName.size() - 10);
        return registerOptionalInstantiation(elementName);
    }
    if (axeaTypeName.starts_with("Result<"))
    {
        // "Result<T,E>" - the canonical form Parser::parseTypeName always
        // produces (see docs/language/0063-result.md). Same bracket-aware
        // comma split as Map<K,V> below (findTopLevelComma handles
        // Result<i32,Map<i32,i32>> correctly).
        const std::string args = axeaTypeName.substr(7, axeaTypeName.size() - 8);
        const auto comma = findTopLevelComma(args);
        return registerResultInstantiation(args.substr(0, comma), args.substr(comma + 1));
    }
    if (axeaTypeName.starts_with("fn("))
    {
        // "fn(T1,T2)->R" - the canonical form Parser::parseTypeName always produces (see
        // docs/language/0067-closures.md). Own copy of
        // TypeChecker::closureParamAndReturnTypes' own paren-depth-aware split (mirrors
        // findTopLevelComma's own "<"/"["/"(" depth tracking, generalized to find the matching
        // top-level ')' instead of a top-level ',').
        int depth = 0;
        std::size_t closeParen = std::string::npos;
        for (std::size_t i = 3; i < axeaTypeName.size(); ++i) // "fn(" is always exactly 3 chars
        {
            const char c = axeaTypeName[i];
            if (c == '<' || c == '[' || c == '(')
            {
                ++depth;
            }
            else if (c == '>' || c == ']' || c == ')')
            {
                if (depth == 0)
                {
                    closeParen = i;
                    break;
                }
                --depth;
            }
        }
        const std::string paramsCsv = axeaTypeName.substr(3, closeParen - 3);
        const std::string returnTypeName = axeaTypeName.substr(closeParen + 3); // skip ")->"

        std::vector<std::string> paramLlvmTypes;
        if (!paramsCsv.empty())
        {
            std::size_t start = 0;
            while (true)
            {
                const auto comma = findTopLevelComma(paramsCsv.substr(start));
                if (comma == std::string::npos)
                {
                    paramLlvmTypes.push_back(llvmType(paramsCsv.substr(start)));
                    break;
                }
                paramLlvmTypes.push_back(llvmType(paramsCsv.substr(start, comma)));
                start += comma + 1;
            }
        }
        return registerClosureInstantiation(paramLlvmTypes, llvmType(returnTypeName)) + "*";
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
        // anonymous 3-field heap record {length, data, capacity} - unlike
        // slice<T>, List<T> genuinely owns its storage (a stable heap
        // pointer, mutated in place on push/pop), so it follows
        // struct/array's "always by pointer" convention, not slice's
        // by-value one. `capacity` (amortized doubling growth - see
        // ensureListCapacity) is deliberately field *2*, at the end, not
        // field 1 the way Buffer's own {length, capacity, data} order
        // puts it: field 1 has to stay `T* data` (unchanged from before
        // this field existed) to avoid colliding with Deque<T>'s own
        // `{i32, i32, T*}*` header text - see ensureListCapacity's own
        // comment for the full reasoning.
        const std::string elementName = axeaTypeName.substr(5, axeaTypeName.size() - 6);
        return "{i32, " + llvmType(elementName) + "*, i32}*";
    }
    if (axeaTypeName.starts_with("Stack<"))
    {
        // Vestigial: `Stack<T>` is a real, user-declared generic struct now (see
        // docs/language/0006-generics.md's own List<T>/Stack<T> port follow-up and
        // std/collections.ax) - a monomorphized instance reaches this function under its own
        // mangled struct name, never as raw "Stack<...>" text, so this branch is believed
        // unreachable in practice. Left in place (matching the identical "List<" branch just
        // above, kept unchanged through List<T>'s own port) rather than deleted outright, since
        // proving a negative here isn't worth the risk for a few harmless lines.
        const std::string elementName = axeaTypeName.substr(6, axeaTypeName.size() - 7);
        return "{i32, " + llvmType(elementName) + "*, i32}*";
    }
    // `Map<K,V>`/`Set<T>` are real, user-declared generic structs now (see
    // docs/language/0034-maps-and-sets.md's own "2026 Update") - a monomorphized instance
    // reaches this function under its own mangled struct name ("%Map$i32$i32*"), never as raw
    // "Map<...>"/"Set<...>" text, so unlike Stack<T>'s own vestigial branch above, this one is
    // deleted outright: the old and new header shapes genuinely differ (the old anonymous
    // 3-field-header-plus-named-entry-type shape vs. an ordinary named struct reference now),
    // matching LinkedList<T>'s own identical port decision.
    // `SortedMap<K,V>`/`SortedSet<T>` are both real, user-declared generic structs now too (see
    // docs/language/0040-sorted-maps.md's own "2026 Update" and docs/language/0041-sorted-sets.md's
    // own "2026 Update") - same reasoning as Map<K,V>/Set<T> just above, deleted outright rather
    // than left vestigial: the old anonymous 2-field-header-plus-named-node-type shape genuinely
    // differs from an ordinary named struct reference now. This is the last of these branches:
    // every collection docs/language/0029-collections.md originally scoped as a compiler intrinsic
    // is now real Axea source.
    // `LinkedList<T>` is a real, user-declared generic struct now (see
    // docs/language/0006-generics.md's own port follow-up and std/collections.ax), reaching this
    // function under its own mangled struct name in practice, not raw "LinkedList<...>" text -
    // deleted outright, unlike its own sibling "vestigial" branches just below, because unlike
    // Stack<T>/PriorityQueue<T> (both LLVM-identical to List<T> both before and after their own
    // ports), LinkedList<T>'s own former self-referential node header (%axea.LLNode.<id>,
    // "{i32, node*, node*}*") is nothing like its new representation (an ordinary struct
    // pointer, "%LinkedList$T*") - there is no single formula that stays correct across both,
    // so keeping the old one "just in case" would be silently wrong if this branch were ever
    // actually reached, not merely harmless.
    if (axeaTypeName.starts_with("Deque<"))
    {
        // Vestigial: `Deque<T>` is a real, user-declared generic struct now (see
        // docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T> port follow-up and
        // std/collections.ax) - a monomorphized instance reaches this function under its own
        // mangled struct name, never as raw "Deque<...>" text, so this branch is believed
        // unreachable in practice. Left in place (matching the identical "List<"/"Stack<"
        // branches above, kept unchanged through those ports) rather than deleted outright,
        // since proving a negative here isn't worth the risk for a few harmless lines.
        const std::string elementName = axeaTypeName.substr(6, axeaTypeName.size() - 7);
        return "{i32, i32, " + llvmType(elementName) + "*}*";
    }
    if (axeaTypeName.starts_with("Queue<"))
    {
        // Queue<T> (see docs/language/0038-queues.md) - a FIFO collection using the same
        // growable-array-with-a-start-offset header shape the retired Deque<T> intrinsic used to
        // ("{i32, i32, " + llvmType(elementName) + "*}*") - genuinely the same LLVM type, not
        // merely a similarly-shaped one. isDequeType's own existing structural check already
        // handles a Queue<T> value correctly with no changes at all.
        const std::string elementName = axeaTypeName.substr(6, axeaTypeName.size() - 7);
        return "{i32, i32, " + llvmType(elementName) + "*}*";
    }
    if (axeaTypeName.starts_with("PriorityQueue<"))
    {
        // Vestigial, same reasoning as "Deque<"/"Queue<" just above: `PriorityQueue<T>` is a
        // real, user-declared generic struct now (see docs/language/0006-generics.md's own
        // List<T>/Stack<T>/Deque<T>/Queue<T>/PriorityQueue<T> port follow-up and
        // std/collections.ax), reaching this function under its own mangled struct name in
        // practice, not raw "PriorityQueue<...>" text - left in place rather than deleted
        // outright. Its header shape happens to be byte-identical to List<T>'s own
        // ("{i32, " + llvmType(elementName) + "*, i32}*"), a coincidence of how it composes
        // over `items: List<T>`, not a distinct representation this function needs to know
        // about anymore.
        const std::string elementName = axeaTypeName.substr(14, axeaTypeName.size() - 15);
        return "{i32, " + llvmType(elementName) + "*, i32}*";
    }
    if (axeaTypeName.starts_with("Shared<") && axeaTypeName.back() == '>')
    {
        // Shared<T> (the move-semantics work's own explicit refcounting escape hatch) - a real
        // bug found while testing: unlike every other generic type string ever reaching this
        // function (already resolved through IrGenerator's own mangleSharedTypeName by the time
        // it gets here, in the common case), a raw *function parameter's* declared type text can
        // still arrive here unmangled, straight from Parser::parseTypeName's own canonical output
        // - '<'/'>' aren't legal LLVM identifier characters, so the naive fallback below would
        // produce literally invalid IR text ("%Shared<Point>*"), causing an opaque
        // structs_.at(...)-style lookup failure downstream once something tries to resolve that
        // text back to a registered struct. Mirrors IrGenerator::registerSharedType's own exact
        // mangling ("Shared." + T) so the two stay consistent.
        return "%Shared." + axeaTypeName.substr(7, axeaTypeName.size() - 8) + "*";
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
        // Bitwise AND (see docs/language/0034-maps-and-sets.md's own "2026 Update") - i32/i64
        // only (TypeChecker already rejects f64), so floatBinOpMnemonic below needs no case at
        // all.
        case TokenKind::Ampersand: return "and";
        case TokenKind::Less: return "icmp slt";
        case TokenKind::LessEqual: return "icmp sle";
        case TokenKind::Greater: return "icmp sgt";
        case TokenKind::GreaterEqual: return "icmp sge";
        case TokenKind::EqualEqual: return "icmp eq";
        case TokenKind::BangEqual: return "icmp ne";
        default: return "add"; // unreachable for a well-checked program
    }
}

std::string LlvmIrEmitter::floatBinOpMnemonic(TokenKind op) const
{
    switch (op)
    {
        case TokenKind::Plus: return "fadd";
        case TokenKind::Minus: return "fsub";
        case TokenKind::Star: return "fmul";
        case TokenKind::Slash: return "fdiv";
        case TokenKind::Less: return "fcmp olt";
        case TokenKind::LessEqual: return "fcmp ole";
        case TokenKind::Greater: return "fcmp ogt";
        case TokenKind::GreaterEqual: return "fcmp oge";
        case TokenKind::EqualEqual: return "fcmp oeq";
        case TokenKind::BangEqual: return "fcmp one";
        default: return "fadd"; // unreachable for a well-checked program
    }
}

void LlvmIrEmitter::emitStrComparison(int axeaDest,
                                      TokenKind op,
                                      const std::string& lhsPtr,
                                      const std::string& rhsPtr,
                                      FunctionContext& fctx)
{
    // `defineRegister(axeaDest, ...)` must be the *last* register allocated
    // in each branch below, right before the line that defines it - LLVM
    // requires unnamed (numeric) registers to be defined in strictly
    // increasing textual order, so any intermediate register this function
    // needs (eqReg/notLessReg) has to be allocated - and its own defining
    // line written - *before* destReg's, not after.
    if (op == TokenKind::EqualEqual)
    {
        const auto [unusedHashFn, eqFn] = registerKeyRuntime("str");
        const int destReg = defineRegister(axeaDest, fctx);
        *fctx.out << "  %" << destReg << " = call i1 " << eqFn << "(i8* " << lhsPtr << ", i8* "
                  << rhsPtr << ")\n";
        return;
    }
    if (op == TokenKind::BangEqual)
    {
        const auto [unusedHashFn, eqFn] = registerKeyRuntime("str");
        const int eqReg = allocateRegister(fctx);
        *fctx.out << "  %" << eqReg << " = call i1 " << eqFn << "(i8* " << lhsPtr << ", i8* "
                  << rhsPtr << ")\n";
        const int destReg = defineRegister(axeaDest, fctx);
        *fctx.out << "  %" << destReg << " = xor i1 %" << eqReg << ", 1\n";
        return;
    }

    const std::string lessFn = registerOrderRuntime("str");
    if (op == TokenKind::Less)
    {
        const int destReg = defineRegister(axeaDest, fctx);
        *fctx.out << "  %" << destReg << " = call i1 " << lessFn << "(i8* " << lhsPtr << ", i8* "
                  << rhsPtr << ")\n";
        return;
    }
    if (op == TokenKind::Greater)
    {
        const int destReg = defineRegister(axeaDest, fctx);
        *fctx.out << "  %" << destReg << " = call i1 " << lessFn << "(i8* " << rhsPtr << ", i8* "
                  << lhsPtr << ")\n";
        return;
    }
    // LessEqual: a<=b iff not(b<a). GreaterEqual: a>=b iff not(a<b) - only a
    // strict less-than primitive exists, so both derive from one call with
    // the operands (not) swapped.
    const bool isLessEqual = op == TokenKind::LessEqual;
    const std::string& notLessLhs = isLessEqual ? rhsPtr : lhsPtr;
    const std::string& notLessRhs = isLessEqual ? lhsPtr : rhsPtr;
    const int notLessReg = allocateRegister(fctx);
    *fctx.out << "  %" << notLessReg << " = call i1 " << lessFn << "(i8* " << notLessLhs << ", i8* "
              << notLessRhs << ")\n";
    const int destReg = defineRegister(axeaDest, fctx);
    *fctx.out << "  %" << destReg << " = xor i1 %" << notLessReg << ", 1\n";
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

bool LlvmIrEmitter::isNamedStructPointerType(const std::string& type) const
{
    // A real, previously-undiscovered bug found while porting LinkedList<T>'s own self-
    // referential `*Node<T>` pointers: this used to match "%Node**" (a genuine raw pointer *to*
    // a struct, see isPointerType's own identical distinction) the same as "%Node*" (an ordinary
    // struct reference) - excluded here the identical way, by rejecting anything still ending in
    // '*' after stripping exactly one trailing star.
    return type.size() > 1 && type.front() == '%' && type.back() == '*' &&
           type[type.size() - 2] != '*';
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

std::string LlvmIrEmitter::registerOptionalInstantiation(const std::string& payloadAxeaType)
{
    return registerOptionalInstantiationForLlvmPayload(llvmType(payloadAxeaType));
}

std::string
LlvmIrEmitter::registerOptionalInstantiationForLlvmPayload(const std::string& payloadLlvmType)
{
    if (const auto it = optionalInstantiationIds_.find(payloadLlvmType);
        it != optionalInstantiationIds_.end())
    {
        return "%axea.Optional." + std::to_string(it->second);
    }

    const int id = nextOptionalInstantiationId_++;
    optionalInstantiationIds_[payloadLlvmType] = id;
    const std::string name = "%axea.Optional." + std::to_string(id);

    optionalTypeDeclsText_ << name << " = type { i1, " << payloadLlvmType << " }\n";
    optionalPayloadTypeById_[id] = payloadLlvmType;

    return name;
}

bool LlvmIrEmitter::isOptionalType(const std::string& type) const
{
    return type.starts_with("%axea.Optional.");
}

std::string LlvmIrEmitter::optionalPayloadType(const std::string& type) const
{
    // "%axea.Optional.<id>" - the id is everything after the last '.'.
    const auto lastDot = type.rfind('.');
    const int id = std::stoi(type.substr(lastDot + 1));
    return optionalPayloadTypeById_.at(id);
}

std::string LlvmIrEmitter::registerResultInstantiation(const std::string& okAxeaType,
                                                       const std::string& errAxeaType)
{
    return registerResultInstantiationForLlvmPayloads(llvmType(okAxeaType), llvmType(errAxeaType));
}

std::string
LlvmIrEmitter::registerResultInstantiationForLlvmPayloads(const std::string& okLlvmType,
                                                          const std::string& errLlvmType)
{
    const std::string key = okLlvmType + "|" + errLlvmType;
    if (const auto it = resultInstantiationIds_.find(key); it != resultInstantiationIds_.end())
    {
        return "%axea.Result." + std::to_string(it->second);
    }

    const int id = nextResultInstantiationId_++;
    resultInstantiationIds_[key] = id;
    const std::string name = "%axea.Result." + std::to_string(id);

    resultTypeDeclsText_ << name << " = type { i1, " << okLlvmType << ", " << errLlvmType << " }\n";
    resultOkPayloadTypeById_[id] = okLlvmType;
    resultErrPayloadTypeById_[id] = errLlvmType;

    return name;
}

bool LlvmIrEmitter::isResultType(const std::string& type) const
{
    return type.starts_with("%axea.Result.");
}

std::string LlvmIrEmitter::resultOkPayloadType(const std::string& type) const
{
    // "%axea.Result.<id>" - the id is everything after the last '.'.
    const auto lastDot = type.rfind('.');
    const int id = std::stoi(type.substr(lastDot + 1));
    return resultOkPayloadTypeById_.at(id);
}

std::string LlvmIrEmitter::resultErrPayloadType(const std::string& type) const
{
    const auto lastDot = type.rfind('.');
    const int id = std::stoi(type.substr(lastDot + 1));
    return resultErrPayloadTypeById_.at(id);
}

std::string
LlvmIrEmitter::registerClosureInstantiation(const std::vector<std::string>& paramLlvmTypes,
                                            const std::string& returnLlvmType)
{
    std::string key = returnLlvmType;
    for (const auto& paramLlvmType : paramLlvmTypes)
    {
        key += "|" + paramLlvmType;
    }
    if (const auto it = closureInstantiationIds_.find(key); it != closureInstantiationIds_.end())
    {
        return "%axea.Closure." + std::to_string(it->second);
    }

    const int id = nextClosureInstantiationId_++;
    closureInstantiationIds_[key] = id;
    const std::string name = "%axea.Closure." + std::to_string(id);

    // The classic "fat pointer" closure representation - captures are hidden behind field 1's
    // own opaque i8*, so field 0's own function-pointer type only ever depends on the closure's
    // declared signature, never on what any one literal actually captures (see
    // closureInstantiationIds_'s own comment). Field 2 (added for closure drop-lifecycle support)
    // is the same "opaque, per-signature, dispatched dynamically" trick applied to *dropping* a
    // closure: since every literal sharing this signature collapses onto the same
    // %axea.Closure.<id> type regardless of what it actually captures, there is no static way to
    // know - from the fat pointer's own LLVM type alone - which specific captures-struct shape a
    // given value holds at runtime (the same reason field 0 can't be the trampoline's own *real*
    // type either - see emitClosureNew's own comment on that). Field 2 is a per-literal
    // drop-wrapper function pointer, populated at construction time by emitClosureNew, that knows
    // exactly which real captures-struct type to bitcast back to and drop - the generic
    // @axea.drop.<name> helper below (one per *signature*, not per literal) just calls through it.
    std::string fnPtrType = returnLlvmType + " (i8*";
    for (const auto& paramLlvmType : paramLlvmTypes)
    {
        fnPtrType += ", " + paramLlvmType;
    }
    fnPtrType += ")*";

    closureTypeDeclsText_ << name << " = type { " << fnPtrType << ", i8*, void (i8*)* }\n";
    closureFnPtrTypeById_[id] = fnPtrType;
    closureReturnTypeById_[id] = returnLlvmType;
    emitClosureDropHelper(name);

    return name;
}

void LlvmIrEmitter::emitClosureDropHelper(const std::string& closureTypeName)
{
    const std::string pointerType = closureTypeName + "*";
    std::ostringstream body;
    body << "define void @axea.drop." << closureTypeName.substr(1) << "(" << pointerType
         << " %v) {\n";
    body << "entry:\n";
    body << "  %isnull = icmp eq " << pointerType << " %v, null\n";
    body << "  br i1 %isnull, label %done, label %body\n";
    body << "body:\n";
    body << "  %dropFnPtr = getelementptr " << closureTypeName << ", " << pointerType
         << " %v, i32 0, i32 2\n";
    body << "  %dropFn = load void (i8*)*, void (i8*)** %dropFnPtr\n";
    body << "  %capturesPtr = getelementptr " << closureTypeName << ", " << pointerType
         << " %v, i32 0, i32 1\n";
    body << "  %captures = load i8*, i8** %capturesPtr\n";
    body << "  call void %dropFn(i8* %captures)\n";
    body << "  %raw = bitcast " << pointerType << " %v to i8*\n";
    body << "  call void @free(i8* %raw)\n";
    body << "  br label %done\n";
    body << "done:\n";
    body << "  ret void\n";
    body << "}\n";
    structRefcountRuntimeText_ << "\n" << body.str();
}

bool LlvmIrEmitter::isClosureType(const std::string& type) const
{
    return type.starts_with("%axea.Closure.");
}

std::string LlvmIrEmitter::closureFnPtrType(const std::string& type) const
{
    const auto lastDot = type.rfind('.');
    const int id = std::stoi(type.substr(lastDot + 1));
    return closureFnPtrTypeById_.at(id);
}

std::string LlvmIrEmitter::closureReturnType(const std::string& type) const
{
    const auto lastDot = type.rfind('.');
    const int id = std::stoi(type.substr(lastDot + 1));
    return closureReturnTypeById_.at(id);
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

bool LlvmIrEmitter::isPointerType(const std::string& type) const
{
    // A raw Axea `*T` pointer renders as a '*'-suffixed type - unlike every heap-backed
    // collection/array/String/Buffer/closure type here, each of which has its own distinctive
    // structural prefix ('{'/'[') checked by the isXType helper above/below it, so any
    // '*'-suffixed text *not* starting with '{'/'[' is a real `*T` pointer at the Axea type
    // level: "i32*"/"i64*"/"double*"/"i1*"/"i24*" (a pointer to a primitive,
    // docs/language/0019-unsafe.md), and also "i8**"/"i32**"/etc - a pointer *to* a primitive
    // pointer (e.g. a `List<str>`'s own `data: *str` field, str already being "i8*"). Explicitly
    // excludes "i8*" itself (str/cstr's own canonical representation) since that's not a real
    // `*T` pointer at the Axea type level - this only matters for a hypothetical `*i8` (pointer
    // to a raw byte), which isn't resolvable this phase anyway ("i8" itself isn't yet a supported
    // primitive - see resolveType's own primitives map) - documented, not fixed, as a follow-up
    // risk.
    if (type.size() <= 1 || type.back() != '*' || type == "i8*")
    {
        return false;
    }
    if (type.front() == '%')
    {
        // A raw Axea `*T` pointer to a *struct* pointee (see docs/language/0036-linked-lists.md's
        // own 2026 self-referential-node port) - a struct value already renders as one star
        // ("%Node$i32*", structs are always by-pointer - see llvmType's own struct fallback), so
        // a genuine raw pointer *to* one renders with one star *more* than that, "%Node$i32**".
        // This is what distinguishes a real struct-pointee `*T` from a plain struct reference
        // itself ("%Node$i32*", exactly one star) - previously excluded here outright (a real
        // limitation, not fixed until this port actually needed it).
        return type.size() > 2 && type[type.size() - 2] == '*';
    }
    return type.front() != '{' && type.front() != '[';
}

std::string LlvmIrEmitter::pointerElementType(const std::string& type) const
{
    return type.substr(0, type.size() - 1); // strip exactly one trailing '*'
}

bool LlvmIrEmitter::isListType(const std::string& type) const
{
    return !type.empty() && type.front() == '{' && type.back() == '*';
}

std::string LlvmIrEmitter::listElementType(const std::string& type) const
{
    // "{i32, T*, i32}*" - strip the leading "{i32, " and trailing
    // "*, i32}*" (capacity - see ensureListCapacity's own comment for why
    // it's field 2, at the end, rather than field 1).
    return type.substr(6, type.size() - 6 - std::string("*, i32}*").size());
}

bool LlvmIrEmitter::isDequeType(const std::string& type) const
{
    // Excludes "%axea." explicitly, not just a trailing "**}*" check -
    // Deque<str>'s own data field is "i8**" (str is itself "i8*"), the same
    // double-star suffix a named entry-pointer-pointer field has, so a
    // suffix-only check would misfire on that case.
    return type.starts_with("{i32, i32, ") && type.ends_with("}*") &&
           !type.starts_with("{i32, i32, %axea.");
}

std::string LlvmIrEmitter::dequeElementType(const std::string& type) const
{
    // "{i32, i32, T*}*" - strip the leading "{i32, i32, " and trailing "*}*".
    return type.substr(11, type.size() - 11 - std::string("*}*").size());
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
    return resolveStrPtrOfType(typeOf(reg, fctx), ref(reg, fctx), fctx);
}

std::string LlvmIrEmitter::resolveStrPtrOfType(const std::string& type,
                                               const std::string& valueRef,
                                               FunctionContext& fctx) const
{
    if (type == "i8*")
    {
        return valueRef;
    }
    // Must be a String header ({i32, i8*}*) - TypeChecker's own
    // isStrCoercible already guarantees no other type reaches here (see
    // docs/language/0042-string.md).
    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr {i32, i8*}, {i32, i8*}* " << valueRef
              << ", i32 0, i32 1\n";
    const int dataPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrReg << " = load i8*, i8** %" << dataPtrPtrReg << "\n";
    return "%" + std::to_string(dataPtrReg);
}

std::string LlvmIrEmitter::registerI32ToStrRuntime()
{
    const std::string fnName = "@axea.i32.to_str";
    if (i32ToStrRegistered_)
    {
        return fnName;
    }
    i32ToStrRegistered_ = true;

    // A real `sprintf` call, not hand-rolled itoa (see
    // docs/language/Axea_Printing_Formatting.md) - reuses libc's own
    // well-tested integer formatting (correct sign handling, including
    // INT32_MIN, for free) rather than risking a hand-written digit-math
    // bug. 12 bytes is always enough ("-2147483648\0" is 12 characters).
    // The format-string global is declared in this exact same
    // self-contained block (not via hoistString/stringGlobals_), so
    // there's no dependency on collectStrings/emitStringGlobals' own
    // timing - referencing a global by name never requires it to be
    // declared earlier in the same module textually.
    toStrRuntimeText_ << R"(
@axea.fmt.d = private unnamed_addr constant [3 x i8] c"%d\00"

define i8* @axea.i32.to_str(i32 %v) {
entry:
  %buf = call i8* @malloc(i64 12)
  %fmtPtr = getelementptr [3 x i8], [3 x i8]* @axea.fmt.d, i64 0, i64 0
  %ignored = call i32 (i8*, i8*, ...) @sprintf(i8* %buf, i8* %fmtPtr, i32 %v)
  ret i8* %buf
}
)";
    return fnName;
}

std::string LlvmIrEmitter::registerI64ToStrRuntime()
{
    const std::string fnName = "@axea.i64.to_str";
    if (i64ToStrRegistered_)
    {
        return fnName;
    }
    i64ToStrRegistered_ = true;

    // Same real-sprintf approach as registerI32ToStrRuntime above, just
    // "%lld" (64-bit) instead of "%d" - 24 bytes is always enough
    // ("-9223372036854775808\00" is 22 characters including the
    // terminator).
    toStrRuntimeText_ << R"(
@axea.fmt.lld = private unnamed_addr constant [5 x i8] c"%lld\00"

define i8* @axea.i64.to_str(i64 %v) {
entry:
  %buf = call i8* @malloc(i64 24)
  %fmtPtr = getelementptr [5 x i8], [5 x i8]* @axea.fmt.lld, i64 0, i64 0
  %ignored = call i32 (i8*, i8*, ...) @sprintf(i8* %buf, i8* %fmtPtr, i64 %v)
  ret i8* %buf
}
)";
    return fnName;
}

std::string LlvmIrEmitter::registerF64ToStrRuntime()
{
    const std::string fnName = "@axea.f64.to_str";
    if (f64ToStrRegistered_)
    {
        return fnName;
    }
    f64ToStrRegistered_ = true;

    // "%g" (not "%f") - matches Interpreter.cpp's own toString exactly
    // (both a real sprintf/snprintf("%g", ...) call), so interpreted and
    // compiled output stay character-for-character identical (see
    // docs/language/0005-type-system.md). 32 bytes is comfortably enough
    // for %g's own output range (6 significant digits by default, plus
    // sign/decimal point/exponent).
    toStrRuntimeText_ << R"(
@axea.fmt.g = private unnamed_addr constant [3 x i8] c"%g\00"

define i8* @axea.f64.to_str(double %v) {
entry:
  %buf = call i8* @malloc(i64 32)
  %fmtPtr = getelementptr [3 x i8], [3 x i8]* @axea.fmt.g, i64 0, i64 0
  %ignored = call i32 (i8*, i8*, ...) @sprintf(i8* %buf, i8* %fmtPtr, double %v)
  ret i8* %buf
}
)";
    return fnName;
}

std::string LlvmIrEmitter::registerBoolToStrRuntime()
{
    const std::string fnName = "@axea.bool.to_str";
    if (boolToStrRegistered_)
    {
        return fnName;
    }
    boolToStrRegistered_ = true;

    // Hand-rolled byte stores (see
    // docs/language/Axea_Printing_Formatting.md), mirroring
    // @axea.parse.bool's own self-contained style - no format-string
    // global needed for a fixed "true"/"false" pair.
    toStrRuntimeText_ << R"(
define i8* @axea.bool.to_str(i1 %v) {
entry:
  br i1 %v, label %istrue, label %isfalse
istrue:
  %tbuf = call i8* @malloc(i64 5)
  %t0 = getelementptr i8, i8* %tbuf, i64 0
  store i8 116, i8* %t0
  %t1 = getelementptr i8, i8* %tbuf, i64 1
  store i8 114, i8* %t1
  %t2 = getelementptr i8, i8* %tbuf, i64 2
  store i8 117, i8* %t2
  %t3 = getelementptr i8, i8* %tbuf, i64 3
  store i8 101, i8* %t3
  %t4 = getelementptr i8, i8* %tbuf, i64 4
  store i8 0, i8* %t4
  ret i8* %tbuf
isfalse:
  %fbuf = call i8* @malloc(i64 6)
  %f0 = getelementptr i8, i8* %fbuf, i64 0
  store i8 102, i8* %f0
  %f1 = getelementptr i8, i8* %fbuf, i64 1
  store i8 97, i8* %f1
  %f2 = getelementptr i8, i8* %fbuf, i64 2
  store i8 108, i8* %f2
  %f3 = getelementptr i8, i8* %fbuf, i64 3
  store i8 115, i8* %f3
  %f4 = getelementptr i8, i8* %fbuf, i64 4
  store i8 101, i8* %f4
  %f5 = getelementptr i8, i8* %fbuf, i64 5
  store i8 0, i8* %f5
  ret i8* %fbuf
}
)";
    return fnName;
}

std::string LlvmIrEmitter::registerFormatRuntime(const std::string& elementType,
                                                 const FormatSpec& spec)
{
    const std::string key =
        elementType + "|" + std::to_string(spec.zeroPad) + "|" + std::to_string(spec.width) + "|" +
        (spec.precision ? std::to_string(*spec.precision) : "-") + "|" + std::string(1, spec.type);
    if (const auto it = formatFnByKey_.find(key); it != formatFnByKey_.end())
    {
        return it->second;
    }

    const int id = nextFormatId_++;
    const std::string idStr = std::to_string(id);
    const std::string fnName = "@axea.format." + idStr;
    formatFnByKey_[key] = fnName;

    const std::string widthText = spec.width > 0 ? std::to_string(spec.width) : "";
    const std::string zeroFlag = spec.zeroPad ? "0" : "";

    if (spec.type == 'b')
    {
        // No printf specifier for binary - hand-rolled: count significant
        // bits (treating 0 itself as one digit), pad to max(that count,
        // spec.width) with '0' (zeroPad) or ' ', then fill the actual
        // digits MSB-first using the already-known digit count to place
        // each one directly (no reverse-the-string step needed, unlike a
        // naive LSB-first digit loop). Always the full 64-bit pattern -
        // see registerFormatRuntime's own header comment for why i32
        // values are sign-extended first.
        const std::string vBits = elementType == "i64" ? "%v" : "%vBits";
        const std::string padChar = spec.zeroPad ? "48" : "32"; // '0' or ' '

        toStrRuntimeText_ << "\ndefine i8* " << fnName << "(" << elementType << " %v) {\n";
        toStrRuntimeText_ << "entry:\n";
        if (elementType != "i64")
        {
            toStrRuntimeText_ << "  %vBits = sext " << elementType << " %v to i64\n";
        }
        toStrRuntimeText_ << "  %countSlot = alloca i32\n";
        toStrRuntimeText_ << "  store i32 0, i32* %countSlot\n";
        toStrRuntimeText_ << "  %remSlot = alloca i64\n";
        toStrRuntimeText_ << "  store i64 " << vBits << ", i64* %remSlot\n";
        toStrRuntimeText_ << "  br label %countHdr\n";
        toStrRuntimeText_ << "countHdr:\n";
        toStrRuntimeText_ << "  %rem = load i64, i64* %remSlot\n";
        toStrRuntimeText_ << "  %remZero = icmp eq i64 %rem, 0\n";
        toStrRuntimeText_ << "  br i1 %remZero, label %countDone, label %countBody\n";
        toStrRuntimeText_ << "countBody:\n";
        toStrRuntimeText_ << "  %remNext = lshr i64 %rem, 1\n";
        toStrRuntimeText_ << "  store i64 %remNext, i64* %remSlot\n";
        toStrRuntimeText_ << "  %c0 = load i32, i32* %countSlot\n";
        toStrRuntimeText_ << "  %c1 = add i32 %c0, 1\n";
        toStrRuntimeText_ << "  store i32 %c1, i32* %countSlot\n";
        toStrRuntimeText_ << "  br label %countHdr\n";
        toStrRuntimeText_ << "countDone:\n";
        toStrRuntimeText_ << "  %rawCount = load i32, i32* %countSlot\n";
        toStrRuntimeText_ << "  %isZeroCount = icmp eq i32 %rawCount, 0\n";
        toStrRuntimeText_ << "  %digitCount = select i1 %isZeroCount, i32 1, i32 %rawCount\n";
        toStrRuntimeText_ << "  %useSpecWidth = icmp sgt i32 " << (spec.width > 0 ? spec.width : 0)
                          << ", %digitCount\n";
        toStrRuntimeText_ << "  %totalWidth = select i1 %useSpecWidth, i32 "
                          << (spec.width > 0 ? spec.width : 0) << ", i32 %digitCount\n";
        toStrRuntimeText_ << "  %padCount = sub i32 %totalWidth, %digitCount\n";
        toStrRuntimeText_ << "  %buf = call i8* @malloc(i64 80)\n";
        toStrRuntimeText_ << "  %padIdxSlot = alloca i32\n";
        toStrRuntimeText_ << "  store i32 0, i32* %padIdxSlot\n";
        toStrRuntimeText_ << "  br label %padHdr\n";
        toStrRuntimeText_ << "padHdr:\n";
        toStrRuntimeText_ << "  %padIdx = load i32, i32* %padIdxSlot\n";
        toStrRuntimeText_ << "  %padDone = icmp sge i32 %padIdx, %padCount\n";
        toStrRuntimeText_ << "  br i1 %padDone, label %padFin, label %padBody\n";
        toStrRuntimeText_ << "padBody:\n";
        toStrRuntimeText_ << "  %padPtr = getelementptr i8, i8* %buf, i32 %padIdx\n";
        toStrRuntimeText_ << "  store i8 " << padChar << ", i8* %padPtr\n";
        toStrRuntimeText_ << "  %padIdxNext = add i32 %padIdx, 1\n";
        toStrRuntimeText_ << "  store i32 %padIdxNext, i32* %padIdxSlot\n";
        toStrRuntimeText_ << "  br label %padHdr\n";
        toStrRuntimeText_ << "padFin:\n";
        toStrRuntimeText_ << "  %digitIdxSlot = alloca i32\n";
        toStrRuntimeText_ << "  store i32 0, i32* %digitIdxSlot\n";
        toStrRuntimeText_ << "  br label %digitHdr\n";
        toStrRuntimeText_ << "digitHdr:\n";
        toStrRuntimeText_ << "  %digitIdx = load i32, i32* %digitIdxSlot\n";
        toStrRuntimeText_ << "  %digitDone = icmp sge i32 %digitIdx, %digitCount\n";
        toStrRuntimeText_ << "  br i1 %digitDone, label %digitFin, label %digitBody\n";
        toStrRuntimeText_ << "digitBody:\n";
        toStrRuntimeText_ << "  %bitPosTmp = sub i32 %digitCount, 1\n";
        toStrRuntimeText_ << "  %bitPos32 = sub i32 %bitPosTmp, %digitIdx\n";
        toStrRuntimeText_ << "  %bitPos64 = zext i32 %bitPos32 to i64\n";
        toStrRuntimeText_ << "  %shifted = lshr i64 " << vBits << ", %bitPos64\n";
        toStrRuntimeText_ << "  %bit = and i64 %shifted, 1\n";
        toStrRuntimeText_ << "  %bit8 = trunc i64 %bit to i8\n";
        toStrRuntimeText_ << "  %ch = add i8 %bit8, 48\n";
        toStrRuntimeText_ << "  %destIdx = add i32 %padCount, %digitIdx\n";
        toStrRuntimeText_ << "  %destPtr = getelementptr i8, i8* %buf, i32 %destIdx\n";
        toStrRuntimeText_ << "  store i8 %ch, i8* %destPtr\n";
        toStrRuntimeText_ << "  %digitIdxNext = add i32 %digitIdx, 1\n";
        toStrRuntimeText_ << "  store i32 %digitIdxNext, i32* %digitIdxSlot\n";
        toStrRuntimeText_ << "  br label %digitHdr\n";
        toStrRuntimeText_ << "digitFin:\n";
        toStrRuntimeText_ << "  %termPtr = getelementptr i8, i8* %buf, i32 %totalWidth\n";
        toStrRuntimeText_ << "  store i8 0, i8* %termPtr\n";
        toStrRuntimeText_ << "  ret i8* %buf\n";
        toStrRuntimeText_ << "}\n";
        return fnName;
    }

    // Every other case reuses libc sprintf with a fixed, self-contained
    // format-string global (never routed through stringPtrConstant/
    // hoistString - see this function's own header comment for why).
    std::string fmtText = "%" + zeroFlag + widthText;
    std::string valueType;
    std::string valueRef = "%v";
    if (spec.type == 'x' || spec.type == 'X' || spec.type == 'o')
    {
        fmtText += "ll";
        fmtText += spec.type == 'o' ? 'o' : spec.type;
        valueType = "i64";
        if (elementType != "i64")
        {
            valueRef = "%vBits";
        }
    }
    else if (spec.precision.has_value())
    {
        fmtText += "." + std::to_string(*spec.precision) + "f";
        valueType = "double";
    }
    else
    {
        // Plain decimal width/zero-pad - native width (TypeChecker
        // already guarantees i32 or i64 here).
        fmtText += elementType == "i64" ? "lld" : "d";
        valueType = elementType;
    }

    const std::string globalName = "@axea.fmt.spec." + idStr;
    const std::size_t fmtLen = fmtText.size() + 1;
    toStrRuntimeText_ << "\n"
                      << globalName << " = private unnamed_addr constant [" << fmtLen
                      << " x i8] c\"" << fmtText << "\\00\"\n";

    toStrRuntimeText_ << "define i8* " << fnName << "(" << elementType << " %v) {\n";
    toStrRuntimeText_ << "entry:\n";
    if (valueRef == "%vBits")
    {
        toStrRuntimeText_ << "  %vBits = sext " << elementType << " %v to i64\n";
    }
    toStrRuntimeText_ << "  %buf = call i8* @malloc(i64 64)\n";
    toStrRuntimeText_ << "  %fmtPtr = getelementptr [" << fmtLen << " x i8], [" << fmtLen
                      << " x i8]* " << globalName << ", i64 0, i64 0\n";
    toStrRuntimeText_ << "  %ignored = call i32 (i8*, i8*, ...) @sprintf(i8* %buf, i8* %fmtPtr, "
                      << valueType << " " << valueRef << ")\n";
    toStrRuntimeText_ << "  ret i8* %buf\n";
    toStrRuntimeText_ << "}\n";
    return fnName;
}

std::string LlvmIrEmitter::registerAlignPadRuntime()
{
    const std::string fnName = "@axea.align.pad";
    if (alignPadRegistered_)
    {
        return fnName;
    }
    alignPadRegistered_ = true;

    // Registered exactly once (unlike registerFormatRuntime's own
    // per-(type,spec) memoization) - see this function's own header
    // comment in LlvmIrEmitter.hpp for why one shared function suffices.
    // Named registers throughout, since this is emitted at most once
    // total, the same "no numbering needed" reasoning
    // @axea.strbuf.new/.append/.finish already rely on.
    toStrRuntimeText_ << R"(
define i8* @axea.align.pad(i8* %text, i32 %width, i8 %alignCode) {
entry:
  %len64 = call i64 @strlen(i8* %text)
  %len = trunc i64 %len64 to i32
  %deficit = sub i32 %width, %len
  %needsPad = icmp sgt i32 %deficit, 0
  br i1 %needsPad, label %pad, label %nopad
nopad:
  ret i8* %text
pad:
  %isLeft = icmp eq i8 %alignCode, 60
  %isRight = icmp eq i8 %alignCode, 62
  %half = sdiv i32 %deficit, 2
  %leftPad0 = select i1 %isLeft, i32 0, i32 %half
  %leftPad = select i1 %isRight, i32 %deficit, i32 %leftPad0
  %rightPad = sub i32 %deficit, %leftPad
  %bufSize64 = zext i32 %width to i64
  %bufSize = add i64 %bufSize64, 1
  %buf = call i8* @malloc(i64 %bufSize)
  %lIdxSlot = alloca i32
  store i32 0, i32* %lIdxSlot
  br label %lHdr
lHdr:
  %lIdx = load i32, i32* %lIdxSlot
  %lDone = icmp sge i32 %lIdx, %leftPad
  br i1 %lDone, label %lFin, label %lBody
lBody:
  %lPtr = getelementptr i8, i8* %buf, i32 %lIdx
  store i8 32, i8* %lPtr
  %lNext = add i32 %lIdx, 1
  store i32 %lNext, i32* %lIdxSlot
  br label %lHdr
lFin:
  %cIdxSlot = alloca i32
  store i32 0, i32* %cIdxSlot
  br label %cHdr
cHdr:
  %cIdx = load i32, i32* %cIdxSlot
  %cDone = icmp sge i32 %cIdx, %len
  br i1 %cDone, label %cFin, label %cBody
cBody:
  %srcPtr = getelementptr i8, i8* %text, i32 %cIdx
  %byte = load i8, i8* %srcPtr
  %dstIdx = add i32 %leftPad, %cIdx
  %dstPtr = getelementptr i8, i8* %buf, i32 %dstIdx
  store i8 %byte, i8* %dstPtr
  %cNext = add i32 %cIdx, 1
  store i32 %cNext, i32* %cIdxSlot
  br label %cHdr
cFin:
  %rIdxSlot = alloca i32
  store i32 0, i32* %rIdxSlot
  br label %rHdr
rHdr:
  %rIdx = load i32, i32* %rIdxSlot
  %rDone = icmp sge i32 %rIdx, %rightPad
  br i1 %rDone, label %rFin, label %rBody
rBody:
  %rDstIdx0 = add i32 %leftPad, %len
  %rDstIdx = add i32 %rDstIdx0, %rIdx
  %rPtr = getelementptr i8, i8* %buf, i32 %rDstIdx
  store i8 32, i8* %rPtr
  %rNext = add i32 %rIdx, 1
  store i32 %rNext, i32* %rIdxSlot
  br label %rHdr
rFin:
  %termPtr = getelementptr i8, i8* %buf, i32 %width
  store i8 0, i8* %termPtr
  ret i8* %buf
}
)";
    return fnName;
}

std::string LlvmIrEmitter::registerDebugQuoteRuntime()
{
    const std::string fnName = "@axea.debug.quote_str";
    if (debugQuoteRegistered_)
    {
        return fnName;
    }
    debugQuoteRegistered_ = true;

    // Registered exactly once, named registers throughout (see
    // registerAlignPadRuntime's own identical reasoning) - mallocs
    // len+3 bytes ('"' + text + '"' + '\0'), copies `text`'s own bytes
    // unchanged (no escaping of embedded quotes/backslashes - see
    // docs/language/0058-debug-formatting.md's own Known Imprecision).
    toStrRuntimeText_ << R"(
define i8* @axea.debug.quote_str(i8* %text) {
entry:
  %len64 = call i64 @strlen(i8* %text)
  %len = trunc i64 %len64 to i32
  %bufSize0 = add i32 %len, 3
  %bufSize64 = zext i32 %bufSize0 to i64
  %buf = call i8* @malloc(i64 %bufSize64)
  store i8 34, i8* %buf
  %idxSlot = alloca i32
  store i32 0, i32* %idxSlot
  br label %hdr
hdr:
  %idx = load i32, i32* %idxSlot
  %done = icmp sge i32 %idx, %len
  br i1 %done, label %fin, label %body
body:
  %srcPtr = getelementptr i8, i8* %text, i32 %idx
  %byte = load i8, i8* %srcPtr
  %dstIdx = add i32 %idx, 1
  %dstPtr = getelementptr i8, i8* %buf, i32 %dstIdx
  store i8 %byte, i8* %dstPtr
  %idxNext = add i32 %idx, 1
  store i32 %idxNext, i32* %idxSlot
  br label %hdr
fin:
  %closeIdx = add i32 %len, 1
  %closePtr = getelementptr i8, i8* %buf, i32 %closeIdx
  store i8 34, i8* %closePtr
  %termIdx = add i32 %len, 2
  %termPtr = getelementptr i8, i8* %buf, i32 %termIdx
  store i8 0, i8* %termPtr
  ret i8* %buf
}
)";
    return fnName;
}

std::string LlvmIrEmitter::stringifyValueDebug(int reg, FunctionContext& fctx)
{
    const std::string type = typeOf(reg, fctx);
    if (type == "i8*" || type == "{i32, i8*}*")
    {
        const std::string rawPtr = resolveStrPtrOfType(type, ref(reg, fctx), fctx);
        const std::string fnName = registerDebugQuoteRuntime();
        const int destReg = allocateRegister(fctx);
        *fctx.out << "  %" << destReg << " = call i8* " << fnName << "(i8* " << rawPtr << ")\n";
        return "%" + std::to_string(destReg);
    }
    return stringifyValue(reg, fctx);
}

std::string LlvmIrEmitter::registerOptionalToStrRuntime(const std::string& optionalType)
{
    if (const auto it = optionalToStrFnByOptionalType_.find(optionalType);
        it != optionalToStrFnByOptionalType_.end())
    {
        return it->second;
    }

    const std::string payloadType = optionalPayloadType(optionalType);
    // "%axea.Optional.<id>" - the id is everything after the last '.'.
    const std::string idStr = optionalType.substr(optionalType.rfind('.') + 1);
    const std::string fnName = "@axea.optional." + idStr + ".to_str";
    optionalToStrFnByOptionalType_[optionalType] = fnName;

    std::string payloadToStrCall;
    if (payloadType == "i32")
    {
        registerI32ToStrRuntime();
        payloadToStrCall = "  %payloadStr = call i8* @axea.i32.to_str(i32 %payload)\n";
    }
    else if (payloadType == "i64")
    {
        registerI64ToStrRuntime();
        payloadToStrCall = "  %payloadStr = call i8* @axea.i64.to_str(i64 %payload)\n";
    }
    else if (payloadType == "double")
    {
        registerF64ToStrRuntime();
        payloadToStrCall = "  %payloadStr = call i8* @axea.f64.to_str(double %payload)\n";
    }
    else if (payloadType == "i1")
    {
        registerBoolToStrRuntime();
        payloadToStrCall = "  %payloadStr = call i8* @axea.bool.to_str(i1 %payload)\n";
    }
    else
    {
        // Some(x)/None themselves accept any payload type (see
        // docs/language/0052-optional.md) - only *printing* one is this
        // narrow, the same T-restricted set `.parse<T>()` itself produces.
        throw std::runtime_error(
            "printing Optional<T> is only supported for T in {i32, i64, f64, bool} this phase, "
            "found payload type " +
            payloadType);
    }

    if (!optionalToStrGlobalsRegistered_)
    {
        optionalToStrGlobalsRegistered_ = true;
        // "Some(%s)\00" is 8 characters + terminator; "None\00" is 4 + 1.
        toStrRuntimeText_ << R"(
@axea.fmt.some = private unnamed_addr constant [9 x i8] c"Some(%s)\00"
@axea.str.none = private unnamed_addr constant [5 x i8] c"None\00"
)";
    }

    // A real sprintf call, same as every other *ToStrRuntime above - 48
    // bytes comfortably covers "Some(" + the longest payload rendering
    // (32 bytes, f64's own bound) + ")" + terminator.
    toStrRuntimeText_ << R"(
define i8* @axea.optional.)"
                      << idStr << "." << R"(to_str()" << optionalType << R"( %v) {
entry:
  %hasValue = extractvalue )"
                      << optionalType << R"( %v, 0
  br i1 %hasValue, label %issome, label %isnone
issome:
  %payload = extractvalue )"
                      << optionalType << R"( %v, 1
)" << payloadToStrCall << R"(  %buf = call i8* @malloc(i64 48)
  %fmtPtr = getelementptr [9 x i8], [9 x i8]* @axea.fmt.some, i64 0, i64 0
  %ignored = call i32 (i8*, i8*, ...) @sprintf(i8* %buf, i8* %fmtPtr, i8* %payloadStr)
  ret i8* %buf
isnone:
  %nonePtr = getelementptr [5 x i8], [5 x i8]* @axea.str.none, i64 0, i64 0
  ret i8* %nonePtr
}
)";
    return fnName;
}

std::string LlvmIrEmitter::registerResultToStrRuntime(const std::string& resultType)
{
    if (const auto it = resultToStrFnByResultType_.find(resultType);
        it != resultToStrFnByResultType_.end())
    {
        return it->second;
    }

    const std::string okType = resultOkPayloadType(resultType);
    const std::string errType = resultErrPayloadType(resultType);
    // "%axea.Result.<id>" - the id is everything after the last '.'.
    const std::string idStr = resultType.substr(resultType.rfind('.') + 1);
    const std::string fnName = "@axea.result." + idStr + ".to_str";
    resultToStrFnByResultType_[resultType] = fnName;

    // Shared by both the Ok and Err branches below - each payload type is
    // independently restricted to i32/i64/f64/bool, the same set
    // registerOptionalToStrRuntime's own single payload already is (see
    // that function's own comment for why: Ok(x)/Err(e) themselves accept
    // any payload type, only *printing* one is this narrow).
    // Distinct destination register names per branch (`okPayloadStr`/
    // `errPayloadStr`, not a shared `payloadStr`) - both branches live in
    // the same function, and LLVM requires every named value defined
    // exactly once *per function*, not merely once per basic block, even
    // when (as here) the two blocks are mutually exclusive at runtime.
    auto payloadToStrCall = [this](const std::string& payloadType,
                                   const char* payloadVar,
                                   const char* destVar) -> std::string
    {
        if (payloadType == "i32")
        {
            registerI32ToStrRuntime();
            return "  %" + std::string(destVar) + " = call i8* @axea.i32.to_str(i32 %" +
                   payloadVar + ")\n";
        }
        if (payloadType == "i64")
        {
            registerI64ToStrRuntime();
            return "  %" + std::string(destVar) + " = call i8* @axea.i64.to_str(i64 %" +
                   payloadVar + ")\n";
        }
        if (payloadType == "double")
        {
            registerF64ToStrRuntime();
            return "  %" + std::string(destVar) + " = call i8* @axea.f64.to_str(double %" +
                   payloadVar + ")\n";
        }
        if (payloadType == "i1")
        {
            registerBoolToStrRuntime();
            return "  %" + std::string(destVar) + " = call i8* @axea.bool.to_str(i1 %" +
                   payloadVar + ")\n";
        }
        throw std::runtime_error(
            "printing Result<T,E> is only supported for T,E in {i32, i64, f64, bool} this "
            "phase, found payload type " +
            payloadType);
    };
    const std::string okToStrCall = payloadToStrCall(okType, "okPayload", "okPayloadStr");
    const std::string errToStrCall = payloadToStrCall(errType, "errPayload", "errPayloadStr");

    if (!resultToStrGlobalsRegistered_)
    {
        resultToStrGlobalsRegistered_ = true;
        // "Ok(%s)\00" is 6 characters + terminator; "Err(%s)\00" is 7 + 1.
        toStrRuntimeText_ << R"(
@axea.fmt.ok = private unnamed_addr constant [7 x i8] c"Ok(%s)\00"
@axea.fmt.err = private unnamed_addr constant [8 x i8] c"Err(%s)\00"
)";
    }

    // A real sprintf call, same as registerOptionalToStrRuntime's own - 48
    // bytes comfortably covers "Err(" + the longest payload rendering (32
    // bytes, f64's own bound) + ")" + terminator.
    toStrRuntimeText_ << R"(
define i8* @axea.result.)"
                      << idStr << "." << R"(to_str()" << resultType << R"( %v) {
entry:
  %isOk = extractvalue )"
                      << resultType << R"( %v, 0
  br i1 %isOk, label %isok, label %iserr
isok:
  %okPayload = extractvalue )"
                      << resultType << R"( %v, 1
)" << okToStrCall << R"(  %okBuf = call i8* @malloc(i64 48)
  %okFmtPtr = getelementptr [7 x i8], [7 x i8]* @axea.fmt.ok, i64 0, i64 0
  %okIgnored = call i32 (i8*, i8*, ...) @sprintf(i8* %okBuf, i8* %okFmtPtr, i8* %okPayloadStr)
  ret i8* %okBuf
iserr:
  %errPayload = extractvalue )"
                      << resultType << R"( %v, 2
)" << errToStrCall << R"(  %errBuf = call i8* @malloc(i64 48)
  %errFmtPtr = getelementptr [8 x i8], [8 x i8]* @axea.fmt.err, i64 0, i64 0
  %errIgnored = call i32 (i8*, i8*, ...) @sprintf(i8* %errBuf, i8* %errFmtPtr, i8* %errPayloadStr)
  ret i8* %errBuf
}
)";
    return fnName;
}

void LlvmIrEmitter::registerStrbufRuntime()
{
    if (strbufRegistered_)
    {
        return;
    }
    strbufRegistered_ = true;

    // A small growable string buffer - `{i32 len, i32 cap, i8* data}*`,
    // the exact same header shape Buffer itself uses (see
    // docs/language/0043-buffer.md), but a fully independent, self-
    // contained implementation: these need to be callable from any
    // hand-written stringify function below, not tied to a specific
    // already-being-emitted function's own live register/label numbering
    // the way emitBufferNew/Append/ensureBufferCapacity's inline codegen
    // is (see docs/language/0054-collection-printing.md).
    toStrRuntimeText_ << R"(
define {i32, i32, i8*}* @axea.strbuf.new() {
entry:
  %hdrSizePtr = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* null, i32 1
  %hdrSizeInt = ptrtoint {i32, i32, i8*}* %hdrSizePtr to i64
  %rawHdr = call i8* @malloc(i64 %hdrSizeInt)
  %hdr = bitcast i8* %rawHdr to {i32, i32, i8*}*
  %initData = call i8* @malloc(i64 1)
  store i8 0, i8* %initData
  %lenPtr = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* %hdr, i32 0, i32 0
  store i32 0, i32* %lenPtr
  %capPtr = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* %hdr, i32 0, i32 1
  store i32 1, i32* %capPtr
  %dataPtrPtr = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* %hdr, i32 0, i32 2
  store i8* %initData, i8** %dataPtrPtr
  ret {i32, i32, i8*}* %hdr
}

define void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* %text) {
entry:
  %textLen64 = call i64 @strlen(i8* %text)
  %textLen = trunc i64 %textLen64 to i32
  %lenPtr = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* %buf, i32 0, i32 0
  %oldLen = load i32, i32* %lenPtr
  %newLen = add i32 %oldLen, %textLen
  %needed = add i32 %newLen, 1
  %capPtr = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* %buf, i32 0, i32 1
  %cap = load i32, i32* %capPtr
  %needGrow = icmp sgt i32 %needed, %cap
  br i1 %needGrow, label %grow, label %afterGrow
grow:
  %doubled = mul i32 %cap, 2
  %needsMore = icmp sgt i32 %needed, %doubled
  %newCap = select i1 %needsMore, i32 %needed, i32 %doubled
  %newCap64 = zext i32 %newCap to i64
  %newData = call i8* @malloc(i64 %newCap64)
  %dataPtrPtr1 = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* %buf, i32 0, i32 2
  %oldData = load i8*, i8** %dataPtrPtr1
  %oldCopyIdxSlot = alloca i32
  store i32 0, i32* %oldCopyIdxSlot
  br label %oldCopyHdr
oldCopyHdr:
  %oldCopyIdx = load i32, i32* %oldCopyIdxSlot
  %oldCopyDone = icmp sge i32 %oldCopyIdx, %oldLen
  br i1 %oldCopyDone, label %oldCopyFin, label %oldCopyBody
oldCopyBody:
  %srcPtr = getelementptr i8, i8* %oldData, i32 %oldCopyIdx
  %srcByte = load i8, i8* %srcPtr
  %dstPtr = getelementptr i8, i8* %newData, i32 %oldCopyIdx
  store i8 %srcByte, i8* %dstPtr
  %oldCopyIdxNext = add i32 %oldCopyIdx, 1
  store i32 %oldCopyIdxNext, i32* %oldCopyIdxSlot
  br label %oldCopyHdr
oldCopyFin:
  store i8* %newData, i8** %dataPtrPtr1
  store i32 %newCap, i32* %capPtr
  br label %afterGrow
afterGrow:
  %dataPtrPtr2 = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* %buf, i32 0, i32 2
  %data = load i8*, i8** %dataPtrPtr2
  %appendIdxSlot = alloca i32
  store i32 0, i32* %appendIdxSlot
  br label %appendHdr
appendHdr:
  %appendIdx = load i32, i32* %appendIdxSlot
  %appendDone = icmp sge i32 %appendIdx, %textLen
  br i1 %appendDone, label %appendFin, label %appendBody
appendBody:
  %textSrcPtr = getelementptr i8, i8* %text, i32 %appendIdx
  %textByte = load i8, i8* %textSrcPtr
  %destIdx = add i32 %oldLen, %appendIdx
  %textDstPtr = getelementptr i8, i8* %data, i32 %destIdx
  store i8 %textByte, i8* %textDstPtr
  %appendIdxNext = add i32 %appendIdx, 1
  store i32 %appendIdxNext, i32* %appendIdxSlot
  br label %appendHdr
appendFin:
  %termPtr = getelementptr i8, i8* %data, i32 %newLen
  store i8 0, i8* %termPtr
  store i32 %newLen, i32* %lenPtr
  ret void
}

define i8* @axea.strbuf.finish({i32, i32, i8*}* %buf) {
entry:
  %dataPtrPtr = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* %buf, i32 0, i32 2
  %data = load i8*, i8** %dataPtrPtr
  ret i8* %data
}
)";

    // A standalone transcription of encodeCharUtf8's own UTF-8 encoding
    // (see docs/language/0044-char.md) - same bit-for-bit shift/mask/tag
    // values per byte-length range, hand-verified against that live-fctx
    // version's own exact `encodeByte(shift, mask, tag)` calls.
    toStrRuntimeText_ << R"(
define i8* @axea.char.to_str(i24 %cp) {
entry:
  %buf = call i8* @malloc(i64 5)
  %isOne = icmp ule i24 %cp, 127
  br i1 %isOne, label %len1, label %check2
len1:
  %b0_1 = trunc i24 %cp to i8
  %p0_1 = getelementptr i8, i8* %buf, i32 0
  store i8 %b0_1, i8* %p0_1
  %pnull_1 = getelementptr i8, i8* %buf, i32 1
  store i8 0, i8* %pnull_1
  br label %done
check2:
  %isTwo = icmp ule i24 %cp, 2047
  br i1 %isTwo, label %len2, label %check3
len2:
  %hi2 = lshr i24 %cp, 6
  %hi2t = or i24 %hi2, 192
  %hi2b = trunc i24 %hi2t to i8
  %lo2 = and i24 %cp, 63
  %lo2t = or i24 %lo2, 128
  %lo2b = trunc i24 %lo2t to i8
  %p0_2 = getelementptr i8, i8* %buf, i32 0
  store i8 %hi2b, i8* %p0_2
  %p1_2 = getelementptr i8, i8* %buf, i32 1
  store i8 %lo2b, i8* %p1_2
  %pnull_2 = getelementptr i8, i8* %buf, i32 2
  store i8 0, i8* %pnull_2
  br label %done
check3:
  %isThree = icmp ule i24 %cp, 65535
  br i1 %isThree, label %len3, label %len4
len3:
  %hi3 = lshr i24 %cp, 12
  %hi3t = or i24 %hi3, 224
  %hi3b = trunc i24 %hi3t to i8
  %mid3 = lshr i24 %cp, 6
  %mid3m = and i24 %mid3, 63
  %mid3t = or i24 %mid3m, 128
  %mid3b = trunc i24 %mid3t to i8
  %lo3 = and i24 %cp, 63
  %lo3t = or i24 %lo3, 128
  %lo3b = trunc i24 %lo3t to i8
  %p0_3 = getelementptr i8, i8* %buf, i32 0
  store i8 %hi3b, i8* %p0_3
  %p1_3 = getelementptr i8, i8* %buf, i32 1
  store i8 %mid3b, i8* %p1_3
  %p2_3 = getelementptr i8, i8* %buf, i32 2
  store i8 %lo3b, i8* %p2_3
  %pnull_3 = getelementptr i8, i8* %buf, i32 3
  store i8 0, i8* %pnull_3
  br label %done
len4:
  %hi4 = lshr i24 %cp, 18
  %hi4t = or i24 %hi4, 240
  %hi4b = trunc i24 %hi4t to i8
  %mid4a = lshr i24 %cp, 12
  %mid4am = and i24 %mid4a, 63
  %mid4at = or i24 %mid4am, 128
  %mid4ab = trunc i24 %mid4at to i8
  %mid4b = lshr i24 %cp, 6
  %mid4bm = and i24 %mid4b, 63
  %mid4bt = or i24 %mid4bm, 128
  %mid4bb = trunc i24 %mid4bt to i8
  %lo4 = and i24 %cp, 63
  %lo4t = or i24 %lo4, 128
  %lo4b = trunc i24 %lo4t to i8
  %p0_4 = getelementptr i8, i8* %buf, i32 0
  store i8 %hi4b, i8* %p0_4
  %p1_4 = getelementptr i8, i8* %buf, i32 1
  store i8 %mid4ab, i8* %p1_4
  %p2_4 = getelementptr i8, i8* %buf, i32 2
  store i8 %mid4bb, i8* %p2_4
  %p3_4 = getelementptr i8, i8* %buf, i32 3
  store i8 %lo4b, i8* %p3_4
  %pnull_4 = getelementptr i8, i8* %buf, i32 4
  store i8 0, i8* %pnull_4
  br label %done
done:
  ret i8* %buf
}
)";
}

std::string LlvmIrEmitter::emitElementToStrCall(const std::string& elementType,
                                                const std::string& valueRef,
                                                std::ostringstream& body,
                                                int& nextTmp)
{
    if (elementType == "i32")
    {
        registerI32ToStrRuntime();
        const std::string dest = "%t" + std::to_string(nextTmp++);
        body << "  " << dest << " = call i8* @axea.i32.to_str(i32 " << valueRef << ")\n";
        return dest;
    }
    if (elementType == "i64")
    {
        registerI64ToStrRuntime();
        const std::string dest = "%t" + std::to_string(nextTmp++);
        body << "  " << dest << " = call i8* @axea.i64.to_str(i64 " << valueRef << ")\n";
        return dest;
    }
    if (elementType == "double")
    {
        registerF64ToStrRuntime();
        const std::string dest = "%t" + std::to_string(nextTmp++);
        body << "  " << dest << " = call i8* @axea.f64.to_str(double " << valueRef << ")\n";
        return dest;
    }
    if (elementType == "i1")
    {
        registerBoolToStrRuntime();
        const std::string dest = "%t" + std::to_string(nextTmp++);
        body << "  " << dest << " = call i8* @axea.bool.to_str(i1 " << valueRef << ")\n";
        return dest;
    }
    if (isCharType(elementType))
    {
        registerStrbufRuntime(); // also registers @axea.char.to_str
        const std::string dest = "%t" + std::to_string(nextTmp++);
        body << "  " << dest << " = call i8* @axea.char.to_str(i24 " << valueRef << ")\n";
        return dest;
    }
    if (isOptionalType(elementType))
    {
        const std::string fnName = registerOptionalToStrRuntime(elementType);
        const std::string dest = "%t" + std::to_string(nextTmp++);
        body << "  " << dest << " = call i8* " << fnName << "(" << elementType << " " << valueRef
             << ")\n";
        return dest;
    }
    if (isResultType(elementType))
    {
        const std::string fnName = registerResultToStrRuntime(elementType);
        const std::string dest = "%t" + std::to_string(nextTmp++);
        body << "  " << dest << " = call i8* " << fnName << "(" << elementType << " " << valueRef
             << ")\n";
        return dest;
    }
    if (isNamedStructPointerType(elementType))
    {
        // Pre-registered, unconditionally, by emitStructToStringHelpers -
        // safe to reference by fixed name regardless of registration
        // order (an ordinary function call, unlike Optional<T>'s own
        // by-value named-type forward-reference constraint - see
        // docs/language/0052-optional.md).
        const std::string structName = structNameFromPointerType(elementType);
        const std::string dest = "%t" + std::to_string(nextTmp++);
        body << "  " << dest << " = call i8* @axea.tostring." << structName << "(" << elementType
             << " " << valueRef << ")\n";
        return dest;
    }
    if (elementType == "i8*")
    {
        // str - already an i8*, nothing to convert.
        return valueRef;
    }
    if (elementType == "{i32, i8*}*")
    {
        // String - extract its own data pointer (field 1), same
        // resolveStrPtrOfType logic every live-fctx caller shares, done
        // inline here since that helper also needs a live fctx.
        const std::string ptrPtr = "%t" + std::to_string(nextTmp++);
        body << "  " << ptrPtr << " = getelementptr {i32, i8*}, {i32, i8*}* " << valueRef
             << ", i32 0, i32 1\n";
        const std::string dest = "%t" + std::to_string(nextTmp++);
        body << "  " << dest << " = load i8*, i8** " << ptrPtr << "\n";
        return dest;
    }
    if (isPointerType(elementType))
    {
        // A raw `*T` field (see docs/language/0006-generics.md's own List<T> port follow-up) -
        // matches emitStructToStringHelpers' own per-field print dispatch and the top-level
        // print dispatch's identical fallback for a bare pointer value (unsafe Milestone 1): the
        // same "()" unit sentinel, not an attempt to stringify whatever it points to.
        return stringPtrConstant("()");
    }
    // A nested collection element - recurse.
    const std::string fnName = registerCollectionToStrRuntime(elementType);
    const std::string dest = "%t" + std::to_string(nextTmp++);
    body << "  " << dest << " = call i8* " << fnName << "(" << elementType << " " << valueRef
         << ")\n";
    return dest;
}

void LlvmIrEmitter::emitStructToStringHelpers(const IrProgram& program)
{
    if (program.structs.empty())
    {
        return;
    }
    registerStrbufRuntime();

    const std::string separator = stringPtrConstant(", ");
    const std::string closeBrace = stringPtrConstant(" }");

    for (const auto& [name, fields] : program.structs)
    {
        const std::string structType = "%" + name;
        const std::string pointerType = structType + "*";

        // Display trait dispatch (see docs/language/0062-display-
        // trait.md) - when `name` has a registered `impl Display`,
        // `@axea.tostring.<name>` calls the user's own already-compiled
        // "format" function (an entirely ordinary function by this
        // point, emitted by emitFunction like any other) into a fresh
        // `@axea.strbuf` - the exact same `{i32, i32, i8*}*` header
        // shape `llvmType("Buffer")` itself produces, confirmed, not
        // assumed, so the strbuf pointer is passed to `format`'s own
        // `Buffer`-typed parameter with no bitcast needed at all - then
        // returns `@axea.strbuf.finish`'s own result directly, instead
        // of building the default `Name { field: value, ... }` text
        // below.
        if (const auto implIt = program.displayImpls.find(name);
            implIt != program.displayImpls.end())
        {
            std::ostringstream displayBody;
            displayBody << "define i8* @axea.tostring." << name << "(" << pointerType << " %v) {\n";
            displayBody << "entry:\n";
            displayBody << "  %buf = call {i32, i32, i8*}* @axea.strbuf.new()\n";
            displayBody << "  call void @" << implIt->second << "(" << pointerType
                        << " %v, {i32, i32, i8*}* %buf)\n";
            displayBody << "  %result = call i8* @axea.strbuf.finish({i32, i32, i8*}* %buf)\n";
            displayBody << "  ret i8* %result\n";
            displayBody << "}\n";
            toStrRuntimeText_ << "\n" << displayBody.str();
            continue;
        }

        // `enum` (see docs/language/0064-enums.md) - `name` is really a flattened
        // `{i32 tag, <every variant's own fields concatenated>}` struct (see
        // IrGenerator::generate's own comment). A real, tag-aware stringifier: switch on the
        // tag, then for the matched variant build "VariantName(field0, field1, ...)" (bare
        // "VariantName" for a no-payload variant) - reusing emitElementToStrCall per field, the
        // exact same generic "stringify any value" dispatch the default struct-to-string body
        // just below already uses, so an enum field can be any printable type (str/struct/
        // collection/Optional/Result included, not restricted the way Result<T,E>'s own
        // printing is - see that doc's own Known Imprecision for why *its* restriction exists
        // and doesn't apply here).
        if (const auto enumIt = program.enums.find(name); enumIt != program.enums.end())
        {
            registerStrbufRuntime();
            std::ostringstream body;
            int nextTmp = 0;
            body << "define i8* @axea.tostring." << name << "(" << pointerType << " %v) {\n";
            body << "entry:\n";
            body << "  %tagPtr = getelementptr " << structType << ", " << pointerType
                 << " %v, i32 0, i32 0\n";
            body << "  %tag = load i32, i32* %tagPtr\n";
            body << "  switch i32 %tag, label %axea_unreachable [\n";
            for (std::size_t vi = 0; vi < enumIt->second.size(); ++vi)
            {
                body << "    i32 " << vi << ", label %variant" << vi << "\n";
            }
            body << "  ]\n";

            for (std::size_t vi = 0; vi < enumIt->second.size(); ++vi)
            {
                const auto& [variantName, fieldCount] = enumIt->second[vi];
                body << "variant" << vi << ":\n";
                if (fieldCount == 0)
                {
                    body << "  ret i8* " << stringPtrConstant(variantName) << "\n";
                    continue;
                }
                const std::string bufVar = "%buf" + std::to_string(vi);
                body << "  " << bufVar << " = call {i32, i32, i8*}* @axea.strbuf.new()\n";
                body << "  call void @axea.strbuf.append({i32, i32, i8*}* " << bufVar << ", i8* "
                     << stringPtrConstant(variantName + "(") << ")\n";
                for (int fi = 0; fi < fieldCount; ++fi)
                {
                    if (fi > 0)
                    {
                        body << "  call void @axea.strbuf.append({i32, i32, i8*}* " << bufVar
                             << ", i8* " << stringPtrConstant(", ") << ")\n";
                    }
                    const auto [fieldIndex, fieldLlvmType] =
                        fieldIndexAndType(name, variantName + "_" + std::to_string(fi));
                    const std::string fieldPtr =
                        "%v" + std::to_string(vi) + "fptr" + std::to_string(fi);
                    const std::string fieldVal =
                        "%v" + std::to_string(vi) + "fval" + std::to_string(fi);
                    body << "  " << fieldPtr << " = getelementptr " << structType << ", "
                         << pointerType << " %v, i32 0, i32 " << fieldIndex << "\n";
                    body << "  " << fieldVal << " = load " << fieldLlvmType << ", " << fieldLlvmType
                         << "* " << fieldPtr << "\n";
                    const std::string fieldStr =
                        emitElementToStrCall(fieldLlvmType, fieldVal, body, nextTmp);
                    body << "  call void @axea.strbuf.append({i32, i32, i8*}* " << bufVar
                         << ", i8* " << fieldStr << ")\n";
                }
                body << "  call void @axea.strbuf.append({i32, i32, i8*}* " << bufVar << ", i8* "
                     << stringPtrConstant(")") << ")\n";
                body << "  %result" << vi << " = call i8* @axea.strbuf.finish({i32, i32, i8*}* "
                     << bufVar << ")\n";
                body << "  ret i8* %result" << vi << "\n";
            }
            body << "axea_unreachable:\n";
            body << "  unreachable\n";
            body << "}\n";
            toStrRuntimeText_ << "\n" << body.str();
            continue;
        }

        const std::string openBrace = stringPtrConstant(name + " { ");

        std::ostringstream body;
        int nextTmp = 0;
        body << "define i8* @axea.tostring." << name << "(" << pointerType << " %v) {\n";
        body << "entry:\n";
        body << "  %buf = call {i32, i32, i8*}* @axea.strbuf.new()\n";
        body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << openBrace << ")\n";

        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            const auto& [fieldName, fieldTypeName] = fields[i];
            if (i > 0)
            {
                body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << separator
                     << ")\n";
            }
            const std::string fieldLabel = stringPtrConstant(fieldName + ": ");
            body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << fieldLabel
                 << ")\n";

            const std::string fieldLlvmType = llvmType(fieldTypeName);
            const std::string fieldPtr = "%fptr" + std::to_string(i);
            const std::string fieldVal = "%fval" + std::to_string(i);
            body << "  " << fieldPtr << " = getelementptr " << structType << ", " << pointerType
                 << " %v, i32 0, i32 " << i << "\n";
            body << "  " << fieldVal << " = load " << fieldLlvmType << ", " << fieldLlvmType << "* "
                 << fieldPtr << "\n";
            const std::string fieldStr =
                emitElementToStrCall(fieldLlvmType, fieldVal, body, nextTmp);
            body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << fieldStr
                 << ")\n";
        }

        body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << closeBrace
             << ")\n";
        body << "  %result = call i8* @axea.strbuf.finish({i32, i32, i8*}* %buf)\n";
        body << "  ret i8* %result\n";
        body << "}\n";

        toStrRuntimeText_ << "\n" << body.str();
    }
}

void LlvmIrEmitter::emitStructRefcountHelpers(const IrProgram& program)
{
    // Move semantics (structs/enums only) - single ownership is proven at compile time (see
    // RegionChecker's move-checking) for every *plain* struct/enum, so those need no runtime
    // refcount at all: @axea.drop.<Name> unconditionally recurses into owned fields and frees.
    // The one exception is Shared<T> (a synthetic "Shared.<T>" struct - see
    // IrGenerator::registerSharedType), the move-semantics work's own explicit, opt-in escape
    // hatch for genuine multi-owner sharing - the *only* place left in the whole backend that
    // still needs a real runtime counter, since a `.clone()` call deliberately creates a second,
    // independently-valid reference to the same allocation. For a "Shared."-prefixed name this
    // generates the classic counted pair (@axea.retain.<Name> increments; @axea.release.<Name>
    // decrements and only frees at zero) instead of the plain unconditional @axea.drop.<Name>.
    for (const auto& [name, fields] : program.structs)
    {
        const bool isShared = name.starts_with("Shared.");
        const std::string structType = "%" + name;
        const std::string pointerType = structType + "*";
        const std::string dropFnName = isShared ? "axea.release." + name : "axea.drop." + name;

        if (isShared)
        {
            std::ostringstream retainBody;
            retainBody << "define void @axea.retain." << name << "(" << pointerType << " %v) {\n";
            retainBody << "entry:\n";
            retainBody << "  %isnull = icmp eq " << pointerType << " %v, null\n";
            retainBody << "  br i1 %isnull, label %done, label %body\n";
            retainBody << "body:\n";
            retainBody << "  %rcPtr = getelementptr " << structType << ", " << pointerType
                       << " %v, i32 0, i32 0\n";
            retainBody << "  %rc = load i32, i32* %rcPtr\n";
            retainBody << "  %rc2 = add i32 %rc, 1\n";
            retainBody << "  store i32 %rc2, i32* %rcPtr\n";
            retainBody << "  br label %done\n";
            retainBody << "done:\n";
            retainBody << "  ret void\n";
            retainBody << "}\n";
            structRefcountRuntimeText_ << "\n" << retainBody.str();
        }

        std::ostringstream dropBody;
        dropBody << "define void @" << dropFnName << "(" << pointerType << " %v) {\n";
        dropBody << "entry:\n";
        dropBody << "  %isnull = icmp eq " << pointerType << " %v, null\n";
        if (isShared)
        {
            // registerSharedType always puts refcount at field index 0, value at index 1 - a
            // real, ordinary field, not a hidden one, unlike the old pre-Part-B scheme (see that
            // function's own comment).
            dropBody << "  br i1 %isnull, label %done, label %body\n";
            dropBody << "body:\n";
            dropBody << "  %rcPtr = getelementptr " << structType << ", " << pointerType
                     << " %v, i32 0, i32 0\n";
            dropBody << "  %rc = load i32, i32* %rcPtr\n";
            dropBody << "  %rc2 = sub i32 %rc, 1\n";
            dropBody << "  store i32 %rc2, i32* %rcPtr\n";
            dropBody << "  %iszero = icmp eq i32 %rc2, 0\n";
            dropBody << "  br i1 %iszero, label %free, label %done\n";
        }
        else
        {
            dropBody << "  br i1 %isnull, label %done, label %free\n";
        }
        dropBody << "free:\n";

        int nextTmp = 0;
        // A field whose own Axea type (union-demangled - see mangleUnionTypeName) is itself a
        // registered struct/enum gets a recursive release; every other field type (primitive, a
        // heap type this phase doesn't cover, or an Optional<Struct>/Result<Struct,E> payload)
        // is skipped by construction - left un-decremented, matching this codebase's existing
        // "leak, don't free" policy for what's out of scope.
        if (const auto enumIt = program.enums.find(name); enumIt != program.enums.end())
        {
            // `name` is really a flattened `{i32 tag, <every variant's own fields
            // concatenated>}` struct (see IrGenerator::generate) - tag-gated so only the
            // *active* variant's own fields are ever read (an inactive variant's own fields are
            // undef, never safe to touch).
            dropBody << "  %tagPtr = getelementptr " << structType << ", " << pointerType
                     << " %v, i32 0, i32 0\n";
            dropBody << "  %tag = load i32, i32* %tagPtr\n";
            dropBody << "  switch i32 %tag, label %freeDone [\n";
            for (std::size_t vi = 0; vi < enumIt->second.size(); ++vi)
            {
                dropBody << "    i32 " << vi << ", label %variant" << vi << "\n";
            }
            dropBody << "  ]\n";

            for (std::size_t vi = 0; vi < enumIt->second.size(); ++vi)
            {
                const auto& [variantName, fieldCount] = enumIt->second[vi];
                dropBody << "variant" << vi << ":\n";
                for (int fi = 0; fi < fieldCount; ++fi)
                {
                    const auto [fieldIndex, fieldLlvmType] =
                        fieldIndexAndType(name, variantName + "_" + std::to_string(fi));
                    const std::string fieldAxeaType =
                        mangleUnionTypeName(fields[fieldIndex].second);
                    if (!structs_.contains(fieldAxeaType) && !program.enums.contains(fieldAxeaType))
                    {
                        continue;
                    }
                    const std::string fieldPtr = "%t" + std::to_string(nextTmp++);
                    const std::string fieldVal = "%t" + std::to_string(nextTmp++);
                    dropBody << "  " << fieldPtr << " = getelementptr " << structType << ", "
                             << pointerType << " %v, i32 0, i32 " << fieldIndex << "\n";
                    dropBody << "  " << fieldVal << " = load " << fieldLlvmType << ", "
                             << fieldLlvmType << "* " << fieldPtr << "\n";
                    dropBody << "  call void @axea.drop." << fieldAxeaType << "(" << fieldLlvmType
                             << " " << fieldVal << ")\n";
                }
                dropBody << "  br label %freeDone\n";
            }
            dropBody << "freeDone:\n";
        }
        else
        {
            for (std::size_t i = 0; i < fields.size(); ++i)
            {
                const auto& [fieldName, fieldTypeName] = fields[i];
                const std::string fieldAxeaType = mangleUnionTypeName(fieldTypeName);
                if (!structs_.contains(fieldAxeaType) && !program.enums.contains(fieldAxeaType))
                {
                    continue;
                }
                const std::string fieldLlvmType = llvmType(fieldTypeName);
                const std::string fieldPtr = "%t" + std::to_string(nextTmp++);
                const std::string fieldVal = "%t" + std::to_string(nextTmp++);
                dropBody << "  " << fieldPtr << " = getelementptr " << structType << ", "
                         << pointerType << " %v, i32 0, i32 " << i << "\n";
                dropBody << "  " << fieldVal << " = load " << fieldLlvmType << ", " << fieldLlvmType
                         << "* " << fieldPtr << "\n";
                dropBody << "  call void @axea.drop." << fieldAxeaType << "(" << fieldLlvmType
                         << " " << fieldVal << ")\n";
            }
        }

        dropBody << "  %raw = bitcast " << pointerType << " %v to i8*\n";
        dropBody << "  call void @free(i8* %raw)\n";
        dropBody << "  br label %done\n";
        dropBody << "done:\n";
        dropBody << "  ret void\n";
        dropBody << "}\n";
        structRefcountRuntimeText_ << "\n" << dropBody.str();
    }
}

std::string LlvmIrEmitter::registerCollectionToStrRuntime(const std::string& llvmType)
{
    if (const auto it = collectionToStrFnByLlvmType_.find(llvmType);
        it != collectionToStrFnByLlvmType_.end())
    {
        return it->second;
    }

    registerStrbufRuntime();

    if (!collectionPunctuationGlobalsRegistered_)
    {
        collectionPunctuationGlobalsRegistered_ = true;
        toStrRuntimeText_ << R"(
@axea.str.openbracket = private unnamed_addr constant [2 x i8] c"[\00"
@axea.str.closebracket = private unnamed_addr constant [2 x i8] c"]\00"
@axea.str.collcomma = private unnamed_addr constant [3 x i8] c", \00"
)";
    }

    const int id = static_cast<int>(collectionToStrFnByLlvmType_.size());
    const std::string fnName = "@axea.tostring.collection." + std::to_string(id);
    // Inserted *before* the body is built - registerCollectionToStrRuntime
    // can call itself recursively for a nested collection element (see
    // emitElementToStrCall), and this guarantees that recursion always
    // terminates by reusing the same name rather than registering a
    // second, identical function for an already-in-progress shape.
    collectionToStrFnByLlvmType_[llvmType] = fnName;

    std::ostringstream body;
    int nextTmp = 0;
    const std::string openBracket =
        "getelementptr ([2 x i8], [2 x i8]* @axea.str.openbracket, i64 0, i64 0)";
    const std::string closeBracket =
        "getelementptr ([2 x i8], [2 x i8]* @axea.str.closebracket, i64 0, i64 0)";
    const std::string comma =
        "getelementptr ([3 x i8], [3 x i8]* @axea.str.collcomma, i64 0, i64 0)";

    // Dispatch order mirrors the top-level binding printer's own exact
    // ordering. LinkedList<T>/Map<K,V>/Set<T>/SortedMap<K,V>/SortedSet<T> are all
    // real, user-declared generic structs now (see docs/language/0006-generics.md's own port
    // follow-up, docs/language/0034-maps-and-sets.md's own "2026 Update", docs/language/0040-
    // sorted-maps.md's own "2026 Update", and docs/language/0041-sorted-sets.md's own "2026
    // Update") - their own headers are ordinary named struct types (isNamedStructPointerType,
    // checked earlier in this same function), reached via the general per-struct
    // @axea.tostring.<Name> helper (emitStructToStringHelpers) like any other struct, not any
    // count-only fallback anymore - there is no such fallback left in this function at all.

    if (isDequeType(llvmType))
    {
        // Full bracket printing - Deque<T>'s
        // growable-array-with-a-start-offset representation directly
        // supports it, same reasoning as the top-level binding printer's
        // own identical Deque branch (see docs/language/0037-deques.md).
        // Queue<T> shares this exact LLVM shape (see
        // docs/language/0038-queues.md), so it's covered here too, with
        // no separate case needed.
        const std::string headerType = llvmType.substr(0, llvmType.size() - 1);
        const std::string elementType = dequeElementType(llvmType);

        body << "define i8* " << fnName << "(" << llvmType << " %v) {\n";
        body << "entry:\n";
        body << "  %buf = call {i32, i32, i8*}* @axea.strbuf.new()\n";
        body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << openBracket
             << ")\n";
        body << "  %lenPtr = getelementptr " << headerType << ", " << llvmType
             << " %v, i32 0, i32 0\n";
        body << "  %len = load i32, i32* %lenPtr\n";
        body << "  %startPtr = getelementptr " << headerType << ", " << llvmType
             << " %v, i32 0, i32 1\n";
        body << "  %start = load i32, i32* %startPtr\n";
        body << "  %dataPtrPtr = getelementptr " << headerType << ", " << llvmType
             << " %v, i32 0, i32 2\n";
        body << "  %data = load " << elementType << "*, " << elementType << "** %dataPtrPtr\n";
        body << "  %idxSlot = alloca i32\n";
        body << "  store i32 0, i32* %idxSlot\n";
        body << "  br label %loophdr\n";
        body << "loophdr:\n";
        body << "  %idx = load i32, i32* %idxSlot\n";
        body << "  %loopDone = icmp sge i32 %idx, %len\n";
        body << "  br i1 %loopDone, label %loopdone, label %loopbody\n";
        body << "loopbody:\n";
        body << "  %notFirst = icmp sgt i32 %idx, 0\n";
        body << "  br i1 %notFirst, label %addComma, label %afterComma\n";
        body << "addComma:\n";
        body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << comma << ")\n";
        body << "  br label %afterComma\n";
        body << "afterComma:\n";
        body << "  %actualIdx = add i32 %start, %idx\n";
        body << "  %elemPtr = getelementptr " << elementType << ", " << elementType
             << "* %data, i32 %actualIdx\n";
        body << "  %elemVal = load " << elementType << ", " << elementType << "* %elemPtr\n";
        const std::string elemStr = emitElementToStrCall(elementType, "%elemVal", body, nextTmp);
        body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << elemStr << ")\n";
        body << "  %idxNext = add i32 %idx, 1\n";
        body << "  store i32 %idxNext, i32* %idxSlot\n";
        body << "  br label %loophdr\n";
        body << "loopdone:\n";
        body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << closeBracket
             << ")\n";
        body << "  %result = call i8* @axea.strbuf.finish({i32, i32, i8*}* %buf)\n";
        body << "  ret i8* %result\n";
        body << "}\n";

        toStrRuntimeText_ << "\n" << body.str();
        return fnName;
    }

    if (isListType(llvmType))
    {
        // Stack<T>/PriorityQueue<T> share List<T>'s exact LLVM shape (see
        // docs/language/0035-stacks.md/0039-priority-queues.md), so
        // they're covered here too, with no separate case needed.
        const std::string headerType = llvmType.substr(0, llvmType.size() - 1);
        const std::string elementType = listElementType(llvmType);

        body << "define i8* " << fnName << "(" << llvmType << " %v) {\n";
        body << "entry:\n";
        body << "  %buf = call {i32, i32, i8*}* @axea.strbuf.new()\n";
        body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << openBracket
             << ")\n";
        body << "  %lenPtr = getelementptr " << headerType << ", " << llvmType
             << " %v, i32 0, i32 0\n";
        body << "  %len = load i32, i32* %lenPtr\n";
        body << "  %dataPtrPtr = getelementptr " << headerType << ", " << llvmType
             << " %v, i32 0, i32 1\n";
        body << "  %data = load " << elementType << "*, " << elementType << "** %dataPtrPtr\n";
        body << "  %idxSlot = alloca i32\n";
        body << "  store i32 0, i32* %idxSlot\n";
        body << "  br label %loophdr\n";
        body << "loophdr:\n";
        body << "  %idx = load i32, i32* %idxSlot\n";
        body << "  %loopDone = icmp sge i32 %idx, %len\n";
        body << "  br i1 %loopDone, label %loopdone, label %loopbody\n";
        body << "loopbody:\n";
        body << "  %notFirst = icmp sgt i32 %idx, 0\n";
        body << "  br i1 %notFirst, label %addComma, label %afterComma\n";
        body << "addComma:\n";
        body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << comma << ")\n";
        body << "  br label %afterComma\n";
        body << "afterComma:\n";
        body << "  %elemPtr = getelementptr " << elementType << ", " << elementType
             << "* %data, i32 %idx\n";
        body << "  %elemVal = load " << elementType << ", " << elementType << "* %elemPtr\n";
        const std::string elemStr = emitElementToStrCall(elementType, "%elemVal", body, nextTmp);
        body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << elemStr << ")\n";
        body << "  %idxNext = add i32 %idx, 1\n";
        body << "  store i32 %idxNext, i32* %idxSlot\n";
        body << "  br label %loophdr\n";
        body << "loopdone:\n";
        body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << closeBracket
             << ")\n";
        body << "  %result = call i8* @axea.strbuf.finish({i32, i32, i8*}* %buf)\n";
        body << "  ret i8* %result\n";
        body << "}\n";

        toStrRuntimeText_ << "\n" << body.str();
        return fnName;
    }

    if (isSliceType(llvmType))
    {
        // {T*, i32} passed *by value* (see docs/language/0032-slices.md and
        // docs/language/0056-slice-printing.md) - both fields come straight
        // out of the already-in-hand struct via `extractvalue`, unlike
        // every branch above which GEPs/loads its length and data pointer
        // out of a heap record reached through a pointer parameter.
        // Otherwise identical to the List<T> branch just above: same
        // bracket-and-comma loop, same single-index GEP element access
        // emitIndexGet's own isSliceType branch already uses.
        const std::string elementType = sliceElementType(llvmType);

        body << "define i8* " << fnName << "(" << llvmType << " %v) {\n";
        body << "entry:\n";
        body << "  %buf = call {i32, i32, i8*}* @axea.strbuf.new()\n";
        body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << openBracket
             << ")\n";
        body << "  %data = extractvalue " << llvmType << " %v, 0\n";
        body << "  %len = extractvalue " << llvmType << " %v, 1\n";
        body << "  %idxSlot = alloca i32\n";
        body << "  store i32 0, i32* %idxSlot\n";
        body << "  br label %loophdr\n";
        body << "loophdr:\n";
        body << "  %idx = load i32, i32* %idxSlot\n";
        body << "  %loopDone = icmp sge i32 %idx, %len\n";
        body << "  br i1 %loopDone, label %loopdone, label %loopbody\n";
        body << "loopbody:\n";
        body << "  %notFirst = icmp sgt i32 %idx, 0\n";
        body << "  br i1 %notFirst, label %addComma, label %afterComma\n";
        body << "addComma:\n";
        body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << comma << ")\n";
        body << "  br label %afterComma\n";
        body << "afterComma:\n";
        body << "  %elemPtr = getelementptr " << elementType << ", " << elementType
             << "* %data, i32 %idx\n";
        body << "  %elemVal = load " << elementType << ", " << elementType << "* %elemPtr\n";
        const std::string elemStr = emitElementToStrCall(elementType, "%elemVal", body, nextTmp);
        body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << elemStr << ")\n";
        body << "  %idxNext = add i32 %idx, 1\n";
        body << "  store i32 %idxNext, i32* %idxSlot\n";
        body << "  br label %loophdr\n";
        body << "loopdone:\n";
        body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << closeBracket
             << ")\n";
        body << "  %result = call i8* @axea.strbuf.finish({i32, i32, i8*}* %buf)\n";
        body << "  ret i8* %result\n";
        body << "}\n";

        toStrRuntimeText_ << "\n" << body.str();
        return fnName;
    }

    // Fixed array (see docs/language/0031-arrays.md) - the element count
    // is statically known, so the loop is unrolled at this (LlvmIrEmitter-
    // build-time) point rather than emitted as a real runtime loop,
    // mirroring the top-level binding printer's own identical choice.
    const std::string elementType = arrayElementType(llvmType);
    const int size = arraySizeFromPointerType(llvmType);
    const std::string arrayType = llvmType.substr(0, llvmType.size() - 1);

    body << "define i8* " << fnName << "(" << llvmType << " %v) {\n";
    body << "entry:\n";
    body << "  %buf = call {i32, i32, i8*}* @axea.strbuf.new()\n";
    body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << openBracket << ")\n";
    for (int i = 0; i < size; ++i)
    {
        if (i > 0)
        {
            body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << comma << ")\n";
        }
        const std::string elemPtr = "%elemPtr" + std::to_string(i);
        const std::string elemVal = "%elemVal" + std::to_string(i);
        body << "  " << elemPtr << " = getelementptr " << arrayType << ", " << llvmType
             << " %v, i32 0, i32 " << i << "\n";
        body << "  " << elemVal << " = load " << elementType << ", " << elementType << "* "
             << elemPtr << "\n";
        const std::string elemStr = emitElementToStrCall(elementType, elemVal, body, nextTmp);
        body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << elemStr << ")\n";
    }
    body << "  call void @axea.strbuf.append({i32, i32, i8*}* %buf, i8* " << closeBracket << ")\n";
    body << "  %result = call i8* @axea.strbuf.finish({i32, i32, i8*}* %buf)\n";
    body << "  ret i8* %result\n";
    body << "}\n";

    toStrRuntimeText_ << "\n" << body.str();
    return fnName;
}

std::string LlvmIrEmitter::stringifyValue(int reg, FunctionContext& fctx)
{
    return stringifyValueOfType(typeOf(reg, fctx), ref(reg, fctx), fctx);
}

std::string LlvmIrEmitter::stringifyValueOfType(const std::string& type,
                                                const std::string& valueRef,
                                                FunctionContext& fctx)
{
    if (type == "i32")
    {
        const std::string fnName = registerI32ToStrRuntime();
        const int destReg = allocateRegister(fctx);
        *fctx.out << "  %" << destReg << " = call i8* " << fnName << "(i32 " << valueRef << ")\n";
        return "%" + std::to_string(destReg);
    }
    if (type == "i64")
    {
        const std::string fnName = registerI64ToStrRuntime();
        const int destReg = allocateRegister(fctx);
        *fctx.out << "  %" << destReg << " = call i8* " << fnName << "(i64 " << valueRef << ")\n";
        return "%" + std::to_string(destReg);
    }
    if (type == "double")
    {
        const std::string fnName = registerF64ToStrRuntime();
        const int destReg = allocateRegister(fctx);
        *fctx.out << "  %" << destReg << " = call i8* " << fnName << "(double " << valueRef
                  << ")\n";
        return "%" + std::to_string(destReg);
    }
    if (type == "i1")
    {
        const std::string fnName = registerBoolToStrRuntime();
        const int destReg = allocateRegister(fctx);
        *fctx.out << "  %" << destReg << " = call i8* " << fnName << "(i1 " << valueRef << ")\n";
        return "%" + std::to_string(destReg);
    }
    if (isCharType(type))
    {
        return encodeCharUtf8(valueRef, fctx);
    }
    if (isOptionalType(type))
    {
        const std::string fnName = registerOptionalToStrRuntime(type);
        const int destReg = allocateRegister(fctx);
        *fctx.out << "  %" << destReg << " = call i8* " << fnName << "(" << type << " " << valueRef
                  << ")\n";
        return "%" + std::to_string(destReg);
    }
    if (isResultType(type))
    {
        const std::string fnName = registerResultToStrRuntime(type);
        const int destReg = allocateRegister(fctx);
        *fctx.out << "  %" << destReg << " = call i8* " << fnName << "(" << type << " " << valueRef
                  << ")\n";
        return "%" + std::to_string(destReg);
    }
    if (isNamedStructPointerType(type))
    {
        // Pre-registered, unconditionally, by emitStructToStringHelpers
        // (see docs/language/0054-collection-printing.md) - safe to
        // reference by fixed name regardless of registration order.
        const std::string fnName = "@axea.tostring." + structNameFromPointerType(type);
        const int destReg = allocateRegister(fctx);
        *fctx.out << "  %" << destReg << " = call i8* " << fnName << "(" << type << " " << valueRef
                  << ")\n";
        return "%" + std::to_string(destReg);
    }
    if (type == "i8*" || type == "{i32, i8*}*")
    {
        // str or String.
        return resolveStrPtrOfType(type, valueRef, fctx);
    }
    if (!type.empty() && (type.front() == '{' || type.front() == '['))
    {
        // A collection - List/Stack/PriorityQueue/Deque/Queue/
        // LinkedList/Map/Set/SortedMap/SortedSet, or a fixed array (see
        // docs/language/0054-collection-printing.md) -
        // registerCollectionToStrRuntime's own dispatch chain covers all
        // of them, keyed structurally exactly like isListType/isDequeType/
        // etc. already are.
        const std::string fnName = registerCollectionToStrRuntime(type);
        const int destReg = allocateRegister(fctx);
        *fctx.out << "  %" << destReg << " = call i8* " << fnName << "(" << type << " " << valueRef
                  << ")\n";
        return "%" + std::to_string(destReg);
    }
    throw std::runtime_error("cannot stringify a value of type " + type + " this phase");
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
        else if (const auto* constInt64 = dynamic_cast<const IrConstInt64*>(inst.get()))
        {
            fctx.registerTypes[constInt64->dest] = "i64";
        }
        else if (const auto* constFloat = dynamic_cast<const IrConstFloat*>(inst.get()))
        {
            fctx.registerTypes[constFloat->dest] = "double";
        }
        else if (const auto* cast = dynamic_cast<const IrCast*>(inst.get()))
        {
            fctx.registerTypes[cast->dest] = llvmType(cast->targetType);
        }
        else if (const auto* sizeOf = dynamic_cast<const IrSizeOf*>(inst.get()))
        {
            fctx.registerTypes[sizeOf->dest] = "i64";
        }
        else if (const auto* hashOf = dynamic_cast<const IrHashOf*>(inst.get()))
        {
            fctx.registerTypes[hashOf->dest] = "i32";
        }
        else if (const auto* keyEq = dynamic_cast<const IrKeyEq*>(inst.get()))
        {
            fctx.registerTypes[keyEq->dest] = "i1";
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
        else if (const auto* constNull = dynamic_cast<const IrConstNull*>(inst.get()))
        {
            // llvmType already turns either shape of `pointeeTypeName` into the right *register*
            // type directly - "*i32" -> "i32*" (a raw pointer register's own natural type), or a
            // plain struct name "Node$i32" -> "%Node$i32*" (structs are always by-pointer) - see
            // IrConstNull's own doc comment in Ir.hpp. No "+ '*'" needed here either way.
            fctx.registerTypes[constNull->dest] = llvmType(constNull->pointeeTypeName);
        }
        else if (const auto* strSlice = dynamic_cast<const IrStrSlice*>(inst.get()))
        {
            // A bare i8* for a str-coercible `object` (see
            // docs/language/0045-str-slicing.md) - the result is always a
            // fresh str, never a String, regardless of whether `object`
            // itself was a str or a String. Widened in
            // docs/language/0050-collection-join-and-slicing.md: for an
            // Array/List `object` (already type-inferred by this point,
            // same reasoning IrFieldGet's own objectType lookup just above
            // relies on), the result is a fresh List<T> instead.
            const std::string objectType = typeOf(strSlice->object, fctx);
            // isStringType is excluded explicitly, not accidentally -
            // isListType's own loose "{...}*" test also matches String's
            // own {i32, i8*}* header (same class of collision
            // isBufferType/isDequeType's own explicit exclusions already
            // guard against elsewhere - see ensureListCapacity's own
            // comment for the analogous Deque<T> case). A String `object`
            // always takes the plain str branch below, matching this
            // comment's own "always a fresh str... regardless of whether
            // object itself was a str or a String" rule.
            const bool isListShaped = isListType(objectType) && !isStringType(objectType);
            if (isListShaped || (!objectType.empty() && objectType.front() == '['))
            {
                const std::string elementType =
                    isListShaped ? listElementType(objectType) : arrayElementType(objectType);
                fctx.registerTypes[strSlice->dest] = "{i32, " + elementType + "*, i32}*";
            }
            else
            {
                fctx.registerTypes[strSlice->dest] = "i8*";
            }
        }
        else if (const auto* join = dynamic_cast<const IrJoin*>(inst.get()))
        {
            // Always a fresh String header (see
            // docs/language/0050-collection-join-and-slicing.md) - built
            // the same way interpolation's own OwnedString result is.
            fctx.registerTypes[join->dest] = "{i32, i8*}*";
        }
        else if (const auto* parse = dynamic_cast<const IrParse*>(inst.get()))
        {
            // Optional<T>, not T directly (see
            // docs/language/0052-optional.md) - TypeChecker already
            // restricts targetType to "i32"/"i64"/"f64"/"bool".
            fctx.registerTypes[parse->dest] = registerOptionalInstantiation(parse->targetType);
        }
        else if (const auto* optionalNew = dynamic_cast<const IrOptionalNew*>(inst.get()))
        {
            // Some(x): payload type read off `value`'s own already-inferred
            // register type. None (value == -1): read off payloadTypeName
            // instead, the only case that carries one (see
            // docs/language/0052-optional.md and IrOptionalNew's own
            // comment for why).
            fctx.registerTypes[optionalNew->dest] =
                optionalNew->value != -1
                    ? registerOptionalInstantiationForLlvmPayload(typeOf(optionalNew->value, fctx))
                    : registerOptionalInstantiation(optionalNew->payloadTypeName);
        }
        else if (const auto* resultNew = dynamic_cast<const IrResultNew*>(inst.get()))
        {
            // See docs/language/0063-result.md and IrResultNew's own
            // comment in Ir.hpp - `value`'s own register gives one of the
            // two payload types (Ok if isOk, Err otherwise);
            // otherPayloadTypeName (an Axea type name, unlike `value`'s
            // own already-LLVM register type) gives the other.
            const std::string knownLlvmType = typeOf(resultNew->value, fctx);
            const std::string otherLlvmType = llvmType(resultNew->otherPayloadTypeName);
            fctx.registerTypes[resultNew->dest] = registerResultInstantiationForLlvmPayloads(
                resultNew->isOk ? knownLlvmType : otherLlvmType,
                resultNew->isOk ? otherLlvmType : knownLlvmType);
        }
        else if (const auto* isSome = dynamic_cast<const IrOptionalIsSome*>(inst.get()))
        {
            fctx.registerTypes[isSome->dest] = "i1";
        }
        else if (const auto* unwrap = dynamic_cast<const IrOptionalUnwrap*>(inst.get()))
        {
            // Shared by Optional<T> and Result<T,E> (see
            // docs/language/0063-result.md and IrOptionalUnwrap's own
            // comment in Ir.hpp) - `object`'s own already-inferred
            // register type tells us which concrete named type this is,
            // so the right registration table is consulted either way.
            const std::string objectType = typeOf(unwrap->object, fctx);
            if (isResultType(objectType))
            {
                fctx.registerTypes[unwrap->dest] = unwrap->field == 2
                                                       ? resultErrPayloadType(objectType)
                                                       : resultOkPayloadType(objectType);
            }
            else
            {
                fctx.registerTypes[unwrap->dest] = optionalPayloadType(objectType);
            }
        }
        else if (const auto* toCstr = dynamic_cast<const IrToCstr*>(inst.get()))
        {
            // Always a bare i8* (see docs/language/0048-ffi.md) - cstr and
            // str share the exact same LLVM representation.
            fctx.registerTypes[toCstr->dest] = "i8*";
        }
        else if (dynamic_cast<const IrPrint*>(inst.get()) ||
                 dynamic_cast<const IrBufferAppendValue*>(inst.get()))
        {
            // Unit-typed, same reasoning as IrBufferAppend above (see
            // docs/language/Axea_Printing_Formatting.md).
            fctx.registerTypes[inst->dest] = "void";
        }
        else if (const auto* binOp = dynamic_cast<const IrBinOp*>(inst.get()))
        {
            const bool isComparison =
                binOp->op == TokenKind::Less || binOp->op == TokenKind::LessEqual ||
                binOp->op == TokenKind::Greater || binOp->op == TokenKind::GreaterEqual ||
                binOp->op == TokenKind::EqualEqual || binOp->op == TokenKind::BangEqual;
            // Arithmetic's own result type is whatever numeric kind the
            // operands share (i32, i64, or f64/"double" - see
            // TypeChecker::requireInt) - no longer always "i32" now that
            // i64/f64 arithmetic exists (see docs/language/0005-type-system.md).
            // `lhs`'s own producing instruction always appears earlier in
            // this same list (IrGenerator emits operands before the
            // IrBinOp that consumes them), so its registerTypes entry is
            // already set by the time this is reached.
            fctx.registerTypes[binOp->dest] = isComparison ? "i1" : typeOf(binOp->lhs, fctx);
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
        else if (const auto* closureNew = dynamic_cast<const IrClosureNew*>(inst.get()))
        {
            std::vector<std::string> paramLlvmTypes;
            paramLlvmTypes.reserve(closureNew->paramTypes.size());
            for (const auto& paramType : closureNew->paramTypes)
            {
                paramLlvmTypes.push_back(llvmType(paramType));
            }
            fctx.registerTypes[closureNew->dest] =
                registerClosureInstantiation(paramLlvmTypes, llvmType(closureNew->returnType)) +
                "*";
        }
        else if (const auto* closureCall = dynamic_cast<const IrClosureCall*>(inst.get()))
        {
            const std::string closureType = typeOf(closureCall->closureObject, fctx);
            fctx.registerTypes[closureCall->dest] =
                closureReturnType(closureType.substr(0, closureType.size() - 1));
        }
        else if (const auto* fieldGet = dynamic_cast<const IrFieldGet*>(inst.get()))
        {
            const std::string objectType = typeOf(fieldGet->object, fctx);
            if (isSliceType(objectType) || isListType(objectType) ||
                isDequeType(objectType) || objectType == "i8*" ||
                isStringType(objectType) || isBufferType(objectType))
            {
                // Only "length"/"bytes"/"capacity" ever reach a slice,
                // List, Deque,
                // str, String, or Buffer via IrFieldGet, and all of them
                // are i32 - TypeChecker already guarantees this (see
                // docs/language/0032-slices.md, 0033-lists.md,
                // 0037-deques.md,
                // 0047-unicode.md). str/String/Buffer are explicitly listed
                // (not left to fall into isDequeType's own coincidental
                // structural match the way Buffer's raw header shape
                // would) for the same "explicit, not accidental" reasoning
                // docs/language/0043-buffer.md already established. LinkedList<T>/Map<K,V>/
                // Set<T>/SortedMap<K,V>/SortedSet<T> are all real, user-declared generic structs
                // now (see docs/language/0006-generics.md's own port follow-up, docs/language/
                // 0034-maps-and-sets.md's own "2026 Update", docs/language/0040-sorted-maps.md's
                // own "2026 Update", and docs/language/0041-sorted-sets.md's own "2026 Update") -
                // their own `.length` is an ordinary struct field, reached via the `else` branch
                // just below like any other struct field, not this fixed-i32-collections list
                // anymore.
                fctx.registerTypes[fieldGet->dest] = "i32";
            }
            else
            {
                const std::string structName = structNameFromPointerType(objectType);
                fctx.registerTypes[fieldGet->dest] =
                    fieldIndexAndType(structName, fieldGet->field).second;
            }
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
            if (objectType == "i8*" || isStringType(objectType))
            {
                // Single-character indexing (`s[i]`) on str/String -
                // always a char (i24), see registerUtf8CharAtRuntime and
                // docs/language/0047-unicode.md. Checked before
                // isSliceType/isListType/etc.: str's own "i8*" would
                // otherwise fall into the final arrayElementType() else
                // branch and read garbage.
                fctx.registerTypes[indexGet->dest] = "i24";
            }
            else if (isSliceType(objectType))
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
        else if (const auto* deref = dynamic_cast<const IrDeref*>(inst.get()))
        {
            fctx.registerTypes[deref->dest] = pointerElementType(typeOf(deref->pointer, fctx));
        }
        else if (const auto* alloca = dynamic_cast<const IrAlloca*>(inst.get()))
        {
            fctx.registerTypes[alloca->dest] = typeOf(alloca->initialValue, fctx) + "*";
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
            // Same reasoning as branch->dest just above, one merged register per outer-scope
            // name either branch reassigned (see docs/language/0021-axea-ir.md's own follow-up
            // and IrGenerator::mergeBranchScopes) - thenReg/elseReg both represent the same
            // Axea-level variable, so either's own type is authoritative.
            for (const auto& [thenReg, elseReg, destReg] : branch->carriedMerges)
            {
                fctx.registerTypes[destReg] = typeOf(thenReg, fctx);
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
        else if (const auto* retainInst = dynamic_cast<const IrRetain*>(inst.get()))
        {
            // `.clone()`'s own dest (see emitInstructions' own IrRetain case) - always the same
            // pointer *type* as what it retained, just a distinct SSA register from it.
            if (retainInst->dest != -1)
            {
                fctx.registerTypes[retainInst->dest] = typeOf(retainInst->value, fctx);
            }
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
        else if (const auto* loop = dynamic_cast<const IrLoop*>(inst.get()))
        {
            // Pre-existing gap, found while verifying
            // docs/language/0057-alignment.md's own worked example (a
            // `for`-loop body containing an interpolated string with a
            // literal segment, e.g. `"{a} {b}"`'s own literal " " piece
            // between the two expression pieces): this recursion into
            // IrBranch's own two blocks was never extended to IrLoop's
            // conditionBlock/body, so any string *literal* appearing
            // inside a loop (an IrConstString `.value`, produced for the
            // literal-text pieces of an interpolated string, or a bare
            // string literal used directly) was never hoisted into this
            // one-time snapshot - only for it to be referenced via
            // `stringGlobalByLiteral_.at(...)` at real emission time,
            // throwing `std::out_of_range` (the interpreter and both
            // compiled optimization levels the usual verification
            // discipline runs never exercise this exact "literal text
            // inside a loop body" combination in existing tests/examples,
            // which is how this went unnoticed). Recursing here the same
            // way IrBranch's own two blocks already do fixes every case,
            // nested loops included, since this function is naturally
            // recursive.
            collectStrings(loop->conditionBlock);
            collectStrings(loop->body);
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
    // Move semantics (structs/enums only) - no hidden refcount field anymore (see
    // emitStructRefcountHelpers's own updated comment): single ownership is proven at compile
    // time, so a plain struct/enum's LLVM layout is exactly its own real fields, nothing more.
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
        // identifiers, and a struct used as a key across several different
        // `hash<T>()`/`keyEq<T>()` call sites should only ever get one hash/equality
        // implementation, generated once (mirrors registerKeyRuntime's own memoization above). Recurses into each field's own
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
        // numeric ID (own ID space - see nextArrayKeyId_). N is compile-time-known,
        // so this unrolls rather than looping.
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

std::string LlvmIrEmitter::registerOrderRuntime(const std::string& axeaKeyType)
{
    if (const auto it = orderRuntimeFns_.find(axeaKeyType); it != orderRuntimeFns_.end())
    {
        return it->second;
    }

    if (axeaKeyType == "i32")
    {
        mapSetRuntimeText_ << R"(
define i1 @axea.less.i32(i32 %a, i32 %b) {
entry:
  %r = icmp slt i32 %a, %b
  ret i1 %r
}
)";
        return orderRuntimeFns_[axeaKeyType] = "@axea.less.i32";
    }

    if (axeaKeyType == "i64")
    {
        mapSetRuntimeText_ << R"(
define i1 @axea.less.i64(i64 %a, i64 %b) {
entry:
  %r = icmp slt i64 %a, %b
  ret i1 %r
}
)";
        return orderRuntimeFns_[axeaKeyType] = "@axea.less.i64";
    }

    if (axeaKeyType == "f64")
    {
        // `fcmp olt` (ordered less-than) - same NaN handling as the
        // general `<` operator (see TypeChecker::isOrderableKind's own
        // comment): a NaN key simply compares false against everything.
        mapSetRuntimeText_ << R"(
define i1 @axea.less.f64(double %a, double %b) {
entry:
  %r = fcmp olt double %a, %b
  ret i1 %r
}
)";
        return orderRuntimeFns_[axeaKeyType] = "@axea.less.f64";
    }

    if (axeaKeyType == "char")
    {
        mapSetRuntimeText_ << R"(
define i1 @axea.less.char(i24 %a, i24 %b) {
entry:
  %r = icmp slt i24 %a, %b
  ret i1 %r
}
)";
        return orderRuntimeFns_[axeaKeyType] = "@axea.less.char";
    }

    if (axeaKeyType == "str")
    {
        // Hand-rolled byte-walk lexicographic order - mirrors
        // registerKeyRuntime's own @axea.eq.str byte-walk exactly, just
        // resolving to a magnitude comparison instead of an (in)equality
        // one. Bytes are compared as unsigned (zext to i32, not sign-
        // extended) - a signed i8 comparison would sort any byte >= 0x80
        // (any non-ASCII UTF-8 continuation/lead byte) before every ASCII
        // byte, which is wrong; this matches a textbook strcmp's own
        // `unsigned char` comparison semantics exactly (see
        // docs/language/0042-string.md).
        mapSetRuntimeText_ << R"(
define i1 @axea.less.str(i8* %a, i8* %b) {
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
  %diff = icmp ne i8 %ac, %bc
  br i1 %diff, label %resolve, label %check
check:
  %iszero = icmp eq i8 %ac, 0
  br i1 %iszero, label %equal, label %next
next:
  %i1 = add i32 %i0, 1
  store i32 %i1, i32* %i
  br label %header
resolve:
  %au = zext i8 %ac to i32
  %bu = zext i8 %bc to i32
  %r = icmp slt i32 %au, %bu
  ret i1 %r
equal:
  ret i1 0
}
)";
        return orderRuntimeFns_[axeaKeyType] = "@axea.less.str";
    }

    // Unreachable for a well-typed program: TypeChecker::isOrderableKind
    // already validated the element/key before this point.
    throw std::runtime_error("internal error: no order runtime registered for type " + axeaKeyType);
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

    // Move semantics (structs/enums only) - no refcount field to initialize anymore (see
    // emitStructTypeDecls's own updated comment); a fresh allocation just needs its real fields
    // populated, which the loop above already did.
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

    if (isListType(objectType) ||
        isDequeType(objectType))
    {
        // A List's ".length" is field 0 of the {i32, T*} heap record; a Deque's
        // ".length" is field 0 (count) of its own 3-field header {count,
        // start, data} - same GEP index, same shape, so this branch covers
        // both (see docs/language/0033-lists.md,
        // docs/language/0037-deques.md).
        // Stack<T> (docs/language/0035-stacks.md)
        // needs no branch of its own here at all: llvmType("Stack<T>")
        // produces the *exact same text* llvmType("List<T>") would, so
        // isListType's own test already matches it - not a coincidence, but a direct
        // consequence of Stack<T> and List<T> being the literal same LLVM
        // type. isDequeType *is* still checked explicitly,
        // even though this shared field-0-i32 code would coincidentally
        // handle its header too (its header shape
        // genuinely differs from List<T>'s) - same "explicit, not accidental"
        // reasoning. `LinkedList<T>`/`Map<K,V>`/`Set<T>`/`SortedMap<K,V>`/`SortedSet<T>` are all
        // real, user-declared generic structs now (see docs/language/0036-linked-lists.md,
        // docs/language/0034-maps-and-sets.md's own "2026 Update", docs/language/0040-sorted-
        // maps.md's own "2026 Update", and docs/language/0041-sorted-sets.md's own "2026 Update")
        // - their own ".length" goes through ordinary struct field access, not this shared
        // dispatch anymore - this is the last of these shared-dispatch collections. Unlike an
        // array's
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

void LlvmIrEmitter::emitClosureNew(const IrClosureNew& closureNew, FunctionContext& fctx)
{
    std::vector<std::string> paramLlvmTypes;
    paramLlvmTypes.reserve(closureNew.paramTypes.size());
    for (const auto& paramType : closureNew.paramTypes)
    {
        paramLlvmTypes.push_back(llvmType(paramType));
    }
    const std::string returnLlvmType = llvmType(closureNew.returnType);
    const std::string closureType = registerClosureInstantiation(paramLlvmTypes, returnLlvmType);
    const std::string fnPtrType = closureFnPtrType(closureType); // "RetType (i8*, ParamTypes...)*"
    const std::string pointerType = closureType + "*";

    // malloc, mirroring emitStructNew's own shape.
    const int sizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizePtrReg << " = getelementptr " << closureType << ", " << pointerType
              << " null, i32 1\n";
    const int sizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizeIntReg << " = ptrtoint " << pointerType << " %" << sizePtrReg
              << " to i64\n";
    const int rawPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawPtrReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";
    const int destReg = defineRegister(closureNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawPtrReg << " to " << pointerType
              << "\n";

    // Field 0: the trampoline's own address. The trampoline's own *real* declared LLVM type
    // (first param is the specific captures struct type, not opaque i8*) never matches
    // `fnPtrType` exactly, so this always needs an explicit bitcast - the one place this whole
    // representation's own genericity (every closure sharing a signature collapsing onto the
    // same %axea.Closure.<id>, regardless of what each literal actually captures) has to be
    // paid for.
    const std::string capturesType = typeOf(closureNew.capturesObject, fctx);
    std::string trampolineRealType = returnLlvmType + " (" + capturesType;
    for (const auto& paramLlvmType : paramLlvmTypes)
    {
        trampolineRealType += ", " + paramLlvmType;
    }
    trampolineRealType += ")*";

    const int castedFnReg = allocateRegister(fctx);
    *fctx.out << "  %" << castedFnReg << " = bitcast " << trampolineRealType << " @"
              << closureNew.trampolineFunctionName << " to " << fnPtrType << "\n";
    const int fnFieldPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << fnFieldPtrReg << " = getelementptr " << closureType << ", " << pointerType
              << " " << ref(closureNew.dest, fctx) << ", i32 0, i32 0\n";
    *fctx.out << "  store " << fnPtrType << " %" << castedFnReg << ", " << fnPtrType << "* %"
              << fnFieldPtrReg << "\n";

    // Field 1: the already-built captures struct, hidden behind an opaque i8*.
    const int capturesI8Reg = allocateRegister(fctx);
    *fctx.out << "  %" << capturesI8Reg << " = bitcast " << capturesType << " "
              << ref(closureNew.capturesObject, fctx) << " to i8*\n";
    const int capturesFieldPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << capturesFieldPtrReg << " = getelementptr " << closureType << ", "
              << pointerType << " " << ref(closureNew.dest, fctx) << ", i32 0, i32 1\n";
    *fctx.out << "  store i8* %" << capturesI8Reg << ", i8** %" << capturesFieldPtrReg << "\n";

    // Field 2: this literal's own drop-wrapper function pointer (see
    // registerClosureInstantiation's own comment on why dropping a closure needs dynamic
    // dispatch through this field, the same reasoning field 0's own real-vs-generic function
    // pointer split already established). One small wrapper per literal, generated here since
    // this is the one place that still knows the *real* captures-struct type - it just bitcasts
    // the opaque i8* back and forwards into that struct's own already-generated @axea.drop.<Name>
    // (a real struct, dropped exactly like any other - see emitStructRefcountHelpers).
    const std::string capturesStructName = capturesType.substr(1, capturesType.size() - 2);
    const std::string dropWrapperName = "axea.dropwrapper." + capturesStructName;
    std::ostringstream wrapperBody;
    wrapperBody << "define void @" << dropWrapperName << "(i8* %opaque) {\n";
    wrapperBody << "entry:\n";
    wrapperBody << "  %real = bitcast i8* %opaque to " << capturesType << "\n";
    wrapperBody << "  call void @axea.drop." << capturesStructName << "(" << capturesType
                << " %real)\n";
    wrapperBody << "  ret void\n";
    wrapperBody << "}\n";
    structRefcountRuntimeText_ << "\n" << wrapperBody.str();

    const int dropWrapperFieldPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dropWrapperFieldPtrReg << " = getelementptr " << closureType << ", "
              << pointerType << " " << ref(closureNew.dest, fctx) << ", i32 0, i32 2\n";
    *fctx.out << "  store void (i8*)* @" << dropWrapperName << ", void (i8*)** %"
              << dropWrapperFieldPtrReg << "\n";
}

void LlvmIrEmitter::emitClosureCall(const IrClosureCall& closureCall, FunctionContext& fctx)
{
    const std::string closureType =
        typeOf(closureCall.closureObject, fctx); // "%axea.Closure.<id>*"
    const std::string closureStructType = closureType.substr(0, closureType.size() - 1);
    const std::string fnPtrType = closureFnPtrType(closureStructType);
    const std::string fnType = fnPtrType.substr(0, fnPtrType.size() - 1); // strip trailing '*'
    const std::string returnLlvmType = closureReturnType(closureStructType);

    const int fnFieldPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << fnFieldPtrReg << " = getelementptr " << closureStructType << ", "
              << closureType << " " << ref(closureCall.closureObject, fctx) << ", i32 0, i32 0\n";
    const int fnReg = allocateRegister(fctx);
    *fctx.out << "  %" << fnReg << " = load " << fnPtrType << ", " << fnPtrType << "* %"
              << fnFieldPtrReg << "\n";

    const int capturesFieldPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << capturesFieldPtrReg << " = getelementptr " << closureStructType << ", "
              << closureType << " " << ref(closureCall.closureObject, fctx) << ", i32 0, i32 1\n";
    const int capturesReg = allocateRegister(fctx);
    *fctx.out << "  %" << capturesReg << " = load i8*, i8** %" << capturesFieldPtrReg << "\n";

    std::string argsText = "i8* %" + std::to_string(capturesReg);
    for (const int argReg : closureCall.args)
    {
        argsText += ", " + typeOf(argReg, fctx) + " " + ref(argReg, fctx);
    }

    *fctx.out << "  ";
    if (returnLlvmType != "void")
    {
        const int destReg = defineRegister(closureCall.dest, fctx);
        *fctx.out << "%" << destReg << " = ";
    }
    *fctx.out << "call " << fnType << " %" << fnReg << "(" << argsText << ")\n";
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

    if (objectType == "i8*" || isStringType(objectType))
    {
        // Single-character indexing (`s[i]`) on str/String - a real
        // Unicode codepoint index via the shared @axea.utf8.char_at
        // runtime (see registerUtf8CharAtRuntime and
        // docs/language/0047-unicode.md), not a byte offset. String is
        // first resolved to its own bare i8* data pointer
        // (resolveStrPtrOfType), the same str-coercion every other
        // String-accepting operation already shares.
        const std::string strPtr =
            resolveStrPtrOfType(objectType, ref(indexGet.object, fctx), fctx);
        const std::string fnName = registerUtf8CharAtRuntime();
        const int destReg = defineRegister(indexGet.dest, fctx);
        *fctx.out << "  %" << destReg << " = call i24 " << fnName << "(i8* " << strPtr << ", i32 "
                  << ref(indexGet.index, fctx) << ")\n";
        return;
    }

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

void LlvmIrEmitter::emitDeref(const IrDeref& deref, FunctionContext& fctx)
{
    const std::string pointerType = typeOf(deref.pointer, fctx);
    const std::string elementType = pointerElementType(pointerType);
    const int destReg = defineRegister(deref.dest, fctx);
    *fctx.out << "  %" << destReg << " = load " << elementType << ", " << pointerType << " "
              << ref(deref.pointer, fctx) << "\n";
}

void LlvmIrEmitter::emitDerefAssign(const IrDerefAssign& derefAssign, FunctionContext& fctx)
{
    const std::string pointerType = typeOf(derefAssign.pointer, fctx);
    const std::string elementType = pointerElementType(pointerType);
    *fctx.out << "  store " << elementType << " " << ref(derefAssign.value, fctx) << ", "
              << pointerType << " " << ref(derefAssign.pointer, fctx) << "\n";
}

void LlvmIrEmitter::emitAlloca(const IrAlloca& alloca, FunctionContext& fctx)
{
    const std::string elementType = typeOf(alloca.initialValue, fctx);
    const int destReg = defineRegister(alloca.dest, fctx);
    *fctx.out << "  %" << destReg << " = alloca " << elementType << "\n";
    *fctx.out << "  store " << elementType << " " << ref(alloca.initialValue, fctx) << ", "
              << elementType << "* %" << destReg << "\n";
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
    const std::string objectType = typeOf(strSlice.object, fctx);

    // Array/List<T> branch (see
    // docs/language/0050-collection-join-and-slicing.md) - a genuinely
    // separate operation from the str branch below (element-wise GEP copy
    // of `elementType`-sized values, not a byte copy), so it's kept
    // entirely self-contained here rather than threaded through the str
    // branch's own byte-indexed logic. isStringType is excluded
    // explicitly, not accidentally - isListType's own loose "{...}*" test
    // also matches String's own {i32, i8*}* header; a String `object`
    // must always take the plain str branch below instead (see
    // docs/language/0042-string.md - str-slicing a String still always
    // produces a fresh str, never a List).
    if ((isListType(objectType) && !isStringType(objectType)) ||
        (!objectType.empty() && objectType.front() == '['))
    {
        const IndexableView view = resolveIndexableView(strSlice.object, fctx);

        std::string startRef = "0";
        if (strSlice.start != -1)
        {
            startRef = ref(strSlice.start, fctx);
        }
        std::string endRef = view.lengthRef;
        if (strSlice.end != -1)
        {
            endRef = ref(strSlice.end, fctx);
        }

        const int lengthReg = allocateRegister(fctx);
        *fctx.out << "  %" << lengthReg << " = sub i32 " << endRef << ", " << startRef << "\n";

        // sizeof(elementType) via the standard null-pointer GEP idiom, same
        // as emitListPush's own identical growth-allocation idiom.
        const int elemSizePtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << elemSizePtrReg << " = getelementptr " << view.elementType << ", "
                  << view.elementType << "* null, i32 1\n";
        const int elemSizeReg = allocateRegister(fctx);
        *fctx.out << "  %" << elemSizeReg << " = ptrtoint " << view.elementType << "* %"
                  << elemSizePtrReg << " to i64\n";
        const int length64Reg = allocateRegister(fctx);
        *fctx.out << "  %" << length64Reg << " = zext i32 %" << lengthReg << " to i64\n";
        const int newBytesReg = allocateRegister(fctx);
        *fctx.out << "  %" << newBytesReg << " = mul i64 %" << length64Reg << ", %" << elemSizeReg
                  << "\n";
        const int rawNewDataReg = allocateRegister(fctx);
        *fctx.out << "  %" << rawNewDataReg << " = call i8* @malloc(i64 %" << newBytesReg << ")\n";
        const int newDataReg = allocateRegister(fctx);
        *fctx.out << "  %" << newDataReg << " = bitcast i8* %" << rawNewDataReg << " to "
                  << view.elementType << "*\n";

        // Copy loop: `length` elements from view.dataPtrRef[start..start+length)
        // into newData[0..length) - same hand-verified alloca/load/store
        // counter idiom emitStrSlice's own str branch (and every other copy
        // loop in this backend) uses, no phi. Element-wise via GEP (not
        // byte-indexed): LLVM computes the correct stride for elementType
        // automatically, so this works unchanged for any elementType width.
        const int labelId = fctx.nextLabel++;
        const std::string headerLabel = "arrslice.copy.header" + std::to_string(labelId);
        const std::string bodyLabel = "arrslice.copy.body" + std::to_string(labelId);
        const std::string doneLabel = "arrslice.copy.done" + std::to_string(labelId);

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
        const int srcIdxReg = allocateRegister(fctx);
        *fctx.out << "  %" << srcIdxReg << " = add i32 " << startRef << ", %" << iForSrcReg << "\n";
        const int srcPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << srcPtrReg << " = getelementptr " << view.elementType << ", "
                  << view.elementType << "* " << view.dataPtrRef << ", i32 %" << srcIdxReg << "\n";
        const int elemValReg = allocateRegister(fctx);
        *fctx.out << "  %" << elemValReg << " = load " << view.elementType << ", "
                  << view.elementType << "* %" << srcPtrReg << "\n";
        const int iForDstReg = allocateRegister(fctx);
        *fctx.out << "  %" << iForDstReg << " = load i32, i32* %" << counterSlotReg << "\n";
        const int dstPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << dstPtrReg << " = getelementptr " << view.elementType << ", "
                  << view.elementType << "* %" << newDataReg << ", i32 %" << iForDstReg << "\n";
        *fctx.out << "  store " << view.elementType << " %" << elemValReg << ", "
                  << view.elementType << "* %" << dstPtrReg << "\n";
        const int iForIncReg = allocateRegister(fctx);
        *fctx.out << "  %" << iForIncReg << " = load i32, i32* %" << counterSlotReg << "\n";
        const int iNextReg = allocateRegister(fctx);
        *fctx.out << "  %" << iNextReg << " = add i32 %" << iForIncReg << ", 1\n";
        *fctx.out << "  store i32 %" << iNextReg << ", i32* %" << counterSlotReg << "\n";
        *fctx.out << "  br label %" << headerLabel << "\n";

        *fctx.out << doneLabel << ":\n";
        fctx.currentLabel = doneLabel;

        // Fresh List<T> header - same malloc + null-GEP sizeof idiom as
        // emitListNew, just with lengthReg (a runtime value) instead of the
        // literal 0 a brand-new empty list stores. Capacity (field 2 - see
        // ensureListCapacity's own comment for why it's field 2, not field
        // 1) is set to exactly `lengthReg` too - this is a one-shot,
        // exact-sized allocation with no extra slack, so capacity and
        // length start out equal, exactly like emitListNew's own capacity=0
        // is equal to its own length=0.
        const std::string headerType = "{i32, " + view.elementType + "*, i32}";
        const std::string pointerType = headerType + "*";
        const int sizePtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << sizePtrReg << " = getelementptr " << headerType << ", " << pointerType
                  << " null, i32 1\n";
        const int sizeIntReg = allocateRegister(fctx);
        *fctx.out << "  %" << sizeIntReg << " = ptrtoint " << pointerType << " %" << sizePtrReg
                  << " to i64\n";
        const int rawHeaderReg = allocateRegister(fctx);
        *fctx.out << "  %" << rawHeaderReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";
        const int destReg = defineRegister(strSlice.dest, fctx);
        *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawHeaderReg << " to " << pointerType
                  << "\n";
        const int lenPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", " << pointerType
                  << " " << ref(strSlice.dest, fctx) << ", i32 0, i32 0\n";
        *fctx.out << "  store i32 %" << lengthReg << ", i32* %" << lenPtrReg << "\n";
        const int dataFieldPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << dataFieldPtrReg << " = getelementptr " << headerType << ", "
                  << pointerType << " " << ref(strSlice.dest, fctx) << ", i32 0, i32 1\n";
        *fctx.out << "  store " << view.elementType << "* %" << newDataReg << ", "
                  << view.elementType << "** %" << dataFieldPtrReg << "\n";
        const int capFieldPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << capFieldPtrReg << " = getelementptr " << headerType << ", "
                  << pointerType << " " << ref(strSlice.dest, fctx) << ", i32 0, i32 2\n";
        *fctx.out << "  store i32 %" << lengthReg << ", i32* %" << capFieldPtrReg << "\n";
        return;
    }

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

LlvmIrEmitter::IndexableView LlvmIrEmitter::resolveIndexableView(int objectReg,
                                                                 FunctionContext& fctx)
{
    const std::string objectType = typeOf(objectReg, fctx);
    const std::string objectRef = ref(objectReg, fctx);

    if (isSliceType(objectType))
    {
        // {T*, i32} passed *by value* (see docs/language/0032-slices.md and
        // docs/language/0056-slice-printing.md) - both fields already sit
        // directly in the by-value struct via `extractvalue`, no GEP/load
        // needed (unlike the List branch below, which reaches through a
        // pointer to a heap record). Only emitJoin ever actually reaches
        // this branch in practice - emitStrSlice's own object is always
        // str-coercible/Array/List, never a bare slice<T> (`array[a..b]`
        // re-slicing a slice remains out of scope, per
        // docs/language/0050-collection-join-and-slicing.md's own Known
        // Imprecision).
        const std::string elementType = sliceElementType(objectType);
        const int dataReg = allocateRegister(fctx);
        *fctx.out << "  %" << dataReg << " = extractvalue " << objectType << " " << objectRef
                  << ", 0\n";
        const int lenReg = allocateRegister(fctx);
        *fctx.out << "  %" << lenReg << " = extractvalue " << objectType << " " << objectRef
                  << ", 1\n";
        return IndexableView{
            elementType, "%" + std::to_string(dataReg), "%" + std::to_string(lenReg)};
    }

    if (isListType(objectType) && !isStringType(objectType))
    {
        // {i32, T*, i32}* - GEP+load both the length field (0) and the data
        // pointer field (1), same field layout emitListNew/emitListPush/
        // emitIndexGet's own List branch already use. isStringType is
        // excluded explicitly, not accidentally - isListType's own loose
        // "{...}*" test also matches String's own {i32, i8*}* header, and
        // neither of this function's own two callers (emitStrSlice,
        // emitJoin) is ever legitimately called with a String object (see
        // emitStrSlice's own identical exclusion at its call site).
        const std::string headerType = objectType.substr(0, objectType.size() - 1);
        const std::string elementType = listElementType(objectType);
        const int lenPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", " << objectType
                  << " " << objectRef << ", i32 0, i32 0\n";
        const int lenReg = allocateRegister(fctx);
        *fctx.out << "  %" << lenReg << " = load i32, i32* %" << lenPtrReg << "\n";
        const int dataPtrPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", "
                  << objectType << " " << objectRef << ", i32 0, i32 1\n";
        const int dataPtrReg = allocateRegister(fctx);
        *fctx.out << "  %" << dataPtrReg << " = load " << elementType << "*, " << elementType
                  << "** %" << dataPtrPtrReg << "\n";
        return IndexableView{
            elementType, "%" + std::to_string(dataPtrReg), "%" + std::to_string(lenReg)};
    }

    // "[N x T]*" - a fixed-size array. N is already known at compile time
    // (arraySizeFromPointerType), so lengthRef is a literal, not a runtime
    // load - the one genuine difference from the List branch above.
    const std::string llvmArrayType = objectType.substr(0, objectType.size() - 1);
    const std::string elementType = arrayElementType(objectType);
    const int size = arraySizeFromPointerType(objectType);
    const int dataPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrReg << " = getelementptr " << llvmArrayType << ", " << objectType
              << " " << objectRef << ", i32 0, i32 0\n";
    return IndexableView{elementType, "%" + std::to_string(dataPtrReg), std::to_string(size)};
}

void LlvmIrEmitter::appendTextToBuffer(const std::string& bufferRef,
                                       const std::string& textPtrRef,
                                       FunctionContext& fctx)
{
    const int textLen64Reg = allocateRegister(fctx);
    *fctx.out << "  %" << textLen64Reg << " = call i64 @strlen(i8* " << textPtrRef << ")\n";
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

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 2\n";
    const int dataReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataReg << " = load i8*, i8** %" << dataPtrPtrReg << "\n";

    const int labelId = fctx.nextLabel++;
    const std::string headerLabel = "join.append.copy.header" + std::to_string(labelId);
    const std::string bodyLabel = "join.append.copy.body" + std::to_string(labelId);
    const std::string doneLabel = "join.append.copy.done" + std::to_string(labelId);

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
    *fctx.out << "  %" << srcElemPtrReg << " = getelementptr i8, i8* " << textPtrRef << ", i32 %"
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

void LlvmIrEmitter::emitJoin(const IrJoin& join, FunctionContext& fctx)
{
    const IndexableView view = resolveIndexableView(join.object, fctx);
    const std::string sepPtr = resolveStrPtr(join.separator, fctx);

    // A fresh Buffer, same initial {length: 0, capacity: 1, data: malloc(1)}
    // state emitBufferNew produces - this one has no Axea-level register of
    // its own (never referenced again after this function), so it's built
    // inline rather than via a real IrBufferNew instruction.
    const int sizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizePtrReg
              << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* null, i32 1\n";
    const int sizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << sizeIntReg << " = ptrtoint {i32, i32, i8*}* %" << sizePtrReg
              << " to i64\n";
    const int rawHeaderReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawHeaderReg << " = call i8* @malloc(i64 %" << sizeIntReg << ")\n";
    const int bufferReg = allocateRegister(fctx);
    *fctx.out << "  %" << bufferReg << " = bitcast i8* %" << rawHeaderReg
              << " to {i32, i32, i8*}*\n";
    const std::string bufferRef = "%" + std::to_string(bufferReg);
    const int initDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << initDataReg << " = call i8* @malloc(i64 1)\n";
    *fctx.out << "  store i8 0, i8* %" << initDataReg << "\n";
    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 0\n";
    *fctx.out << "  store i32 0, i32* %" << lenPtrReg << "\n";
    const int capPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << capPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 1\n";
    *fctx.out << "  store i32 1, i32* %" << capPtrReg << "\n";
    const int dataFieldPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataFieldPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 2\n";
    *fctx.out << "  store i8* %" << initDataReg << ", i8** %" << dataFieldPtrReg << "\n";

    // Guard: skip element 0 (and the whole loop) entirely if the object is
    // empty - avoids needing an `i != 0` branch inside the loop body to
    // decide whether to append the separator (see
    // docs/language/0050-collection-join-and-slicing.md's own Design
    // section: "append element 0 unconditionally, then loop from 1
    // appending separator+element").
    const int labelId = fctx.nextLabel++;
    const std::string nonEmptyLabel = "join.nonempty" + std::to_string(labelId);
    const std::string headerLabel = "join.loop.header" + std::to_string(labelId);
    const std::string bodyLabel = "join.loop.body" + std::to_string(labelId);
    const std::string doneLabel = "join.done" + std::to_string(labelId);

    const int isEmptyReg = allocateRegister(fctx);
    *fctx.out << "  %" << isEmptyReg << " = icmp eq i32 " << view.lengthRef << ", 0\n";
    *fctx.out << "  br i1 %" << isEmptyReg << ", label %" << doneLabel << ", label %"
              << nonEmptyLabel << "\n";

    *fctx.out << nonEmptyLabel << ":\n";
    fctx.currentLabel = nonEmptyLabel;
    const int elem0PtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << elem0PtrReg << " = getelementptr " << view.elementType << ", "
              << view.elementType << "* " << view.dataPtrRef << ", i32 0\n";
    const int elem0ValReg = allocateRegister(fctx);
    *fctx.out << "  %" << elem0ValReg << " = load " << view.elementType << ", " << view.elementType
              << "* %" << elem0PtrReg << "\n";
    const std::string elem0Text =
        stringifyValueOfType(view.elementType, "%" + std::to_string(elem0ValReg), fctx);
    appendTextToBuffer(bufferRef, elem0Text, fctx);

    const int counterSlotReg = allocateRegister(fctx);
    *fctx.out << "  %" << counterSlotReg << " = alloca i32\n";
    *fctx.out << "  store i32 1, i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << headerLabel << ":\n";
    fctx.currentLabel = headerLabel;
    const int iReg = allocateRegister(fctx);
    *fctx.out << "  %" << iReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int condReg = allocateRegister(fctx);
    *fctx.out << "  %" << condReg << " = icmp slt i32 %" << iReg << ", " << view.lengthRef << "\n";
    *fctx.out << "  br i1 %" << condReg << ", label %" << bodyLabel << ", label %" << doneLabel
              << "\n";

    *fctx.out << bodyLabel << ":\n";
    fctx.currentLabel = bodyLabel;
    appendTextToBuffer(bufferRef, sepPtr, fctx);
    const int iForElemReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForElemReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int elemPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << elemPtrReg << " = getelementptr " << view.elementType << ", "
              << view.elementType << "* " << view.dataPtrRef << ", i32 %" << iForElemReg << "\n";
    const int elemValReg = allocateRegister(fctx);
    *fctx.out << "  %" << elemValReg << " = load " << view.elementType << ", " << view.elementType
              << "* %" << elemPtrReg << "\n";
    const std::string elemText =
        stringifyValueOfType(view.elementType, "%" + std::to_string(elemValReg), fctx);
    appendTextToBuffer(bufferRef, elemText, fctx);
    const int iForIncReg = allocateRegister(fctx);
    *fctx.out << "  %" << iForIncReg << " = load i32, i32* %" << counterSlotReg << "\n";
    const int iNextReg = allocateRegister(fctx);
    *fctx.out << "  %" << iNextReg << " = add i32 %" << iForIncReg << ", 1\n";
    *fctx.out << "  store i32 %" << iNextReg << ", i32* %" << counterSlotReg << "\n";
    *fctx.out << "  br label %" << headerLabel << "\n";

    *fctx.out << doneLabel << ":\n";
    fctx.currentLabel = doneLabel;

    // Fresh String header, stealing the (possibly still fresh-empty, if the
    // object was empty) buffer's own length/data field values directly - no
    // byte copy, same "steal the fields" shape emitBufferFinish already
    // uses. The buffer itself is discarded here (never reset to a fresh
    // state the way emitBufferFinish resets its own buffer) since it has no
    // Axea-level variable anyone could still be holding a reference to.
    const int finalLenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << finalLenPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 0\n";
    const int finalLenReg = allocateRegister(fctx);
    *fctx.out << "  %" << finalLenReg << " = load i32, i32* %" << finalLenPtrReg << "\n";
    const int finalDataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << finalDataPtrPtrReg
              << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* " << bufferRef
              << ", i32 0, i32 2\n";
    const int finalDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << finalDataReg << " = load i8*, i8** %" << finalDataPtrPtrReg << "\n";

    const int strSizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << strSizePtrReg << " = getelementptr {i32, i8*}, {i32, i8*}* null, i32 1\n";
    const int strSizeIntReg = allocateRegister(fctx);
    *fctx.out << "  %" << strSizeIntReg << " = ptrtoint {i32, i8*}* %" << strSizePtrReg
              << " to i64\n";
    const int rawStrHeaderReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawStrHeaderReg << " = call i8* @malloc(i64 %" << strSizeIntReg << ")\n";
    const int destReg = defineRegister(join.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawStrHeaderReg << " to {i32, i8*}*\n";
    const int newLenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << newLenPtrReg << " = getelementptr {i32, i8*}, {i32, i8*}* "
              << ref(join.dest, fctx) << ", i32 0, i32 0\n";
    *fctx.out << "  store i32 %" << finalLenReg << ", i32* %" << newLenPtrReg << "\n";
    const int newDataFieldPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << newDataFieldPtrReg << " = getelementptr {i32, i8*}, {i32, i8*}* "
              << ref(join.dest, fctx) << ", i32 0, i32 1\n";
    *fctx.out << "  store i8* %" << finalDataReg << ", i8** %" << newDataFieldPtrReg << "\n";
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

void LlvmIrEmitter::ensureListCapacity(const std::string& headerType,
                                       const std::string& objectType,
                                       const std::string& listRef,
                                       const std::string& elementType,
                                       const std::string& neededRef,
                                       FunctionContext& fctx)
{
    // Capacity is field 2 here - see this function's own header comment for
    // why that's field 2, not field 1 the way Buffer's identical helper
    // uses (avoiding a real type collision with Deque<T>'s own header).
    const int capPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << capPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << listRef << ", i32 0, i32 2\n";
    const int capReg = allocateRegister(fctx);
    *fctx.out << "  %" << capReg << " = load i32, i32* %" << capPtrReg << "\n";
    const int needGrowReg = allocateRegister(fctx);
    *fctx.out << "  %" << needGrowReg << " = icmp sgt i32 " << neededRef << ", %" << capReg << "\n";

    const int labelId = fctx.nextLabel++;
    const std::string growLabel = "list.grow" + std::to_string(labelId);
    const std::string doneLabel = "list.grow.done" + std::to_string(labelId);
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

    // sizeof(elementType) via the standard null-pointer GEP idiom, same as
    // emitListPush's own original growth-allocation idiom.
    const int elemSizePtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << elemSizePtrReg << " = getelementptr " << elementType << ", "
              << elementType << "* null, i32 1\n";
    const int elemSizeReg = allocateRegister(fctx);
    *fctx.out << "  %" << elemSizeReg << " = ptrtoint " << elementType << "* %" << elemSizePtrReg
              << " to i64\n";
    const int newCap64Reg = allocateRegister(fctx);
    *fctx.out << "  %" << newCap64Reg << " = zext i32 %" << newCapReg << " to i64\n";
    const int newBytesReg = allocateRegister(fctx);
    *fctx.out << "  %" << newBytesReg << " = mul i64 %" << newCap64Reg << ", %" << elemSizeReg
              << "\n";
    const int rawNewDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawNewDataReg << " = call i8* @malloc(i64 %" << newBytesReg << ")\n";
    const int newDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << newDataReg << " = bitcast i8* %" << rawNewDataReg << " to " << elementType
              << "*\n";

    const int lenPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << listRef << ", i32 0, i32 0\n";
    const int lenReg = allocateRegister(fctx);
    *fctx.out << "  %" << lenReg << " = load i32, i32* %" << lenPtrReg << "\n";
    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr " << headerType << ", " << objectType
              << " " << listRef << ", i32 0, i32 1\n";
    const int oldDataReg = allocateRegister(fctx);
    *fctx.out << "  %" << oldDataReg << " = load " << elementType << "*, " << elementType << "** %"
              << dataPtrPtrReg << "\n";

    // Copy loop: the list's own existing `length` elements, old data -> new
    // data - same hand-verified alloca/load/store counter idiom
    // emitListPush's own original copy loop always used (no phi - see that
    // history for the full rationale).
    const std::string headerLabel = "list.grow.copy.header" + std::to_string(labelId);
    const std::string bodyLabel = "list.grow.copy.body" + std::to_string(labelId);
    const std::string copyDoneLabel = "list.grow.copy.done" + std::to_string(labelId);

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

    *fctx.out << copyDoneLabel << ":\n";
    fctx.currentLabel = copyDoneLabel;

    *fctx.out << "  store i32 %" << newCapReg << ", i32* %" << capPtrReg << "\n";
    *fctx.out << "  store " << elementType << "* %" << newDataReg << ", " << elementType << "** %"
              << dataPtrPtrReg << "\n";
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
    // Returns Optional<T> - {i1, T} - not T directly (see
    // docs/language/0052-optional.md): invalid input now yields a real
    // None instead of a silently-returned fallback like 0/0.0/false.
    const std::string optionalType = registerOptionalInstantiation(targetType);

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
        // non-digit/null terminator. No overflow checking. Success
        // requires both at least one digit consumed (digitCount > 0) and
        // the *entire* string consumed (the loop stopped at the null
        // terminator, not some other non-digit byte) - "123abc" is None,
        // not a truncated Some(123), matching typical `str::parse`
        // semantics. alloca/load/store for every loop-carried value
        // (idx/acc/sign/digitCount), no phi, `select` to apply the sign at
        // the end - the same idioms ensureBufferCapacity/encodeCharUtf8
        // already established.
        parseRuntimeText_ << R"(
define )" << optionalType << R"( @axea.parse.i32(i8* %s) {
entry:
  %idxSlot = alloca i32
  %accSlot = alloca i32
  %negSlot = alloca i32
  %digitCountSlot = alloca i32
  store i32 0, i32* %accSlot
  store i32 0, i32* %digitCountSlot
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
  %dc0 = load i32, i32* %digitCountSlot
  %dc1 = add i32 %dc0, 1
  store i32 %dc1, i32* %digitCountSlot
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
  %digitCount = load i32, i32* %digitCountSlot
  %hasDigits = icmp sgt i32 %digitCount, 0
  %stoppedAtNull = icmp eq i8 %c, 0
  %ok = and i1 %hasDigits, %stoppedAtNull
  %withOk = insertvalue )" << optionalType
                          << R"( undef, i1 %ok, 0
  %withValue = insertvalue )"
                          << optionalType << R"( %withOk, i32 %result, 1
  ret )" << optionalType << R"( %withValue
}
)";
        return fnName;
    }

    if (targetType == "i64")
    {
        const std::string fnName = "@axea.parse.i64";
        if (parseI64Registered_)
        {
            return fnName;
        }
        parseI64Registered_ = true;

        // The identical hand-rolled digit loop @axea.parse.i32 above uses,
        // just at 64-bit width throughout (see docs/language/0051-numeric-widening.md)
        // - same full-string-consumption success rule, no overflow checking.
        parseRuntimeText_ << R"(
define )" << optionalType << R"( @axea.parse.i64(i8* %s) {
entry:
  %idxSlot = alloca i32
  %accSlot = alloca i64
  %negSlot = alloca i32
  %digitCountSlot = alloca i32
  store i64 0, i64* %accSlot
  store i32 0, i32* %digitCountSlot
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
  %acc0 = load i64, i64* %accSlot
  %acc1 = mul i64 %acc0, 10
  %dv8 = sub i8 %c, 48
  %dv64 = zext i8 %dv8 to i64
  %acc2 = add i64 %acc1, %dv64
  store i64 %acc2, i64* %accSlot
  %dc0 = load i32, i32* %digitCountSlot
  %dc1 = add i32 %dc0, 1
  store i32 %dc1, i32* %digitCountSlot
  %idx2 = load i32, i32* %idxSlot
  %idx3 = add i32 %idx2, 1
  store i32 %idx3, i32* %idxSlot
  br label %loophdr
loopdone:
  %finalAcc = load i64, i64* %accSlot
  %negFlag = load i32, i32* %negSlot
  %isNegBool = icmp ne i32 %negFlag, 0
  %negated = sub i64 0, %finalAcc
  %result = select i1 %isNegBool, i64 %negated, i64 %finalAcc
  %digitCount = load i32, i32* %digitCountSlot
  %hasDigits = icmp sgt i32 %digitCount, 0
  %stoppedAtNull = icmp eq i8 %c, 0
  %ok = and i1 %hasDigits, %stoppedAtNull
  %withOk = insertvalue )" << optionalType
                          << R"( undef, i1 %ok, 0
  %withValue = insertvalue )"
                          << optionalType << R"( %withOk, i64 %result, 1
  ret )" << optionalType << R"( %withValue
}
)";
        return fnName;
    }

    if (targetType == "f64")
    {
        const std::string fnName = "@axea.parse.f64";
        if (parseF64Registered_)
        {
            return fnName;
        }
        parseF64Registered_ = true;

        // A real libc `strtod` call, not hand-rolled decimal-to-binary
        // float parsing (see this function's own header comment for why -
        // mirrors registerI32ToStrRuntime's own identical "reuse libc"
        // choice, in the opposite direction). `endptr` - previously always
        // `null` and discarded - is now a real output slot: success
        // requires both that at least one character was consumed
        // (endptr != s) and that the *entire* string was consumed (the
        // byte at endptr is the null terminator) - "3.14abc" is None, not
        // a truncated Some(3.14), matching parse<i32>'s own full-string-
        // consumption rule above.
        parseRuntimeText_ << R"(
define )" << optionalType << R"( @axea.parse.f64(i8* %s) {
entry:
  %endptrSlot = alloca i8*
  %result = call double @strtod(i8* %s, i8** %endptrSlot)
  %endptr = load i8*, i8** %endptrSlot
  %consumedSome = icmp ne i8* %endptr, %s
  %stopChar = load i8, i8* %endptr
  %fullyConsumed = icmp eq i8 %stopChar, 0
  %ok = and i1 %consumedSome, %fullyConsumed
  %withOk = insertvalue )" << optionalType
                          << R"( undef, i1 %ok, 0
  %withValue = insertvalue )"
                          << optionalType << R"( %withOk, double %result, 1
  ret )" << optionalType << R"( %withValue
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

    // Hand-rolled short-circuit byte comparison against the fixed literals
    // "true"/"false" (see docs/language/0046-generic-methods.md) -
    // branches, not an unrolled straight-line chain, specifically so a
    // short input (e.g. "t") never reads past its own null terminator:
    // each subsequent byte is only read after confirming the previous one
    // matched a non-null expected character, guaranteeing the string
    // continues at least that far. Exactly "true" or exactly "false" is
    // Some(true)/Some(false); anything else (including "TRUE", "", or
    // "trueish") is None - unlike the pre-Optional<T> version, which
    // treated every non-"true" input as a defined Some(false).
    parseRuntimeText_ << R"(
define )" << optionalType
                      << R"( @axea.parse.bool(i8* %s) {
entry:
  %c0p = getelementptr i8, i8* %s, i32 0
  %c0 = load i8, i8* %c0p
  %eT = icmp eq i8 %c0, 116
  br i1 %eT, label %t1, label %checkF0
t1:
  %tc1p = getelementptr i8, i8* %s, i32 1
  %tc1 = load i8, i8* %tc1p
  %te1 = icmp eq i8 %tc1, 114
  br i1 %te1, label %t2, label %invalid
t2:
  %tc2p = getelementptr i8, i8* %s, i32 2
  %tc2 = load i8, i8* %tc2p
  %te2 = icmp eq i8 %tc2, 117
  br i1 %te2, label %t3, label %invalid
t3:
  %tc3p = getelementptr i8, i8* %s, i32 3
  %tc3 = load i8, i8* %tc3p
  %te3 = icmp eq i8 %tc3, 101
  br i1 %te3, label %t4, label %invalid
t4:
  %tc4p = getelementptr i8, i8* %s, i32 4
  %tc4 = load i8, i8* %tc4p
  %te4 = icmp eq i8 %tc4, 0
  br i1 %te4, label %istrue, label %invalid
checkF0:
  %eF = icmp eq i8 %c0, 102
  br i1 %eF, label %f1, label %invalid
f1:
  %fc1p = getelementptr i8, i8* %s, i32 1
  %fc1 = load i8, i8* %fc1p
  %fe1 = icmp eq i8 %fc1, 97
  br i1 %fe1, label %f2, label %invalid
f2:
  %fc2p = getelementptr i8, i8* %s, i32 2
  %fc2 = load i8, i8* %fc2p
  %fe2 = icmp eq i8 %fc2, 108
  br i1 %fe2, label %f3, label %invalid
f3:
  %fc3p = getelementptr i8, i8* %s, i32 3
  %fc3 = load i8, i8* %fc3p
  %fe3 = icmp eq i8 %fc3, 115
  br i1 %fe3, label %f4, label %invalid
f4:
  %fc4p = getelementptr i8, i8* %s, i32 4
  %fc4 = load i8, i8* %fc4p
  %fe4 = icmp eq i8 %fc4, 101
  br i1 %fe4, label %f5, label %invalid
f5:
  %fc5p = getelementptr i8, i8* %s, i32 5
  %fc5 = load i8, i8* %fc5p
  %fe5 = icmp eq i8 %fc5, 0
  br i1 %fe5, label %isfalse, label %invalid
istrue:
  %rt0 = insertvalue )"
                      << optionalType << R"( undef, i1 1, 0
  %rt1 = insertvalue )"
                      << optionalType << R"( %rt0, i1 1, 1
  ret )" << optionalType
                      << R"( %rt1
isfalse:
  %rf0 = insertvalue )"
                      << optionalType << R"( undef, i1 1, 0
  %rf1 = insertvalue )"
                      << optionalType << R"( %rf0, i1 0, 1
  ret )" << optionalType
                      << R"( %rf1
invalid:
  %ri0 = insertvalue )"
                      << optionalType << R"( undef, i1 0, 0
  ret )" << optionalType
                      << R"( %ri0
}
)";
    return fnName;
}

void LlvmIrEmitter::emitParse(const IrParse& parse, FunctionContext& fctx)
{
    const std::string objectPtr = resolveStrPtr(parse.object, fctx);
    const std::string fnName = registerParseRuntime(parse.targetType);
    const std::string retType = registerOptionalInstantiation(parse.targetType);
    const int destReg = defineRegister(parse.dest, fctx);
    *fctx.out << "  %" << destReg << " = call " << retType << " " << fnName << "(i8* " << objectPtr
              << ")\n";
}

void LlvmIrEmitter::emitOptionalNew(const IrOptionalNew& optionalNew, FunctionContext& fctx)
{
    // Some(x)/None (see docs/language/0052-optional.md) - builds the
    // `{i1, T}` literal via two `insertvalue`s, mirroring slice<T>'s own
    // by-value fat-pointer construction in emitStrSlice/emitStrLiteral
    // (ptr then length, insertvalue twice) - just `hasValue` then payload
    // here. The intermediate `withOk` register must be allocated (and its
    // line emitted) *before* `optionalNew.dest` is defined, not after -
    // LLVM requires unnamed registers in strictly increasing textual
    // order (see docs/language/0021-axea-ir.md's own note on this, and
    // every other insertvalue-chain emitter in this file).
    const std::string optionalType = typeOf(optionalNew.dest, fctx);
    const std::string payloadType = optionalPayloadType(optionalType);
    const std::string hasValueBit = optionalNew.value != -1 ? "1" : "0";
    const int withOk = allocateRegister(fctx);
    *fctx.out << "  %" << withOk << " = insertvalue " << optionalType << " undef, i1 "
              << hasValueBit << ", 0\n";
    // None's payload field is never read (every consumer checks hasValue
    // first) - `undef` is the correct, standard LLVM spelling for "this
    // bit pattern is never observed", not a placeholder for a missing
    // feature.
    const std::string payloadRef = optionalNew.value != -1 ? ref(optionalNew.value, fctx) : "undef";
    const int destReg = defineRegister(optionalNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = insertvalue " << optionalType << " %" << withOk << ", "
              << payloadType << " " << payloadRef << ", 1\n";
}

void LlvmIrEmitter::emitResultNew(const IrResultNew& resultNew, FunctionContext& fctx)
{
    // Ok(x)/Err(e) (see docs/language/0063-result.md) - the 3-field direct
    // extension of emitOptionalNew's own `{i1, T}` construction just
    // above: `{i1, T, E}` via three `insertvalue`s (isOk bit, Ok payload,
    // Err payload) instead of two. Whichever payload `resultNew.value`
    // doesn't cover gets `undef`, the identical "never observed, so
    // `undef` is the correct spelling, not a placeholder" reasoning
    // emitOptionalNew's own None case already established - every
    // consumer here checks `isOk` before reading either payload.
    const std::string resultType = typeOf(resultNew.dest, fctx);
    const std::string okType = resultOkPayloadType(resultType);
    const std::string errType = resultErrPayloadType(resultType);
    const std::string isOkBit = resultNew.isOk ? "1" : "0";
    const int withTag = allocateRegister(fctx);
    *fctx.out << "  %" << withTag << " = insertvalue " << resultType << " undef, i1 " << isOkBit
              << ", 0\n";

    const std::string okRef = resultNew.isOk ? ref(resultNew.value, fctx) : "undef";
    const int withOkPayload = allocateRegister(fctx);
    *fctx.out << "  %" << withOkPayload << " = insertvalue " << resultType << " %" << withTag
              << ", " << okType << " " << okRef << ", 1\n";

    const std::string errRef = !resultNew.isOk ? ref(resultNew.value, fctx) : "undef";
    const int destReg = defineRegister(resultNew.dest, fctx);
    *fctx.out << "  %" << destReg << " = insertvalue " << resultType << " %" << withOkPayload
              << ", " << errType << " " << errRef << ", 2\n";
}

void LlvmIrEmitter::emitOptionalIsSome(const IrOptionalIsSome& isSome, FunctionContext& fctx)
{
    // `.is_some()`/`.is_none()` (see docs/language/0052-optional.md) - a
    // plain `extractvalue` of field 0, optionally inverted with `xor` for
    // is_none (negate) - avoids a second near-identical instruction.
    const std::string optionalType = typeOf(isSome.object, fctx);
    if (!isSome.negate)
    {
        const int destReg = defineRegister(isSome.dest, fctx);
        *fctx.out << "  %" << destReg << " = extractvalue " << optionalType << " "
                  << ref(isSome.object, fctx) << ", 0\n";
        return;
    }
    const int rawReg = allocateRegister(fctx);
    *fctx.out << "  %" << rawReg << " = extractvalue " << optionalType << " "
              << ref(isSome.object, fctx) << ", 0\n";
    const int destReg = defineRegister(isSome.dest, fctx);
    *fctx.out << "  %" << destReg << " = xor i1 %" << rawReg << ", true\n";
}

void LlvmIrEmitter::emitOptionalUnwrap(const IrOptionalUnwrap& unwrap, FunctionContext& fctx)
{
    // `?`'s then-branch and `.unwrap_or`'s is-some branch (see
    // docs/language/0052-optional.md) - a plain `extractvalue` of `field`
    // (1 in every case but one: `?`'s Result-flavored Err-propagation
    // path sets it to 2 - see docs/language/0063-result.md and this
    // instruction's own comment in Ir.hpp); the surrounding IrBranch each
    // is emitted inside already guarantees the extraction is safe
    // whenever this runs, so no check is needed here.
    const std::string optionalType = typeOf(unwrap.object, fctx);
    const int destReg = defineRegister(unwrap.dest, fctx);
    *fctx.out << "  %" << destReg << " = extractvalue " << optionalType << " "
              << ref(unwrap.object, fctx) << ", " << unwrap.field << "\n";
}

void LlvmIrEmitter::emitToCstr(const IrToCstr& toCstr, FunctionContext& fctx)
{
    const std::string objectPtr = resolveStrPtr(toCstr.object, fctx);
    const int destReg = defineRegister(toCstr.dest, fctx);
    *fctx.out << "  %" << destReg << " = bitcast i8* " << objectPtr << " to i8*\n";
}

void LlvmIrEmitter::registerPrintRuntime()
{
    if (printRuntimeRegistered_)
    {
        return;
    }
    printRuntimeRegistered_ = true;

    // Three fixed format-string globals (see
    // docs/language/Axea_Printing_Formatting.md) - declared in this own
    // self-contained runtime-text block for the same "no dependency on
    // collectStrings/emitStringGlobals' own timing" reason
    // registerI32ToStrRuntime's own format-string global is.
    printRuntimeText_ << R"(
@axea.fmt.s = private unnamed_addr constant [3 x i8] c"%s\00"
@axea.fmt.space = private unnamed_addr constant [2 x i8] c" \00"
@axea.fmt.nl = private unnamed_addr constant [2 x i8] c"\0A\00"
)";
}

void LlvmIrEmitter::emitPrint(const IrPrint& print, FunctionContext& fctx)
{
    registerPrintRuntime();
    const std::string sFmt = "getelementptr ([3 x i8], [3 x i8]* @axea.fmt.s, i64 0, i64 0)";
    const std::string spaceFmt =
        "getelementptr ([2 x i8], [2 x i8]* @axea.fmt.space, i64 0, i64 0)";
    const std::string nlFmt = "getelementptr ([2 x i8], [2 x i8]* @axea.fmt.nl, i64 0, i64 0)";

    // Each argument stringified independently (see stringifyValue),
    // printed via a bare "%s" format, with a literal space between
    // consecutive arguments (docs/language/Axea_Printing_Formatting.md's
    // own "multiple arguments are separated by spaces") - a plain
    // literal-format-string printf call, no varargs needed for the
    // space/newline themselves.
    for (std::size_t i = 0; i < print.args.size(); ++i)
    {
        if (i > 0)
        {
            *fctx.out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                      << spaceFmt << ")\n";
        }
        const std::string argType = typeOf(print.args[i], fctx);
        if (isNamedStructPointerType(argType))
        {
            // A struct argument prints directly - reuses the exact
            // per-struct-type helper the top-level binding printer
            // already calls (emitStructPrintHelpers), rather than
            // routing through stringifyValue: structs have no
            // "stringify to a heap string" sibling (only Optional<T>
            // does - see docs/language/0052-optional.md's own
            // @axea.optional.<id>.to_str), but print()/write() never
            // actually need a string, only to print directly, so this
            // is real support, not a workaround.
            const std::string structName = structNameFromPointerType(argType);
            *fctx.out << "  call void @axea.print." << structName << "(" << argType << " "
                      << ref(print.args[i], fctx) << ")\n";
            continue;
        }
        const std::string ptr = stringifyValue(print.args[i], fctx);
        *fctx.out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                  << sFmt << ", i8* " << ptr << ")\n";
    }
    if (print.addNewline)
    {
        *fctx.out << "  %" << allocateRegister(fctx) << " = call i32 (i8*, ...) @printf(i8* "
                  << nlFmt << ")\n";
    }
    // IrPrint's own dest is unit-typed - never defined as a real LLVM
    // register (mirrors IrBufferAppend/IrBufferAppendValue's own
    // identical choice), since a "void"-typed Axea register is never
    // read via ref() anywhere in this backend (see
    // docs/language/0033-lists.md).
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

std::string LlvmIrEmitter::registerUtf8CharAtRuntime()
{
    const std::string fnName = "@axea.utf8.char_at";
    if (utf8CharAtRegistered_)
    {
        return fnName;
    }
    utf8CharAtRegistered_ = true;

    // Single-character indexing (`s[i]`) - a real Unicode *codepoint*
    // index, matching `.length`'s own codepoint-not-byte counting (see
    // registerUtf8CountRuntime above and docs/language/0047-unicode.md).
    // Unlike that function's own byte-at-a-time walk (which only ever
    // needs to *skip* continuation bytes, never decode them), this has to
    // decode a full multi-byte sequence once the target codepoint is
    // found - lead-byte-length detection and continuation-byte bit
    // formulas mirror Parser::decodeCharLiteral's own compile-time-literal
    // decoder exactly, just as a runtime loop over arbitrary string data
    // instead. alloca/load/store throughout (posSlot/cntSlot/resultSlot/
    // lenSlot), no phi - every decode-length branch below stores its own
    // result/len pair before jumping to the shared afterDecode merge
    // point, the same "store-then-reload-after-the-branch" technique this
    // whole backend already uses everywhere else instead of phi. Out-of-
    // range (including a malformed/too-short string) returns codepoint 0
    // - the walk never reads past the string's own nul terminator (the
    // `atEnd` check happens before any read of that iteration's own lead
    // byte), so this is memory-safe by construction, mirroring
    // registerUtf8CountRuntime's own identical property - no separate
    // bounds check needed.
    utf8CharAtRuntimeText_ << R"(
define i24 @axea.utf8.char_at(i8* %s, i32 %index) {
entry:
  %posSlot = alloca i32
  %cntSlot = alloca i32
  %resultSlot = alloca i32
  %lenSlot = alloca i32
  store i32 0, i32* %posSlot
  store i32 0, i32* %cntSlot
  br label %loophdr
loophdr:
  %pos = load i32, i32* %posSlot
  %leadPtr = getelementptr i8, i8* %s, i32 %pos
  %lead = load i8, i8* %leadPtr
  %atEnd = icmp eq i8 %lead, 0
  br i1 %atEnd, label %notfound, label %check1
check1:
  %masked1 = and i8 %lead, 128
  %is1 = icmp eq i8 %masked1, 0
  br i1 %is1, label %len1, label %check2
len1:
  %cp1 = zext i8 %lead to i32
  store i32 %cp1, i32* %resultSlot
  store i32 1, i32* %lenSlot
  br label %afterDecode
check2:
  %masked2 = and i8 %lead, 224
  %is2 = icmp eq i8 %masked2, 192
  br i1 %is2, label %len2, label %check3
len2:
  %cp0_2 = and i8 %lead, 31
  %cp0_2i = zext i8 %cp0_2 to i32
  %pos1_2 = add i32 %pos, 1
  %c1ptr_2 = getelementptr i8, i8* %s, i32 %pos1_2
  %c1_2 = load i8, i8* %c1ptr_2
  %c1m_2 = and i8 %c1_2, 63
  %c1mi_2 = zext i8 %c1m_2 to i32
  %sh0_2 = shl i32 %cp0_2i, 6
  %r_2 = or i32 %sh0_2, %c1mi_2
  store i32 %r_2, i32* %resultSlot
  store i32 2, i32* %lenSlot
  br label %afterDecode
check3:
  %masked3 = and i8 %lead, 240
  %is3 = icmp eq i8 %masked3, 224
  br i1 %is3, label %len3, label %len4
len3:
  %cp0_3 = and i8 %lead, 15
  %cp0_3i = zext i8 %cp0_3 to i32
  %pos1_3 = add i32 %pos, 1
  %c1ptr_3 = getelementptr i8, i8* %s, i32 %pos1_3
  %c1_3 = load i8, i8* %c1ptr_3
  %c1m_3 = and i8 %c1_3, 63
  %c1mi_3 = zext i8 %c1m_3 to i32
  %pos2_3 = add i32 %pos, 2
  %c2ptr_3 = getelementptr i8, i8* %s, i32 %pos2_3
  %c2_3 = load i8, i8* %c2ptr_3
  %c2m_3 = and i8 %c2_3, 63
  %c2mi_3 = zext i8 %c2m_3 to i32
  %sh0_3 = shl i32 %cp0_3i, 12
  %sh1_3 = shl i32 %c1mi_3, 6
  %t01_3 = or i32 %sh0_3, %sh1_3
  %r_3 = or i32 %t01_3, %c2mi_3
  store i32 %r_3, i32* %resultSlot
  store i32 3, i32* %lenSlot
  br label %afterDecode
len4:
  %cp0_4 = and i8 %lead, 7
  %cp0_4i = zext i8 %cp0_4 to i32
  %pos1_4 = add i32 %pos, 1
  %c1ptr_4 = getelementptr i8, i8* %s, i32 %pos1_4
  %c1_4 = load i8, i8* %c1ptr_4
  %c1m_4 = and i8 %c1_4, 63
  %c1mi_4 = zext i8 %c1m_4 to i32
  %pos2_4 = add i32 %pos, 2
  %c2ptr_4 = getelementptr i8, i8* %s, i32 %pos2_4
  %c2_4 = load i8, i8* %c2ptr_4
  %c2m_4 = and i8 %c2_4, 63
  %c2mi_4 = zext i8 %c2m_4 to i32
  %pos3_4 = add i32 %pos, 3
  %c3ptr_4 = getelementptr i8, i8* %s, i32 %pos3_4
  %c3_4 = load i8, i8* %c3ptr_4
  %c3m_4 = and i8 %c3_4, 63
  %c3mi_4 = zext i8 %c3m_4 to i32
  %sh0_4 = shl i32 %cp0_4i, 18
  %sh1_4 = shl i32 %c1mi_4, 12
  %sh2_4 = shl i32 %c2mi_4, 6
  %t01_4 = or i32 %sh0_4, %sh1_4
  %t012_4 = or i32 %t01_4, %sh2_4
  %r_4 = or i32 %t012_4, %c3mi_4
  store i32 %r_4, i32* %resultSlot
  store i32 4, i32* %lenSlot
  br label %afterDecode
afterDecode:
  %cnt = load i32, i32* %cntSlot
  %isMatch = icmp eq i32 %cnt, %index
  br i1 %isMatch, label %found, label %advance
found:
  %finalI32 = load i32, i32* %resultSlot
  %finalI24 = trunc i32 %finalI32 to i24
  ret i24 %finalI24
advance:
  %len = load i32, i32* %lenSlot
  %newPos = add i32 %pos, %len
  store i32 %newPos, i32* %posSlot
  %newCnt = add i32 %cnt, 1
  store i32 %newCnt, i32* %cntSlot
  br label %loophdr
notfound:
  ret i24 0
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

void LlvmIrEmitter::emitBufferAppendValue(const IrBufferAppendValue& appendValue,
                                          FunctionContext& fctx)
{
    // Structurally identical to emitBufferAppend just above - the only
    // difference is stringifyValue in place of resolveStrPtr, since
    // `value` isn't necessarily str-coercible on its own (i32/bool/char
    // - see docs/language/Axea_Printing_Formatting.md). Not factored
    // into a shared helper, per this codebase's own "separate over
    // shared" convention for whole operations.
    const std::string bufferRef = ref(appendValue.buffer, fctx);
    std::string textPtr;
    if (appendValue.debug)
    {
        // `{expr:?}` (see docs/language/0058-debug-formatting.md) -
        // TypeChecker guarantees `formatSpec` is empty whenever `debug`
        // is set, so this is checked first, independent of the
        // formatSpec branch below.
        textPtr = stringifyValueDebug(appendValue.value, fctx);
    }
    else if (appendValue.formatSpec.empty())
    {
        textPtr = stringifyValue(appendValue.value, fctx);
    }
    else
    {
        // `{expr:spec}` (see docs/language/0055-numeric-format-specs.md
        // and docs/language/0057-alignment.md).
        const FormatSpec spec = parseFormatSpec(appendValue.formatSpec);
        if (spec.align == '\0')
        {
            // TypeChecker already validated the spec against this value's
            // own checked type, so its LLVM type here is always exactly
            // one of i32/i64/double.
            const std::string elementType = typeOf(appendValue.value, fctx);
            const std::string fnName = registerFormatRuntime(elementType, spec);
            const int destReg = allocateRegister(fctx);
            *fctx.out << "  %" << destReg << " = call i8* " << fnName << "(" << elementType << " "
                      << ref(appendValue.value, fctx) << ")\n";
            textPtr = "%" + std::to_string(destReg);
        }
        else
        {
            // Alignment applies to any text-representable type, not just
            // numeric (see TypeChecker's own relaxation and
            // registerAlignPadRuntime's header comment) - compute the
            // piece's own unpadded core text first (a radix conversion or
            // precision still delegates to registerFormatRuntime, called
            // with width/zeroPad zeroed out so it produces natural-width
            // text, reusing its existing conversion logic rather than
            // duplicating it; anything else falls back to the same
            // generic stringifyValue every unformatted piece already
            // uses), then pad that text to width via the shared
            // @axea.align.pad runtime function.
            std::string coreTextPtr;
            if (spec.type != '\0' || spec.precision.has_value())
            {
                FormatSpec unpadded = spec;
                unpadded.width = 0;
                unpadded.zeroPad = false;
                const std::string elementType = typeOf(appendValue.value, fctx);
                const std::string fnName = registerFormatRuntime(elementType, unpadded);
                const int coreReg = allocateRegister(fctx);
                *fctx.out << "  %" << coreReg << " = call i8* " << fnName << "(" << elementType
                          << " " << ref(appendValue.value, fctx) << ")\n";
                coreTextPtr = "%" + std::to_string(coreReg);
            }
            else
            {
                coreTextPtr = stringifyValue(appendValue.value, fctx);
            }
            const std::string padFnName = registerAlignPadRuntime();
            const int paddedReg = allocateRegister(fctx);
            *fctx.out << "  %" << paddedReg << " = call i8* " << padFnName << "(i8* " << coreTextPtr
                      << ", i32 " << spec.width << ", i8 " << static_cast<int>(spec.align) << ")\n";
            textPtr = "%" + std::to_string(paddedReg);
        }
    }

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

    const int dataPtrPtrReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataPtrPtrReg << " = getelementptr {i32, i32, i8*}, {i32, i32, i8*}* "
              << bufferRef << ", i32 0, i32 2\n";
    const int dataReg = allocateRegister(fctx);
    *fctx.out << "  %" << dataReg << " = load i8*, i8** %" << dataPtrPtrReg << "\n";

    const int labelId = fctx.nextLabel++;
    const std::string headerLabel = "buffer.appendvalue.copy.header" + std::to_string(labelId);
    const std::string bodyLabel = "buffer.appendvalue.copy.body" + std::to_string(labelId);
    const std::string doneLabel = "buffer.appendvalue.copy.done" + std::to_string(labelId);

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
        // sides that both reached the merge block *and* produced a real,
        // non-void register are valid phi predecessors. A side whose own
        // trailing expression is a unit-typed call (e.g. `self.items.set(...)`,
        // a struct method returning nothing) still gets a real, non-"-1"
        // dest register (see IrCall's own "x = numbers.push(4) is legal"
        // convention) - but a void-typed register is never assigned a real
        // LLVM SSA value (no "%N = ..." is ever emitted for it, matching
        // every other void-producing construct in this backend), so ref()
        // on it would look up a register llvmRegisterOf never received -
        // excluded here via the same typeOf(...) != "void" check that
        // gates every other "never ref() a void register" site.
        std::vector<std::pair<std::string, std::string>> incoming; // (value ref, predecessor label)
        if (!thenTerminated && branch.thenValue != -1 && typeOf(branch.thenValue, fctx) != "void")
        {
            incoming.emplace_back(ref(branch.thenValue, fctx), thenExitLabel);
        }
        if (!elseTerminated && branch.elseValue != -1 && typeOf(branch.elseValue, fctx) != "void")
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

    // Every outer-scope name either branch reassigned (see IrGenerator::mergeBranchScopes and
    // docs/language/0021-axea-ir.md's own follow-up) - orthogonal to branch.dest above (that's
    // the if-*expression's* own value; this is side-effecting reassignment, present or absent
    // independently of whether the if's own result is ever used). Same "only a side that both
    // reached the merge block *and* produced a real, non-void register is a valid phi
    // predecessor" filtering as branch.dest's own phi just above.
    for (const auto& [thenReg, elseReg, mergedDest] : branch.carriedMerges)
    {
        std::vector<std::pair<std::string, std::string>> incoming;
        if (!thenTerminated && typeOf(thenReg, fctx) != "void")
        {
            incoming.emplace_back(ref(thenReg, fctx), thenExitLabel);
        }
        if (!elseTerminated && typeOf(elseReg, fctx) != "void")
        {
            incoming.emplace_back(ref(elseReg, fctx), elseExitLabel);
        }
        if (incoming.empty())
        {
            continue;
        }
        const int destReg = defineRegister(mergedDest, fctx);
        *fctx.out << "  %" << destReg << " = phi " << typeOf(mergedDest, fctx);
        for (std::size_t i = 0; i < incoming.size(); ++i)
        {
            *fctx.out << (i == 0 ? " [ " : ", [ ") << incoming[i].first << ", %"
                      << incoming[i].second << " ]";
        }
        *fctx.out << "\n";
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

    // The break-value phi (if any) must come *before* the carried-slot reads just below: LLVM
    // requires every phi node in a basic block to appear first, before any non-phi instruction -
    // emitting the carried-slot loads first (as this used to) produces a phi preceded by a load
    // in the same block, which is invalid IR that clang's own verifier only catches at codegen
    // time when running without the (deliberately disabled, see 0021-axea-ir.md) IR verifier.
    // The two are independent (the phi's own values were already captured as resolved LLVM
    // value-reference strings while lowering the loop body, via IrBreak's own handling - nothing
    // here depends on the carried-slot reads below), so reordering is purely cosmetic to LLVM's
    // own well-formedness rule, not a behavior change.
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
        if (const auto* constInt64 = dynamic_cast<const IrConstInt64*>(inst.get()))
        {
            const int destReg = defineRegister(constInt64->dest, fctx);
            *fctx.out << "  %" << destReg << " = add i64 0, " << constInt64->value << "\n";
            continue;
        }
        if (const auto* constFloat = dynamic_cast<const IrConstFloat*>(inst.get()))
        {
            const int destReg = defineRegister(constFloat->dest, fctx);
            *fctx.out << "  %" << destReg << " = fadd double 0.0, "
                      << formatDoubleLiteral(constFloat->value) << "\n";
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
        if (const auto* constNull = dynamic_cast<const IrConstNull*>(inst.get()))
        {
            // Same "materialize as a trivial SSA value" convention as every other IrConst* above,
            // adapted for a pointer: the established null-GEP idiom this file already uses for
            // sizeof<T>() (a compile-time-known offset from a null pointer of the right type) -
            // offset 0 yields back exactly `null` itself as a real, uniformly-"%N"-addressable
            // register, not an inlined "null" text constant.
            const std::string pointerType = typeOf(constNull->dest, fctx);
            const std::string pointeeType = pointerType.substr(0, pointerType.size() - 1);
            const int destReg = defineRegister(constNull->dest, fctx);
            *fctx.out << "  %" << destReg << " = getelementptr " << pointeeType << ", "
                      << pointerType << " null, i32 0\n";
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

            // str/String compare by real content (emitStrComparison, via
            // registerKeyRuntime's own @axea.eq.str / registerOrderRuntime's
            // own @axea.less.str) rather than pointer identity - String is
            // first resolved to its own bare i8* data pointer via
            // resolveStrPtrOfType, the same str-coercion every other
            // String-accepting operation already shares (see
            // docs/language/0042-string.md). Ordering (`<`/`<=`/`>`/`>=`)
            // never reaches here with a String operand -
            // TypeChecker::isOrderableKind only accepts str, not String.
            // defineRegister(binOp->dest, ...) is deliberately *not* called
            // up front here - emitStrComparison calls it itself, as the
            // last register it allocates (see its own comment).
            if (lhsType == "i8*" || isStringType(lhsType))
            {
                const std::string lhsPtr = resolveStrPtrOfType(lhsType, lhsRef, fctx);
                const std::string rhsPtr = resolveStrPtrOfType(lhsType, rhsRef, fctx);
                emitStrComparison(binOp->dest, binOp->op, lhsPtr, rhsPtr, fctx);
                continue;
            }

            // `ptr + i` / `ptr - i` (see docs/language/0019-unsafe.md) - a single-index GEP,
            // element-scaled automatically by LLVM's own type system, exactly like every slice/
            // List/Array index GEP already is (see emitIndexGet). Minus negates the offset first
            // so both operators reduce to the identical GEP shape. Deliberately excludes
            // EqualEqual/BangEqual (`ptr == ptr2`/`ptr != null`, see docs/language/0019-unsafe.md's
            // own null-pointer support) - a real, previously-undiscovered bug found while porting
            // LinkedList<T>'s own self-referential `*Node<T>` traversal: this check used to fire
            // for *every* operator whenever the left operand was pointer-typed, so a pointer
            // equality comparison was silently treated as pointer arithmetic instead (emitting a
            // GEP whose own "offset" operand was the *right-hand pointer value itself*, reinterpreted
            // as an integer index - malformed IR, not merely wrong results), rather than falling
            // through to the ordinary `icmp eq`/`icmp ne` path below, which already handles pointer
            // operands correctly (LLVM's `icmp` accepts pointer types natively). Untested before now
            // - nothing in this codebase ever compared two pointer values until this port.
            if (isPointerType(lhsType) && binOp->op != TokenKind::EqualEqual &&
                binOp->op != TokenKind::BangEqual)
            {
                const std::string elementType = pointerElementType(lhsType);
                const std::string rhsType = typeOf(binOp->rhs, fctx);
                if (binOp->op == TokenKind::Minus)
                {
                    const int negReg = allocateRegister(fctx);
                    *fctx.out << "  %" << negReg << " = sub " << rhsType << " 0, " << rhsRef
                              << "\n";
                    const int destReg = defineRegister(binOp->dest, fctx);
                    *fctx.out << "  %" << destReg << " = getelementptr " << elementType << ", "
                              << lhsType << " " << lhsRef << ", " << rhsType << " %" << negReg
                              << "\n";
                }
                else
                {
                    const int destReg = defineRegister(binOp->dest, fctx);
                    *fctx.out << "  %" << destReg << " = getelementptr " << elementType << ", "
                              << lhsType << " " << lhsRef << ", " << rhsType << " " << rhsRef
                              << "\n";
                }
                continue;
            }

            // f64 gets its own opcode table (fadd/fsub/.../fcmp - see
            // floatBinOpMnemonic) - i32/i64 share binOpMnemonic's existing
            // integer opcodes unchanged, since LLVM's int opcodes are
            // already width-agnostic text (same reasoning icmp/char's own
            // i24 already established).
            const std::string mnemonic =
                lhsType == "double" ? floatBinOpMnemonic(binOp->op) : binOpMnemonic(binOp->op);
            const int destReg = defineRegister(binOp->dest, fctx);
            *fctx.out << "  %" << destReg << " = " << mnemonic << " " << lhsType << " " << lhsRef
                      << ", " << rhsRef << "\n";
            continue;
        }
        if (const auto* cast = dynamic_cast<const IrCast*>(inst.get()))
        {
            // `<operand> as <targetType>` (see docs/language/0005-type-system.md)
            // - both sides are always i32/i64/f64 (TypeChecker::checkExpr's
            // own CastExpr case already guarantees this), so exactly the 9
            // (srcType, dstType) pairs below are ever reached: same-type
            // (harmless, materialized the same trivial no-op-arithmetic way
            // every constant here already is - not an identity bitcast,
            // to sidestep any LLVM-version question about whether that's
            // still legal IR), i32<->i64 (sext/trunc), and int<->double
            // (sitofp/fptosi).
            const std::string srcType = typeOf(cast->operand, fctx);
            const std::string srcRef = ref(cast->operand, fctx);
            const std::string dstType = llvmType(cast->targetType);
            const int destReg = defineRegister(cast->dest, fctx);

            // Pointer-to-pointer cast (see docs/language/0006-generics.md's own List<T> port
            // follow-up) - a plain bitcast, free and correct by construction (the underlying
            // bytes are unambiguous machine bytes; TypeChecker's own CastExpr case already
            // guarantees this only reaches here inside an `unsafe` block).
            if (isPointerType(srcType) && isPointerType(dstType))
            {
                *fctx.out << "  %" << destReg << " = bitcast " << srcType << " " << srcRef
                          << " to " << dstType << "\n";
                continue;
            }

            if (srcType == dstType)
            {
                const std::string zeroOp = dstType == "double" ? "fadd double 0.0, " + srcRef
                                                               : "add " + dstType + " 0, " + srcRef;
                *fctx.out << "  %" << destReg << " = " << zeroOp << "\n";
            }
            else if (srcType == "i32" && dstType == "i64")
            {
                *fctx.out << "  %" << destReg << " = sext i32 " << srcRef << " to i64\n";
            }
            else if (srcType == "i64" && dstType == "i32")
            {
                *fctx.out << "  %" << destReg << " = trunc i64 " << srcRef << " to i32\n";
            }
            else if (dstType == "double")
            {
                *fctx.out << "  %" << destReg << " = sitofp " << srcType << " " << srcRef
                          << " to double\n";
            }
            else
            {
                *fctx.out << "  %" << destReg << " = fptosi double " << srcRef << " to " << dstType
                          << "\n";
            }
            continue;
        }
        if (const auto* sizeOf = dynamic_cast<const IrSizeOf*>(inst.get()))
        {
            // `sizeof<T>()` (see docs/language/0006-generics.md's own List<T> port follow-up) -
            // the same null-pointer-GEP + ptrtoint idiom emitListNew/emitStructNew/emitArrayNew
            // already use internally to compute a concrete LLVM type's own byte size, exposed
            // here as a real user-facing instruction for the first time.
            std::string elementType = llvmType(sizeOf->typeName);
            std::string pointerType;
            if (!elementType.empty() && elementType.back() == '*')
            {
                pointerType = elementType;
                elementType.pop_back();
            }
            else
            {
                pointerType = elementType + "*";
            }
            const int sizePtrReg = allocateRegister(fctx);
            *fctx.out << "  %" << sizePtrReg << " = getelementptr " << elementType << ", "
                      << pointerType << " null, i32 1\n";
            const int destReg = defineRegister(sizeOf->dest, fctx);
            *fctx.out << "  %" << destReg << " = ptrtoint " << pointerType << " %" << sizePtrReg
                      << " to i64\n";
            continue;
        }

        if (const auto* hashOf = dynamic_cast<const IrHashOf*>(inst.get()))
        {
            // `hash<T>()` (see docs/language/0034-maps-and-sets.md's own "2026 Update") - the same
            // registerKeyRuntime call Map<K,V>/Set<T>'s own set/get/etc used to make internally
            // before they became real generic structs, now reachable directly from Axea source.
            const std::string keyType = llvmType(hashOf->typeName);
            const auto [hashFn, unusedEqFn] = registerKeyRuntime(hashOf->typeName);
            const int destReg = defineRegister(hashOf->dest, fctx);
            *fctx.out << "  %" << destReg << " = call i32 " << hashFn << "(" << keyType << " "
                      << ref(hashOf->value, fctx) << ")\n";
            continue;
        }

        if (const auto* keyEq = dynamic_cast<const IrKeyEq*>(inst.get()))
        {
            const std::string keyType = llvmType(keyEq->typeName);
            const auto [unusedHashFn, eqFn] = registerKeyRuntime(keyEq->typeName);
            const int destReg = defineRegister(keyEq->dest, fctx);
            *fctx.out << "  %" << destReg << " = call i1 " << eqFn << "(" << keyType << " "
                      << ref(keyEq->left, fctx) << ", " << keyType << " " << ref(keyEq->right, fctx)
                      << ")\n";
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

            // `malloc`/`free` (see docs/language/0019-unsafe.md) always call the one shared
            // `i8* @malloc(i64)` / `void @free(i8*)` symbol this backend already unconditionally
            // declares for its own internal allocation machinery (never a second, user-typed
            // declare - see the declare loop's own comment), bitcasting around it to/from the
            // call site's own concrete `*T` type - mirrors the exact "malloc raw bytes, then
            // bitcast to the wanted concrete type" idiom already used at every one of this
            // backend's own internal allocation sites.
            if (call->callee == "malloc")
            {
                const int rawReg = allocateRegister(fctx);
                *fctx.out << "  %" << rawReg << " = call i8* @malloc(i64 " << args[0].second
                          << ")\n";
                const int destReg = defineRegister(call->dest, fctx);
                *fctx.out << "  %" << destReg << " = bitcast i8* %" << rawReg << " to " << returnType
                          << "\n";
                continue;
            }
            if (call->callee == "free")
            {
                const int argReg = allocateRegister(fctx);
                *fctx.out << "  %" << argReg << " = bitcast " << args[0].first << " "
                          << args[0].second << " to i8*\n";
                *fctx.out << "  call void @free(i8* %" << argReg << ")\n";
                continue;
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
        if (const auto* closureNew = dynamic_cast<const IrClosureNew*>(inst.get()))
        {
            emitClosureNew(*closureNew, fctx);
            continue;
        }
        if (const auto* closureCall = dynamic_cast<const IrClosureCall*>(inst.get()))
        {
            emitClosureCall(*closureCall, fctx);
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
        if (const auto* deref = dynamic_cast<const IrDeref*>(inst.get()))
        {
            emitDeref(*deref, fctx);
            continue;
        }
        if (const auto* derefAssign = dynamic_cast<const IrDerefAssign*>(inst.get()))
        {
            emitDerefAssign(*derefAssign, fctx);
            continue;
        }
        if (const auto* alloca = dynamic_cast<const IrAlloca*>(inst.get()))
        {
            emitAlloca(*alloca, fctx);
            continue;
        }
        if (const auto* strSlice = dynamic_cast<const IrStrSlice*>(inst.get()))
        {
            emitStrSlice(*strSlice, fctx);
            continue;
        }
        if (const auto* join = dynamic_cast<const IrJoin*>(inst.get()))
        {
            emitJoin(*join, fctx);
            continue;
        }
        if (const auto* parse = dynamic_cast<const IrParse*>(inst.get()))
        {
            emitParse(*parse, fctx);
            continue;
        }
        if (const auto* optionalNew = dynamic_cast<const IrOptionalNew*>(inst.get()))
        {
            emitOptionalNew(*optionalNew, fctx);
            continue;
        }
        if (const auto* resultNew = dynamic_cast<const IrResultNew*>(inst.get()))
        {
            emitResultNew(*resultNew, fctx);
            continue;
        }
        if (const auto* isSome = dynamic_cast<const IrOptionalIsSome*>(inst.get()))
        {
            emitOptionalIsSome(*isSome, fctx);
            continue;
        }
        if (const auto* unwrap = dynamic_cast<const IrOptionalUnwrap*>(inst.get()))
        {
            emitOptionalUnwrap(*unwrap, fctx);
            continue;
        }
        if (const auto* toCstr = dynamic_cast<const IrToCstr*>(inst.get()))
        {
            emitToCstr(*toCstr, fctx);
            continue;
        }
        if (const auto* print = dynamic_cast<const IrPrint*>(inst.get()))
        {
            emitPrint(*print, fctx);
            continue;
        }
        if (const auto* appendValue = dynamic_cast<const IrBufferAppendValue*>(inst.get()))
        {
            emitBufferAppendValue(*appendValue, fctx);
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
        // Move semantics (structs/enums only) - Drop is the one exception to the
        // "informational only" comment just below: lowered to a genuine call to either the
        // unconditional @axea.drop.<Name> (a plain struct/enum) or the counted
        // @axea.release.<Name> (a "Shared."-prefixed synthetic wrapper - see
        // emitStructRefcountHelpers). Anything not a named-struct-pointer-typed register (a
        // primitive, or a heap type this phase doesn't cover) is silently skipped - IrGenerator
        // only ever emits this for a struct/enum-typed value, but the check is cheap
        // defense-in-depth regardless.
        if (const auto* dropInst = dynamic_cast<const IrDrop*>(inst.get()))
        {
            const std::string valueType = typeOf(dropInst->value, fctx);
            if (isClosureType(valueType))
            {
                // Closure drop lifecycle - see registerClosureInstantiation/emitClosureDropHelper.
                // valueType is the pointer form ("%axea.Closure.<id>*"); strip both the leading
                // '%' and the trailing '*' to match the helper's own generated name exactly
                // (emitClosureDropHelper's own closureTypeName parameter never had a trailing '*'
                // to begin with).
                const std::string closureTypeName = valueType.substr(1, valueType.size() - 2);
                *fctx.out << "  call void @axea.drop." << closureTypeName << "(" << valueType << " "
                          << ref(dropInst->value, fctx) << ")\n";
            }
            else if (isNamedStructPointerType(valueType))
            {
                const std::string name = structNameFromPointerType(valueType);
                if (structs_.contains(name))
                {
                    const std::string fnName =
                        name.starts_with("Shared.") ? "axea.release." + name : "axea.drop." + name;
                    *fctx.out << "  call void @" << fnName << "(" << valueType << " "
                              << ref(dropInst->value, fctx) << ")\n";
                }
            }
            continue;
        }
        // IrRetain: only ever emitted for a Shared<T> value (`.clone()` - see
        // IrGenerator's own MethodCallExpr case) - a plain struct/enum needs no runtime refcount
        // at all (move semantics - see emitStructRefcountHelpers's own comment), so this silently
        // no-ops for anything that isn't a "Shared."-prefixed name (defense-in-depth, matching
        // IrDrop's own identical check just above - should never actually happen in a
        // well-formed program).
        if (const auto* retainInst = dynamic_cast<const IrRetain*>(inst.get()))
        {
            const std::string valueType = typeOf(retainInst->value, fctx);
            if (isNamedStructPointerType(valueType))
            {
                const std::string name = structNameFromPointerType(valueType);
                if (name.starts_with("Shared.") && structs_.contains(name))
                {
                    *fctx.out << "  call void @axea.retain." << name << "(" << valueType << " "
                              << ref(retainInst->value, fctx) << ")\n";
                }
            }
            // `.clone()` deliberately points at the exact same allocation as its receiver -
            // bumping the refcount above is the only thing that actually changes. But every
            // existing "this register's value is already tracked for drop, untrack it on reuse"
            // mechanism (IrGenerator::consumeTrackedRegister, keyed purely by register identity)
            // would otherwise conflate the clone's own drop obligation with the original's,
            // silently losing one of the two releases a genuine second owner needs (confirmed via
            // a real leak: examples/shared.ax's `copy = original.clone()` produced only one
            // @axea.release call for two live handles until this fix). A zero-offset GEP gives
            // `dest` a genuinely distinct SSA register carrying the identical pointer value, so
            // every downstream register-identity-based check sees two independent owners, with no
            // special-casing needed at any individual consuming-use site.
            if (retainInst->dest != -1)
            {
                const std::string pointeeType = valueType.substr(0, valueType.size() - 1);
                const int destReg = defineRegister(retainInst->dest, fctx);
                *fctx.out << "  %" << destReg << " = getelementptr " << pointeeType << ", "
                          << valueType << " " << ref(retainInst->value, fctx) << ", i32 0\n";
            }
            continue;
        }
        // BorrowRead/BorrowWrite/Move/RegionEnter/RegionExit: informational
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
    const std::string unitStr = stringPtrConstant("()");

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

        // Display trait dispatch (see docs/language/0062-display-
        // trait.md) - this one function is the shared print path for
        // every struct-printing call site (top-level auto-print,
        // print()/write()'s own direct struct argument, and every
        // nested-struct-in-struct/nested-struct-in-collection print),
        // so patching it here covers all of them at once, the same way
        // patching @axea.tostring.<name> above covers every
        // interpolation/collection-stringify call site at once.
        if (const auto implIt = program.displayImpls.find(name);
            implIt != program.displayImpls.end())
        {
            registerStrbufRuntime();
            out << "define void @axea.print." << name << "(" << pointerType << " %0) {\n";
            out << "entry:\n";
            out << "  %1 = call {i32, i32, i8*}* @axea.strbuf.new()\n";
            out << "  call void @" << implIt->second << "(" << pointerType
                << " %0, {i32, i32, i8*}* %1)\n";
            out << "  %2 = call i8* @axea.strbuf.finish({i32, i32, i8*}* %1)\n";
            out << "  %3 = call i32 (i8*, ...) @printf(i8* " << bareStrFmt << ", i8* %2)\n";
            out << "  ret void\n";
            out << "}\n\n";
            continue;
        }

        // `enum` (see docs/language/0064-enums.md) - `name` is really a flattened
        // `{i32 tag, <every variant's own fields concatenated>}` struct (see
        // IrGenerator::generate's own comment on why), never the generic "print every field"
        // struct printer below - that would leak the tag and every *other* variant's own
        // undef/garbage fields. Delegates to @axea.tostring.<name> (always emitted somewhere
        // in this same output - see emitStructToStringHelpers's own identical enum branch - no
        // textual ordering constraint, LLVM doesn't require declare-before-use for module-level
        // functions) then a single "%s" printf, the identical shape the Display-trait branch
        // just above already established for the same reason (a real per-shape body, not the
        // generic field-by-field one).
        if (program.enums.contains(name))
        {
            registerStrbufRuntime();
            out << "define void @axea.print." << name << "(" << pointerType << " %0) {\n";
            out << "entry:\n";
            out << "  %1 = call i8* @axea.tostring." << name << "(" << pointerType << " %0)\n";
            out << "  %2 = call i32 (i8*, ...) @printf(i8* " << bareStrFmt << ", i8* %1)\n";
            out << "  ret void\n";
            out << "}\n\n";
            continue;
        }

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
            else if (fieldLlvmType == "i64" || fieldLlvmType == "double")
            {
                // Same "%s"-of-a-stringified-value approach as the
                // top-level print dispatch's own i64/double branch (see
                // emitMain below) - a 64-bit varargs value needs "%lld",
                // not bareIntFmt's own "%d", and a double needs a numeric
                // format entirely - stringifyValueOfType already has both
                // (@axea.i64.to_str/@axea.f64.to_str).
                charHelperFctx.nextLlvmRegister = nextReg;
                const std::string strPtr = stringifyValueOfType(
                    fieldLlvmType, "%" + std::to_string(valReg), charHelperFctx);
                nextReg = charHelperFctx.nextLlvmRegister;
                out << "  %" << nextReg++ << " = call i32 (i8*, ...) @printf(i8* " << bareStrFmt
                    << ", i8* " << strPtr << ")\n";
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
            else if (isOptionalType(fieldLlvmType) || isResultType(fieldLlvmType))
            {
                // Same "%s"-of-a-stringified-value approach as the i64/
                // double branch above, and the top-level print dispatch's
                // own Optional/Result branch below (see
                // docs/language/0052-optional.md) - checked before the
                // generic "must be a nested struct pointer" fallback,
                // which would otherwise misparse "%axea.Optional.<id>" as
                // a struct name and try to print a non-existent struct.
                charHelperFctx.nextLlvmRegister = nextReg;
                const std::string strPtr = stringifyValueOfType(
                    fieldLlvmType, "%" + std::to_string(valReg), charHelperFctx);
                nextReg = charHelperFctx.nextLlvmRegister;
                out << "  %" << nextReg++ << " = call i32 (i8*, ...) @printf(i8* " << bareStrFmt
                    << ", i8* " << strPtr << ")\n";
            }
            else if (isNamedStructPointerType(fieldLlvmType)) // nested struct pointer
            {
                const std::string nestedStructName = structNameFromPointerType(fieldLlvmType);
                out << "  call void @axea.print." << nestedStructName << "(" << fieldLlvmType
                    << " %" << valReg << ")\n";
            }
            else if (isPointerType(fieldLlvmType))
            {
                // A raw `*T` field (see docs/language/0006-generics.md's own List<T> port
                // follow-up) - matches the top-level print dispatch's own identical fallback for
                // a bare pointer value (unsafe Milestone 1): prints the same "()" unit sentinel
                // rather than attempting to dereference/print whatever it points to.
                out << "  %" << nextReg++ << " = call i32 (i8*, ...) @printf(i8* " << bareStrFmt
                    << ", i8* " << unitStr << ")\n";
            }
            else
            {
                // A nested collection field, not a struct - see the
                // top-level dispatch's own identical guard/comment
                // (docs/language/0053-nested-generics.md).
                throw std::runtime_error(
                    "printing a struct field of type " + fieldLlvmType +
                    " is not supported this phase - a nested collection field");
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
    // extern c declarations (see docs/language/0048-ffi.md) register into
    // these exact same maps - a call site's own argument-coercion logic
    // (e.g. the "String lends str" resolution below) doesn't need to
    // know or care whether the callee is a real Axea function or an
    // externally-linked one.
    for (const auto& externDecl : program.externs)
    {
        functionReturnTypes_[externDecl.name] = externDecl.returnType;
        functionParamTypes_[externDecl.name] = externDecl.paramTypes;
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
    // Optional<T>'s own named type (see docs/language/0052-optional.md and
    // registerOptionalInstantiation) is, unlike every other named type in
    // this backend (a closure's own captures struct, a struct's own field
    // types), used *by value* - a function signature or extractvalue/
    // insertvalue referencing it needs its body already known, not merely
    // forward-declared (hand-verified against clang: `insertvalue %foo
    // undef, ...` fails with "invalid indices" if `%foo = type {...}`
    // appears later in the same file, even though the identical pattern
    // with `%foo*` - always how every *other* named type here is used -
    // is fine either order). So every user function's own Optional<T>
    // registrations must happen *before* `out` starts being built below
    // (emitMain, just after this, already registers program.topLevel's
    // own Optional<T> usages the same way for the same reason) - a
    // throwaway discovery pass, safe to repeat: registerOptionalInstantiation
    // is memoized, so emitFunction's own later, real inferTypes call for
    // each function just resolves the identical, already-registered names.
    for (const auto& function : program.functions)
    {
        FunctionContext discoveryFctx;
        std::ostringstream discardedOut;
        discoveryFctx.out = &discardedOut;
        inferTypes(function, discoveryFctx);
    }

    // Built into their own buffers first so any synthetic strings they need
    // (format strings, punctuation, binding/field names) get hoisted into
    // stringGlobals_ before the globals block below is emitted - LLVM
    // doesn't require forward-reference ordering for globals or calls, this
    // just keeps every string constant declared in one place for readability.
    std::ostringstream helpers;
    emitStructPrintHelpers(program, helpers);
    // Every struct's own @axea.tostring.<Name> (see
    // docs/language/0054-collection-printing.md), unconditionally -
    // mirrors emitStructPrintHelpers' own "build every shape regardless
    // of use" choice just above, and needs to run before `out` starts
    // being built for the same reason the Optional<T> discovery pass
    // does (writes into toStrRuntimeText_ directly, snapshotted only at
    // the very end - see that stream's own final emission point below -
    // so the *timing* doesn't actually matter here, only that it's not
    // forgotten).
    emitStructToStringHelpers(program);
    // Move semantics (structs/enums only) - `@axea.drop.<Name>` for every struct/enum,
    // unconditionally and upfront, same timing reasoning as emitStructToStringHelpers just above
    // (writes into structRefcountRuntimeText_ directly, snapshotted only at the very end).
    emitStructRefcountHelpers(program);
    std::ostringstream mainOut;
    emitMain(program, mainOut);

    std::ostringstream out;

    emitStructTypeDecls(out);
    // Optional<T>'s named type must appear before any by-value use of it
    // (see the discovery-pass comment above, near this function's own
    // start, for why) - every user function and program.topLevel has
    // already been through inferTypes by this point (the discovery pass
    // above, plus emitMain/emitStructPrintHelpers just above), so
    // optionalTypeDeclsText_ is fully populated here, unlike every other
    // instantiation-keyed *TypeDeclsText_ in this file (mapSetTypeDeclsText_/
    // closureTypeDeclsText_), which are all safe to emit much later precisely because they're
    // never used by value.
    out << optionalTypeDeclsText_.str();
    // Result<T,E>'s own named type needs the identical by-value ordering
    // treatment as Optional<T>'s just above (see docs/language/0063-result.md)
    // - the same discovery pass (which runs inferTypes over every
    // function/program.topLevel unconditionally) already populates
    // resultTypeDeclsText_ too, since IrResultNew/IrOptionalUnwrap's own
    // Result-aware type inference is reached by that identical walk.
    out << resultTypeDeclsText_.str();
    out << closureTypeDeclsText_.str();
    out << "\ndeclare i8* @malloc(i64)\n";
    // Move semantics (structs/enums only) - @axea.drop.<Name> calls this unconditionally once a
    // struct/enum instance's own last owning reference goes out of scope (see
    // emitStructRefcountHelpers).
    out << "declare void @free(i8*)\n";
    out << "declare i32 @printf(i8*, ...)\n";
    // String<>'s own construction/append need a runtime str length (see
    // docs/language/0042-string.md) - str (i8*) has no length field of its
    // own, unlike every other collection here, so this is the one place a
    // third libc function beyond malloc/printf gets declared.
    out << "declare i64 @strlen(i8*)\n";
    // i32-to-string conversion for `print`/`write`/interpolation (see
    // docs/language/Axea_Printing_Formatting.md and
    // registerI32ToStrRuntime) - a fourth libc function, reusing libc's
    // own well-tested integer formatting rather than hand-rolling itoa.
    out << "declare i32 @sprintf(i8*, i8*, ...)\n";
    // `.parse<f64>()`'s own string-to-double conversion (see
    // registerParseRuntime and docs/language/0046-generic-methods.md) - a
    // fifth libc function, reusing libc's own well-tested decimal-to-
    // binary float parsing rather than risking a hand-rolled bug (the
    // exact same "reuse libc" reasoning @sprintf's own use just above
    // already established, in the opposite direction). The second
    // argument (`endptr`) is always passed `null` - `.parse<T>()`'s own
    // "invalid input yields a harmless default" contract (see
    // registerParseRuntime) means the exact stopping point is never
    // consulted.
    out << "declare double @strtod(i8*, i8**)\n\n";

    // extern c declarations (see docs/language/0048-ffi.md) - a real LLVM
    // `declare`, one per extern, exactly like the fixed malloc/printf/
    // strlen externs just above but user-specified rather than hardcoded.
    // Emitted unmangled (`@<name>`, no prefix), same as every ordinary
    // Axea function - `TypeChecker::registerSignatures` already prevents
    // an extern name from colliding with a real Axea function's own name
    // (both live in the same `functions_`/`externs_` lookup space at the
    // call site).
    for (const auto& externDecl : program.externs)
    {
        // `malloc`/`free` (see docs/language/0019-unsafe.md) are already unconditionally
        // declared above (`i8* @malloc(i64)` / `void @free(i8*)`) for this backend's own internal
        // allocation machinery - a user's own `extern c malloc`/`free` declaration must never get
        // a second, differently-typed `declare` for the same symbol name (LLVM forbids two
        // declares of one external symbol with different signatures, and a user's own `*T`
        // return/param type renders differently from the fixed "i8*" above). Call sites bitcast
        // around the one shared declaration instead - see the IrCall handler's own malloc/free
        // special case.
        if (externDecl.name == "malloc" || externDecl.name == "free")
        {
            continue;
        }
        out << "declare " << llvmReturnType(externDecl.returnType) << " @" << externDecl.name
            << "(";
        for (std::size_t i = 0; i < externDecl.paramTypes.size(); ++i)
        {
            if (i > 0)
            {
                out << ", ";
            }
            out << llvmType(externDecl.paramTypes[i]);
        }
        out << ")\n";
    }
    if (!program.externs.empty())
    {
        out << "\n";
    }

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
    // Same reasoning again, for single-character indexing's own
    // codepoint-decoding runtime (see docs/language/0047-unicode.md) -
    // registerUtf8CharAtRuntime is driven by emitIndexGet itself, same
    // lazy-during-emission timing as registerUtf8CountRuntime above.
    out << utf8CharAtRuntimeText_.str();
    // Same reasoning again, for `print`/`write`/interpolation's own
    // stringification runtime (see
    // docs/language/Axea_Printing_Formatting.md).
    out << toStrRuntimeText_.str();
    out << printRuntimeText_.str();
    out << structRefcountRuntimeText_.str();
    out << helpers.str();
    out << mainOut.str();

    return out.str();
}
