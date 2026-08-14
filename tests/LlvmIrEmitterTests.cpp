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

        // Skip the hand-authored Map/Set hash-table runtime
        // (emitMapSetRuntime, docs/language/0034-maps-and-sets.md): unlike
        // every function above, generated via allocateRegister's dynamic
        // numbering (the exact mechanism this test regression-checks),
        // these are fixed, hand-verified LLVM text using named registers
        // (%h, %key, %cptr, ...) - never subject to the numbering bug this
        // test guards against.
        const std::size_t atPos = ir.find('@', definePos);
        const std::string calleeName = ir.substr(atPos + 1, openParen - atPos - 1);
        if (calleeName.starts_with("axea.hash.") || calleeName.starts_with("axea.map.") ||
            calleeName.starts_with("axea.set."))
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

TEST("LlvmIrEmitter represents List<T> as a pointer to an anonymous {length, data} heap record")
{
    auto ir = emitLlvmIr("sum(numbers: List<i32>) -> i32 { return numbers[0] }");
    EXPECT_TRUE(ir.find("define i32 @sum({i32, i32*}* %0) {") != std::string::npos);
}

TEST("LlvmIrEmitter's List<T>() construction mallocs a header and zero-initializes it")
{
    auto ir = emitLlvmIr("f() -> i32 { numbers = List<i32>()  return numbers.length }");
    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32 0, i32*") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32* null, i32**") != std::string::npos);
}

TEST("LlvmIrEmitter's push grows via malloc plus a hand-rolled copy loop, no phi")
{
    auto ir = emitLlvmIr("f() { numbers = List<i32>()  numbers.push(4) }");
    // Two mallocs: one for the fresh empty list, one for push's grown buffer.
    const auto firstMalloc = ir.find("call i8* @malloc(i64");
    EXPECT_TRUE(firstMalloc != std::string::npos);
    EXPECT_TRUE(ir.find("call i8* @malloc(i64", firstMalloc + 1) != std::string::npos);
    EXPECT_TRUE(ir.find("list.push.copy.header") != std::string::npos);
    EXPECT_TRUE(ir.find("list.push.copy.body") != std::string::npos);
    EXPECT_TRUE(ir.find("list.push.copy.done") != std::string::npos);
    // Loop-carried state here uses alloca/load/store, not a phi node (see
    // docs/language/0033-lists.md - unnamed sequential registers here can't
    // forward-reference a not-yet-emitted value the way a phi would need).
    EXPECT_TRUE(ir.find(" phi ") == std::string::npos);
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
    // Field 1 (data pointer) of the header is loaded first...
    EXPECT_TRUE(ir.find("getelementptr {i32, i32*}, {i32, i32*}* %0, i32 0, i32 1") !=
                std::string::npos);
    // ...then a flat single-index GEP into it (same idiom slice indexing uses).
    EXPECT_TRUE(ir.find("getelementptr i32, i32* %") != std::string::npos);
}

TEST("LlvmIrEmitter reads a List's .length via GEP+load, not a compile-time constant")
{
    auto ir = emitLlvmIr("len(numbers: List<i32>) -> i32 { return numbers.length }");
    EXPECT_TRUE(ir.find("getelementptr {i32, i32*}, {i32, i32*}* %0, i32 0, i32 0") !=
                std::string::npos);
}

TEST("LlvmIrEmitter represents Stack<T> as the exact same LLVM type as List<T>")
{
    auto ir = emitLlvmIr("useStack(s: Stack<i32>) -> i32 { return s.length }");
    EXPECT_TRUE(ir.find("define i32 @useStack({i32, i32*}* %0) {") != std::string::npos);
}

TEST("LlvmIrEmitter's Stack<T>() construction mallocs a header and zero-initializes it")
{
    auto ir = emitLlvmIr("f() -> i32 { s = Stack<i32>()  return s.length }");
    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32 0, i32*") != std::string::npos);
    EXPECT_TRUE(ir.find("store i32* null, i32**") != std::string::npos);
}

TEST("LlvmIrEmitter's Stack push grows via malloc plus a hand-rolled copy loop, no phi")
{
    auto ir = emitLlvmIr("f() { s = Stack<i32>()  s.push(4) }");
    EXPECT_TRUE(ir.find("stack.push.copy.header") != std::string::npos);
    EXPECT_TRUE(ir.find("stack.push.copy.body") != std::string::npos);
    EXPECT_TRUE(ir.find("stack.push.copy.done") != std::string::npos);
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
    EXPECT_TRUE(ir.find("getelementptr {i32, i32*}, {i32, i32*}* %0, i32 0, i32 0") !=
                std::string::npos);
}

TEST("LlvmIrEmitter List<T> and Stack<T> push/pop resolve to distinct emit functions producing "
     "the same underlying shape")
{
    auto ir = emitLlvmIr("useList(l: List<i32>) { l.push(1) } "
                         "useStack(s: Stack<i32>) { s.push(1) }");
    EXPECT_TRUE(ir.find("list.push.copy.header") != std::string::npos);
    EXPECT_TRUE(ir.find("stack.push.copy.header") != std::string::npos);
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
