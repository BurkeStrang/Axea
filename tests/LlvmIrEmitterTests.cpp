#include "TestFramework.hpp"

#include "ir/IrGenerator.hpp"
#include "lexer/Lexer.hpp"
#include "llvmir/LlvmIrEmitter.hpp"
#include "parser/Parser.hpp"
#include "sema/CapabilityChecker.hpp"
#include "sema/RegionChecker.hpp"
#include "sema/TypeChecker.hpp"

namespace
{
    // Mirrors the real pipeline (compiler/main.cpp): TypeChecker, then
    // CapabilityChecker, then RegionChecker, then IrGenerator, then the
    // LLVM IR emitter - each stage's output feeds the next.
    std::string emitLlvmIr(const std::string& source)
    {
        Lexer lexer(source);
        Parser parser(lexer.lex());
        auto program = parser.parseProgram();

        TypeChecker typeChecker;
        typeChecker.check(program);

        CapabilityChecker capabilityChecker;
        capabilityChecker.check(program);

        RegionChecker regionChecker;
        regionChecker.check(program, capabilityChecker.effectiveCapabilities());

        IrGenerator irGenerator;
        auto irProgram = irGenerator.generate(
            program, capabilityChecker.effectiveCapabilities(), regionChecker.regions());

        LlvmIrEmitter emitter;
        return emitter.emit(irProgram);
    }
} // namespace

TEST("LlvmIrEmitter emits a function signature with i32 params and return type")
{
    auto ir = emitLlvmIr("add(a: i32, b: i32) -> i32 { return a + b }");
    EXPECT_TRUE(ir.find("define i32 @add(i32 %0, i32 %1) {") != std::string::npos);
    EXPECT_TRUE(ir.find("ret i32") != std::string::npos);
}

TEST("LlvmIrEmitter lowers arithmetic and comparison operators to the right opcodes")
{
    auto ir = emitLlvmIr("f(a: i32, b: i32) -> bool { "
                         "  x = a + b  y = a - b  z = a * b  w = a / b "
                         "  return a < b "
                         "}");
    EXPECT_TRUE(ir.find("= add i32") != std::string::npos);
    EXPECT_TRUE(ir.find("= sub i32") != std::string::npos);
    EXPECT_TRUE(ir.find("= mul i32") != std::string::npos);
    EXPECT_TRUE(ir.find("= sdiv i32") != std::string::npos);
    EXPECT_TRUE(ir.find("= icmp slt i32") != std::string::npos);
}

TEST("LlvmIrEmitter every numbered SSA register is defined before any later-numbered one")
{
    // LLVM requires unnamed local values within a function to be defined in
    // strictly increasing textual order - this is exactly the bug fixed
    // during Phase 6 (extra GEP/malloc temporaries used to be numbered past
    // the end of a function's own registers, producing out-of-order defs).
    // Regression-check it structurally: scan every "%N = " definition site
    // in emission order and assert N never goes backwards or repeats.
    auto ir = emitLlvmIr("struct Point { x: i32  y: i32 } "
                         "make(x: i32, y: i32) -> Point { return Point { x: x  y: y } } "
                         "sum(p: Point) -> i32 { return p.x + p.y }");

    int definitionCount = 0;
    std::size_t definePos = ir.find("define ");
    while (definePos != std::string::npos)
    {
        const std::size_t openParen = ir.find('(', definePos);
        const std::size_t closeParen = ir.find(')', openParen);
        const std::size_t nextDefine = ir.find("define ", closeParen);
        const std::size_t functionEnd = nextDefine == std::string::npos ? ir.size() : nextDefine;

        // Skip every hand-authored runtime helper - the Map/Set hash-table
        // runtime (emitMapSetRuntime, docs/language/0034-maps-and-sets.md),
        // struct/collection stringification (registerStrbufRuntime/
        // emitStructToStringHelpers/registerCollectionToStrRuntime, see
        // docs/language/0054-collection-printing.md), and every scalar
        // to_str/parse/hash/eq/less runtime (registerI32ToStrRuntime and
        // siblings, docs/language/Axea_Printing_Formatting.md/
        // docs/language/0046-generic-methods.md): unlike every function
        // above, all of these are generated as fixed, hand-verified LLVM
        // text using named registers (%h, %key, %buf, %fptr0, ...), not
        // allocateRegister's dynamic numbering (the exact mechanism this
        // test regression-checks) - never subject to the numbering bug
        // this test guards against. `axea.tostring.<StructName>`
        // (struct-shaped, not one of these fixed prefixes) still needs
        // its own explicit "axea.tostring." check below, since a struct
        // can be named almost anything.
        const std::size_t atPos = ir.find('@', definePos);
        const std::string calleeName = ir.substr(atPos + 1, openParen - atPos - 1);
        if (calleeName.starts_with("axea.hash.") || calleeName.starts_with("axea.map.") ||
            calleeName.starts_with("axea.set.") || calleeName.starts_with("axea.eq.") ||
            calleeName.starts_with("axea.less.") || calleeName.starts_with("axea.parse.") ||
            calleeName.starts_with("axea.optional.") || calleeName.starts_with("axea.tostring.") ||
            calleeName.starts_with("axea.strbuf.") || calleeName.starts_with("axea.char.") ||
            calleeName.starts_with("axea.utf8.") || calleeName.starts_with("axea.i32.") ||
            calleeName.starts_with("axea.i64.") || calleeName.starts_with("axea.f64.") ||
            calleeName.starts_with("axea.bool."))
        {
            definePos = nextDefine;
            continue;
        }

        // Parameters occupy the first N numbered slots, one per
        // comma-separated entry in the signature, before any "%N = "
        // definition in the body. (Counting '%' characters would overcount:
        // a struct-pointer param type like "%Point*" itself starts with
        // '%', on top of the parameter's own "%0".)
        const std::string paramList = ir.substr(openParen + 1, closeParen - openParen - 1);
        int paramCount = paramList.empty() ? 0 : 1;
        for (char c : paramList)
        {
            if (c == ',')
            {
                ++paramCount;
            }
        }
        int highestSeen = paramCount - 1;

        std::size_t defPos = closeParen;
        while ((defPos = ir.find("\n  %", defPos)) != std::string::npos && defPos < functionEnd)
        {
            const std::size_t numberStart = defPos + 4;
            const std::size_t numberEnd = ir.find_first_not_of("0123456789", numberStart);
            EXPECT_TRUE(numberEnd != std::string::npos && numberEnd > numberStart);
            EXPECT_TRUE(ir.compare(numberEnd, 3, " = ") == 0);

            const int number = std::stoi(ir.substr(numberStart, numberEnd - numberStart));
            EXPECT_TRUE(number == highestSeen + 1); // strictly sequential within a function
            highestSeen = number;
            ++definitionCount;
            defPos = numberEnd;
        }

        definePos = nextDefine;
    }
    EXPECT_TRUE(definitionCount > 0); // sanity: the scan actually found definitions
}

TEST("LlvmIrEmitter lowers if/else into two labeled blocks and a two-predecessor phi")
{
    auto ir = emitLlvmIr("pick(flag: bool) -> i32 { return if flag { 1 } else { 2 } }");
    EXPECT_TRUE(ir.find("if.then0:") != std::string::npos);
    EXPECT_TRUE(ir.find("if.else0:") != std::string::npos);
    EXPECT_TRUE(ir.find("if.merge0:") != std::string::npos);
    EXPECT_TRUE(ir.find("= phi i32") != std::string::npos);
    EXPECT_TRUE(ir.find("[ %if.then0") != std::string::npos ||
                ir.find(", %if.then0") != std::string::npos);
    EXPECT_TRUE(ir.find("[ %if.else0") != std::string::npos ||
                ir.find(", %if.else0") != std::string::npos);
}

TEST("LlvmIrEmitter emits no phi when both branches return")
{
    auto ir = emitLlvmIr("pick(flag: bool) -> i32 { "
                         "  if flag { return 1 } else { return 2 } "
                         "  0 "
                         "}");
    EXPECT_TRUE(ir.find("= phi") == std::string::npos);
    EXPECT_TRUE(ir.find("unreachable") != std::string::npos);
}

TEST("LlvmIrEmitter emits no phi for an if-without-else used as a statement")
{
    // The implicit unit else-branch produces no value (Axea IR register -1
    // for that side) - regression check for the crash this used to hit
    // (ref() was called on the -1 sentinel as if it were a real register).
    auto ir = emitLlvmIr("f(n: i32, flag: bool) -> i32 { "
                         "  if flag { return n } "
                         "  return n "
                         "}");
    EXPECT_TRUE(ir.find("= phi") == std::string::npos);
}

TEST("LlvmIrEmitter lowers a struct literal to malloc plus a store per field")
{
    auto ir = emitLlvmIr("struct Point { x: i32  y: i32 } "
                         "make(x: i32, y: i32) -> Point { return Point { x: x  y: y } }");
    EXPECT_TRUE(ir.find("%Point = type { i32, i32 }") != std::string::npos);
    EXPECT_TRUE(ir.find("declare i8* @malloc(i64)") != std::string::npos);
    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("bitcast i8*") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32") != std::string::npos);
    EXPECT_TRUE(ir.find("define %Point* @make(i32 %0, i32 %1) {") != std::string::npos);
}

TEST("LlvmIrEmitter passes struct parameters by pointer")
{
    auto ir = emitLlvmIr("struct Point { x: i32 } "
                         "getx(p: Point) -> i32 { return p.x }");
    EXPECT_TRUE(ir.find("define i32 @getx(%Point* %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("getelementptr %Point,") != std::string::npos);
}

TEST("LlvmIrEmitter lowers an array literal to malloc plus a store per element, no named type")
{
    auto ir = emitLlvmIr("f() -> i32 { values = [1, 2, 3]  return values[0] }");
    // Arrays are anonymous LLVM types - no "%<name> = type ..." declaration,
    // unlike struct (see docs/language/0031-arrays.md).
    EXPECT_TRUE(ir.find("declare i8* @malloc(i64)") != std::string::npos);
    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("bitcast i8*") != std::string::npos);
    EXPECT_TRUE(ir.find("getelementptr [3 x i32], [3 x i32]*") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32") != std::string::npos);
}

TEST("LlvmIrEmitter passes array parameters by pointer to an anonymous array type")
{
    auto ir = emitLlvmIr("first(values: [i32; 4]) -> i32 { return values[0] }");
    EXPECT_TRUE(ir.find("define i32 @first([4 x i32]* %0) {") != std::string::npos);
}

TEST("LlvmIrEmitter indexes with the register value, not a constant field index")
{
    auto ir = emitLlvmIr("get(values: [i32; 4], i: i32) -> i32 { return values[i] }");
    // A struct field GEP index is always a literal constant; an array index
    // is the index register itself, e.g. "i32 %1" rather than "i32 0".
    EXPECT_TRUE(ir.find("getelementptr [4 x i32], [4 x i32]* %0, i32 0, i32 %1") !=
                std::string::npos);
}

TEST("LlvmIrEmitter constant-folds .length instead of emitting a runtime load")
{
    auto ir = emitLlvmIr("f() -> i32 { values = [1, 2, 3, 4]  return values.length }");
    // Zero-cost per docs/language/0031-arrays.md: the size is baked in as
    // "add i32 0, 4" (the same trivial-constant shape every IrConstInt gets),
    // never a load through a GEP.
    EXPECT_TRUE(ir.find("add i32 0, 4") != std::string::npos);
}

TEST("LlvmIrEmitter passes a slice<T> parameter as an anonymous fat-pointer struct by value")
{
    auto ir = emitLlvmIr("sum(values: slice<i32>) -> i32 { return values[0] }");
    EXPECT_TRUE(ir.find("define i32 @sum({i32*, i32} %0) {") != std::string::npos);
}

TEST("LlvmIrEmitter converts an array argument to a slice at the call site")
{
    auto ir = emitLlvmIr("sum(values: slice<i32>) -> i32 { return values[0] }  x = sum([1, 2, 3])");
    // Flat-pointer GEP down to element 0, then build the {ptr, length} pair.
    EXPECT_TRUE(ir.find("getelementptr [3 x i32], [3 x i32]*") != std::string::npos);
    EXPECT_TRUE(ir.find("insertvalue {i32*, i32} undef, i32*") != std::string::npos);
    EXPECT_TRUE(ir.find("insertvalue {i32*, i32} %") != std::string::npos);
    EXPECT_TRUE(ir.find(", i32 3, 1") != std::string::npos); // the array's own size, as the length
    EXPECT_TRUE(ir.find("call i32 @sum({i32*, i32} %") != std::string::npos);
}

TEST("LlvmIrEmitter does not re-wrap a slice forwarded to another slice parameter")
{
    auto ir = emitLlvmIr("helper(values: slice<i32>) -> i32 { return values[0] } "
                         "wrapper(values: slice<i32>) -> i32 { return helper(values) }");
    // Inside wrapper, `values` is already {i32*, i32} - forwarding it must
    // not emit a second GEP/insertvalue conversion sequence.
    EXPECT_TRUE(ir.find("insertvalue") == std::string::npos);
    EXPECT_TRUE(ir.find("call i32 @helper({i32*, i32} %0)") != std::string::npos);
}

TEST("LlvmIrEmitter indexes a slice via extractvalue and a single-index GEP, not the array's "
     "two-index form")
{
    auto ir = emitLlvmIr("get(values: slice<i32>, i: i32) -> i32 { return values[i] }");
    EXPECT_TRUE(ir.find("extractvalue {i32*, i32} %0, 0") != std::string::npos);
    EXPECT_TRUE(ir.find("getelementptr i32, i32* %") != std::string::npos);
    // Must not contain the array-shaped two-index GEP form anywhere.
    EXPECT_TRUE(ir.find(", i32 0, i32 %1") == std::string::npos);
}

TEST("LlvmIrEmitter reads a slice's .length via extractvalue, not a compile-time constant")
{
    auto ir = emitLlvmIr("len(values: slice<i32>) -> i32 { return values.length }");
    EXPECT_TRUE(ir.find("extractvalue {i32*, i32} %0, 1") != std::string::npos);
}

TEST("LlvmIrEmitter represents List<T> as a pointer to an anonymous {length, data, capacity} "
     "heap record - capacity (amortized growth, see ensureListCapacity) is field 2, at the "
     "end, not field 1, to avoid colliding with Deque<T>'s own {i32, i32, T*}* header text")
{
    auto ir = emitLlvmIr("sum(numbers: List<i32>) -> i32 { return numbers[0] }");
    EXPECT_TRUE(ir.find("define i32 @sum({i32, i32*, i32}* %0) {") != std::string::npos);
}

TEST("LlvmIrEmitter's List<T>() construction mallocs a header and zero-initializes it")
{
    auto ir = emitLlvmIr("f() -> i32 { numbers = List<i32>()  return numbers.length }");
    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32 0, i32*") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32* null, i32**") != std::string::npos);
}

TEST("LlvmIrEmitter's push grows (via the shared ensureListCapacity helper) plus a hand-rolled "
     "copy loop, no phi - capacity starts at 0, so the very first push always grows")
{
    auto ir = emitLlvmIr("f() { numbers = List<i32>()  numbers.push(4) }");
    // Two mallocs: one for the fresh empty list, one for push's grown buffer.
    const auto firstMalloc = ir.find("call i8* @malloc(i64");
    EXPECT_TRUE(firstMalloc != std::string::npos);
    EXPECT_TRUE(ir.find("call i8* @malloc(i64", firstMalloc + 1) != std::string::npos);
    EXPECT_TRUE(ir.find("list.grow.copy.header") != std::string::npos);
    EXPECT_TRUE(ir.find("list.grow.copy.body") != std::string::npos);
    EXPECT_TRUE(ir.find("list.grow.copy.done") != std::string::npos);
    // Loop-carried state here uses alloca/load/store, not a phi node (see
    // docs/language/0033-lists.md - unnamed sequential registers here can't
    // forward-reference a not-yet-emitted value the way a phi would need).
    EXPECT_TRUE(ir.find(" phi ") == std::string::npos);
}

TEST("LlvmIrEmitter's List push grows by doubling capacity (or exactly enough, whichever is "
     "larger) - amortized O(1) push, not a reallocate-to-exactly-length-every-time O(n) one "
     "(see ensureListCapacity)")
{
    auto ir = emitLlvmIr("f() { numbers = List<i32>()  numbers.push(4) }");
    // The doubling formula itself: capacity * 2, then select the larger of
    // that and the actually-needed length - the same shape
    // ensureBufferCapacity already established for Buffer.
    EXPECT_TRUE(ir.find("mul i32 %") != std::string::npos);
    const auto mulPos = ir.find("mul i32 %");
    EXPECT_TRUE(ir.find(", 2\n", mulPos) != std::string::npos);
    EXPECT_TRUE(ir.find("select i1 %") != std::string::npos);
}

TEST("LlvmIrEmitter pop has no bounds check, matching every other out-of-bounds case here")
{
    auto ir =
        emitLlvmIr("f() -> i32 { numbers = List<i32>()  numbers.push(1)  return numbers.pop() }");
    // Decrements length and loads the element at the old (pre-decrement)
    // length - 1, with no runtime check that length was > 0 beforehand.
    EXPECT_TRUE(ir.find("sub i32") != std::string::npos);
}

TEST("LlvmIrEmitter indexes a List via a header-then-flat-pointer GEP chain, not array's "
     "direct two-index form")
{
    auto ir = emitLlvmIr("get(numbers: List<i32>, i: i32) -> i32 { return numbers[i] }");
    // Field 1 (data pointer) of the header is loaded first - still field 1,
    // even with the new capacity field at the end (field 2) - see
    // ensureListCapacity's own comment for why capacity went at the end
    // instead of field 1.
    EXPECT_TRUE(ir.find("getelementptr {i32, i32*, i32}, {i32, i32*, i32}* %0, i32 0, i32 1") !=
                std::string::npos);
    // ...then a flat single-index GEP into it (same idiom slice indexing uses).
    EXPECT_TRUE(ir.find("getelementptr i32, i32* %") != std::string::npos);
}

TEST("LlvmIrEmitter reads a List's .length via GEP+load, not a compile-time constant")
{
    auto ir = emitLlvmIr("len(numbers: List<i32>) -> i32 { return numbers.length }");
    EXPECT_TRUE(ir.find("getelementptr {i32, i32*, i32}, {i32, i32*, i32}* %0, i32 0, i32 0") !=
                std::string::npos);
}

TEST("LlvmIrEmitter represents Stack<T> as the exact same LLVM type as List<T>")
{
    auto ir = emitLlvmIr("useStack(s: Stack<i32>) -> i32 { return s.length }");
    EXPECT_TRUE(ir.find("define i32 @useStack({i32, i32*, i32}* %0) {") != std::string::npos);
}

TEST("LlvmIrEmitter's Stack<T>() construction mallocs a header and zero-initializes it")
{
    auto ir = emitLlvmIr("f() -> i32 { s = Stack<i32>()  return s.length }");
    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32 0, i32*") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32* null, i32**") != std::string::npos);
}

TEST("LlvmIrEmitter's Stack push grows (via the same shared ensureListCapacity helper List's "
     "own push uses) plus a hand-rolled copy loop, no phi")
{
    auto ir = emitLlvmIr("f() { s = Stack<i32>()  s.push(4) }");
    EXPECT_TRUE(ir.find("list.grow.copy.header") != std::string::npos);
    EXPECT_TRUE(ir.find("list.grow.copy.body") != std::string::npos);
    EXPECT_TRUE(ir.find("list.grow.copy.done") != std::string::npos);
    EXPECT_TRUE(ir.find(" phi ") == std::string::npos);
}

TEST("LlvmIrEmitter Stack pop has no bounds check")
{
    auto ir = emitLlvmIr("f() -> i32 { s = Stack<i32>()  s.push(1)  return s.pop() }");
    EXPECT_TRUE(ir.find("sub i32") != std::string::npos);
}

TEST("LlvmIrEmitter Stack peek reads the top element without decrementing length")
{
    // Isolated to a function taking Stack<i32> as a *parameter* (no
    // construction/push in the same function) - emitStackPeek's own output
    // is then the function's entire body: computes length-1 (sub) to index
    // the top element, but - unlike pop - never stores anything back (see
    // docs/language/0035-stacks.md); pop's own decrement-and-store-back
    // would introduce a "store i32 %..." into the length field that peek's
    // pure-read shape never does.
    auto ir = emitLlvmIr("f(s: Stack<i32>) -> i32 { return s.peek() }");
    EXPECT_TRUE(ir.find("sub i32") != std::string::npos);
    EXPECT_TRUE(ir.find("store") == std::string::npos);
}

TEST("LlvmIrEmitter reads a Stack's .length via GEP+load, not a compile-time constant")
{
    auto ir = emitLlvmIr("len(s: Stack<i32>) -> i32 { return s.length }");
    EXPECT_TRUE(ir.find("getelementptr {i32, i32*, i32}, {i32, i32*, i32}* %0, i32 0, i32 0") !=
                std::string::npos);
}

TEST("LlvmIrEmitter List<T> and Stack<T> push each call ensureListCapacity (two separate "
     "grow-check sites, since push itself is still an independently hand-duplicated emit "
     "function per collection) and produce the same underlying shape")
{
    auto ir = emitLlvmIr("useList(l: List<i32>) { l.push(1) } "
                         "useStack(s: Stack<i32>) { s.push(1) }");
    // Two independent "did we need to grow" checks, one per push call site -
    // not one shared check reused across both (each push call still emits
    // its own inline call into ensureListCapacity).
    const auto firstGrowCheck = ir.find("icmp sgt i32 %");
    EXPECT_TRUE(firstGrowCheck != std::string::npos);
    EXPECT_TRUE(ir.find("icmp sgt i32 %", firstGrowCheck + 1) != std::string::npos);
    EXPECT_TRUE(ir.find("define void @useList({i32, i32*, i32}* %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("define void @useStack({i32, i32*, i32}* %0) {") != std::string::npos);
}

TEST("LlvmIrEmitter declares a named self-referential node type for LinkedList<T>")
{
    // Monomorphized (see docs/language/0036-linked-lists.md), same
    // lazy-registration-by-canonical-string pattern as Map/Set's own entry
    // types - the first LinkedList instantiation actually used gets id 0.
    auto ir = emitLlvmIr("f() -> i32 { s = LinkedList<i32>()  return s.length }");
    EXPECT_TRUE(ir.find("%axea.LLNode.0 = type { i32, %axea.LLNode.0*, %axea.LLNode.0* }") !=
                std::string::npos);
}

TEST("LlvmIrEmitter represents LinkedList<i32> as a pointer to a 3-field heap header")
{
    auto ir = emitLlvmIr("useLinkedList(s: LinkedList<i32>) -> i32 { return s.length }");
    EXPECT_TRUE(
        ir.find("define i32 @useLinkedList({i32, %axea.LLNode.0*, %axea.LLNode.0*}* %0) {") !=
        std::string::npos);
}

TEST("LlvmIrEmitter's LinkedList<T>() construction mallocs a header and zero-initializes it")
{
    auto ir = emitLlvmIr("f() -> i32 { s = LinkedList<i32>()  return s.length }");
    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32 0, i32*") != std::string::npos); // length = 0
    EXPECT_TRUE(ir.find("store %axea.LLNode.0* null, %axea.LLNode.0**") !=
                std::string::npos); // head/tail = null
}

TEST("LlvmIrEmitter LinkedList push_front/push_back/pop_front/pop_back each emit a single call "
     "into that instantiation's own runtime function")
{
    auto ir = emitLlvmIr("f(s: LinkedList<i32>, v: i32) { "
                         "  s.push_front(v) "
                         "  s.push_back(v) "
                         "} "
                         "g(s: LinkedList<i32>) -> i32 { "
                         "  a = s.pop_front() "
                         "  b = s.pop_back() "
                         "  return a + b "
                         "}");
    EXPECT_TRUE(ir.find("call void @axea.linkedlist.0.push_front(") != std::string::npos);
    EXPECT_TRUE(ir.find("call void @axea.linkedlist.0.push_back(") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 @axea.linkedlist.0.pop_front(") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 @axea.linkedlist.0.pop_back(") != std::string::npos);
}

TEST("LlvmIrEmitter LinkedList push_front's runtime function contains a real br i1, unlike "
     "List<T>.push's straight-line-plus-loop shape")
{
    // The reason push_front/push_back/pop_front/pop_back are template-text
    // runtime functions (named %registers) rather than inlined the way
    // List<T>.push/.pop are: maintaining the head/tail invariant on an
    // empty-list transition needs real conditional control flow (see
    // docs/language/0036-linked-lists.md).
    auto ir = emitLlvmIr("f() { s = LinkedList<i32>()  s.push_front(1) }");
    const auto fnStart = ir.find("define void @axea.linkedlist.0.push_front(");
    EXPECT_TRUE(fnStart != std::string::npos);
    const auto fnEnd = ir.find("\n}\n", fnStart);
    const std::string fnBody = ir.substr(fnStart, fnEnd - fnStart);
    EXPECT_TRUE(fnBody.find("br i1") != std::string::npos);
    EXPECT_TRUE(fnBody.find("emptycase:") != std::string::npos);
    EXPECT_TRUE(fnBody.find("nonemptycase:") != std::string::npos);
}

TEST("LlvmIrEmitter reads a LinkedList's .length via GEP+load, not a compile-time constant")
{
    auto ir = emitLlvmIr("len(s: LinkedList<i32>) -> i32 { return s.length }");
    EXPECT_TRUE(ir.find("getelementptr {i32, %axea.LLNode.0*, %axea.LLNode.0*}, "
                        "{i32, %axea.LLNode.0*, %axea.LLNode.0*}* %0, i32 0, i32 0") !=
                std::string::npos);
}

TEST("LlvmIrEmitter prints a top-level LinkedList<T> binding as a count only, unlike List/Stack's "
     "runtime print loop")
{
    auto ir = emitLlvmIr("s = LinkedList<i32>()");
    EXPECT_TRUE(ir.find("LinkedList(") != std::string::npos);
}

TEST("LlvmIrEmitter represents Deque<i32> as a pointer to an anonymous 3-field heap header, no "
     "named type at all")
{
    // Unlike Map/Set/LinkedList (all named, self-referential), Deque<T>'s
    // third field is a plain T*, not an entry pointer - no monomorphization
    // needed (see docs/language/0037-deques.md).
    auto ir = emitLlvmIr("useDeque(d: Deque<i32>) -> i32 { return d.length }");
    EXPECT_TRUE(ir.find("define i32 @useDeque({i32, i32, i32*}* %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("%axea.") == std::string::npos);
}

TEST("LlvmIrEmitter's Deque<T>() construction mallocs a header and zero-initializes all 3 fields")
{
    auto ir = emitLlvmIr("f() -> i32 { d = Deque<i32>()  return d.length }");
    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32 0, i32*") != std::string::npos); // count = 0 (and start = 0)
    EXPECT_TRUE(ir.find("store i32* null, i32**") != std::string::npos); // data = null
}

TEST("LlvmIrEmitter Deque push_front/push_back reallocate via a hand-rolled copy loop, no phi")
{
    auto ir = emitLlvmIr("f() { d = Deque<i32>()  d.push_front(1)  d.push_back(2) }");
    EXPECT_TRUE(ir.find("deque.push.copy.header") != std::string::npos);
    EXPECT_TRUE(ir.find("deque.push.copy.body") != std::string::npos);
    EXPECT_TRUE(ir.find("deque.push.copy.done") != std::string::npos);
    EXPECT_TRUE(ir.find(" phi ") == std::string::npos);
}

TEST("LlvmIrEmitter Deque pop_front/pop_back have no bounds check and need no loop or branch")
{
    // Isolated to a function taking Deque<i32> as a parameter (no
    // construction/push in the same function) - simpler than even List's
    // own pop: just start/count arithmetic, no loop label of any kind.
    auto ir = emitLlvmIr("f(d: Deque<i32>) -> i32 { return d.pop_front() + d.pop_back() }");
    EXPECT_TRUE(ir.find("add i32") != std::string::npos); // pop_front's start+1
    EXPECT_TRUE(ir.find("sub i32") != std::string::npos); // pop_back's count-1
    EXPECT_TRUE(ir.find("deque.push.copy") == std::string::npos);
}

TEST("LlvmIrEmitter Deque [i] read/write offsets the index by `start`, not a plain GEP")
{
    auto ir = emitLlvmIr("f(d: Deque<i32>) -> i32 { d[0] = 5  return d[1] }");
    // The `start` field (index 1) is read and added to the given index -
    // distinguishes Deque's own indexing from List's plain `i32 %index` GEP.
    EXPECT_TRUE(ir.find("i32 0, i32 1\n") != std::string::npos);
    EXPECT_TRUE(ir.find("add i32 %") != std::string::npos);
}

TEST("LlvmIrEmitter reads a Deque's .length via GEP+load, not a compile-time constant")
{
    auto ir = emitLlvmIr("len(d: Deque<i32>) -> i32 { return d.length }");
    EXPECT_TRUE(ir.find("getelementptr {i32, i32, i32*}, {i32, i32, i32*}* %0, i32 0, i32 0") !=
                std::string::npos);
}

TEST("LlvmIrEmitter prints a top-level Deque<T> binding with full bracket contents, unlike "
     "LinkedList/Map/Set's count-only fallback")
{
    auto ir = emitLlvmIr("d = Deque<i32>()");
    EXPECT_TRUE(ir.find("print.deque.") != std::string::npos);
    EXPECT_TRUE(ir.find("Deque(") == std::string::npos);
}

TEST("LlvmIrEmitter LinkedList<T> and Deque<T> push_front/pop_front resolve to distinct emit "
     "functions")
{
    auto ir = emitLlvmIr("useLinkedList(l: LinkedList<i32>) { l.push_front(1) } "
                         "useDeque(d: Deque<i32>) { d.push_front(1) }");
    EXPECT_TRUE(ir.find("call void @axea.linkedlist.0.push_front(") != std::string::npos);
    EXPECT_TRUE(ir.find("deque.push.copy.header") != std::string::npos);
}

TEST("LlvmIrEmitter represents Queue<i32> with the literal same header text as Deque<i32> - no "
     "isQueueType predicate exists anywhere")
{
    // Mirrors Stack<T>/List<T>'s own identical relationship (see
    // docs/language/0038-queues.md) - llvmType("Queue<T>") produces the
    // exact same text llvmType("Deque<T>") does.
    auto ir = emitLlvmIr("useQueue(q: Queue<i32>) -> i32 { return q.length }");
    EXPECT_TRUE(ir.find("define i32 @useQueue({i32, i32, i32*}* %0) {") != std::string::npos);
}

TEST("LlvmIrEmitter's Queue<T>() construction mallocs a header and zero-initializes all 3 "
     "fields, structurally identical to Deque<T>'s own")
{
    auto ir = emitLlvmIr("f() -> i32 { q = Queue<i32>()  return q.length }");
    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32 0, i32*") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32* null, i32**") != std::string::npos);
}

TEST("LlvmIrEmitter Queue enqueue reallocates via the same hand-rolled copy loop Deque push "
     "uses, no phi")
{
    auto ir = emitLlvmIr("f() { q = Queue<i32>()  q.enqueue(1) }");
    EXPECT_TRUE(ir.find("deque.push.copy.header") != std::string::npos);
    EXPECT_TRUE(ir.find("deque.push.copy.body") != std::string::npos);
    EXPECT_TRUE(ir.find("deque.push.copy.done") != std::string::npos);
    EXPECT_TRUE(ir.find(" phi ") == std::string::npos);
}

TEST("LlvmIrEmitter Queue dequeue has no bounds check and needs no loop or branch, just "
     "start/count arithmetic")
{
    auto ir = emitLlvmIr("f(q: Queue<i32>) -> i32 { return q.dequeue() }");
    EXPECT_TRUE(ir.find("add i32") != std::string::npos); // start + 1
    EXPECT_TRUE(ir.find("sub i32") != std::string::npos); // count - 1
    EXPECT_TRUE(ir.find("deque.push.copy") == std::string::npos);
}

TEST("LlvmIrEmitter reads a Queue's .length via GEP+load, not a compile-time constant")
{
    auto ir = emitLlvmIr("len(q: Queue<i32>) -> i32 { return q.length }");
    EXPECT_TRUE(ir.find("getelementptr {i32, i32, i32*}, {i32, i32, i32*}* %0, i32 0, i32 0") !=
                std::string::npos);
}

TEST("LlvmIrEmitter Deque<T> and Queue<T> push/pop-equivalents resolve to distinct emit "
     "functions despite sharing the exact same header shape")
{
    auto ir = emitLlvmIr("useDeque(d: Deque<i32>) { d.push_back(1) } "
                         "useQueue(q: Queue<i32>) { q.enqueue(1) }");
    EXPECT_TRUE(ir.find("define void @useDeque({i32, i32, i32*}* %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("define void @useQueue({i32, i32, i32*}* %0) {") != std::string::npos);
}

TEST("LlvmIrEmitter represents PriorityQueue<T> as the exact same LLVM type as List<T>/Stack<T>")
{
    auto ir = emitLlvmIr("usePQ(q: PriorityQueue<i32>) -> i32 { return q.length }");
    EXPECT_TRUE(ir.find("define i32 @usePQ({i32, i32*, i32}* %0) {") != std::string::npos);
}

TEST("LlvmIrEmitter's PriorityQueue<T>() construction mallocs a header and zero-initializes it")
{
    auto ir = emitLlvmIr("f() -> i32 { q = PriorityQueue<i32>()  return q.length }");
    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32 0, i32*") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32* null, i32**") != std::string::npos);
}

TEST("LlvmIrEmitter PriorityQueue push grows via the same shared ensureListCapacity helper "
     "Stack/List push use, then sifts the new element up - no phi anywhere")
{
    auto ir = emitLlvmIr("f() { q = PriorityQueue<i32>()  q.push(4) }");
    EXPECT_TRUE(ir.find("list.grow.copy.header") != std::string::npos);
    EXPECT_TRUE(ir.find("list.grow.copy.body") != std::string::npos);
    EXPECT_TRUE(ir.find("list.grow.copy.done") != std::string::npos);
    EXPECT_TRUE(ir.find("priorityqueue.push.siftup.header") != std::string::npos);
    EXPECT_TRUE(ir.find("priorityqueue.push.siftup.swap") != std::string::npos);
    EXPECT_TRUE(ir.find(" phi ") == std::string::npos);
}

TEST("LlvmIrEmitter PriorityQueue pop has no bounds check and sifts the new root down - no phi "
     "anywhere")
{
    auto ir = emitLlvmIr("f() -> i32 { q = PriorityQueue<i32>()  q.push(1)  return q.pop() }");
    EXPECT_TRUE(ir.find("sub i32") != std::string::npos);
    EXPECT_TRUE(ir.find("priorityqueue.pop.siftdown.header") != std::string::npos);
    EXPECT_TRUE(ir.find("priorityqueue.pop.siftdown.swap") != std::string::npos);
    EXPECT_TRUE(ir.find(" phi ") == std::string::npos);
}

TEST("LlvmIrEmitter PriorityQueue<char> push/pop compare at char's own i24 width, not a "
     "hardcoded i32 - char is orderable by codepoint, same as i32 (see "
     "docs/language/0044-char.md)")
{
    auto ir = emitLlvmIr("f() -> char { "
                         "  q = PriorityQueue<char>() "
                         "  q.push('b')  q.push('a') "
                         "  return q.pop() "
                         "}");
    EXPECT_TRUE(ir.find("icmp sle i24") != std::string::npos);
    EXPECT_TRUE(ir.find("icmp slt i24") != std::string::npos);
    EXPECT_TRUE(ir.find("icmp sle i32") == std::string::npos);
    // The only remaining "icmp slt i32"/"icmp sgt i32" in this program are
    // the sift loops' own index-vs-length bookkeeping (always genuinely
    // i32, regardless of element type) - not element comparisons.
}

TEST("LlvmIrEmitter PriorityQueue<str> push/pop compare via a real lexicographic @axea.less.str "
     "call, not a pointer-identity icmp on i8* - str has a real order, same as i32/char (see "
     "docs/language/0042-string.md)")
{
    auto ir = emitLlvmIr("f() -> str { "
                         "  q = PriorityQueue<str>() "
                         "  q.push(\"b\")  q.push(\"a\") "
                         "  return q.pop() "
                         "}");
    EXPECT_TRUE(ir.find("define i1 @axea.less.str(i8* %a, i8* %b)") != std::string::npos);
    EXPECT_TRUE(ir.find("call i1 @axea.less.str(") != std::string::npos);
    // Never a raw icmp directly on two i8* values - that would compare
    // pointer identity, not string content.
    EXPECT_TRUE(ir.find("icmp slt i8*") == std::string::npos);
    EXPECT_TRUE(ir.find("icmp sle i8*") == std::string::npos);
}

TEST("LlvmIrEmitter PriorityQueue peek reads index 0 directly, no length arithmetic and no store")
{
    // Isolated to a function taking PriorityQueue<i32> as a *parameter* (no
    // construction/push in the same function), mirroring the equivalent
    // Stack<T> peek test - emitPriorityQueuePeek's own output is then the
    // function's entire body.
    auto ir = emitLlvmIr("f(q: PriorityQueue<i32>) -> i32 { return q.peek() }");
    EXPECT_TRUE(ir.find("getelementptr i32, i32* %2, i32 0") != std::string::npos);
    EXPECT_TRUE(ir.find("store") == std::string::npos);
}

TEST("LlvmIrEmitter reads a PriorityQueue's .length via GEP+load, not a compile-time constant")
{
    auto ir = emitLlvmIr("len(q: PriorityQueue<i32>) -> i32 { return q.length }");
    EXPECT_TRUE(ir.find("getelementptr {i32, i32*, i32}, {i32, i32*, i32}* %0, i32 0, i32 0") !=
                std::string::npos);
}

TEST("LlvmIrEmitter List<T>/Stack<T>/PriorityQueue<T> push resolve to three distinct emit "
     "functions on the same-shaped element type, each calling the same shared "
     "ensureListCapacity helper")
{
    auto ir = emitLlvmIr("useList(l: List<i32>) { l.push(1) } "
                         "useStack(s: Stack<i32>) { s.push(1) } "
                         "usePQ(q: PriorityQueue<i32>) { q.push(1) }");
    EXPECT_TRUE(ir.find("define void @useList({i32, i32*, i32}* %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("define void @useStack({i32, i32*, i32}* %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("define void @usePQ({i32, i32*, i32}* %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("priorityqueue.push.siftup.header") != std::string::npos);
}

TEST("LlvmIrEmitter Stack<T>.peek() and PriorityQueue<T>.peek() resolve to distinct emit "
     "functions despite sharing the same method name")
{
    auto ir = emitLlvmIr("useStack(s: Stack<i32>) -> i32 { return s.peek() } "
                         "usePQ(q: PriorityQueue<i32>) -> i32 { return q.peek() }");
    // Stack peek computes length-1 (a "sub") before indexing; PriorityQueue
    // peek indexes 0 directly with no arithmetic - so useStack's body
    // contains a "sub" and usePQ's does not, verified by isolating each
    // function's own text.
    const auto useStackPos = ir.find("define i32 @useStack");
    const auto usePQPos = ir.find("define i32 @usePQ");
    EXPECT_TRUE(useStackPos != std::string::npos && usePQPos != std::string::npos);
    const std::string useStackBody = ir.substr(useStackPos, usePQPos - useStackPos);
    EXPECT_TRUE(useStackBody.find("sub i32") != std::string::npos);
}

TEST("LlvmIrEmitter declares named self-referential types for Map/Set entries")
{
    // Monomorphized (see docs/language/0034-maps-and-sets.md's generic
    // rewrite): the first Map/Set instantiation actually used gets id 0
    // (Map's and Set's own id counters are independent, so both land on
    // "0" here even though this program only builds a Map).
    auto ir = emitLlvmIr("f() -> i32 { m = Map<i32,i32>()  return m.length }");
    EXPECT_TRUE(ir.find("%axea.MapEntry.0 = type { i32, i32, %axea.MapEntry.0* }") !=
                std::string::npos);
}

TEST("LlvmIrEmitter represents Map<i32,i32>/Set<i32> as a pointer to a 3-field heap header")
{
    auto ir = emitLlvmIr("useMap(m: Map<i32,i32>) -> i32 { return m.length } "
                         "useSet(s: Set<i32>) -> i32 { return s.length }");
    EXPECT_TRUE(ir.find("define i32 @useMap({i32, i32, %axea.MapEntry.0**}* %0) {") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("define i32 @useSet({i32, i32, %axea.SetEntry.0**}* %0) {") !=
                std::string::npos);
}

TEST("LlvmIrEmitter's Map<i32,i32>() construction mallocs a header and an 8-slot bucket array")
{
    auto ir = emitLlvmIr("f() -> i32 { m = Map<i32,i32>()  return m.length }");
    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32 0, i32*") != std::string::npos); // count = 0
    EXPECT_TRUE(ir.find("store i32 8, i32*") != std::string::npos); // bucketCount = 8
    // 8 unrolled null-initializing stores into the fresh bucket array, within
    // `f` itself - scoped to before the per-instantiation axea.map.0.*
    // runtime functions (registerMapInstantiation), which contain a couple
    // more of this same substring in their own, unrelated resize/remove
    // logic.
    const std::size_t runtimeStart = ir.find("define i32 @axea.hash.i32");
    EXPECT_TRUE(runtimeStart != std::string::npos);
    std::size_t nullStoreCount = 0;
    for (std::size_t pos = ir.find("store %axea.MapEntry.0* null,");
         pos != std::string::npos && pos < runtimeStart;
         pos = ir.find("store %axea.MapEntry.0* null,", pos + 1))
    {
        ++nullStoreCount;
    }
    EXPECT_EQ(nullStoreCount, static_cast<std::size_t>(8));
}

TEST("LlvmIrEmitter's Map/Set operations call that instantiation's own axea.map.N/axea.set.N "
     "runtime functions")
{
    auto ir = emitLlvmIr("f() { "
                         "  m = Map<i32,i32>() "
                         "  m.set(1, 2) "
                         "  v = m.get(1) "
                         "  hit = m.contains(1) "
                         "  m.remove(1) "
                         "  s = Set<i32>() "
                         "  s.add(1) "
                         "  shit = s.contains(1) "
                         "  s.remove(1) "
                         "}");
    EXPECT_TRUE(ir.find("call void @axea.map.0.set(") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 @axea.map.0.get(") != std::string::npos);
    EXPECT_TRUE(ir.find("call i1 @axea.map.0.contains(") != std::string::npos);
    EXPECT_TRUE(ir.find("call void @axea.map.0.remove(") != std::string::npos);
    EXPECT_TRUE(ir.find("call void @axea.set.0.add(") != std::string::npos);
    EXPECT_TRUE(ir.find("call i1 @axea.set.0.contains(") != std::string::npos);
    EXPECT_TRUE(ir.find("call void @axea.set.0.remove(") != std::string::npos);
    // Shared primitive key hash (i32 keys, both Map and Set), and each
    // instantiation's own resize function.
    EXPECT_TRUE(ir.find("define i32 @axea.hash.i32(i32 %key) {") != std::string::npos);
    EXPECT_TRUE(ir.find("define void @axea.map.0.resize(") != std::string::npos);
    EXPECT_TRUE(ir.find("define void @axea.set.0.resize(") != std::string::npos);
}

TEST("LlvmIrEmitter reads a Map/Set's .length via GEP+load field 0, not a compile-time constant")
{
    auto ir = emitLlvmIr("mlen(m: Map<i32,i32>) -> i32 { return m.length } "
                         "slen(s: Set<i32>) -> i32 { return s.length }");
    EXPECT_TRUE(ir.find("getelementptr {i32, i32, %axea.MapEntry.0**}, "
                        "{i32, i32, %axea.MapEntry.0**}* %0, i32 0, i32 0") != std::string::npos);
    EXPECT_TRUE(ir.find("getelementptr {i32, i32, %axea.SetEntry.0**}, "
                        "{i32, i32, %axea.SetEntry.0**}* %0, i32 0, i32 0") != std::string::npos);
}

TEST("LlvmIrEmitter monomorphizes each distinct Map<K,V> shape into its own entry type/functions")
{
    // Map<i32,i32> and Map<i32,str> are two genuinely different
    // instantiations (different V) - each gets its own numbered entry type
    // and its own axea.map.N.* functions, coexisting correctly (see
    // docs/language/0034-maps-and-sets.md's generic rewrite).
    auto ir = emitLlvmIr("useA(m: Map<i32,i32>) -> i32 { return m.get(1) } "
                         "useB(m: Map<i32,str>) -> str { return m.get(1) }");
    EXPECT_TRUE(ir.find("%axea.MapEntry.0 = type { i32, i32, %axea.MapEntry.0* }") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("%axea.MapEntry.1 = type { i32, i8*, %axea.MapEntry.1* }") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("define i32 @axea.map.0.get(") != std::string::npos);
    EXPECT_TRUE(ir.find("define i8* @axea.map.1.get(") != std::string::npos);
}

TEST("LlvmIrEmitter generates a byte-walk hash/equality pair for str keys")
{
    auto ir = emitLlvmIr("f() -> i32 { m = Map<str,i32>()  m.set(\"a\", 1)  return m.get(\"a\") }");
    EXPECT_TRUE(ir.find("define i32 @axea.hash.str(i8* %s) {") != std::string::npos);
    EXPECT_TRUE(ir.find("define i1 @axea.eq.str(i8* %a, i8* %b) {") != std::string::npos);
    // .set/.get call through the shared str hash/eq, not an inline icmp
    // (which would be pointer-identity comparison - wrong for string
    // content equality).
    EXPECT_TRUE(ir.find("call i1 @axea.eq.str(") != std::string::npos);
}

TEST("LlvmIrEmitter generates a recursive derive-hash/equality pair for a struct key")
{
    auto ir = emitLlvmIr("struct Point { x: i32  y: i32 } "
                         "f() { s = Set<Point>()  s.add(Point { x: 1  y: 2 }) }");
    EXPECT_TRUE(ir.find("define i32 @axea.hash.Point(%Point* %v) {") != std::string::npos);
    EXPECT_TRUE(ir.find("define i1 @axea.eq.Point(%Point* %a, %Point* %b) {") != std::string::npos);
    // Combines each field's own i32 hash (djb2-style: acc = acc*31 + fieldHash).
    EXPECT_TRUE(ir.find("call i32 @axea.hash.i32(") != std::string::npos);
    EXPECT_TRUE(ir.find("mul i32") != std::string::npos);
}

TEST("LlvmIrEmitter generates an unrolled hash/equality pair for a fixed-array key")
{
    auto ir = emitLlvmIr("f() { s = Set<[i32;3]>()  s.add([1, 2, 3]) }");
    EXPECT_TRUE(ir.find("define i32 @axea.hash.arr.0([3 x i32]* %v) {") != std::string::npos);
    EXPECT_TRUE(ir.find("define i1 @axea.eq.arr.0([3 x i32]* %a, [3 x i32]* %b) {") !=
                std::string::npos);
}

TEST("LlvmIrEmitter generates a runtime-loop hash/equality pair for a List<T> key")
{
    auto ir = emitLlvmIr("f() { s = Set<List<i32>>()  l = List<i32>()  s.add(l) }");
    EXPECT_TRUE(ir.find("define i32 @axea.hash.list.0({i32, i32*}* %v) {") != std::string::npos);
    EXPECT_TRUE(ir.find("define i1 @axea.eq.list.0({i32, i32*}* %a, {i32, i32*}* %b) {") !=
                std::string::npos);
    // Length-mismatch short-circuit before any element walk.
    EXPECT_TRUE(ir.find("icmp eq i32 %alen, %blen") != std::string::npos);
}

TEST("LlvmIrEmitter prints a top-level Map/Set binding by count, not contents")
{
    auto ir = emitLlvmIr("m = Map<i32,i32>() s = Set<i32>()");
    EXPECT_TRUE(ir.find("Map(") != std::string::npos);
    EXPECT_TRUE(ir.find("Set(") != std::string::npos);
    EXPECT_TRUE(ir.find(" entries)") != std::string::npos);
}

TEST("LlvmIrEmitter represents SortedMap<i32,i32> as a pointer to a named-node 2-field heap "
     "header, distinct from Map/Set's own 3-field header")
{
    auto ir = emitLlvmIr("useSM(m: SortedMap<i32,i32>) -> i32 { return m.length }");
    EXPECT_TRUE(ir.find("define i32 @useSM({i32, %axea.SortedMapNode.0*}* %0) {") !=
                std::string::npos);
}

TEST("LlvmIrEmitter declares a 5-field self-referential node type for SortedMap<K,V> - key, "
     "value, height, left, right")
{
    auto ir = emitLlvmIr("f() -> i32 { m = SortedMap<i32,i32>()  m.set(1, 2)  return m.length }");
    EXPECT_TRUE(ir.find("%axea.SortedMapNode.0 = type { i32, i32, i32, %axea.SortedMapNode.0*, "
                        "%axea.SortedMapNode.0* }") != std::string::npos);
}

TEST("LlvmIrEmitter's SortedMap<K,V>() construction mallocs a 2-field header and zero-"
     "initializes it - no bucket array, unlike Map/Set's own 3-field header")
{
    auto ir = emitLlvmIr("f() -> i32 { m = SortedMap<i32,i32>()  return m.length }");
    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32 0, i32*") != std::string::npos); // count = 0
    EXPECT_TRUE(ir.find("store %axea.SortedMapNode.0* null, %axea.SortedMapNode.0**") !=
                std::string::npos); // root = null
}

TEST("LlvmIrEmitter's SortedMap operations call that instantiation's own axea.sortedmap.N "
     "runtime functions, including the AVL rotation/height helpers")
{
    auto ir = emitLlvmIr("f() { "
                         "  m = SortedMap<i32,i32>() "
                         "  m.set(1, 2) "
                         "  v = m.get(1) "
                         "  hit = m.contains(1) "
                         "  m.remove(1) "
                         "}");
    EXPECT_TRUE(ir.find("call void @axea.sortedmap.0.set(") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 @axea.sortedmap.0.get(") != std::string::npos);
    EXPECT_TRUE(ir.find("call i1 @axea.sortedmap.0.contains(") != std::string::npos);
    EXPECT_TRUE(ir.find("call void @axea.sortedmap.0.remove(") != std::string::npos);
    EXPECT_TRUE(ir.find("define i32 @axea.sortedmap.0.height(") != std::string::npos);
    EXPECT_TRUE(ir.find("define %axea.SortedMapNode.0* @axea.sortedmap.0.rotateLeft(") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("define %axea.SortedMapNode.0* @axea.sortedmap.0.rotateRight(") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("define %axea.SortedMapNode.0* @axea.sortedmap.0.insertNode(") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("define %axea.SortedMapNode.0* @axea.sortedmap.0.removeNode(") !=
                std::string::npos);
    // No `free` call anywhere - matches this codebase's established "leak,
    // don't free" policy (Map<K,V>.remove doesn't free its entry either).
    EXPECT_TRUE(ir.find("call void @free") == std::string::npos);
    // No `phi` anywhere - every loop/recursive helper here uses the same
    // alloca/load/store convention this whole backend establishes (see
    // docs/language/0040-sorted-maps.md).
    EXPECT_TRUE(ir.find(" phi ") == std::string::npos);
}

TEST("LlvmIrEmitter reads a SortedMap's .length via GEP+load field 0, not a compile-time "
     "constant")
{
    auto ir = emitLlvmIr("len(m: SortedMap<i32,i32>) -> i32 { return m.length }");
    EXPECT_TRUE(ir.find("getelementptr {i32, %axea.SortedMapNode.0*}, "
                        "{i32, %axea.SortedMapNode.0*}* %0, i32 0, i32 0") != std::string::npos);
}

TEST("LlvmIrEmitter's SortedMap<char,V> node type stores the key at char's own i24 width, not "
     "a hardcoded i32 - char is orderable by codepoint, same as i32 (see "
     "docs/language/0044-char.md)")
{
    auto ir = emitLlvmIr("f() { m = SortedMap<char,i32>()  m.set('a', 1) }");
    EXPECT_TRUE(ir.find("type { i24, i32, i32,") != std::string::npos);
    EXPECT_TRUE(ir.find("icmp slt i24") != std::string::npos);
}

TEST("LlvmIrEmitter's SortedMap<str,V> node type stores the key as a bare i8*, comparing via a "
     "real lexicographic @axea.less.str call, not a pointer-identity icmp - str has a real "
     "order, same as i32/char (see docs/language/0042-string.md)")
{
    auto ir = emitLlvmIr("f() { m = SortedMap<str,i32>()  m.set(\"a\", 1) }");
    EXPECT_TRUE(ir.find("type { i8*, i32, i32,") != std::string::npos);
    EXPECT_TRUE(ir.find("define i1 @axea.less.str(i8* %a, i8* %b)") != std::string::npos);
    EXPECT_TRUE(ir.find("call i1 @axea.less.str(") != std::string::npos);
    EXPECT_TRUE(ir.find("icmp slt i8*") == std::string::npos);
    EXPECT_TRUE(ir.find("icmp sgt i8*") == std::string::npos);
}

TEST("LlvmIrEmitter monomorphizes each distinct SortedMap<K,V> shape into its own node type/"
     "functions")
{
    auto ir = emitLlvmIr("useA(m: SortedMap<i32,i32>) -> i32 { return m.get(1) } "
                         "useB(m: SortedMap<i32,str>) -> str { return m.get(1) }");
    EXPECT_TRUE(ir.find("%axea.SortedMapNode.0 = type { i32, i32, i32, %axea.SortedMapNode.0*, "
                        "%axea.SortedMapNode.0* }") != std::string::npos);
    EXPECT_TRUE(ir.find("%axea.SortedMapNode.1 = type { i32, i8*, i32, %axea.SortedMapNode.1*, "
                        "%axea.SortedMapNode.1* }") != std::string::npos);
    EXPECT_TRUE(ir.find("define i32 @axea.sortedmap.0.get(") != std::string::npos);
    EXPECT_TRUE(ir.find("define i8* @axea.sortedmap.1.get(") != std::string::npos);
}

TEST("LlvmIrEmitter prints a top-level SortedMap binding by count, not contents - matches the "
     "interpreter's own identical choice (see docs/language/0040-sorted-maps.md)")
{
    auto ir = emitLlvmIr("m = SortedMap<i32,i32>()");
    EXPECT_TRUE(ir.find("SortedMap(") != std::string::npos);
    EXPECT_TRUE(ir.find(" entries)") != std::string::npos);
}

TEST("LlvmIrEmitter Map<K,V>/SortedMap<K,V> set/get resolve to distinct emit functions despite "
     "sharing the same method names")
{
    auto ir = emitLlvmIr("useMap(m: Map<i32,i32>) { m.set(1, 2) } "
                         "useSortedMap(m: SortedMap<i32,i32>) { m.set(1, 2) }");
    EXPECT_TRUE(ir.find("call void @axea.map.0.set(") != std::string::npos);
    EXPECT_TRUE(ir.find("call void @axea.sortedmap.0.set(") != std::string::npos);
}

TEST("LlvmIrEmitter represents SortedSet<i32> as a pointer to a named-node 2-field heap header, "
     "distinct from Set's own 3-field header")
{
    auto ir = emitLlvmIr("useSS(s: SortedSet<i32>) -> i32 { return s.length }");
    EXPECT_TRUE(ir.find("define i32 @useSS({i32, %axea.SortedSetNode.0*}* %0) {") !=
                std::string::npos);
}

TEST("LlvmIrEmitter declares a 4-field self-referential node type for SortedSet<T> - key, "
     "height, left, right - no value field, unlike SortedMap<K,V>'s own 5-field node")
{
    auto ir = emitLlvmIr("f() -> i32 { s = SortedSet<i32>()  s.add(1)  return s.length }");
    EXPECT_TRUE(ir.find("%axea.SortedSetNode.0 = type { i32, i32, %axea.SortedSetNode.0*, "
                        "%axea.SortedSetNode.0* }") != std::string::npos);
}

TEST("LlvmIrEmitter's SortedSet<T>() construction mallocs a 2-field header and zero-initializes "
     "it - no bucket array, unlike Set's own 3-field header")
{
    auto ir = emitLlvmIr("f() -> i32 { s = SortedSet<i32>()  return s.length }");
    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32 0, i32*") != std::string::npos); // count = 0
    EXPECT_TRUE(ir.find("store %axea.SortedSetNode.0* null, %axea.SortedSetNode.0**") !=
                std::string::npos); // root = null
}

TEST("LlvmIrEmitter's SortedSet operations call that instantiation's own axea.sortedset.N "
     "runtime functions, including the AVL rotation/height helpers")
{
    auto ir = emitLlvmIr("f() { "
                         "  s = SortedSet<i32>() "
                         "  s.add(1) "
                         "  hit = s.contains(1) "
                         "  s.remove(1) "
                         "}");
    EXPECT_TRUE(ir.find("call void @axea.sortedset.0.add(") != std::string::npos);
    EXPECT_TRUE(ir.find("call i1 @axea.sortedset.0.contains(") != std::string::npos);
    EXPECT_TRUE(ir.find("call void @axea.sortedset.0.remove(") != std::string::npos);
    EXPECT_TRUE(ir.find("define i32 @axea.sortedset.0.height(") != std::string::npos);
    EXPECT_TRUE(ir.find("define %axea.SortedSetNode.0* @axea.sortedset.0.rotateLeft(") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("define %axea.SortedSetNode.0* @axea.sortedset.0.rotateRight(") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("define %axea.SortedSetNode.0* @axea.sortedset.0.insertNode(") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("define %axea.SortedSetNode.0* @axea.sortedset.0.removeNode(") !=
                std::string::npos);
    // No `free` call, no `phi` anywhere - matches SortedMap<K,V>'s own
    // identical choices (see docs/language/0041-sorted-sets.md).
    EXPECT_TRUE(ir.find("call void @free") == std::string::npos);
    EXPECT_TRUE(ir.find(" phi ") == std::string::npos);
}

TEST("LlvmIrEmitter's SortedSet<char> node type stores the key at char's own i24 width, not "
     "a hardcoded i32 - char is orderable by codepoint, same as i32 (see "
     "docs/language/0044-char.md)")
{
    auto ir = emitLlvmIr("f() { s = SortedSet<char>()  s.add('a') }");
    EXPECT_TRUE(ir.find("type { i24, i32,") != std::string::npos);
    EXPECT_TRUE(ir.find("icmp slt i24") != std::string::npos);
}

TEST("LlvmIrEmitter's SortedSet<str> node type stores the key as a bare i8*, comparing via a "
     "real lexicographic @axea.less.str call, not a pointer-identity icmp - str has a real "
     "order, same as i32/char (see docs/language/0042-string.md)")
{
    auto ir = emitLlvmIr("f() { s = SortedSet<str>()  s.add(\"a\") }");
    EXPECT_TRUE(ir.find("type { i8*, i32,") != std::string::npos);
    EXPECT_TRUE(ir.find("define i1 @axea.less.str(i8* %a, i8* %b)") != std::string::npos);
    EXPECT_TRUE(ir.find("call i1 @axea.less.str(") != std::string::npos);
    EXPECT_TRUE(ir.find("icmp slt i8*") == std::string::npos);
    EXPECT_TRUE(ir.find("icmp sgt i8*") == std::string::npos);
}

TEST("LlvmIrEmitter reads a SortedSet's .length via GEP+load field 0, not a compile-time "
     "constant")
{
    auto ir = emitLlvmIr("len(s: SortedSet<i32>) -> i32 { return s.length }");
    EXPECT_TRUE(ir.find("getelementptr {i32, %axea.SortedSetNode.0*}, "
                        "{i32, %axea.SortedSetNode.0*}* %0, i32 0, i32 0") != std::string::npos);
}

TEST("LlvmIrEmitter prints a top-level SortedSet binding by count, not contents - matches the "
     "interpreter's own identical choice (see docs/language/0041-sorted-sets.md)")
{
    auto ir = emitLlvmIr("s = SortedSet<i32>()");
    EXPECT_TRUE(ir.find("SortedSet(") != std::string::npos);
    EXPECT_TRUE(ir.find(" entries)") != std::string::npos);
}

TEST("LlvmIrEmitter Set<T>/SortedSet<T> add resolve to distinct emit functions despite sharing "
     "the same method name")
{
    auto ir = emitLlvmIr("useSet(s: Set<i32>) { s.add(1) } "
                         "useSortedSet(s: SortedSet<i32>) { s.add(1) }");
    EXPECT_TRUE(ir.find("call void @axea.set.0.add(") != std::string::npos);
    EXPECT_TRUE(ir.find("call void @axea.sortedset.0.add(") != std::string::npos);
}

TEST("LlvmIrEmitter represents String as the exact same LLVM type as List<i8> would - a "
     "2-field {i32, i8*}* header")
{
    auto ir = emitLlvmIr("useString(s: String) -> i32 { return s.length }");
    EXPECT_TRUE(ir.find("define i32 @useString({i32, i8*}* %0) {") != std::string::npos);
}

TEST("LlvmIrEmitter declares @strlen as a third libc extern alongside @malloc/@printf - str has "
     "no length field of its own, unlike every element type every other collection copies (see "
     "docs/language/0042-string.md)")
{
    auto ir = emitLlvmIr("f() {}");
    EXPECT_TRUE(ir.find("declare i64 @strlen(i8*)") != std::string::npos);
}

TEST("LlvmIrEmitter's String(text) construction mallocs a header and a null-terminated buffer "
     "via a real @strlen call, copying text's own bytes in a hand-rolled loop, no phi")
{
    auto ir = emitLlvmIr("f() -> i32 { s = String(\"hi\")  return s.length }");
    EXPECT_TRUE(ir.find("call i64 @strlen(i8*") != std::string::npos);
    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("string.new.copy.header") != std::string::npos);
    EXPECT_TRUE(ir.find("string.new.copy.body") != std::string::npos);
    EXPECT_TRUE(ir.find("string.new.copy.done") != std::string::npos);
    EXPECT_TRUE(ir.find(" phi ") == std::string::npos);
}

TEST("LlvmIrEmitter's String.append grows via two copy loops - the existing content, then the "
     "newly appended bytes - no phi")
{
    auto ir = emitLlvmIr("f() { s = String(\"hi\")  s.append(\"!\") }");
    EXPECT_TRUE(ir.find("string.append.copyold.header") != std::string::npos);
    EXPECT_TRUE(ir.find("string.append.copynew.header") != std::string::npos);
    EXPECT_TRUE(ir.find(" phi ") == std::string::npos);
}

TEST("LlvmIrEmitter reads a String's .bytes via GEP+load field 0, not a compile-time constant - "
     "the raw stored byte count, what .length itself used to mean (see "
     "docs/language/0047-unicode.md)")
{
    auto ir = emitLlvmIr("len(s: String) -> i32 { return s.bytes }");
    EXPECT_TRUE(ir.find("getelementptr {i32, i8*}, {i32, i8*}* %0, i32 0, i32 0") !=
                std::string::npos);
}

TEST("LlvmIrEmitter's String.length now counts Unicode codepoints via the shared "
     "@axea.utf8.count runtime, not a stored field read")
{
    auto ir = emitLlvmIr("len(s: String) -> i32 { return s.length }");
    EXPECT_TRUE(ir.find("define i32 @axea.utf8.count(i8* %s)") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 @axea.utf8.count(i8*") != std::string::npos);
    EXPECT_TRUE(ir.find(" phi ") == std::string::npos);
}

TEST("LlvmIrEmitter passes a String argument as a bare i8* at a str-parameter call boundary - "
     "'String automatically lends a str' (see docs/std/strings/0001-str.md)")
{
    auto ir = emitLlvmIr("useStr(s: str) -> str { return s } "
                         "f() -> str { s = String(\"hi\")  return useStr(s) }");
    EXPECT_TRUE(ir.find("call i8* @useStr(i8* %") != std::string::npos);
}

TEST("LlvmIrEmitter prints a top-level String binding via a direct %s of its data pointer, not "
     "the generic byte-print loop that would misread each byte as a struct pointer")
{
    auto ir = emitLlvmIr("s = String(\"hi\")");
    EXPECT_TRUE(ir.find("call i32 (i8*, ...) @printf(i8* getelementptr") != std::string::npos);
    // No struct-print-helper call - a byte-print loop misreading "i8" as a
    // nested struct element type would emit one of these.
    EXPECT_TRUE(ir.find("@axea.print.i8") == std::string::npos);
}

TEST("LlvmIrEmitter represents Buffer as a 3-field {i32, i32, i8*}* header - one field more "
     "than String's own 2-field header")
{
    auto ir = emitLlvmIr("useBuffer(b: Buffer) -> i32 { return b.length }");
    EXPECT_TRUE(ir.find("define i32 @useBuffer({i32, i32, i8*}* %0) {") != std::string::npos);
}

TEST("LlvmIrEmitter's Buffer() construction mallocs a header and a minimal 1-byte data buffer, "
     "with length 0 and capacity 1")
{
    auto ir = emitLlvmIr("f() -> i32 { b = Buffer()  return b.length }");
    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("call i8* @malloc(i64 1)") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32 0, i32*") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32 1, i32*") != std::string::npos);
}

TEST("LlvmIrEmitter's Buffer.append grows conditionally via a real br i1 branch, not "
     "unconditionally like every push/append/set/add before it - no phi")
{
    auto ir = emitLlvmIr("f() { b = Buffer()  b.append(\"hi\") }");
    EXPECT_TRUE(ir.find("buffer.grow") != std::string::npos);
    EXPECT_TRUE(ir.find("icmp sgt i32") != std::string::npos);
    EXPECT_TRUE(ir.find("= select i1") != std::string::npos);
    EXPECT_TRUE(ir.find("buffer.append.copy.header") != std::string::npos);
    EXPECT_TRUE(ir.find(" phi ") == std::string::npos);
}

TEST("LlvmIrEmitter's Buffer.append_line writes a trailing newline byte before the null "
     "terminator")
{
    auto ir = emitLlvmIr("f() { b = Buffer()  b.append_line(\"hi\") }");
    EXPECT_TRUE(ir.find("store i8 10,") != std::string::npos);
    EXPECT_TRUE(ir.find("buffer.appendline.copy.header") != std::string::npos);
}

TEST("LlvmIrEmitter's Buffer.clear resets length to 0 and null-terminates data[0] without "
     "touching capacity")
{
    auto ir = emitLlvmIr("f() { b = Buffer()  b.clear() }");
    EXPECT_TRUE(ir.find("store i32 0, i32*") != std::string::npos);
    EXPECT_TRUE(ir.find("store i8 0, i8*") != std::string::npos);
}

TEST("LlvmIrEmitter's Buffer.reserve shares the same grow-if-needed helper as .append")
{
    auto ir = emitLlvmIr("f() { b = Buffer()  b.reserve(64) }");
    EXPECT_TRUE(ir.find("buffer.grow") != std::string::npos);
    EXPECT_TRUE(ir.find("icmp sgt i32") != std::string::npos);
}

TEST("LlvmIrEmitter's Buffer.finish mallocs a fresh 2-field String header and resets the "
     "original buffer back to a fresh, minimal state")
{
    auto ir = emitLlvmIr("f() -> i32 { b = Buffer()  s = b.finish()  return s.length }");
    EXPECT_TRUE(ir.find("bitcast i8* %") != std::string::npos);
    EXPECT_TRUE(ir.find("{i32, i8*}*") != std::string::npos);
    // The reset path mallocs a second fresh 1-byte buffer for the
    // now-emptied original Buffer.
    EXPECT_TRUE(ir.find("call i8* @malloc(i64 1)") != std::string::npos);
}

TEST("LlvmIrEmitter reads a Buffer's .bytes via field 0 and .capacity via field 1, distinct GEP "
     "indices - .bytes is the raw stored count, what .length itself used to mean (see "
     "docs/language/0047-unicode.md)")
{
    auto ir = emitLlvmIr("f(b: Buffer) -> i32 { return b.bytes + b.capacity }");
    EXPECT_TRUE(ir.find("getelementptr {i32, i32, i8*}, {i32, i32, i8*}* %0, i32 0, i32 0") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("getelementptr {i32, i32, i8*}, {i32, i32, i8*}* %0, i32 0, i32 1") !=
                std::string::npos);
}

TEST("LlvmIrEmitter's Buffer.length now counts Unicode codepoints via the shared "
     "@axea.utf8.count runtime, called on the extracted data pointer (field 2), not a stored "
     "field read")
{
    auto ir = emitLlvmIr("f(b: Buffer) -> i32 { return b.length }");
    EXPECT_TRUE(ir.find("getelementptr {i32, i32, i8*}, {i32, i32, i8*}* %0, i32 0, i32 2") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("call i32 @axea.utf8.count(i8*") != std::string::npos);
}

TEST("LlvmIrEmitter reads a bare str's .bytes via @strlen and .length via @axea.utf8.count - "
     "previously unreachable here at all, since str had no field access before")
{
    auto ir = emitLlvmIr("bytesOf(s: str) -> i32 { return s.bytes }");
    EXPECT_TRUE(ir.find("call i64 @strlen(i8* %0)") != std::string::npos);

    auto ir2 = emitLlvmIr("lengthOf(s: str) -> i32 { return s.length }");
    EXPECT_TRUE(ir2.find("call i32 @axea.utf8.count(i8* %0)") != std::string::npos);
}

TEST("LlvmIrEmitter registers @axea.utf8.count only once even when .length is read on str, "
     "String, and Buffer in the same program")
{
    auto ir = emitLlvmIr("f(s: str, o: String, b: Buffer) -> i32 { "
                         "  return s.length + o.length + b.length "
                         "}");
    const auto first = ir.find("define i32 @axea.utf8.count");
    EXPECT_TRUE(first != std::string::npos);
    const auto second = ir.find("define i32 @axea.utf8.count", first + 1);
    EXPECT_TRUE(second == std::string::npos);
}

TEST("LlvmIrEmitter Buffer.append and String.append resolve to distinct emit functions despite "
     "sharing the same method name")
{
    auto ir = emitLlvmIr("f() { buf = Buffer()  buf.append(\"a\")  s = String(\"b\")  "
                         "s.append(\"c\") }");
    EXPECT_TRUE(ir.find("buffer.grow") != std::string::npos);
    EXPECT_TRUE(ir.find("string.append.copyold.header") != std::string::npos);
}

TEST("LlvmIrEmitter prints a top-level Buffer binding via a direct %s of its data pointer at "
     "field index 2, not the generic byte-print loop or Deque's own field-2 print branch")
{
    auto ir = emitLlvmIr("b = Buffer()");
    EXPECT_TRUE(ir.find("call i32 (i8*, ...) @printf(i8* getelementptr") != std::string::npos);
    EXPECT_TRUE(ir.find("@axea.print.i8") == std::string::npos);
}

TEST("LlvmIrEmitter represents char as i24, genuinely distinct from i32's own width - not a "
     "stylistic choice, since a plain 'i32' char register would be indistinguishable from a "
     "real i32 one downstream")
{
    auto ir = emitLlvmIr("useChar(c: char) -> char { return c }");
    EXPECT_TRUE(ir.find("define i24 @useChar(i24 %0) {") != std::string::npos);
}

TEST("LlvmIrEmitter's char literal materializes as a trivial i24 SSA constant, same shape as "
     "IrConstInt's own i32 constant")
{
    auto ir = emitLlvmIr("f() -> char { return 'A' }");
    EXPECT_TRUE(ir.find("= add i24 0, 65") != std::string::npos);
}

TEST("LlvmIrEmitter's char equality/ordering reuse the exact same icmp opcodes as i32, just at "
     "i24 width - zero new opcode-selection code needed")
{
    auto ir = emitLlvmIr("f() -> bool { a = 'A'  b = 'B'  return a < b }");
    EXPECT_TRUE(ir.find("icmp slt i24") != std::string::npos);
}

TEST("LlvmIrEmitter's i64 arithmetic/comparison reuse the exact same opcodes as i32, just at "
     "i64 width - zero new opcode-selection code needed, unlike f64's own genuinely different "
     "opcode table (see docs/language/0005-type-system.md)")
{
    auto ir = emitLlvmIr("f() -> i64 { a = 100i64  b = 25i64  c = a + b  return c }");
    EXPECT_TRUE(ir.find("= add i64 %") != std::string::npos);
    EXPECT_TRUE(ir.find("= add i64 0, 100") != std::string::npos);
}

TEST("LlvmIrEmitter's f64 arithmetic/comparison use real floating-point opcodes "
     "(fadd/fsub/fmul/fdiv, fcmp with an ordered predicate) - not the integer add/icmp every "
     "other numeric kind here shares")
{
    auto ir = emitLlvmIr("f() -> bool { "
                         "  a = 1.5  b = 2.5 "
                         "  sum = a + b  quot = a / b  lt = a < b "
                         "  return lt "
                         "}");
    EXPECT_TRUE(ir.find("= fadd double %") != std::string::npos);
    EXPECT_TRUE(ir.find("= fdiv double %") != std::string::npos);
    EXPECT_TRUE(ir.find("= fcmp olt double %") != std::string::npos);
}

TEST("LlvmIrEmitter materializes a float constant via LLVM's own exact hex float form, not "
     "plain decimal notation (formatDoubleLiteral)")
{
    auto ir = emitLlvmIr("f() -> f64 { return 1.5 }");
    EXPECT_TRUE(ir.find("= fadd double 0.0, 0x3FF8000000000000") != std::string::npos);
}

TEST("LlvmIrEmitter's 'as' cast emits sext for i32->i64, trunc for i64->i32, sitofp for "
     "int->f64, and fptosi for f64->int - the four real numeric conversion opcodes, not a "
     "no-op bitcast (see docs/language/0005-type-system.md)")
{
    auto ir = emitLlvmIr("f() -> f64 { "
                         "  a = 5 "
                         "  b = a as i64 "
                         "  c = b as i32 "
                         "  d = a as f64 "
                         "  e = d as i32 "
                         "  return d "
                         "}");
    EXPECT_TRUE(ir.find("= sext i32 %") != std::string::npos);
    EXPECT_TRUE(ir.find(" to i64") != std::string::npos);
    EXPECT_TRUE(ir.find("= trunc i64 %") != std::string::npos);
    EXPECT_TRUE(ir.find(" to i32") != std::string::npos);
    EXPECT_TRUE(ir.find("= sitofp i32 %") != std::string::npos);
    EXPECT_TRUE(ir.find(" to double") != std::string::npos);
    EXPECT_TRUE(ir.find("= fptosi double %") != std::string::npos);
}

TEST("LlvmIrEmitter's 'as' cast to the same kind materializes via the same trivial no-op-"
     "arithmetic convention every constant here already uses, not an identity bitcast")
{
    auto ir = emitLlvmIr("f() -> i32 { a = 5  b = a as i32  return b }");
    EXPECT_TRUE(ir.find("= add i32 0, %") != std::string::npos);
    EXPECT_TRUE(ir.find("bitcast") == std::string::npos);
}

TEST("LlvmIrEmitter stringifies an i64 print argument via a real sprintf(\"%lld\", ...) call, "
     "and an f64 one via sprintf(\"%g\", ...) - matching Interpreter.cpp's own toString exactly")
{
    auto ir = emitLlvmIr("f() { a = 100i64  b = 1.5  p1 = print(a)  p2 = print(b) }");
    EXPECT_TRUE(ir.find("define i8* @axea.i64.to_str(i64 %v)") != std::string::npos);
    EXPECT_TRUE(ir.find("c\"%lld\\00\"") != std::string::npos);
    EXPECT_TRUE(ir.find("define i8* @axea.f64.to_str(double %v)") != std::string::npos);
    EXPECT_TRUE(ir.find("c\"%g\\00\"") != std::string::npos);
}

TEST("LlvmIrEmitter prints a top-level i64/f64 binding via the same \"%s = %s\\n\" path char/"
     "str/String already use, not a bare \"%d\" (which would misread a 64-bit value's upper "
     "bits, or crash entirely for a double)")
{
    auto ir = emitLlvmIr("a = 100i64 b = 1.5");
    EXPECT_TRUE(ir.find("call i8* @axea.i64.to_str(") != std::string::npos);
    EXPECT_TRUE(ir.find("call i8* @axea.f64.to_str(") != std::string::npos);
}

TEST("LlvmIrEmitter prints an i64/f64 struct field via the same stringifyValueOfType path top-"
     "level bindings use, not the generic byte-print loop or a misread as a nested struct "
     "pointer")
{
    auto ir = emitLlvmIr("struct Point { x: i64 y: f64 } p = Point { x: 100i64, y: 1.5 }");
    EXPECT_TRUE(ir.find("call i8* @axea.i64.to_str(") != std::string::npos);
    EXPECT_TRUE(ir.find("call i8* @axea.f64.to_str(") != std::string::npos);
}

TEST("LlvmIrEmitter's PriorityQueue<f64> push/pop compare via fcmp (ordered predicate), not "
     "icmp - f64 has a real order, same as i32/i64/char/str (see "
     "docs/language/0039-priority-queues.md)")
{
    auto ir = emitLlvmIr("f() -> f64 { "
                         "  q = PriorityQueue<f64>() "
                         "  q.push(2.5)  q.push(1.5) "
                         "  return q.pop() "
                         "}");
    EXPECT_TRUE(ir.find("fcmp ole double") != std::string::npos);
    EXPECT_TRUE(ir.find("fcmp olt double") != std::string::npos);
}

TEST("LlvmIrEmitter's SortedMap<i64,f64> node type stores the key at i64 width and compares "
     "via @axea.less.i64 - a real order, same as i32 (see docs/language/0040-sorted-maps.md)")
{
    auto ir = emitLlvmIr("f() { m = SortedMap<i64,f64>()  m.set(1i64, 1.5) }");
    EXPECT_TRUE(ir.find("type { i64, double, i32,") != std::string::npos);
    EXPECT_TRUE(ir.find("define i1 @axea.less.i64(i64 %a, i64 %b)") != std::string::npos);
    EXPECT_TRUE(ir.find("call i1 @axea.less.i64(") != std::string::npos);
}

TEST("LlvmIrEmitter's str `<`/`<=`/`>`/`>=` compare via a real lexicographic @axea.less.str "
     "call, not a pointer-identity icmp on i8* (see docs/language/0042-string.md)")
{
    auto ir = emitLlvmIr("f() -> bool { "
                         "  a = \"apple\"  b = \"banana\" "
                         "  lt = a < b  le = a <= b  gt = a > b  ge = a >= b "
                         "  return lt "
                         "}");
    EXPECT_TRUE(ir.find("define i1 @axea.less.str(i8* %a, i8* %b)") != std::string::npos);
    // 4 comparisons -> 4 calls (le/ge each derive from one extra `xor i1
    // ..., 1` after their own @axea.less.str call, per emitStrComparison).
    const auto firstCall = ir.find("call i1 @axea.less.str(");
    EXPECT_TRUE(firstCall != std::string::npos);
    EXPECT_TRUE(ir.find("call i1 @axea.less.str(", firstCall + 1) != std::string::npos);
    EXPECT_TRUE(ir.find("icmp slt i8*") == std::string::npos);
    EXPECT_TRUE(ir.find("icmp sle i8*") == std::string::npos);
    EXPECT_TRUE(ir.find("icmp sgt i8*") == std::string::npos);
    EXPECT_TRUE(ir.find("icmp sge i8*") == std::string::npos);
}

TEST("LlvmIrEmitter's str `==`/`!=` compare via registerKeyRuntime's own @axea.eq.str call "
     "(already built for Map<K,V>/Set<T> key comparisons), not a pointer-identity icmp on i8*")
{
    auto ir =
        emitLlvmIr("f() -> bool { a = \"x\"  b = \"y\"  eq = a == b  ne = a != b  return eq }");
    EXPECT_TRUE(ir.find("define i1 @axea.eq.str(i8* %a, i8* %b)") != std::string::npos);
    EXPECT_TRUE(ir.find("call i1 @axea.eq.str(") != std::string::npos);
    EXPECT_TRUE(ir.find("icmp eq i8*") == std::string::npos);
    EXPECT_TRUE(ir.find("icmp ne i8*") == std::string::npos);
}

TEST("LlvmIrEmitter's String `==` first resolves both operands to their own bare i8* data "
     "pointer (resolveStrPtrOfType), then compares via @axea.eq.str - not a pointer-identity "
     "icmp on the {i32,i8*}* header itself, which would compare two distinct String objects as "
     "unequal even with identical content")
{
    auto ir =
        emitLlvmIr("f() -> bool { a = String(\"hello\")  b = String(\"hello\")  return a == b }");
    EXPECT_TRUE(ir.find("call i1 @axea.eq.str(") != std::string::npos);
    EXPECT_TRUE(ir.find("icmp eq {i32, i8*}*") == std::string::npos);
}

TEST("LlvmIrEmitter prints a top-level char binding via a real UTF-8 encoder, not the numeric "
     "codepoint - exercises the malloc'd 5-byte buffer and the codepoint-range branch")
{
    auto ir = emitLlvmIr("a = 'A'");
    EXPECT_TRUE(ir.find("call i8* @malloc(i64 5)") != std::string::npos);
    EXPECT_TRUE(ir.find("char.utf8.len1") != std::string::npos);
    EXPECT_TRUE(ir.find("char.utf8.check2") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 (i8*, ...) @printf(i8* getelementptr") != std::string::npos);
}

TEST("LlvmIrEmitter prints a char struct field via the same UTF-8 encoder, not the generic "
     "nested-struct-pointer fallback that a bare i24 would otherwise be misread as")
{
    auto ir = emitLlvmIr("struct Letter { value: char } l = Letter { value: 'A' }");
    EXPECT_TRUE(ir.find("char.utf8.len1") != std::string::npos);
    // No misfired nested-struct print call for a non-existent struct.
    EXPECT_TRUE(ir.find("@axea.print.i24") == std::string::npos);
}

TEST("LlvmIrEmitter prints a struct with two char fields without a duplicate-label collision "
     "between them - each field's own UTF-8 encoder call gets uniquely numbered labels")
{
    auto ir = emitLlvmIr("struct Pair { a: char  b: char } "
                         "p = Pair { a: 'X'  b: 'Y' }");
    EXPECT_TRUE(ir.find("char.utf8.len1.0") != std::string::npos);
    EXPECT_TRUE(ir.find("char.utf8.len1.1") != std::string::npos);
}

TEST("LlvmIrEmitter prints a List<char> element via the same UTF-8 encoder, not the generic "
     "nested-struct-pointer fallback")
{
    auto ir = emitLlvmIr("xs = List<char>()  t = xs.push('A')");
    EXPECT_TRUE(ir.find("char.utf8.len1") != std::string::npos);
    EXPECT_TRUE(ir.find("@axea.print.i24") == std::string::npos);
}

TEST("LlvmIrEmitter's bounded str slice mallocs a fresh buffer and copies exactly end-start "
     "bytes via a hand-rolled loop, no phi")
{
    auto ir = emitLlvmIr("f() -> str { date = \"2026-08-18\"  return date[5..7] }");
    EXPECT_TRUE(ir.find("= sub i32") != std::string::npos);
    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("strslice.copy.header") != std::string::npos);
    EXPECT_TRUE(ir.find("strslice.copy.body") != std::string::npos);
    EXPECT_TRUE(ir.find("strslice.copy.done") != std::string::npos);
    EXPECT_TRUE(ir.find(" phi ") == std::string::npos);
}

TEST("LlvmIrEmitter's open-start str slice defaults start to the literal 0")
{
    auto ir = emitLlvmIr("f() -> str { date = \"2026-08-18\"  return date[..4] }");
    EXPECT_TRUE(ir.find("= sub i32 %") != std::string::npos);
    EXPECT_TRUE(ir.find(", 0\n") != std::string::npos);
}

TEST("LlvmIrEmitter's open-end str slice computes the missing end via a runtime @strlen call")
{
    auto ir = emitLlvmIr("f() -> str { date = \"2026-08-18\"  return date[8..] }");
    EXPECT_TRUE(ir.find("call i64 @strlen(i8*") != std::string::npos);
}

TEST("LlvmIrEmitter's str slice result is a bare i8*, not a String header - always str, "
     "regardless of whether the sliced object was itself a str or a String")
{
    auto ir = emitLlvmIr("useStr(s: str) -> str { return s } "
                         "f() -> str { s = String(\"Axea Language\")  return useStr(s[0..4]) }");
    EXPECT_TRUE(ir.find("call i8* @useStr(i8* %") != std::string::npos);
}

TEST("LlvmIrEmitter's str-slice dispatch explicitly excludes String's own header from the "
     "array/List branch's isListType check - String's {i32, i8*}* header would otherwise "
     "structurally collide with isListType's own loose \"{...}*\" test (a real bug found while "
     "adding List<T>'s own capacity field: the old 2-field listElementType parsing happened to "
     "reconstruct the exact same header text by coincidence, masking it; the new 3-field "
     "parsing legitimately garbles on a header too short to hold a capacity field, exposing it)")
{
    auto ir =
        emitLlvmIr("useStr(s: str) -> str { return s } "
                   "f() -> str { s = String(\"Axea Language\")  t = s[0..4]  return useStr(t) }");
    EXPECT_TRUE(ir.find("call i8* @useStr(i8* %") != std::string::npos);
    // Never a garbled type anywhere in the module - "}**," (a closing
    // brace directly followed by two stars and a comma) is never a
    // well-formed fragment of any real type this backend emits; it's the
    // specific tell-tale artifact of mis-parsing String's own too-short
    // header as if it had a third (capacity) field.
    EXPECT_TRUE(ir.find("}**,") == std::string::npos);
}

TEST("LlvmIrEmitter's single-character str indexing calls the shared @axea.utf8.char_at "
     "runtime function, not the generic array-element GEP - a real Unicode codepoint index, "
     "not a byte offset (see docs/language/0047-unicode.md)")
{
    auto ir = emitLlvmIr("f() -> char { s = \"hello\"  return s[0] }");
    EXPECT_TRUE(ir.find("define i24 @axea.utf8.char_at(i8* %s, i32 %index)") != std::string::npos);
    EXPECT_TRUE(ir.find("call i24 @axea.utf8.char_at(i8* ") != std::string::npos);
}

TEST("LlvmIrEmitter's String indexing first resolves to its own bare i8* data pointer "
     "(resolveStrPtrOfType) before calling @axea.utf8.char_at, the same str-coercion every "
     "other String-accepting operation already shares")
{
    auto ir = emitLlvmIr("f() -> char { s = String(\"hello\")  return s[0] }");
    EXPECT_TRUE(ir.find("call i24 @axea.utf8.char_at(i8* ") != std::string::npos);
}

TEST("LlvmIrEmitter registers @axea.utf8.char_at only once even when str indexing appears "
     "multiple times in the same program")
{
    auto ir = emitLlvmIr("f() -> char { s = \"hello\"  a = s[0]  b = s[1]  return a }");
    const auto firstDef = ir.find("define i24 @axea.utf8.char_at(");
    EXPECT_TRUE(firstDef != std::string::npos);
    EXPECT_TRUE(ir.find("define i24 @axea.utf8.char_at(", firstDef + 1) == std::string::npos);
}

TEST("LlvmIrEmitter's parse<i32>() calls a single shared @axea.parse.i32 runtime function, not "
     "inlined logic at each call site, returning Optional<i32> - see "
     "docs/language/0052-optional.md")
{
    auto ir = emitLlvmIr("f() -> Optional<i32> { return \"42\".parse<i32>() }");
    EXPECT_TRUE(ir.find("define %axea.Optional.0 @axea.parse.i32(i8* %s)") != std::string::npos);
    EXPECT_TRUE(ir.find("call %axea.Optional.0 @axea.parse.i32(i8*") != std::string::npos);
    EXPECT_TRUE(ir.find(" phi ") == std::string::npos);
}

TEST("LlvmIrEmitter's parse<bool>() calls a single shared @axea.parse.bool runtime function, "
     "returning Optional<bool> ({i1, i1})")
{
    auto ir = emitLlvmIr("f() -> Optional<bool> { return \"true\".parse<bool>() }");
    EXPECT_TRUE(ir.find("define %axea.Optional.0 @axea.parse.bool(i8* %s)") != std::string::npos);
    EXPECT_TRUE(ir.find("call %axea.Optional.0 @axea.parse.bool(i8*") != std::string::npos);
}

TEST("LlvmIrEmitter registers @axea.parse.i32 only once even when parse<i32>() is called "
     "multiple times in the same program")
{
    auto ir = emitLlvmIr("f() -> i32 { "
                         "  a = \"1\".parse<i32>() "
                         "  b = \"2\".parse<i32>() "
                         "  return a.unwrap_or(0) + b.unwrap_or(0) "
                         "}");
    const auto first = ir.find("define %axea.Optional.0 @axea.parse.i32");
    EXPECT_TRUE(first != std::string::npos);
    const auto second = ir.find("define %axea.Optional.0 @axea.parse.i32", first + 1);
    EXPECT_TRUE(second == std::string::npos);
}

TEST("LlvmIrEmitter's parse<i32> resolves a String argument to a bare i8* first, the same "
     "str-coercion resolveStrPtr already shares with String.append/Buffer.append")
{
    auto ir = emitLlvmIr("f() -> Optional<i32> { s = String(\"42\")  return s.parse<i32>() }");
    EXPECT_TRUE(ir.find("call %axea.Optional.0 @axea.parse.i32(i8*") != std::string::npos);
}

TEST("LlvmIrEmitter's parse<i64>() calls a single shared @axea.parse.i64 runtime function, "
     "the identical digit loop parse<i32>() uses just at 64-bit width (see "
     "docs/language/0051-numeric-widening.md)")
{
    auto ir = emitLlvmIr("f() -> Optional<i64> { return \"123456789012\".parse<i64>() }");
    EXPECT_TRUE(ir.find("define %axea.Optional.0 @axea.parse.i64(i8* %s)") != std::string::npos);
    EXPECT_TRUE(ir.find("call %axea.Optional.0 @axea.parse.i64(i8*") != std::string::npos);
    EXPECT_TRUE(ir.find(" phi ") == std::string::npos);
}

TEST("LlvmIrEmitter's parse<f64>() declares and calls a real libc @strtod, not a hand-rolled "
     "decimal-to-binary float parser - endptr now drives real success detection (see "
     "docs/language/0052-optional.md)")
{
    auto ir = emitLlvmIr("f() -> Optional<f64> { return \"3.14\".parse<f64>() }");
    EXPECT_TRUE(ir.find("declare double @strtod(i8*, i8**)") != std::string::npos);
    EXPECT_TRUE(ir.find("define %axea.Optional.0 @axea.parse.f64(i8* %s)") != std::string::npos);
    EXPECT_TRUE(ir.find("call double @strtod(i8* %s, i8** %endptrSlot)") != std::string::npos);
    EXPECT_TRUE(ir.find("call %axea.Optional.0 @axea.parse.f64(i8*") != std::string::npos);
}

TEST("LlvmIrEmitter declares a real LLVM `declare` (not `define`) for an extern c function, "
     "with FFI-safe param/return types translated the same way ordinary functions are")
{
    auto ir = emitLlvmIr("extern c abs(x: i32) -> i32 "
                         "f() -> i32 { return abs(5) }");
    EXPECT_TRUE(ir.find("declare i32 @abs(i32)") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 @abs(i32") != std::string::npos);
    // No `define i32 @abs` anywhere - it's never lowered as a real Axea
    // function body, only registered.
    EXPECT_TRUE(ir.find("define i32 @abs") == std::string::npos);
}

TEST("LlvmIrEmitter declares an extern c function with no return type as void, and cstr as a "
     "bare i8* - the exact same representation str already has")
{
    auto ir = emitLlvmIr("extern c puts(text: cstr) "
                         "f() { s = \"hi\"  called = puts(s.to_cstr()) }");
    EXPECT_TRUE(ir.find("declare void @puts(i8*)") != std::string::npos);
    EXPECT_TRUE(ir.find("call void @puts(i8*") != std::string::npos);
}

TEST("LlvmIrEmitter's .to_cstr() resolves its object to a bare i8* via resolveStrPtr, the same "
     "str-coercion shared with String.append/Buffer.append/.parse<T>(), emitted as a real "
     "(if representationally redundant) bitcast instruction")
{
    auto ir = emitLlvmIr("s = String(\"hi\")  x = s.to_cstr()");
    EXPECT_TRUE(ir.find("= bitcast i8*") != std::string::npos);
}

TEST("LlvmIrEmitter passes a String argument to an extern cstr parameter via .to_cstr(), and a "
     "bare str argument to a cstr parameter is rejected at the type-checking layer (verified "
     "separately) - here just confirming the accepted path emits correctly")
{
    auto ir = emitLlvmIr("extern c puts(text: cstr) "
                         "s = String(\"hi\") "
                         "called = puts(s.to_cstr())");
    EXPECT_TRUE(ir.find("declare void @puts(i8*)") != std::string::npos);
    EXPECT_TRUE(ir.find("call void @puts(i8*") != std::string::npos);
}

TEST("LlvmIrEmitter's print(...) calls the struct's own @axea.print.<Name> helper directly for "
     "a struct argument, instead of routing through stringifyValue (see "
     "docs/language/0049-printing-formatting.md's own follow-up)")
{
    auto ir = emitLlvmIr("struct Point { x: i32  y: i32 } "
                         "f() { p = Point { x: 1, y: 2 }  print(p) }");
    EXPECT_TRUE(ir.find("call void @axea.print.Point(%Point*") != std::string::npos);
}

TEST("LlvmIrEmitter registers a real @axea.tostring.<Name> stringifier for every struct, "
     "unconditionally, even one never passed to print/write/interpolation directly - mirrors "
     "emitStructPrintHelpers' own identical unconditional registration (see "
     "docs/language/0054-collection-printing.md)")
{
    auto ir = emitLlvmIr("struct Point { x: i32  y: i32 } "
                         "f() { p = Point { x: 1, y: 2 } }");
    EXPECT_TRUE(ir.find("define i8* @axea.tostring.Point(%Point* %v)") != std::string::npos);
}

TEST("LlvmIrEmitter's print(...) of an Array<i32> routes through "
     "registerCollectionToStrRuntime, not @axea.print.<Name> (arrays have no named struct "
     "type to call) - see docs/language/0054-collection-printing.md")
{
    auto ir = emitLlvmIr("f() { arr = [1, 2, 3]  print(arr) }");
    EXPECT_TRUE(ir.find("define i8* @axea.tostring.collection.0(") != std::string::npos);
    EXPECT_TRUE(ir.find("call i8* @axea.tostring.collection.0(") != std::string::npos);
}

TEST("LlvmIrEmitter's collection stringifier for Map<K,V> is count-only, matching the "
     "top-level binding printer's own identical fallback (no iteration support this phase)")
{
    auto ir = emitLlvmIr("f() { m: Map<i32,i32> = Map<i32,i32>()  print(m) }");
    EXPECT_TRUE(ir.find("@axea.str.map_open") != std::string::npos);
    EXPECT_TRUE(ir.find("@axea.i32.to_str") != std::string::npos);
}

TEST("LlvmIrEmitter's collection stringifier for a nested List<List<i32>> recurses correctly "
     "- the outer stringifier (registered first, from print(outer) itself) calls the inner "
     "one (registered second, while building the outer's own body) by name")
{
    auto ir = emitLlvmIr("f() { "
                         "  inner: List<i32> = List<i32>() "
                         "  outer: List<List<i32>> = List<List<i32>>() "
                         "  pushed = outer.push(inner) "
                         "  print(outer) "
                         "}");
    const auto outerFn = ir.find("define i8* @axea.tostring.collection.0(");
    const auto innerFn = ir.find("define i8* @axea.tostring.collection.1(");
    EXPECT_TRUE(outerFn != std::string::npos);
    EXPECT_TRUE(innerFn != std::string::npos);
    EXPECT_TRUE(ir.find("call i8* @axea.tostring.collection.1(", outerFn) != std::string::npos);
}

TEST("LlvmIrEmitter's print(...) calls printf(\"%s\", ...) once per argument, space-separated, "
     "with a trailing newline printf - see docs/language/Axea_Printing_Formatting.md")
{
    auto ir = emitLlvmIr("f() { print(\"hi\", \"there\") }");
    EXPECT_TRUE(ir.find("@axea.fmt.s") != std::string::npos);
    EXPECT_TRUE(ir.find("@axea.fmt.space") != std::string::npos);
    EXPECT_TRUE(ir.find("@axea.fmt.nl") != std::string::npos);
    const auto firstPrintf = ir.find("call i32 (i8*, ...) @printf");
    EXPECT_TRUE(firstPrintf != std::string::npos);
}

TEST("LlvmIrEmitter's write(...) calls printf per argument with no trailing newline printf call, "
     "even though the shared runtime still registers the @axea.fmt.nl global")
{
    auto ir = emitLlvmIr("f() { write(\"hi\") }");
    EXPECT_TRUE(ir.find("@axea.fmt.s") != std::string::npos);
    // The format global is registered (shared with print()), but write()
    // itself never emits a printf call against it.
    EXPECT_TRUE(ir.find("@printf(i8* getelementptr ([2 x i8], [2 x i8]* @axea.fmt.nl") ==
                std::string::npos);
}

TEST("LlvmIrEmitter emits a bare top-level print(...)/write(...) call into @main itself, not "
     "silently dropped, via IrGenerator::generate's own new ExprStmt case in its top-level "
     "item loop (see docs/language/0049-printing-formatting.md's own Parsing follow-up)")
{
    auto ir = emitLlvmIr("write(\"a\") print(\"b\")");
    const auto mainStart = ir.find("define i32 @main()");
    EXPECT_TRUE(mainStart != std::string::npos);
    const auto firstPrintf = ir.find("call i32 (i8*, ...) @printf", mainStart);
    EXPECT_TRUE(firstPrintf != std::string::npos);
}

TEST("LlvmIrEmitter registers the print/write runtime format globals only once across multiple "
     "print/write calls in the same program")
{
    auto ir = emitLlvmIr("f() { print(\"a\") write(\"b\") print(\"c\") }");
    const auto first = ir.find("@axea.fmt.s =");
    EXPECT_TRUE(first != std::string::npos);
    const auto second = ir.find("@axea.fmt.s =", first + 1);
    EXPECT_TRUE(second == std::string::npos);
}

TEST("LlvmIrEmitter stringifies an i32 print argument via a real sprintf call, declaring "
     "@sprintf and defining @axea.i32.to_str")
{
    auto ir = emitLlvmIr("f() { n = 42  print(n) }");
    EXPECT_TRUE(ir.find("declare i32 @sprintf(i8*, i8*, ...)") != std::string::npos);
    EXPECT_TRUE(ir.find("define i8* @axea.i32.to_str(i32 %v)") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 (i8*, i8*, ...) @sprintf") != std::string::npos);
}

TEST("LlvmIrEmitter stringifies a bool print argument via a hand-rolled @axea.bool.to_str, no "
     "global string constant needed")
{
    auto ir = emitLlvmIr("f() { b = true  print(b) }");
    EXPECT_TRUE(ir.find("define i8* @axea.bool.to_str(i1 %v)") != std::string::npos);
}

TEST("LlvmIrEmitter registers @axea.i32.to_str/@axea.bool.to_str only once even across multiple "
     "print calls")
{
    auto ir = emitLlvmIr("f() { a = 1  b = 2  print(a)  print(b) }");
    const auto first = ir.find("define i8* @axea.i32.to_str");
    EXPECT_TRUE(first != std::string::npos);
    const auto second = ir.find("define i8* @axea.i32.to_str", first + 1);
    EXPECT_TRUE(second == std::string::npos);
}

TEST("LlvmIrEmitter lowers an interpolated string literal into Buffer new/append/appendValue/"
     "finish instructions, reusing the existing Buffer runtime machinery")
{
    auto ir = emitLlvmIr("f() -> String { name = \"Ada\"  return \"hi {name}\" }");
    // Buffer.finish's real signature: mallocs a fresh String header.
    EXPECT_TRUE(ir.find("@malloc") != std::string::npos);
    EXPECT_TRUE(ir.find("@axea.i32.to_str") == std::string::npos); // no i32 piece here
}

TEST("LlvmIrEmitter's interpolation of an i32 piece calls the same @axea.i32.to_str runtime "
     "print(...) itself uses")
{
    auto ir = emitLlvmIr("f() -> String { age = 30  return \"age: {age}\" }");
    EXPECT_TRUE(ir.find("define i8* @axea.i32.to_str(i32 %v)") != std::string::npos);
}

TEST("LlvmIrEmitter hoists a string literal into a module-level global constant")
{
    auto ir = emitLlvmIr("greeting(name: str) -> str { return \"hello\" }");
    EXPECT_TRUE(ir.find("@.str.0 = private unnamed_addr constant [6 x i8] c\"hello\\00\"") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("getelementptr [6 x i8], [6 x i8]* @.str.0") != std::string::npos);
}

TEST("LlvmIrEmitter does not double-terminate the merge block when both branches return")
{
    // Regression coverage: generateFunction must not append a second
    // terminator after a Branch that's already fully covered by explicit
    // returns on both sides - the merge block's only instruction must be
    // `unreachable`, with no trailing `ret` after it (which would be
    // invalid LLVM IR - two terminators in one block).
    auto ir = emitLlvmIr("sign(x: i32) -> i32 { if x < 0 { return 0 - 1 } else { return 1 } }");
    const std::size_t unreachablePos = ir.find("  unreachable\n");
    EXPECT_TRUE(unreachablePos != std::string::npos);
    // The function must close right after `unreachable` - no further
    // instruction (in particular no synthetic trailing `ret`) in between.
    const std::size_t afterUnreachable = unreachablePos + std::string("  unreachable\n").size();
    EXPECT_TRUE(ir.compare(afterUnreachable, 1, "}") == 0);
}

TEST("LlvmIrEmitter emits a main that returns i32 and prints an i32 top-level binding")
{
    auto ir = emitLlvmIr("x = 1 + 2");
    EXPECT_TRUE(ir.find("define i32 @main() {") != std::string::npos);
    EXPECT_TRUE(ir.find("ret i32 0") != std::string::npos);
    EXPECT_TRUE(ir.find("@printf") != std::string::npos);
    EXPECT_TRUE(ir.find("c\"x\\00\"") != std::string::npos); // the binding's own name, hoisted
    EXPECT_TRUE(ir.find("c\"%s = %d\\0A\\00\"") != std::string::npos); // i32 binding format
}

TEST("LlvmIrEmitter prints multiple top-level bindings in source order")
{
    auto ir = emitLlvmIr("a = 1  b = 2  c = 3");
    const std::size_t aPos = ir.find("c\"a\\00\"");
    const std::size_t bPos = ir.find("c\"b\\00\"");
    const std::size_t cPos = ir.find("c\"c\\00\"");
    EXPECT_TRUE(aPos != std::string::npos && bPos != std::string::npos &&
                cPos != std::string::npos);
    EXPECT_TRUE(aPos < bPos);
    EXPECT_TRUE(bPos < cPos);
}

TEST("LlvmIrEmitter every printf call captures its (discarded) result into a numbered register")
{
    // Regression coverage: a discarded-result call to a non-void function
    // (printf returns i32) still implicitly consumes a numbered SSA slot in
    // LLVM IR - an unnamed "call i32 (...) @printf(...)" with no "%N ="
    // prefix would desynchronize every later explicit register number in
    // the same function, which the real LLVM parser (clang/llc) rejects
    // outright even though this project's own hand-review missed it.
    auto ir = emitLlvmIr("struct Point { x: i32  y: i32 }  p = Point { x: 1  y: 2 }");
    std::size_t pos = 0;
    while ((pos = ir.find("call i32 (i8*, ...) @printf(", pos)) != std::string::npos)
    {
        const std::size_t lineStart = ir.rfind('\n', pos) + 1;
        EXPECT_TRUE(ir.compare(lineStart, 3, "  %") == 0);
        pos += 1;
    }
}

TEST("LlvmIrEmitter prints a struct top-level binding via its type's print helper")
{
    auto ir = emitLlvmIr("struct Point { x: i32  y: i32 }  p = Point { x: 1  y: 2 }");
    EXPECT_TRUE(ir.find("define void @axea.print.Point(%Point* %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("call void @axea.print.Point(%Point* ") != std::string::npos);
    EXPECT_TRUE(ir.find("c\"Point { \\00\"") != std::string::npos);
    EXPECT_TRUE(ir.find("c\"x: \\00\"") != std::string::npos);
}

TEST("LlvmIrEmitter round-trips a recursive self-call with no forward declaration needed")
{
    auto ir = emitLlvmIr("factorial(n: i32) -> i32 { "
                         "  if n <= 1 { return 1 } "
                         "  return n * factorial(n - 1) "
                         "}");
    EXPECT_TRUE(ir.find("define i32 @factorial(i32 %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 @factorial(i32") != std::string::npos);
}

TEST("LlvmIrEmitter lowers a while loop's carried variable via alloca/load/store")
{
    auto ir = emitLlvmIr("f(limit: i32) -> i32 { "
                         "  n = 0 "
                         "  while n < limit { n = n + 1 } "
                         "  return n "
                         "}");
    EXPECT_TRUE(ir.find("= alloca i32") != std::string::npos);
    EXPECT_TRUE(ir.find("loop.header0:") != std::string::npos);
    EXPECT_TRUE(ir.find("loop.body0:") != std::string::npos);
    EXPECT_TRUE(ir.find("loop.exit0:") != std::string::npos);
    // A while loop's own dest is never consumed - no phi for the loop itself.
    EXPECT_TRUE(ir.find("= phi") == std::string::npos);
}

TEST("LlvmIrEmitter marks an infinite loop with no break as unreachable at exit")
{
    auto ir = emitLlvmIr("f() { loop { } }");
    EXPECT_TRUE(ir.find("loop.exit0:\n  unreachable") != std::string::npos);
}

TEST("LlvmIrEmitter builds an exit-block phi from a loop's break values")
{
    auto ir = emitLlvmIr("f(flag: bool) -> i32 { "
                         "  return loop { "
                         "    if flag { break 1 } else { break 2 } "
                         "  } "
                         "}");
    EXPECT_TRUE(ir.find("loop.exit0:") != std::string::npos);
    EXPECT_TRUE(ir.find("= phi i32") != std::string::npos);
    EXPECT_TRUE(ir.find("[ %1, %if.then") != std::string::npos ||
                ir.find(", %if.then") != std::string::npos);
}

TEST("LlvmIrEmitter does not double-terminate a block when both loop branches break")
{
    // Regression coverage: alwaysTerminates must recognize IrBreak/IrContinue
    // as terminators, matching how emitInstructions already treats them -
    // otherwise emitLoop's "did the body fall through naturally" fallback
    // incorrectly fires and appends a second terminator after the
    // already-`unreachable` merge block from a both-sides-break IrBranch.
    auto ir = emitLlvmIr("f(flag: bool) -> i32 { "
                         "  return loop { "
                         "    if flag { break 1 } else { break 2 } "
                         "  } "
                         "}");
    const std::size_t unreachablePos = ir.find("  unreachable\n");
    EXPECT_TRUE(unreachablePos != std::string::npos);
    const std::size_t afterUnreachable = unreachablePos + std::string("  unreachable\n").size();
    // Nothing but a new label (or the closing brace) may follow.
    EXPECT_TRUE(ir[afterUnreachable] == 'l' || ir[afterUnreachable] == '}');
}

TEST("LlvmIrEmitter continue re-checks the loop header instead of falling through")
{
    auto ir = emitLlvmIr("f() { "
                         "  n = 0 "
                         "  while n < 10 { "
                         "    n = n + 1 "
                         "    if n == 3 { continue } "
                         "    n = n + 100 "
                         "  } "
                         "}");
    EXPECT_TRUE(ir.find("br label %loop.header0") != std::string::npos);
}

TEST("LlvmIrEmitter slices a fixed-size array element-wise into a fresh List<T> header, via the "
     "same arrslice.copy loop shape emitStrSlice's own str branch uses - see "
     "docs/language/0050-collection-join-and-slicing.md")
{
    auto ir = emitLlvmIr("f() -> List<i32> { numbers = [1, 2, 3, 4] return numbers[..2] }");
    EXPECT_TRUE(ir.find("br label %arrslice.copy.header0") != std::string::npos);
    EXPECT_TRUE(ir.find("getelementptr {i32, i32*, i32}, {i32, i32*, i32}* null, i32 1") !=
                std::string::npos);
}

TEST("LlvmIrEmitter slices a List<T> into another fresh List<T>, reading the source's own "
     "runtime length field rather than a compile-time size")
{
    auto ir = emitLlvmIr("f() -> List<i32> { numbers = List<i32>() a = numbers.push(1) return "
                         "numbers[..] }");
    EXPECT_TRUE(ir.find("br label %arrslice.copy.header") != std::string::npos);
}

TEST("LlvmIrEmitter's .join(separator) on an Array<i32> stringifies each element via the same "
     "@axea.i32.to_str runtime print()/interpolation already share, appended through its own "
     "join.append.copy loop")
{
    auto ir = emitLlvmIr("f() -> String { numbers = [1, 2, 3] return numbers.join(\",\") }");
    EXPECT_TRUE(ir.find("define i8* @axea.i32.to_str(i32 %v)") != std::string::npos);
    EXPECT_TRUE(ir.find("br i1 %") != std::string::npos &&
                ir.find("join.nonempty0") != std::string::npos);
    EXPECT_TRUE(ir.find("join.loop.header0") != std::string::npos);
    EXPECT_TRUE(ir.find("join.append.copy.header") != std::string::npos);
    EXPECT_TRUE(ir.find("join.done0") != std::string::npos);
}

TEST("LlvmIrEmitter's .join() result is a fresh {i32, i8*}* String header, same representation "
     "interpolation's own OwnedString result uses")
{
    auto ir = emitLlvmIr("f() -> String { numbers = [1, 2, 3] return numbers.join(\",\") }");
    EXPECT_TRUE(ir.find("ret {i32, i8*}*") != std::string::npos);
}
