#include "TestFramework.hpp"

#include "generics/GenericMonomorphizer.hpp"
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
        monomorphizeGenerics(program);

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
            calleeName.starts_with("axea.bool.") || calleeName.starts_with("axea.retain.") ||
            calleeName.starts_with("axea.drop."))
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

// List<T>/Stack<T> are real, user-declared generic structs now, not compiler intrinsics (see
// docs/language/0006-generics.md's own List<T>/Stack<T> port follow-up and std/collections.ax) -
// their own internal LLVM representation is no longer this compiler's concern to unit-test; their
// behavior is verified via examples/list.ax and examples/stack.ax instead (both interpreted and
// compiled).

// LinkedList<T> is a real, user-declared generic struct now, not a compiler intrinsic (see
// docs/language/0036-linked-lists.md's own "2026 Update" - this needed real `null` pointer
// support as a prerequisite, see docs/language/0019-unsafe.md) - its own internal LLVM
// representation (a self-referential `*Node<T>` pointer pair, ordinary Axea `unsafe`/struct-field
// source instead of a hand-rolled node type and hand-emitted runtime functions) is no longer this
// compiler's concern to unit-test; its behavior is verified via examples/linked_list.ax instead
// (both interpreted and compiled).

// Deque<T>/Queue<T> are real, user-declared generic structs now, not compiler intrinsics (see
// docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T>/Queue<T> port follow-up and
// std/collections.ax) - their own internal LLVM representation is no longer this compiler's
// concern to unit-test; their behavior is verified via examples/deque.ax and examples/queue.ax
// instead (both interpreted and compiled).

// PriorityQueue<T> is a real, user-declared generic struct now, not a compiler intrinsic (see
// docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T>/Queue<T>/PriorityQueue<T> port
// follow-up and std/collections.ax) - its own internal LLVM representation (including its
// hand-rolled sift-up/sift-down loops, now ordinary Axea `loop`/`if` source instead of hand-
// emitted LLVM basic blocks) is no longer this compiler's concern to unit-test; its behavior is
// verified via examples/priority_queue.ax instead (both interpreted and compiled).

// Map<K,V>/Set<T> are real, user-declared generic structs now, not compiler intrinsics (see
// docs/language/0034-maps-and-sets.md's own "2026 Update") - their own internal LLVM
// representation (a self-referential `*MapEntry<K,V>`/`*SetEntry<T>` pointer, `**Entry<K,V>`
// bucket array, ordinary Axea `unsafe`/struct-field source instead of a hand-rolled entry type
// and hand-emitted runtime functions) is no longer this compiler's concern to unit-test; their
// behavior is verified via examples/map_set.ax instead (both interpreted and compiled).
// `hash<T>()`/`keyEq<T>()` (below) are the new generic-code-facing entry point into
// `registerKeyRuntime`'s own per-type hash/equality generation, which *is* still this compiler's
// concern to unit-test directly - it's a real, standalone intrinsic now, not something only
// reachable through Map/Set's own former internal dispatch.

TEST("LlvmIrEmitter's hash<T>()/keyEq<T>() generate a byte-walk hash/equality pair for str keys")
{
    auto ir = emitLlvmIr("f() -> i32 { return hash<str>(\"a\") } "
                         "g() -> bool { return keyEq<str>(\"a\", \"b\") }");
    EXPECT_TRUE(ir.find("define i32 @axea.hash.str(i8* %s) {") != std::string::npos);
    EXPECT_TRUE(ir.find("define i1 @axea.eq.str(i8* %a, i8* %b) {") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 @axea.hash.str(") != std::string::npos);
    EXPECT_TRUE(ir.find("call i1 @axea.eq.str(") != std::string::npos);
}

TEST("LlvmIrEmitter's hash<T>()/keyEq<T>() generate a recursive derive-hash/equality pair for a "
     "struct key")
{
    auto ir = emitLlvmIr("struct Point { x: i32  y: i32 } "
                         "f() -> i32 { p = Point { x: 1, y: 2 }  return hash<Point>(p) }");
    EXPECT_TRUE(ir.find("define i32 @axea.hash.Point(%Point* %v) {") != std::string::npos);
    EXPECT_TRUE(ir.find("define i1 @axea.eq.Point(%Point* %a, %Point* %b) {") != std::string::npos);
    // Combines each field's own i32 hash (djb2-style: acc = acc*31 + fieldHash).
    EXPECT_TRUE(ir.find("call i32 @axea.hash.i32(") != std::string::npos);
    EXPECT_TRUE(ir.find("mul i32") != std::string::npos);
}

TEST("LlvmIrEmitter's hash<T>()/keyEq<T>() generate an unrolled hash/equality pair for a "
     "fixed-array key")
{
    auto ir = emitLlvmIr("f() -> i32 { a = [1, 2, 3]  return hash<[i32;3]>(a) }");
    EXPECT_TRUE(ir.find("define i32 @axea.hash.arr.0([3 x i32]* %v) {") != std::string::npos);
    EXPECT_TRUE(ir.find("define i1 @axea.eq.arr.0([3 x i32]* %a, [3 x i32]* %b) {") !=
                std::string::npos);
}

// List<T> as a hash<T>()/keyEq<T>() key is no longer supported - it's a real, user-declared
// generic struct now (see docs/language/0006-generics.md's own List<T> port follow-up and
// std/collections.ax), and hashing/equality for it would need a real user-level implementation,
// not the retired intrinsic's own runtime-loop hash/equality pair.

// SortedMap<K,V> is a real, user-declared generic struct now (see
// docs/language/0040-sorted-maps.md's own "2026 Update") - there is no internal-codegen-shape
// test left here at all anymore (mirrors Map<K,V>/Set<T>'s own identical, already-complete
// removal from this file): its own monomorphized node type/set/get/contains/remove functions are
// ordinary real Axea source in std/collections.ax now, compiled the same way any other generic
// struct's own impl methods already are - already covered by this file's own generic
// struct/generic method codegen tests elsewhere, with nothing SortedMap-specific left to assert.

// SortedSet<T> is a real, user-declared generic struct now (see
// docs/language/0041-sorted-sets.md's own "2026 Update") - there is no internal-codegen-shape
// test left here at all anymore (mirrors SortedMap<K,V>/Map<K,V>/Set<T>'s own identical,
// already-complete removal from this file): its own monomorphized node type/add/contains/remove
// functions are ordinary real Axea source in std/collections.ax now, compiled the same way any
// other generic struct's own impl methods already are - already covered by this file's own
// generic struct/generic method codegen tests elsewhere, with nothing SortedSet-specific left to
// assert. This is the last of these internal-codegen-shape tests: every collection
// docs/language/0029-collections.md originally scoped as a compiler intrinsic is now real Axea
// source.

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

TEST("LlvmIrEmitter's Buffer.write lowers identically to Buffer.append - same grow branch, same "
     "copy-loop label, since 'write' is a plain alias (see docs/language/0061-buffer-write.md)")
{
    auto appendIr = emitLlvmIr("f() { b = Buffer()  b.append(\"hi\") }");
    auto writeIr = emitLlvmIr("f() { b = Buffer()  b.write(\"hi\") }");
    EXPECT_TRUE(writeIr.find("buffer.grow") != std::string::npos);
    EXPECT_TRUE(writeIr.find("buffer.append.copy.header") != std::string::npos);
    EXPECT_EQ(appendIr, writeIr);
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

TEST("LlvmIrEmitter's @axea.tostring.<Name> calls the user's own compiled 'format' function "
     "into a fresh @axea.strbuf instead of building the default field-by-field text, when a "
     "Display impl is registered for that struct (see docs/language/0062-display-trait.md)")
{
    auto ir = emitLlvmIr("struct Point { x: i32  y: i32 } "
                         "impl Display for Point { "
                         "  format(self, buf: Buffer) { buf.write(\"hi\") } "
                         "} "
                         "f(p: Point) -> String { return \"{p}\" }");
    EXPECT_TRUE(ir.find("define i8* @axea.tostring.Point(%Point* %v) {") != std::string::npos);
    EXPECT_TRUE(ir.find("call {i32, i32, i8*}* @axea.strbuf.new()") != std::string::npos);
    EXPECT_TRUE(ir.find("call void @Point.format(%Point* %v, {i32, i32, i8*}* %buf)") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("call i8* @axea.strbuf.finish({i32, i32, i8*}* %buf)") !=
                std::string::npos);
    // The default field-by-field printer's own struct-name-prefix global
    // ("Point { ") is never generated when Display dispatch takes over.
    EXPECT_TRUE(ir.find("c\"Point { \\00\"") == std::string::npos);
}

TEST("LlvmIrEmitter's @axea.print.<Name> - the shared direct-print path for a top-level "
     "binding, a bare print()/write() struct argument, and any nested struct field - also "
     "dispatches to the user's 'format' function when a Display impl is registered")
{
    auto ir = emitLlvmIr("struct Point { x: i32  y: i32 } "
                         "impl Display for Point { "
                         "  format(self, buf: Buffer) { buf.write(\"hi\") } "
                         "} "
                         "p = Point { x: 1, y: 2 }");
    EXPECT_TRUE(ir.find("define void @axea.print.Point(%Point* %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("call void @Point.format(%Point* %0, {i32, i32, i8*}* %1)") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("%2 = call i8* @axea.strbuf.finish({i32, i32, i8*}* %1)") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("%3 = call i32 (i8*, ...) @printf(i8*") != std::string::npos);
}

TEST("LlvmIrEmitter compiles impl Display's own 'format' method as an entirely ordinary "
     "function, self and buf both real parameters with no special calling convention")
{
    auto ir = emitLlvmIr("struct Point { x: i32  y: i32 } "
                         "impl Display for Point { "
                         "  format(self, buf: Buffer) { buf.write(\"({self.x}, {self.y})\") } "
                         "} "
                         "p = Point { x: 1, y: 2 }");
    EXPECT_TRUE(ir.find("define void @Point.format(%Point* %0, {i32, i32, i8*}* %1) {") !=
                std::string::npos);
}

TEST("LlvmIrEmitter's struct-to-string dispatch is per-struct-type - a struct with no impl "
     "Display in the same program still gets the default field-by-field printer")
{
    auto ir = emitLlvmIr("struct Point { x: i32  y: i32 } "
                         "struct Other { n: i32 } "
                         "impl Display for Point { "
                         "  format(self, buf: Buffer) { buf.write(\"hi\") } "
                         "} "
                         "f(o: Other) -> String { return \"{o}\" }");
    EXPECT_TRUE(ir.find("define i8* @axea.tostring.Other(%Other* %v) {") != std::string::npos);
    EXPECT_TRUE(ir.find("c\"Other { \\00\"") != std::string::npos);
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

TEST("LlvmIrEmitter prints an i64/f64 struct field via the same stringifyValueOfType path top-"
     "level bindings use, not the generic byte-print loop or a misread as a nested struct "
     "pointer")
{
    auto ir = emitLlvmIr("struct Point { x: i64 y: f64 } p = Point { x: 100i64, y: 1.5 }");
    EXPECT_TRUE(ir.find("call i8* @axea.i64.to_str(") != std::string::npos);
    EXPECT_TRUE(ir.find("call i8* @axea.f64.to_str(") != std::string::npos);
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

TEST("LlvmIrEmitter represents an enum as a flattened {i32 tag, <every variant's own fields "
     "concatenated>} struct, reusing the ordinary struct type-decl/malloc/GEP+store "
     "machinery unchanged (see docs/language/0064-enums.md)")
{
    auto ir = emitLlvmIr("enum Shape { Circle(f64)  Rectangle(f64, f64)  Point } "
                         "f() -> Shape { return Shape.Circle(5.0) }");
    EXPECT_TRUE(ir.find("%Shape = type { i32, double, double, double }") != std::string::npos);
    EXPECT_TRUE(ir.find("call i8* @malloc(") != std::string::npos);
    EXPECT_TRUE(ir.find("getelementptr %Shape, %Shape* %") != std::string::npos);
}

TEST("LlvmIrEmitter's 'match' lowers to a nested icmp-eq/br chain against the tag field, "
     "the exact same shape 'else if' chains already produce - no new branch instruction")
{
    auto ir = emitLlvmIr("enum Shape { Circle(f64)  Rectangle(f64, f64)  Point } "
                         "area(s: Shape) -> f64 { "
                         "  return match s { "
                         "    Circle(r) => r "
                         "    Rectangle(w, h) => w "
                         "    Point => 0.0 "
                         "  } "
                         "}");
    EXPECT_TRUE(ir.find("icmp eq i32") != std::string::npos);
    EXPECT_TRUE(ir.find(" phi ") != std::string::npos);
    // Field 0 is always the tag - a real match against a real variant's own payload field
    // (index 1+) confirms binding extraction is wired up, not just the tag check.
    EXPECT_TRUE(ir.find(", i32 0, i32 1") != std::string::npos);
}

TEST("LlvmIrEmitter's @axea.tostring.<EnumName> is a real tag-aware stringifier (a genuine "
     "LLVM 'switch', not the generic 'print every field' struct-to-string body every ordinary "
     "struct gets), producing \"VariantName(field0, ...)\" or bare \"VariantName\"")
{
    auto ir = emitLlvmIr("enum Shape { Circle(f64)  Point } "
                         "f() -> String { return \"{Shape.Circle(5.0)}\" }");
    EXPECT_TRUE(ir.find("define i8* @axea.tostring.Shape(%Shape* %v) {") != std::string::npos);
    EXPECT_TRUE(ir.find("switch i32 %tag") != std::string::npos);
    EXPECT_TRUE(ir.find("c\"Circle(\\00\"") != std::string::npos);
    EXPECT_TRUE(ir.find("c\"Point\\00\"") != std::string::npos);
    // The generic struct-to-string "Name { " opening brace text must never be generated for
    // an enum - confirms the enum branch's own `continue` actually skipped the generic path.
    EXPECT_TRUE(ir.find("c\"Shape { \\00\"") == std::string::npos);
}

TEST("LlvmIrEmitter's @axea.print.<EnumName> delegates to @axea.tostring.<EnumName> then a "
     "single \"%s\" printf, the identical shape the Display-trait branch already established "
     "- never the generic per-field struct printer, which would leak the tag and every other "
     "variant's own garbage fields")
{
    auto ir = emitLlvmIr("enum Shape { Circle(f64)  Point } "
                         "p = Shape.Circle(5.0)");
    EXPECT_TRUE(ir.find("define void @axea.print.Shape(%Shape* %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("call i8* @axea.tostring.Shape(%Shape* %0)") != std::string::npos);
}

TEST("LlvmIrEmitter's enum-to-string field access reuses fieldIndexAndType by the variant's "
     "own synthetic field name, so a payload of any printable type (not just i32/i64/f64/bool) "
     "works - unlike Result<T,E>'s own printing, which is restricted to those four")
{
    auto ir = emitLlvmIr("enum Message { Text(str) } "
                         "f() -> String { return \"{Message.Text(\"hi\")}\" }");
    EXPECT_TRUE(ir.find("define i8* @axea.tostring.Message(%Message* %v) {") != std::string::npos);
}

TEST("LlvmIrEmitter's Ok(x)/Err(e) build a named 3-field {i1, T, E} struct via three "
     "insertvalues, the direct extension of Optional<T>'s own 2-field {i1, T} construction "
     "(see docs/language/0063-result.md)")
{
    auto ir = emitLlvmIr("f(a: i32, b: i32) -> Result<i32, i32> { "
                         "  if b == 0 { return Err(0 - 1) } "
                         "  return Ok(a / b) "
                         "}");
    EXPECT_TRUE(ir.find("%axea.Result.0 = type { i1, i32, i32 }") != std::string::npos);
    EXPECT_TRUE(ir.find("insertvalue %axea.Result.0 undef, i1 1, 0") != std::string::npos);
    EXPECT_TRUE(ir.find("insertvalue %axea.Result.0 undef, i1 0, 0") != std::string::npos);
}

TEST("LlvmIrEmitter's Result<T,E> is a genuinely named type, registered before every by-value "
     "use, exactly like Optional<T>'s own discovery-pass ordering requirement")
{
    auto ir = emitLlvmIr("f() -> Result<i32, i32> { return Ok(5) }");
    const auto typeDeclPos = ir.find("%axea.Result.0 = type");
    const auto firstUsePos = ir.find("%axea.Result.0", typeDeclPos + 1);
    EXPECT_TRUE(typeDeclPos != std::string::npos);
    EXPECT_TRUE(firstUsePos != std::string::npos);
    EXPECT_TRUE(typeDeclPos < firstUsePos);
}

TEST("LlvmIrEmitter's '?' on a Result<T,E> operand reuses the exact same IrOptionalIsSome/"
     "IrOptionalUnwrap instructions '?' on an Optional<T> operand does - field 0/1 mean the "
     "same thing in both layouts, so no separate Result-specific check/unwrap codegen exists")
{
    auto ir = emitLlvmIr("inner(a: i32) -> Result<i32, i32> { return Ok(a) } "
                         "outer(a: i32) -> Result<i32, i32> { x = inner(a)?  return Ok(x) }");
    EXPECT_TRUE(ir.find("extractvalue %axea.Result.0") != std::string::npos);
    // The Err-propagation path additionally extracts field 2 (the one
    // position Optional<T>'s own {i1, T} layout has no equivalent of).
    EXPECT_TRUE(ir.find(", 2\n") != std::string::npos);
}

TEST("LlvmIrEmitter's is_ok/is_err reuse emitOptionalIsSome verbatim (a plain field-0 "
     "extractvalue, xor'd for is_err) - no separate Result-specific instruction exists")
{
    auto ir = emitLlvmIr("f() -> Result<i32, i32> { return Ok(5) } "
                         "g() -> i32 { r = f()  ok = r.is_ok()  err = r.is_err()  return 0 }");
    EXPECT_TRUE(ir.find("extractvalue %axea.Result.0") != std::string::npos);
    EXPECT_TRUE(ir.find("xor i1") != std::string::npos);
}

TEST("LlvmIrEmitter's @axea.result.<id>.to_str builds \"Ok(%s)\"/\"Err(%s)\" via sprintf, "
     "restricted to i32/i64/f64/bool payloads on each side independently, same restriction "
     "registerOptionalToStrRuntime already has")
{
    auto ir = emitLlvmIr("f() -> Result<i32, i32> { return Ok(5) } "
                         "g() -> String { return \"{f()}\" }");
    EXPECT_TRUE(ir.find("define i8* @axea.result.0.to_str(%axea.Result.0 %v)") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("c\"Ok(%s)") != std::string::npos);
    EXPECT_TRUE(ir.find("c\"Err(%s)") != std::string::npos);
}

TEST("LlvmIrEmitter throws a clear error printing a Result<T,E> whose payload isn't i32/i64/"
     "f64/bool on either side, rather than emitting invalid IR")
{
    EXPECT_THROWS(emitLlvmIr("f() -> Result<i32, str> { return Ok(5) } "
                             "g() -> String { return \"{f()}\" }"));
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

TEST("LlvmIrEmitter emits a main that returns i32 with no auto-echo of a top-level binding that "
     "was never explicitly printed - a real, previously-undiscovered bug: commit 9af0af4 ('remove "
     "auto output') removed this echo from `ax run`'s own interpreted path but left the "
     "equivalent codegen here in place, so every compiled binary silently double-printed all "
     "top-level state (see LlvmIrEmitter.hpp's own note on emitMain)")
{
    auto ir = emitLlvmIr("x = 1 + 2");
    EXPECT_TRUE(ir.find("define i32 @main() {") != std::string::npos);
    EXPECT_TRUE(ir.find("ret i32 0") != std::string::npos);
    // "declare i32 @printf(i8*, ...)" (the shared runtime's own unconditional extern
    // declaration) is always present - what must be absent is an actual *call*.
    EXPECT_TRUE(ir.find("call i32 (i8*, ...) @printf(") == std::string::npos);
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

TEST("LlvmIrEmitter's compiled main does not auto-call a struct's own print helper for a "
     "top-level binding that was never explicitly printed - the helper function itself is still "
     "generated unconditionally (see the '@axea.print.<Name>' test above, for explicit print() "
     "call sites and nested struct fields), just never called from an unprompted top-level echo")
{
    auto ir = emitLlvmIr("struct Point { x: i32  y: i32 }  p = Point { x: 1  y: 2 }");
    EXPECT_TRUE(ir.find("define void @axea.print.Point(%Point* %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("call void @axea.print.Point(%Point* ") == std::string::npos);
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

TEST("LlvmIrEmitter hoists a string literal used inside a for-loop body's own interpolated "
     "string (a literal piece between two expression pieces, e.g. \"{a} {b}\"'s own literal "
     "\" \") - collectStrings' pre-pass previously recursed into IrBranch's own two blocks but "
     "not IrLoop's conditionBlock/body, so this literal was never hoisted and its later "
     "reference threw std::out_of_range at emission time; a real, pre-existing bug found while "
     "verifying docs/language/0057-alignment.md's own worked example, unrelated to alignment "
     "itself (reproduced with no format spec at all)")
{
    auto ir = emitLlvmIr("f(values: [i32; 2]) -> i32 { "
                         "  for v in values { print(\"{v} suffix\") } "
                         "  return 0 "
                         "}");
    EXPECT_TRUE(ir.find("c\" suffix\\00\"") != std::string::npos);
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

// Array/List slicing (arr[a..b] producing a fresh List<T>) is no longer supported - narrowed
// back to str-only slicing now that List<T> is a real, user-declared generic struct (see
// docs/language/0006-generics.md's own List<T> port follow-up and TypeChecker's own StrSliceExpr
// comment for why).

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

TEST("LlvmIrEmitter registers a self-contained sprintf-based format helper for a zero-padded "
     "width spec, with the format string as a fixed global (not stringPtrConstant/hoistString - "
     "see docs/language/0055-numeric-format-specs.md)")
{
    auto ir = emitLlvmIr("f() -> String { n = 42 return \"{n:05}\" }");
    EXPECT_TRUE(ir.find("@axea.fmt.spec.0 = private unnamed_addr constant") != std::string::npos);
    EXPECT_TRUE(ir.find("c\"%05d\\00\"") != std::string::npos);
    EXPECT_TRUE(ir.find("define i8* @axea.format.0(i32 %v)") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 (i8*, i8*, ...) @sprintf") != std::string::npos);
}

TEST("LlvmIrEmitter registers a format helper for a radix conversion (x/X/o) that always "
     "operates on the full 64-bit sign-extended value, regardless of the piece's i32/i64 type")
{
    auto ir = emitLlvmIr("f() -> String { n = 42 return \"{n:x}\" }");
    EXPECT_TRUE(ir.find("c\"%llx\\00\"") != std::string::npos);
    EXPECT_TRUE(ir.find("%vBits = sext i32 %v to i64") != std::string::npos);
}

TEST("LlvmIrEmitter registers a format helper for a float precision spec using printf's own "
     "%.Nf, not the hand-rolled binary path")
{
    auto ir = emitLlvmIr("f() -> String { pi = 3.14159 return \"{pi:.2}\" }");
    EXPECT_TRUE(ir.find("c\"%.2f\\00\"") != std::string::npos);
    EXPECT_TRUE(ir.find("define i8* @axea.format.0(double %v)") != std::string::npos);
}

TEST("LlvmIrEmitter's binary format spec is hand-rolled (no printf specifier exists for it), "
     "computing digit count via a bit-shift loop rather than calling sprintf")
{
    auto ir = emitLlvmIr("f() -> String { n = 42 return \"{n:b}\" }");
    EXPECT_TRUE(ir.find("define i8* @axea.format.0(i32 %v)") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 (i8*, i8*, ...) @sprintf") == std::string::npos);
    EXPECT_TRUE(ir.find("countHdr:") != std::string::npos);
    EXPECT_TRUE(ir.find("digitHdr:") != std::string::npos);
}

TEST("LlvmIrEmitter memoizes registerFormatRuntime by (type, spec) so the same format spec used "
     "twice for the same element type emits only one helper function")
{
    auto ir = emitLlvmIr("f() -> String { a = 1 b = 2 return \"{a:05} {b:05}\" }");
    const auto first = ir.find("define i8* @axea.format.0(i32 %v)");
    const auto second = ir.find("define i8* @axea.format.0(i32 %v)", first + 1);
    EXPECT_TRUE(first != std::string::npos);
    EXPECT_TRUE(second == std::string::npos);
    EXPECT_TRUE(ir.find("define i8* @axea.format.1(") == std::string::npos);
}

TEST("LlvmIrEmitter's slice<T> parameter is a by-value {T*, i32} fat pointer, not a pointer to a "
     "heap record like every other collection - see docs/language/0056-slice-printing.md")
{
    auto ir = emitLlvmIr("f(s: slice<i32>) -> String { return s.join(\",\") }");
    EXPECT_TRUE(ir.find("define {i32, i8*}* @f({i32*, i32} %0)") != std::string::npos);
}

TEST("LlvmIrEmitter's collection stringifier for a slice<T> extracts its data pointer and "
     "length via extractvalue (no GEP/load), unlike the pointer-based List<T> branch it "
     "otherwise mirrors exactly")
{
    auto ir = emitLlvmIr("f(s: slice<i32>) -> String { return \"vals: {s}\" }");
    EXPECT_TRUE(ir.find("define i8* @axea.tostring.collection.0({i32*, i32} %v)") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("%data = extractvalue {i32*, i32} %v, 0") != std::string::npos);
    EXPECT_TRUE(ir.find("%len = extractvalue {i32*, i32} %v, 1") != std::string::npos);
    EXPECT_TRUE(ir.find("getelementptr ([2 x i8], [2 x i8]* @axea.str.openbracket") !=
                std::string::npos);
}

TEST("LlvmIrEmitter's .join() on a slice<T> extracts its data pointer/length via extractvalue "
     "(resolveIndexableView's own by-value branch), then reuses the same join loop shape as "
     "Array/List")
{
    auto ir = emitLlvmIr("f(s: slice<i32>) -> String { return s.join(\",\") }");
    EXPECT_TRUE(ir.find("extractvalue {i32*, i32} %0, 0") != std::string::npos);
    EXPECT_TRUE(ir.find("extractvalue {i32*, i32} %0, 1") != std::string::npos);
    EXPECT_TRUE(ir.find("join.loop.header0") != std::string::npos);
    EXPECT_TRUE(ir.find("join.append.copy.header") != std::string::npos);
}

TEST("LlvmIrEmitter's align spec calls the shared @axea.align.pad runtime function, passing "
     "the align char's own raw code (60 '<', 62 '>', 94 '^') as a compile-time-known i8 "
     "literal (see docs/language/0057-alignment.md)")
{
    auto ir = emitLlvmIr("f() -> String { name = \"hi\" return \"[{name:<10}]\" }");
    EXPECT_TRUE(ir.find("call i8* @axea.align.pad(i8* %") != std::string::npos);
    EXPECT_TRUE(ir.find(", i32 10, i8 60)") != std::string::npos);
}

TEST("LlvmIrEmitter registers @axea.align.pad at most once even across multiple differently-"
     "aligned interpolation spans in the same program")
{
    auto ir = emitLlvmIr("g() -> String { name = \"x\" return \"{name:>1}{name:>2}{name:^3}\" }");
    const auto first = ir.find("define i8* @axea.align.pad(");
    const auto second = ir.find("define i8* @axea.align.pad(", first + 1);
    EXPECT_TRUE(first != std::string::npos);
    EXPECT_TRUE(second == std::string::npos);
    EXPECT_TRUE(ir.find(", i32 1, i8 62)") != std::string::npos);
    EXPECT_TRUE(ir.find(", i32 2, i8 62)") != std::string::npos);
    EXPECT_TRUE(ir.find(", i32 3, i8 94)") != std::string::npos);
}

TEST("LlvmIrEmitter combines alignment with a radix conversion by first computing the "
     "unpadded core text via registerFormatRuntime (width/zeroPad zeroed out, reusing its "
     "existing hex conversion), then padding that text via @axea.align.pad")
{
    auto ir = emitLlvmIr("f() -> String { n = 255 return \"{n:>10x}\" }");
    const auto formatCall = ir.find("call i8* @axea.format.0(i32");
    const auto padCall = ir.find("call i8* @axea.align.pad(i8* %", formatCall);
    EXPECT_TRUE(formatCall != std::string::npos);
    EXPECT_TRUE(padCall != std::string::npos && padCall > formatCall);
    EXPECT_TRUE(ir.find(", i32 10, i8 62)") != std::string::npos);
    // The core-text call itself has no width baked into its own format string
    // (unpadded - "%llx", not "%010llx") - width belongs to align.pad alone.
    EXPECT_TRUE(ir.find("c\"%llx\\00\"") != std::string::npos);
}

TEST("LlvmIrEmitter lowers a self-documenting '{n=}' piece as an ordinary literal-text append "
     "(the raw source text plus '=', hoisted as a fixed string global) emitted immediately "
     "before the value's own append - no new instruction field needed for self-doc at all "
     "(see docs/language/0058-debug-formatting.md)")
{
    auto ir = emitLlvmIr("f() -> String { n = 42 return \"{n=}\" }");
    EXPECT_TRUE(ir.find("c\"n=\\00\"") != std::string::npos);
}

TEST("LlvmIrEmitter's debug format '{s:?}' calls the shared @axea.debug.quote_str runtime "
     "function to wrap a str/String value in quotes")
{
    auto ir = emitLlvmIr("f() -> String { s = \"hi\" return \"{s:?}\" }");
    EXPECT_TRUE(ir.find("call i8* @axea.debug.quote_str(i8* %") != std::string::npos);
    EXPECT_TRUE(ir.find("define i8* @axea.debug.quote_str(i8* %text)") != std::string::npos);
}

TEST("LlvmIrEmitter's debug format on a non-str type (i32) is identical to the unformatted "
     "path - it calls @axea.i32.to_str directly, never @axea.debug.quote_str")
{
    auto ir = emitLlvmIr("f() -> String { n = 42 return \"{n:?}\" }");
    EXPECT_TRUE(ir.find("call i8* @axea.i32.to_str(i32") != std::string::npos);
    EXPECT_TRUE(ir.find("@axea.debug.quote_str") == std::string::npos);
}

TEST("LlvmIrEmitter registers @axea.debug.quote_str at most once even across multiple debug-"
     "formatted str pieces in the same program")
{
    auto ir = emitLlvmIr("f() -> String { a = \"x\" b = \"y\" return \"{a:?}{b:?}\" }");
    const auto first = ir.find("define i8* @axea.debug.quote_str(");
    const auto second = ir.find("define i8* @axea.debug.quote_str(", first + 1);
    EXPECT_TRUE(first != std::string::npos);
    EXPECT_TRUE(second == std::string::npos);
}

TEST("LlvmIrEmitter lowers a union type onto the exact same flattened-struct machinery a real "
     "enum uses - '|' isn't a legal LLVM identifier character, so the canonical \"i32|str\" name "
     "is rewritten to \"i32.str\" wherever it becomes an actual LLVM symbol (see "
     "docs/language/0065-unions.md)")
{
    auto ir = emitLlvmIr("f(x: i32 | str) -> i32 | str { return x }");
    EXPECT_TRUE(ir.find("%i32.str = type { i32, i32, i8* }") != std::string::npos);
    EXPECT_TRUE(ir.find("@f(%i32.str* %0)") != std::string::npos);
    EXPECT_TRUE(ir.find("i32|str") == std::string::npos);
}

TEST("LlvmIrEmitter implicitly wraps a plain i32 argument into a tagged union struct at a call "
     "boundary - a real IrStructNew (insertvalue-free malloc+GEP+store, matching every other "
     "enum construction), not just a type-level coercion with no codegen")
{
    auto ir = emitLlvmIr("f(x: i32 | str) -> i32 | str { return x } "
                         "y = f(5)");
    EXPECT_TRUE(ir.find("call i8* @malloc(") != std::string::npos);
    EXPECT_TRUE(ir.find("getelementptr %i32.str, %i32.str* %") != std::string::npos);
}

TEST("LlvmIrEmitter's union 'match' dispatches by each alternative's own tag - the same nested "
     "icmp-eq/br/phi chain a real enum's match already lowers to")
{
    auto ir = emitLlvmIr("f(x: i32 | str) -> str { "
                         "  return match x { i32(n) => \"number\"  str(s) => \"string\" } "
                         "}");
    EXPECT_TRUE(ir.find("icmp eq i32") != std::string::npos);
    EXPECT_TRUE(ir.find(" phi ") != std::string::npos);
}

TEST("LlvmIrEmitter forwards an already-union-typed value through another union-typed call "
     "boundary without re-wrapping it - only one %i32.str allocation for the whole call chain")
{
    auto ir = emitLlvmIr("f(x: i32 | str) -> i32 | str { return x } "
                         "g(x: i32 | str) -> i32 | str { return f(x) } "
                         "y = g(5)");
    // "bitcast i8* ... to %i32.str*" is the malloc+bitcast pair specific to allocating a *new*
    // %i32.str struct - a type-scoped signal, unlike counting every "call i8* @malloc(" in the
    // whole program text (which would also match mallocs inside always-emitted runtime helpers
    // like @axea.strbuf.new/@axea.i32.to_str, never actually called by this program's own
    // top-level code). Only one should appear: `g`'s own call to `f(5)` wraps the literal `5`
    // once; `f` returning `x` straight back out, and `g` returning `f(x)` straight back out,
    // both forward the already-tagged struct pointer with no wrapping of their own.
    std::size_t allocCount = 0;
    for (std::size_t pos = ir.find("to %i32.str*"); pos != std::string::npos;
         pos = ir.find("to %i32.str*", pos + 1))
    {
        ++allocCount;
    }
    EXPECT_EQ(allocCount, 1);
}

TEST("LlvmIrEmitter lowers a closure literal's own body into a genuine new top-level function "
     "(the trampoline), reached only through the closure value's own field-0 function pointer - "
     "never a direct, ordinary @closure$N(...) call (see docs/language/0067-closures.md)")
{
    auto ir = emitLlvmIr("add: fn(i32, i32) -> i32 = fn(x: i32, y: i32) -> i32 { return x + y } "
                         "y = add(2, 3)");
    EXPECT_TRUE(ir.find("define i32 @closure$0(") != std::string::npos);
    EXPECT_TRUE(ir.find("%axea.Closure.0 = type { i32 (i8*, i32, i32)*, i8*, void (i8*)* }") !=
                std::string::npos);
    // Never called directly by name - only indirectly, through a loaded function-pointer
    // register (no "@closure$0(" call site anywhere in the emitted call sequence).
    EXPECT_TRUE(ir.find("call i32 @closure$0(") == std::string::npos);
}

TEST("LlvmIrEmitter's closure struct is a real, structurally-keyed \"fat pointer\" - two "
     "closures with the exact same signature but different captures share one "
     "%axea.Closure.<id> type, even though each gets its own distinct captures struct")
{
    auto ir = emitLlvmIr("makeAdder(base: i32) -> fn(i32) -> i32 { "
                         "  return fn(x: i32) -> i32 { return x + base } "
                         "} "
                         "makeMultiplier(factor: i32, extra: i32) -> fn(i32) -> i32 { "
                         "  return fn(x: i32) -> i32 { return x * factor + extra } "
                         "} "
                         "add5 = makeAdder(5) "
                         "mul3 = makeMultiplier(3, 1)");
    // Only one %axea.Closure.<id> type *declaration* exists - both closures share the exact
    // same fn(i32)->i32 signature, regardless of how many things each one captures. (Searching
    // for the full declaration text specifically, not just any mention of "%axea.Closure." -
    // that substring legitimately appears many more times, as an ordinary type annotation
    // wherever a closure value flows through the program.)
    const std::string declText = "%axea.Closure.0 = type { i32 (i8*, i32)*, i8*, void (i8*)* }";
    const auto first = ir.find(declText);
    const auto second = ir.find(declText, first + declText.size());
    EXPECT_TRUE(first != std::string::npos);
    EXPECT_TRUE(second == std::string::npos);
    // Two distinct captures structs though - one field (base), two fields (factor, extra) -
    // captures structs go through the same emitStructNew/structs_ machinery as a real struct
    // (no hidden refcount field anymore - move semantics needs none - so each has exactly its
    // own real field count). Each closure literal also gets its own per-shape drop wrapper,
    // reached dynamically through the fat pointer's own field-2 function pointer (see
    // emitClosureDropHelper) - the field-0 function-pointer trick's own drop-lifecycle analog.
    EXPECT_TRUE(ir.find("%closure.captures.0 = type { i32 }") != std::string::npos);
    EXPECT_TRUE(ir.find("%closure.captures.1 = type { i32, i32 }") != std::string::npos);
}

TEST("LlvmIrEmitter calls a closure value through an indirect call - load its own field-0 "
     "function pointer, then 'call RetType (ParamTypes...) %reg(captures, args...)', never a "
     "direct-by-name call")
{
    auto ir = emitLlvmIr("apply(f: fn(i32) -> i32, x: i32) -> i32 { return f(x) } "
                         "doubler: fn(i32) -> i32 = fn(x: i32) -> i32 { return x * 2 } "
                         "y = apply(doubler, 5)");
    EXPECT_TRUE(ir.find("getelementptr %axea.Closure.0, %axea.Closure.0* %") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 (i8*, i32) %") != std::string::npos);
}

TEST("LlvmIrEmitter wraps a bare top-level function name passed as a call argument into a real "
     "closure value, via a synthesized trampoline that just forwards into the real function (see "
     "docs/language/0067-closures.md's implicit function-reference-to-closure coercion)")
{
    auto ir = emitLlvmIr("double(x: i32) -> i32 { return x * 2 } "
                         "apply(f: fn(i32) -> i32, x: i32) -> i32 { return f(x) } "
                         "y = apply(double, 5)");
    EXPECT_TRUE(ir.find("define i32 @fnref$double(") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 @double(") != std::string::npos);
    // The always-empty captures struct this coercion needs (a bare function name captures
    // nothing) is shared across every reference, not one per site.
    EXPECT_TRUE(ir.find("%closure.captures.fnref = type {") != std::string::npos);
}

TEST("LlvmIrEmitter memoizes one trampoline per distinct function name for the implicit "
     "function-reference-to-closure coercion - three separate references to the same top-level "
     "function share one @fnref$<name> trampoline, not three")
{
    auto ir = emitLlvmIr("double(x: i32) -> i32 { return x * 2 } "
                         "apply(f: fn(i32) -> i32, x: i32) -> i32 { return f(x) } "
                         "getDouble() -> fn(i32) -> i32 { return double } "
                         "run() -> i32 { "
                         "  d: fn(i32) -> i32 = double "
                         "  a = apply(double, 5) "
                         "  return a "
                         "} "
                         "y = run()");
    const std::string defText = "define i32 @fnref$double(";
    const auto first = ir.find(defText);
    const auto second = ir.find(defText, first + defText.size());
    EXPECT_TRUE(first != std::string::npos);
    EXPECT_TRUE(second == std::string::npos);
}

TEST("LlvmIrEmitter compiles a self-referential (recursive) closure's own self-call into an "
     "ordinary direct call to its own trampoline function, forwarding its own captures pointer "
     "straight through - never through the indirect function-pointer machinery a real closure "
     "*value* call needs (see docs/language/0067-closures.md's self-referential closures)")
{
    auto ir = emitLlvmIr("run() -> i32 { "
                         "  fact: fn(i32) -> i32 = fn(n: i32) -> i32 { "
                         "    if n <= 1 { return 1 } "
                         "    return n * fact(n - 1) "
                         "  } "
                         "  return fact(5) "
                         "} "
                         "y = run()");
    EXPECT_TRUE(ir.find("define i32 @closure$0(%closure.captures.0* %0, i32 %1)") !=
                std::string::npos);
    EXPECT_TRUE(ir.find("call i32 @closure$0(%closure.captures.0* %0,") != std::string::npos);
}

TEST("LlvmIrEmitter emits a valid struct type declaration for a generic struct instantiation, "
     "mangled with '$' rather than the illegal '<'/'>'/',' bracket syntax")
{
    auto ir = emitLlvmIr("struct Box<T> { value: T } "
                         "b = Box<i32> { value: 5 }");
    EXPECT_TRUE(ir.find("%Box$i32 = type { i32 }") != std::string::npos);
    EXPECT_TRUE(ir.find('<') == std::string::npos);
    EXPECT_TRUE(ir.find('>') == std::string::npos);
}

TEST("LlvmIrEmitter registers one struct type per distinct generic instantiation, reused across "
     "repeated literals of the same concrete type")
{
    auto ir = emitLlvmIr("struct Box<T> { value: T } "
                         "a = Box<i32> { value: 1 } "
                         "b = Box<i32> { value: 2 }");
    const auto first = ir.find("%Box$i32 = type { i32 }");
    EXPECT_TRUE(first != std::string::npos);
    const auto second = ir.find("%Box$i32 = type { i32 }", first + 1);
    EXPECT_TRUE(second == std::string::npos);
}

TEST("LlvmIrEmitter emits a plain load for '*ptr' dereference")
{
    auto ir = emitLlvmIr("f(ptr: *i32) -> i32 { unsafe { return *ptr } }");
    EXPECT_TRUE(ir.find("= load i32, i32*") != std::string::npos);
}

TEST("LlvmIrEmitter emits a plain store for '*ptr = v' dereference assignment")
{
    auto ir = emitLlvmIr("f(ptr: *i32) { unsafe { *ptr = 5 } }");
    EXPECT_TRUE(ir.find("store i32") != std::string::npos &&
               ir.find(", i32*") != std::string::npos);
}

TEST("LlvmIrEmitter emits a getelementptr for 'ptr + 1' pointer arithmetic - element-scaled "
     "automatically by LLVM's own type system, the identical shape every slice/List/Array index "
     "GEP already uses")
{
    auto ir = emitLlvmIr("f(ptr: *i32) -> *i32 { unsafe { return ptr + 1 } }");
    EXPECT_TRUE(ir.find("= getelementptr i32, i32*") != std::string::npos);
}

TEST("LlvmIrEmitter emits a negation followed by a getelementptr for 'ptr - 1' pointer "
     "arithmetic")
{
    auto ir = emitLlvmIr("f(ptr: *i32) -> *i32 { unsafe { return ptr - 1 } }");
    const auto subPos = ir.find("= sub i32 0,");
    EXPECT_TRUE(subPos != std::string::npos);
    EXPECT_TRUE(ir.find("= getelementptr i32, i32*", subPos) != std::string::npos);
}

TEST("LlvmIrEmitter emits no duplicate '@malloc'/'@free' declares for a user extern reusing "
     "those exact names, and their call sites bitcast to/from the shared i8*-based declaration")
{
    auto ir = emitLlvmIr("extern c malloc(size: i64) -> *i32 "
                         "extern c free(ptr: *i32) "
                         "buf = malloc(4i64) "
                         "called = free(buf)");

    const auto firstMallocDecl = ir.find("declare i8* @malloc(i64)");
    EXPECT_TRUE(firstMallocDecl != std::string::npos);
    EXPECT_TRUE(ir.find("declare i8* @malloc(i64)", firstMallocDecl + 1) == std::string::npos);

    const auto firstFreeDecl = ir.find("declare void @free(i8*)");
    EXPECT_TRUE(firstFreeDecl != std::string::npos);
    EXPECT_TRUE(ir.find("declare void @free(i8*)", firstFreeDecl + 1) == std::string::npos);

    EXPECT_TRUE(ir.find("call i8* @malloc(i64") != std::string::npos);
    EXPECT_TRUE(ir.find("bitcast i8*") != std::string::npos && ir.find(" to i32*") != std::string::npos);
    EXPECT_TRUE(ir.find(" to i8*") != std::string::npos);
    EXPECT_TRUE(ir.find("call void @free(i8*") != std::string::npos);
}

TEST("LlvmIrEmitter emits an alloca plus an immediate store for '&x'")
{
    auto ir = emitLlvmIr("f() -> i32 { "
                         "  x = 5 "
                         "  p = &x "
                         "  return unsafe { *p } "
                         "}");
    const auto allocaPos = ir.find("= alloca i32");
    EXPECT_TRUE(allocaPos != std::string::npos);
    EXPECT_TRUE(ir.find("store i32", allocaPos) != std::string::npos);
}

TEST("LlvmIrEmitter emits a load off the alloca's own register for '*(&x)'")
{
    auto ir = emitLlvmIr("f() -> i32 { "
                         "  x = 5 "
                         "  p = &x "
                         "  return unsafe { *p } "
                         "}");
    EXPECT_TRUE(ir.find("= load i32, i32*") != std::string::npos);
}

TEST("LlvmIrEmitter emits a store off the alloca's own register for '(*&x) = v'")
{
    auto ir = emitLlvmIr("f() -> i32 { "
                         "  x = 5 "
                         "  p = &x "
                         "  unsafe { *p = 9 } "
                         "  return x "
                         "}");
    const auto allocaPos = ir.find("= alloca i32");
    EXPECT_TRUE(allocaPos != std::string::npos);
    // The alloca's own dest register is stored to twice: once for the initial value (5), once for
    // the later '*p = 9' write-through - both as plain "store i32 ..., i32* %<reg>" text.
    const auto firstStore = ir.find("store i32", allocaPos);
    EXPECT_TRUE(firstStore != std::string::npos);
    EXPECT_TRUE(ir.find("store i32", firstStore + 1) != std::string::npos);
}

TEST("LlvmIrEmitter lowers an inherent struct method call to an ordinary LLVM 'call' to the "
     "mangled 'TypeName.method' function, the receiver passed as the first argument")
{
    auto ir = emitLlvmIr("struct Point { x: i32  y: i32 } "
                         "impl Point { sum(self) -> i32 { return self.x + self.y } } "
                         "run() -> i32 { p = Point { x: 1, y: 2 } return p.sum() } "
                         "r = run()");
    EXPECT_TRUE(ir.find("define i32 @Point.sum(%Point* %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 @Point.sum(%Point*") != std::string::npos);
}

TEST("LlvmIrEmitter lowers a C-style struct-embedded method identically to an old-style 'impl' "
     "method - same mangled 'TypeName.method' function, same receiver-as-first-argument shape "
     "(see docs/language/0068-c-style-syntax.md) - and lowers a self-less C-style associated "
     "function ('new') to an ordinary call with no receiver argument at all, via the pre-existing "
     "module-qualified-call mechanism (moduleNames_ is derived from every '.'-containing "
     "function key, so 'Counter.new' already makes 'Counter' look like a module name for free -"
     " no new IrGenerator dispatch branch was needed for this)")
{
    auto ir = emitLlvmIr("struct Counter { "
                         "  i32 value "
                         "  pub Counter new(i32 initial) { return Counter { value: initial } } "
                         "  pub void increment(self) { self.value++ } "
                         "} "
                         "run() -> i32 { c = Counter.new(5)  c.increment()  return c.value } "
                         "r = run()");
    EXPECT_TRUE(ir.find("define %Counter* @Counter.new(i32 %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("call %Counter* @Counter.new(i32") != std::string::npos);
    EXPECT_TRUE(ir.find("define void @Counter.increment(%Counter* %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("call void @Counter.increment(%Counter*") != std::string::npos);
}

TEST("LlvmIrEmitter lowers an associated-function call on an explicit generic instantiation - "
     "'Box<i32>.new(41)' - to a plain call against the monomorphized 'Box$i32.new' function, "
     "same mangled-name shape GenericMonomorphizer already produces for any other Box<i32> "
     "usage (see docs/language/0068-c-style-syntax.md)")
{
    auto ir = emitLlvmIr("struct Box<T> { "
                         "  T value "
                         "  pub Box<T> new(T v) { return Box<T> { value: v } } "
                         "  pub T get(self) { return self.value } "
                         "} "
                         "run() -> i32 { b = Box<i32>.new(41)  return b.get() } "
                         "r = run()");
    EXPECT_TRUE(ir.find("define %Box$i32* @Box$i32.new(i32 %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("call %Box$i32* @Box$i32.new(i32") != std::string::npos);
}

TEST("LlvmIrEmitter emits sizeof<T>() via the null-pointer-GEP + ptrtoint idiom, matching real "
     "byte sizes for a primitive and a struct")
{
    auto ir = emitLlvmIr("struct Point { x: i32  y: i32 } "
                         "a = sizeof<i32>() "
                         "b = sizeof<Point>()");
    EXPECT_TRUE(ir.find("getelementptr i32, i32* null, i32 1") != std::string::npos);
    EXPECT_TRUE(ir.find("getelementptr %Point, %Point* null, i32 1") != std::string::npos);
    EXPECT_TRUE(ir.find("ptrtoint") != std::string::npos);
}

TEST("LlvmIrEmitter emits a pointer-to-pointer cast as a plain bitcast")
{
    auto ir = emitLlvmIr("extern c malloc(size: i64) -> *i32 "
                         "raw = malloc(8i64) "
                         "p = unsafe { raw as *i64 }");
    EXPECT_TRUE(ir.find("bitcast i32* ") != std::string::npos);
}

TEST("LlvmIrEmitter compiles a generic top-level function's mangled clone as an entirely "
     "ordinary function, called via a plain 'call' instruction")
{
    auto ir = emitLlvmIr("identity<T>(x: T) -> T { return x } "
                         "run() -> i32 { return identity<i32>(42) } "
                         "r = run()");
    EXPECT_TRUE(ir.find("define i32 @identity$i32(i32 %0) {") != std::string::npos);
    EXPECT_TRUE(ir.find("call i32 @identity$i32(i32") != std::string::npos);
}
