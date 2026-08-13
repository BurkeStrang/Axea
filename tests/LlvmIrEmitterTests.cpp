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

TEST("LlvmIrEmitter round-trips a recursive self-call with no forward declaration needed")
{
    auto ir = emitLlvmIr("factorial(n: i32) -> i32 { "
                         "  if n <= 1 { return 1 } "
                         "  return n * factorial(n - 1) "
                         "}");
    EXPECT_TRUE(ir.find("define i32 @factorial(i32 %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 @factorial(i32") != std::string::npos);
}
