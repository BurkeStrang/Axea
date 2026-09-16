#include "TestFramework.hpp"

#include "generics/GenericMonomorphizer.hpp"
#include "ir/IrGenerator.hpp"
#include "lexer/Lexer.hpp"
#include "module/ModuleLoader.hpp"
#include "parser/Parser.hpp"
#include "sema/CapabilityChecker.hpp"
#include "sema/RegionChecker.hpp"
#include "sema/TypeChecker.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace
{
    // Mirrors the real pipeline (compiler/main.cpp): TypeChecker, then
    // CapabilityChecker, then RegionChecker, then IrGenerator - each stage's
    // output feeds the next.
    IrProgram generateIr(const std::string& source)
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
        regionChecker.check(program,
                            capabilityChecker.effectiveCapabilities(),
                            capabilityChecker.closureEffectiveCapabilities());

        IrGenerator irGenerator;
        return irGenerator.generate(program,
                                    capabilityChecker.effectiveCapabilities(),
                                    regionChecker.regions(),
                                    capabilityChecker.closureEffectiveCapabilities(),
                                    regionChecker.closureRegions());
    }

    const IrFunction& functionNamed(const IrProgram& program, const std::string& name)
    {
        for (const auto& function : program.functions)
        {
            if (function.name == name)
            {
                return function;
            }
        }
        throw std::runtime_error("no such IR function: " + name);
    }

    // A fresh, uniquely-named scratch directory per test, mirroring ModuleLoaderTests.cpp's own
    // TempDir - loadProgram discovers a used module by scanning the entry file's own directory,
    // so a real on-disk module file is needed to exercise module-qualified call syntax
    // (`module.name<T>(...)`, parsed as a MethodCallExpr rather than a CallExpr) at all; a single
    // in-memory source string (generateIr above) can never produce one.
    struct TempModuleDir
    {
        std::filesystem::path path;

        TempModuleDir()
            : path(std::filesystem::temp_directory_path() /
                   ("axea_irgen_module_test_" + std::to_string(nextId())))
        {
            std::filesystem::create_directories(path);
        }

        static int nextId()
        {
            static std::atomic<int> counter{0};
            return counter++;
        }

        ~TempModuleDir()
        {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }

        std::string write(const std::string& filename, const std::string& content) const
        {
            const auto filePath = path / filename;
            std::ofstream out(filePath);
            out << content;
            out.close();
            return filePath.string();
        }
    };

    // Same pipeline as generateIr above, starting from a real merged (ModuleLoader) program
    // instead of a single in-memory source string.
    IrProgram generateIrFromModule(const std::string& entrySource, const std::string& moduleName,
                                   const std::string& moduleSource)
    {
        TempModuleDir dir;
        dir.write(moduleName + ".ax", moduleSource);
        const std::string entryPath = dir.write("main.ax", entrySource);

        auto program = loadProgram(entryPath);
        monomorphizeGenerics(program);

        TypeChecker typeChecker;
        typeChecker.check(program);

        CapabilityChecker capabilityChecker;
        capabilityChecker.check(program);

        RegionChecker regionChecker;
        regionChecker.check(program,
                            capabilityChecker.effectiveCapabilities(),
                            capabilityChecker.closureEffectiveCapabilities());

        IrGenerator irGenerator;
        return irGenerator.generate(program,
                                    capabilityChecker.effectiveCapabilities(),
                                    regionChecker.regions(),
                                    capabilityChecker.closureEffectiveCapabilities(),
                                    regionChecker.closureRegions());
    }
} // namespace

TEST("IrGenerator lowers arithmetic and a call with correctly wired registers")
{
    auto program = generateIr(R"AXEA(i32 square(i32 x)
{ return x * x } i32 add(i32 a, i32 b)
{ return square(a) + b })AXEA");
    const auto& add = functionNamed(program, "add");

    const IrCall* call = nullptr;
    const IrBinOp* binOp = nullptr;
    const IrReturn* returnInst = nullptr;
    for (const auto& inst : add.body)
    {
        if (const auto* c = dynamic_cast<const IrCall*>(inst.get()))
        {
            call = c;
        }
        if (const auto* b = dynamic_cast<const IrBinOp*>(inst.get()))
        {
            binOp = b;
        }
        if (const auto* r = dynamic_cast<const IrReturn*>(inst.get()))
        {
            returnInst = r;
        }
    }

    EXPECT_TRUE(call != nullptr);
    EXPECT_EQ(call->callee, "square");
    EXPECT_EQ(call->args.size(), static_cast<std::size_t>(1));

    EXPECT_TRUE(binOp != nullptr);
    EXPECT_EQ(binOp->lhs, call->dest); // square(a) feeds directly into the addition
    EXPECT_TRUE(binOp->op == TokenKind::Plus);

    EXPECT_TRUE(returnInst != nullptr);
    EXPECT_EQ(returnInst->value, binOp->dest);
}

TEST("IrGenerator lowers if/else into one Branch with two populated instruction lists")
{
    // return wraps the whole if-expression (not pushed into each branch) so
    // this still exercises Branch producing populated then/else values,
    // exactly as before explicit return was required.
    auto program = generateIr(R"AXEA(i32 pick(bool flag)
{ return if flag { 1 } else { 2 } })AXEA");
    const auto& pick = functionNamed(program, "pick");

    const IrBranch* branch = nullptr;
    for (const auto& inst : pick.body)
    {
        if (const auto* b = dynamic_cast<const IrBranch*>(inst.get()))
        {
            branch = b;
        }
    }

    EXPECT_TRUE(branch != nullptr);
    EXPECT_TRUE(!branch->thenBlock.empty());
    EXPECT_TRUE(!branch->elseBlock.empty());
    EXPECT_TRUE(branch->thenValue != -1);
    EXPECT_TRUE(branch->elseValue != -1);

    bool thenValueProduced = false;
    for (const auto& inst : branch->thenBlock)
    {
        if (inst->dest == branch->thenValue)
        {
            thenValueProduced = true;
        }
    }
    EXPECT_TRUE(thenValueProduced);
}

TEST("IrGenerator desugars ++ on a name into const+binop and rebinds the name")
{
    auto program = generateIr(R"AXEA(i32 bump(i32 n)
{ n++  return n })AXEA");
    const auto& bump = functionNamed(program, "bump");

    const IrConstInt* deltaConst = nullptr;
    const IrBinOp* binOp = nullptr;
    const IrReturn* returnInst = nullptr;
    for (const auto& inst : bump.body)
    {
        if (const auto* c = dynamic_cast<const IrConstInt*>(inst.get()))
        {
            deltaConst = c;
        }
        if (const auto* b = dynamic_cast<const IrBinOp*>(inst.get()))
        {
            binOp = b;
        }
        if (const auto* r = dynamic_cast<const IrReturn*>(inst.get()))
        {
            returnInst = r;
        }
    }

    EXPECT_TRUE(deltaConst != nullptr);
    EXPECT_EQ(deltaConst->value, 1);
    EXPECT_TRUE(binOp != nullptr);
    EXPECT_EQ(binOp->lhs, 0); // n is parameter 0
    EXPECT_EQ(binOp->rhs, deltaConst->dest);
    EXPECT_TRUE(returnInst != nullptr);
    EXPECT_EQ(returnInst->value, binOp->dest); // trailing `n` now resolves to the incremented value
}

TEST("IrGenerator desugars ++ on a field target into get/const/binop/set")
{
    auto program = generateIr(R"AXEA(struct Point
{
    i32 x
} i32 bump(Point p)
{ p.x++  return p.x })AXEA");
    const auto& bump = functionNamed(program, "bump");

    std::vector<const IrFieldGet*> gets;
    const IrFieldSet* set = nullptr;
    for (const auto& inst : bump.body)
    {
        if (const auto* g = dynamic_cast<const IrFieldGet*>(inst.get()))
        {
            gets.push_back(g);
        }
        if (const auto* s = dynamic_cast<const IrFieldSet*>(inst.get()))
        {
            set = s;
        }
    }

    EXPECT_EQ(
        gets.size(),
        static_cast<std::size_t>(2)); // one to read for the increment, one for the trailing `p.x`
    EXPECT_TRUE(set != nullptr);
    EXPECT_EQ(set->field, "x");
    EXPECT_EQ(set->object, 0); // p is parameter 0
}

TEST("IrGenerator emits BorrowRead for a read parameter and Move for a take parameter")
{
    auto program = generateIr(R"AXEA(struct User
{
    str name
} str peek(User user)
{ return user.name } str absorb(take User user)
{ return user.name })AXEA");
    const auto& peek = functionNamed(program, "peek");
    const auto& absorb = functionNamed(program, "absorb");

    bool sawBorrowRead = false;
    for (const auto& inst : peek.body)
    {
        if (const auto* borrow = dynamic_cast<const IrBorrowRead*>(inst.get());
            borrow && borrow->value == 0)
        {
            sawBorrowRead = true;
        }
    }
    EXPECT_TRUE(sawBorrowRead);

    bool sawMove = false;
    for (const auto& inst : absorb.body)
    {
        if (const auto* move = dynamic_cast<const IrMove*>(inst.get()); move && move->value == 0)
        {
            sawMove = true;
        }
    }
    EXPECT_TRUE(sawMove);
}

TEST("IrGenerator emits BorrowWrite for a write parameter")
{
    auto program = generateIr(R"AXEA(struct Point
{
    i32 x
} i32 bump(Point p)
{ p.x++  return p.x })AXEA");
    const auto& bump = functionNamed(program, "bump");

    bool sawBorrowWrite = false;
    for (const auto& inst : bump.body)
    {
        if (const auto* borrow = dynamic_cast<const IrBorrowWrite*>(inst.get());
            borrow && borrow->value == 0)
        {
            sawBorrowWrite = true;
        }
    }
    EXPECT_TRUE(sawBorrowWrite);
}

TEST("IrGenerator drops a struct-typed local at its block's end")
{
    auto program = generateIr(R"AXEA(struct Point
{
    i32 x
    i32 y
} i32 sum_point(i32 x, i32 y)
{   p = Point { x: x  y: y }   return p.x + p.y })AXEA");
    const auto& fn = functionNamed(program, "sum_point");

    const IrStructNew* structNew = nullptr;
    const IrDrop* drop = nullptr;
    for (const auto& inst : fn.body)
    {
        if (const auto* s = dynamic_cast<const IrStructNew*>(inst.get()))
        {
            structNew = s;
        }
        if (const auto* d = dynamic_cast<const IrDrop*>(inst.get()))
        {
            drop = d;
        }
    }

    EXPECT_TRUE(structNew != nullptr);
    EXPECT_TRUE(drop != nullptr);
    EXPECT_EQ(drop->value, structNew->dest);
}

TEST("IrGenerator drops an owned (take) struct parameter at function exit")
{
    auto program = generateIr(R"AXEA(struct Packet
{
    i32 id
} i32 absorb(take Packet packet)
{ return packet.id })AXEA");
    const auto& fn = functionNamed(program, "absorb");

    bool sawDrop = false;
    for (const auto& inst : fn.body)
    {
        if (const auto* d = dynamic_cast<const IrDrop*>(inst.get()); d && d->value == 0)
        {
            sawDrop = true;
        }
    }
    EXPECT_TRUE(sawDrop);
}

TEST("IrGenerator merges a name mutated inside an if-branch into a real phi-backed register "
     "visible past the branch (see docs/language/0021-axea-ir.md's own follow-up correcting its "
     "own originally-documented 'no merge' limitation - accurate when that IR was only ever "
     "printed for `ax ir`, wrong once it started driving real LLVM codegen: a compiled `if flag "
     "{ n++ } return n` used to silently return the pre-if `n`, regardless of `flag`)")
{
    // No else: the parser desugars this to an empty (unit) else, so both
    // branches stay type-compatible. The trailing `n` is outside the if -
    // IrGenerator::mergeBranchScopes must record a carriedMerges entry on the
    // IrBranch for `n` (thenReg = the incremented register, elseReg = the
    // original parameter register 0) and reassign `n`'s own IrScope binding
    // to that entry's destReg, so `return n` reads the merged register, not
    // register 0 directly.
    auto program = generateIr(R"AXEA(i32 f(i32 n, bool flag)
{   if flag { n++ }   return n })AXEA");
    const auto& fn = functionNamed(program, "f");

    const IrBranch* branch = nullptr;
    const IrReturn* returnInst = nullptr;
    for (const auto& inst : fn.body)
    {
        if (const auto* b = dynamic_cast<const IrBranch*>(inst.get()))
        {
            branch = b;
        }
        if (const auto* r = dynamic_cast<const IrReturn*>(inst.get()))
        {
            returnInst = r;
        }
    }
    EXPECT_TRUE(branch != nullptr);
    EXPECT_TRUE(returnInst != nullptr);
    EXPECT_TRUE(returnInst->value != 0); // no longer the original parameter register

    bool foundMerge = false;
    for (const auto& [thenReg, elseReg, destReg] : branch->carriedMerges)
    {
        if (destReg == returnInst->value)
        {
            foundMerge = true;
            EXPECT_EQ(elseReg, 0); // else-branch never touched `n` - still the original register
            EXPECT_TRUE(thenReg != 0); // then-branch's own `n++` rebound it to a fresh register
        }
    }
    EXPECT_TRUE(foundMerge);
}

TEST("IrGenerator lets a name mutated inside an if-branch persist for the rest of that same branch")
{
    // return wraps the whole if-expression (this test is specifically about
    // Branch's thenValue/elseValue, not branch-level early return).
    auto program = generateIr(R"AXEA(i32 f(i32 n, bool flag)
{   return if flag { n++  n } else { n } })AXEA");
    const auto& fn = functionNamed(program, "f");

    const IrBranch* branch = nullptr;
    for (const auto& inst : fn.body)
    {
        if (const auto* b = dynamic_cast<const IrBranch*>(inst.get()))
        {
            branch = b;
        }
    }
    EXPECT_TRUE(branch != nullptr);

    const IrBinOp* incrementBinOp = nullptr;
    for (const auto& inst : branch->thenBlock)
    {
        if (const auto* b = dynamic_cast<const IrBinOp*>(inst.get()))
        {
            incrementBinOp = b;
        }
    }
    EXPECT_TRUE(incrementBinOp != nullptr);
    EXPECT_EQ(branch->thenValue,
              incrementBinOp->dest); // the then-branch's trailing `n` sees the increment
    EXPECT_EQ(branch->elseValue, 0); // the else-branch's `n` is untouched by the then-branch
}

TEST("IrGenerator lowers a function whose entire body is an if/else where both branches return, "
     "without appending a second trailing Return")
{
    // Regression coverage for the case docs/language/0027-explicit-return.md
    // exists to make possible: previously unreachable under implicit-return
    // semantics (the if-expression's own type was unit). generateFunction
    // must not append its synthetic final Return after a Branch that's
    // already fully covered by explicit returns on both sides.
    auto program =
        generateIr(R"AXEA(i32 sign(i32 x)
{ if x < 0 { return 0 - 1 } else { return 1 } })AXEA");
    const auto& sign = functionNamed(program, "sign");

    int returnCount = 0;
    const IrBranch* branch = nullptr;
    for (const auto& inst : sign.body)
    {
        if (dynamic_cast<const IrReturn*>(inst.get()))
        {
            ++returnCount;
        }
        if (const auto* b = dynamic_cast<const IrBranch*>(inst.get()))
        {
            branch = b;
        }
    }

    EXPECT_EQ(returnCount, 0); // no top-level Return - both returns are nested inside the Branch
    EXPECT_TRUE(branch != nullptr);

    auto containsReturn = [](const std::vector<std::unique_ptr<IrInst>>& block)
    {
        for (const auto& inst : block)
        {
            if (dynamic_cast<const IrReturn*>(inst.get()))
            {
                return true;
            }
        }
        return false;
    };
    EXPECT_TRUE(containsReturn(branch->thenBlock));
    EXPECT_TRUE(containsReturn(branch->elseBlock));
}

TEST("IrGenerator lowers a while loop with a conditionBlock and detects carried variables")
{
    auto program = generateIr(R"AXEA(i32 sumTo(i32 limit)
{   n = 0  total = 0   while n < limit { n = n + 1  total = total + n }   return total })AXEA");
    const auto& sumTo = functionNamed(program, "sumTo");

    const IrLoop* loop = nullptr;
    for (const auto& inst : sumTo.body)
    {
        if (const auto* l = dynamic_cast<const IrLoop*>(inst.get()))
        {
            loop = l;
        }
    }
    EXPECT_TRUE(loop != nullptr);
    EXPECT_TRUE(!loop->conditionBlock.empty());
    EXPECT_TRUE(loop->conditionValue != -1);
    EXPECT_EQ(loop->carried.size(), static_cast<std::size_t>(2)); // n and total both carried
}

TEST("IrGenerator lowers an infinite loop with no conditionBlock")
{
    auto program = generateIr(R"AXEA(i32 f()
{ return loop { break 1 } })AXEA");
    const auto& f = functionNamed(program, "f");

    const IrLoop* loop = nullptr;
    for (const auto& inst : f.body)
    {
        if (const auto* l = dynamic_cast<const IrLoop*>(inst.get()))
        {
            loop = l;
        }
    }
    EXPECT_TRUE(loop != nullptr);
    EXPECT_TRUE(loop->conditionBlock.empty());
    EXPECT_EQ(loop->conditionValue, -1);
    EXPECT_TRUE(loop->carried.empty());
}

TEST("IrGenerator records a continue's own carried snapshot, distinct from the loop's own")
{
    auto program = generateIr(R"AXEA(void f()
{   n = 0   while n < 10 {     n = n + 1     if n == 3 { continue }     n = n + 100   } })AXEA");
    const auto& f = functionNamed(program, "f");

    const IrLoop* loop = nullptr;
    for (const auto& inst : f.body)
    {
        if (const auto* l = dynamic_cast<const IrLoop*>(inst.get()))
        {
            loop = l;
        }
    }
    EXPECT_TRUE(loop != nullptr);

    const IrContinue* continueInst = nullptr;
    for (const auto& inst : loop->body)
    {
        if (const auto* branch = dynamic_cast<const IrBranch*>(inst.get()))
        {
            for (const auto& thenInst : branch->thenBlock)
            {
                if (const auto* c = dynamic_cast<const IrContinue*>(thenInst.get()))
                {
                    continueInst = c;
                }
            }
        }
    }
    EXPECT_TRUE(continueInst != nullptr);
    // At the continue, only `n`'s first increment has happened yet - not the
    // `+ 100` that comes after it in the body.
    EXPECT_EQ(continueInst->carried.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(continueInst->carried.front().second != loop->carried.front().second);
}

TEST("IrGenerator records a break's own carried snapshot")
{
    auto program = generateIr(R"AXEA(i32 f()
{   n = 0   return loop {     n = n + 1     if n > 3 { break n }   } })AXEA");
    const auto& f = functionNamed(program, "f");

    const IrLoop* loop = nullptr;
    for (const auto& inst : f.body)
    {
        if (const auto* l = dynamic_cast<const IrLoop*>(inst.get()))
        {
            loop = l;
        }
    }
    EXPECT_TRUE(loop != nullptr);

    const IrBreak* breakInst = nullptr;
    for (const auto& inst : loop->body)
    {
        if (const auto* branch = dynamic_cast<const IrBranch*>(inst.get()))
        {
            for (const auto& thenInst : branch->thenBlock)
            {
                if (const auto* b = dynamic_cast<const IrBreak*>(thenInst.get()))
                {
                    breakInst = b;
                }
            }
        }
    }
    EXPECT_TRUE(breakInst != nullptr);
    EXPECT_TRUE(breakInst->value != -1);
    EXPECT_EQ(breakInst->carried.size(), static_cast<std::size_t>(1));
}

TEST("IrGenerator lowers .join(separator) into an IrJoin with object/separator wired to the "
     "right registers - see docs/language/0050-collection-join-and-slicing.md")
{
    auto program = generateIr(R"AXEA(String f()
{ numbers = [1, 2, 3] return numbers.join(",") })AXEA");
    const auto& f = functionNamed(program, "f");

    const IrJoin* join = nullptr;
    for (const auto& inst : f.body)
    {
        if (const auto* j = dynamic_cast<const IrJoin*>(inst.get()))
        {
            join = j;
        }
    }
    EXPECT_TRUE(join != nullptr);
    EXPECT_TRUE(join->object != -1);
    EXPECT_TRUE(join->separator != -1);
    EXPECT_TRUE(join->object != join->separator);
}

TEST("IrGenerator's closure trampoline emits a real IrBorrowRead (not the pre-existing "
     "unconditional IrMove fallback) for a struct-typed closure parameter that CapabilityChecker "
     "inferred as read-only and RegionChecker resolved as Borrowed (see "
     "docs/language/0067-closures.md's own struct-typed closure parameter support). Nested inside "
     "a function whose own return type is itself struct-like (Point) - RegionChecker only walks "
     "a function's own body at all when its own return type could carry an aliasing risk (see "
     "checkFunction's own early-return check), so a closure nested inside an i32-returning "
     "function would never actually get analyzed either.")
{
    auto program = generateIr(R"AXEA(struct Point
{
    i32 x
} Point run()
{   get: fn(Point) -> i32 = fn(p: Point) -> i32 { return p.x }   n = get(Point { x: 5 })   return Point { x: n } } y = run())AXEA");
    const auto& trampoline = functionNamed(program, "closure$0");

    // Two params in the trampoline's own IR: __captures (always IrBorrowRead - see
    // generateClosureTrampoline) and p itself.
    const IrBorrowRead* borrowRead = nullptr;
    const IrMove* move = nullptr;
    int borrowReadCount = 0;
    for (const auto& inst : trampoline.body)
    {
        if (const auto* b = dynamic_cast<const IrBorrowRead*>(inst.get()))
        {
            borrowRead = b;
            ++borrowReadCount;
        }
        if (const auto* m = dynamic_cast<const IrMove*>(inst.get()))
        {
            move = m;
        }
    }
    EXPECT_TRUE(borrowRead != nullptr);
    // __captures's own IrBorrowRead, plus p's own - not collapsed into just one.
    EXPECT_EQ(borrowReadCount, 2);
    EXPECT_TRUE(move == nullptr);
}

TEST("IrGenerator's closure trampoline still falls back to its own original unconditional "
     "IrMove for a `take`-declared struct-typed closure parameter - ownership genuinely "
     "transfers, so RegionChecker resolves it as Owned, not Borrowed. Nested inside a function "
     "whose own return type is itself struct-like, for the same reason the read-only test just "
     "above needs it (RegionChecker skips walking a function's body at all otherwise).")
{
    auto program = generateIr(R"AXEA(struct Point
{
    i32 x
} Point run()
{   consume: fn(Point) -> i32 = fn(take p: Point) -> i32 {     return p.x   }   n = consume(Point { x: 5 })   return Point { x: n } } y = run())AXEA");
    const auto& trampoline = functionNamed(program, "closure$0");

    int moveCount = 0;
    for (const auto& inst : trampoline.body)
    {
        if (dynamic_cast<const IrMove*>(inst.get()))
        {
            ++moveCount;
        }
    }
    // p's own IrMove alone - __captures always gets IrBorrowRead, never IrMove (see
    // generateClosureTrampoline), regardless of what any actual closure param resolves to.
    EXPECT_EQ(moveCount, 1);
}

TEST("IrGenerator lowers a self-referential (recursive) closure's own self-call into a genuine "
     "direct IrCall back to the trampoline's own name, forwarding its own captures register as "
     "the hidden first argument - never an indirect IrClosureCall, since no closure *value* "
     "exists yet at the point the literal's own body is still being compiled (see "
     "docs/language/0067-closures.md's self-referential closures)")
{
    auto program = generateIr(R"AXEA(i32 run()
{   fact: fn(i32) -> i32 = fn(n: i32) -> i32 {     if n <= 1 { return 1 }     return n * fact(n - 1)   }   return fact(5) } y = run())AXEA");
    const auto& trampoline = functionNamed(program, "closure$0");

    // `if n <= 1 { return 1 }` has no explicit `else`, so the branch's own elseBlock is empty -
    // `return n * fact(n - 1)` is a separate statement in the trampoline's own top-level body,
    // reached only when the `if` doesn't take its own early return.
    const IrCall* recursiveCall = nullptr;
    int closureCallCount = 0;
    for (const auto& inst : trampoline.body)
    {
        if (const auto* call = dynamic_cast<const IrCall*>(inst.get()))
        {
            recursiveCall = call;
        }
        if (dynamic_cast<const IrClosureCall*>(inst.get()))
        {
            ++closureCallCount;
        }
    }
    EXPECT_TRUE(recursiveCall != nullptr);
    EXPECT_EQ(recursiveCall->callee, "closure$0");
    // The forwarded captures register, plus (n - 1).
    EXPECT_EQ(recursiveCall->args.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(closureCallCount, 0);
}

TEST("IrGenerator registers a monomorphized generic struct instantiation under its mangled name, "
     "with correctly substituted field types")
{
    auto program = generateIr(R"AXEA(struct Box<T>
{
    T value
} b = Box<i32> { value: 5 })AXEA");

    const auto it = program.structs.find("Box$i32");
    EXPECT_TRUE(it != program.structs.end());
    EXPECT_EQ(it->second.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(it->second[0].first, "value");
    EXPECT_EQ(it->second[0].second, "i32");
}

TEST("IrGenerator lowers '*ptr' to an IrDeref instruction")
{
    auto program = generateIr(R"AXEA(i32 f(*i32 ptr)
{ unsafe { return *ptr } })AXEA");
    const auto& f = functionNamed(program, "f");

    bool foundDeref = false;
    for (const auto& inst : f.body)
    {
        if (dynamic_cast<const IrDeref*>(inst.get()))
        {
            foundDeref = true;
        }
    }
    EXPECT_TRUE(foundDeref);
}

TEST("IrGenerator lowers '*ptr = v' to an IrDerefAssign instruction")
{
    auto program = generateIr(R"AXEA(void f(*i32 ptr)
{ unsafe { *ptr = 5 } })AXEA");
    const auto& f = functionNamed(program, "f");

    bool foundDerefAssign = false;
    for (const auto& inst : f.body)
    {
        if (dynamic_cast<const IrDerefAssign*>(inst.get()))
        {
            foundDerefAssign = true;
        }
    }
    EXPECT_TRUE(foundDerefAssign);
}

TEST("IrGenerator lowers 'ptr + 1' to a plain IrBinOp - the pointer-vs-arithmetic decision is "
     "made entirely at the LlvmIrEmitter layer, not here")
{
    auto program = generateIr(R"AXEA(*i32 f(*i32 ptr)
{ unsafe { return ptr + 1 } })AXEA");
    const auto& f = functionNamed(program, "f");

    bool foundBinOp = false;
    for (const auto& inst : f.body)
    {
        if (const auto* binOp = dynamic_cast<const IrBinOp*>(inst.get()))
        {
            EXPECT_TRUE(binOp->op == TokenKind::Plus);
            foundBinOp = true;
        }
    }
    EXPECT_TRUE(foundBinOp);
}

namespace
{
    std::size_t countAllocas(const std::vector<std::unique_ptr<IrInst>>& body)
    {
        std::size_t count = 0;
        for (const auto& inst : body)
        {
            if (dynamic_cast<const IrAlloca*>(inst.get()))
            {
                ++count;
            }
        }
        return count;
    }
} // namespace

TEST("IrGenerator lowers '&x' at a local's own definition to an IrAlloca")
{
    auto program = generateIr(R"AXEA(i32 f()
{   x = 5   p = &x   return unsafe { *p } })AXEA");
    const auto& f = functionNamed(program, "f");
    EXPECT_EQ(countAllocas(f.body), static_cast<std::size_t>(1));
}

TEST("IrGenerator produces no IrAlloca anywhere for a local whose address is never taken - "
     "regression guard against the escape analysis over-firing")
{
    auto program = generateIr(R"AXEA(i32 f()
{   x = 5   return x })AXEA");
    const auto& f = functionNamed(program, "f");
    EXPECT_EQ(countAllocas(f.body), static_cast<std::size_t>(0));
}

TEST("IrGenerator lowers '*(&x)' and '(*&x)=v' to IrDeref/IrDerefAssign off the alloca's own dest "
     "register, not a dedicated load/store instruction")
{
    auto program = generateIr(R"AXEA(i32 f()
{   x = 5   p = &x   unsafe { *p = 9 }   return unsafe { *p } })AXEA");
    const auto& f = functionNamed(program, "f");

    int allocaDest = -1;
    for (const auto& inst : f.body)
    {
        if (const auto* alloca = dynamic_cast<const IrAlloca*>(inst.get()))
        {
            allocaDest = alloca->dest;
        }
    }
    EXPECT_TRUE(allocaDest != -1);

    bool foundDerefOffSlot = false;
    bool foundDerefAssignOffSlot = false;
    for (const auto& inst : f.body)
    {
        if (const auto* deref = dynamic_cast<const IrDeref*>(inst.get());
            deref && deref->pointer == allocaDest)
        {
            foundDerefOffSlot = true;
        }
        if (const auto* derefAssign = dynamic_cast<const IrDerefAssign*>(inst.get());
            derefAssign && derefAssign->pointer == allocaDest)
        {
            foundDerefAssignOffSlot = true;
        }
    }
    EXPECT_TRUE(foundDerefOffSlot);
    EXPECT_TRUE(foundDerefAssignOffSlot);
}

TEST("IrGenerator emits an IrAlloca for a parameter whose address is taken, right after entry")
{
    auto program = generateIr(R"AXEA(i32 f(i32 x)
{   p = &x   return unsafe { *p } })AXEA");
    const auto& f = functionNamed(program, "f");
    EXPECT_EQ(countAllocas(f.body), static_cast<std::size_t>(1));
}

TEST("IrGenerator emits exactly one IrAlloca (not one per iteration) for a name reassigned "
     "inside a loop after its address was taken outside it")
{
    auto program = generateIr(R"AXEA(i32 f()
{   n = 0   p = &n   i = 0   while i < 3 {     n = n + 1     i = i + 1   }   return unsafe { *p } })AXEA");
    const auto& f = functionNamed(program, "f");
    // Only `n` is ever address-taken (via `p = &n`) - `i` never is - so exactly one alloca is
    // expected. This is a static, compile-time count regardless of how many times the loop
    // actually runs, so the real regression this guards against is an IrAlloca appearing *inside*
    // IrLoop::body (re-executed every iteration) instead of once, before the IrLoop itself.
    EXPECT_EQ(countAllocas(f.body), static_cast<std::size_t>(1));
    bool allocaBeforeLoop = false;
    bool sawLoop = false;
    for (const auto& inst : f.body)
    {
        if (dynamic_cast<const IrAlloca*>(inst.get()) && !sawLoop)
        {
            allocaBeforeLoop = true;
        }
        if (const auto* loop = dynamic_cast<const IrLoop*>(inst.get()))
        {
            sawLoop = true;
            EXPECT_EQ(countAllocas(loop->body), static_cast<std::size_t>(0));
        }
    }
    EXPECT_TRUE(allocaBeforeLoop);
}

TEST("IrGenerator emits an IrAlloca for a top-level 'p = &x' - the top-level topCtx needs the "
     "identical address-taken-name treatment generateFunction's own Context already gets")
{
    auto program = generateIr("x = 5 "
                              "p = &x");
    EXPECT_EQ(countAllocas(program.topLevel), static_cast<std::size_t>(1));
}

TEST("IrGenerator lowers an inherent (no-trait) struct method call to an ordinary IrCall to "
     "the mangled 'TypeName.method' name, with the receiver prepended as the first argument")
{
    auto program = generateIr(R"AXEA(struct Point
{
    i32 x
    i32 y

    i32 sum(self)
    { return self.x + self.y }
} i32 run()
{ p = Point { x: 1, y: 2 } return p.sum() })AXEA");
    const auto& run = functionNamed(program, "run");

    const IrCall* methodCall = nullptr;
    for (const auto& inst : run.body)
    {
        if (const auto* c = dynamic_cast<const IrCall*>(inst.get()); c && c->callee == "Point.sum")
        {
            methodCall = c;
        }
    }
    EXPECT_TRUE(methodCall != nullptr);
    EXPECT_EQ(methodCall->args.size(), static_cast<std::size_t>(1));
}

TEST("IrGenerator lowers sizeof<T>() to an IrSizeOf instruction")
{
    auto program = generateIr(R"AXEA(struct Point
{
    i32 x
    i32 y
} i64 run()
{ return sizeof<Point>() })AXEA");
    const auto& run = functionNamed(program, "run");

    const IrSizeOf* sizeOf = nullptr;
    for (const auto& inst : run.body)
    {
        if (const auto* s = dynamic_cast<const IrSizeOf*>(inst.get()))
        {
            sizeOf = s;
        }
    }
    EXPECT_TRUE(sizeOf != nullptr);
    EXPECT_EQ(sizeOf->typeName, "Point");
}

TEST("IrGenerator lowers a generic top-level function call to an ordinary IrCall to the "
     "mangled name")
{
    auto program = generateIr(R"AXEA(T identity<T>(T x)
{ return x } i32 run()
{ return identity<i32>(42) })AXEA");
    const auto& run = functionNamed(program, "run");

    const IrCall* call = nullptr;
    for (const auto& inst : run.body)
    {
        if (const auto* c = dynamic_cast<const IrCall*>(inst.get());
            c && c->callee == "identity$i32")
        {
            call = c;
        }
    }
    EXPECT_TRUE(call != nullptr);
    EXPECT_EQ(call->args.size(), static_cast<std::size_t>(1));
}

TEST("IrGenerator resolves a struct method call on a local built via a module-qualified generic "
     "constructor with no declared type, dispatching to the real method rather than falling "
     "through to a stale builtin-collection fallback")
{
    // A real, previously-undiscovered bug found while verifying PriorityQueue<T>'s own port
    // (docs/language/0039-priority-queues.md's own "2026 Update"): `module.newX<T>()` parses as
    // a MethodCallExpr, not a CallExpr - simpleTypeOfExpr had a case for the latter (used to
    // resolve a bare local's own type for the general struct method dispatch a few lines later)
    // but none for the former, so a bare local built this way with no declared type
    // (`b = boxmod.newBox<i32>()`, as opposed to `b: Box<i32> = boxmod.newBox<i32>()`) never got
    // its own simpleType recorded at all - a later `b.get()` then couldn't resolve
    // resolveStructOrEnumType(b) and silently fell through every builtin-collection branch in
    // IrGenerator's own MethodCallExpr dispatch to the final Map/Set/SortedMap/SortedSet
    // catch-all "remove" case, which unconditionally reads `methodCall->arguments.front()` -
    // crashing outright for a zero-arg call like `.get()`'s own analogue here, or silently
    // calling the wrong method entirely for a call whose own argument count happened to match.
    // Affected every List<T>-shaped composed collection this session ported identically
    // (confirmed directly for Stack<T> too, not just PriorityQueue<T>) - this repro uses a
    // minimal inline generic struct with the same "construct via a module-qualified generic
    // function, no declared type, then call a method" shape rather than a real collection.
    const std::string moduleSource = "module boxmod "
                                     "struct Box<T> { value: T } "
                                     "pub newBox<T>(v: T) -> Box<T> { return Box<T> { value: v } } "
                                     "impl<T> Box<T> { get(self) -> T { return self.value } }";
    auto program = generateIrFromModule("use boxmod "
                                        "run() -> i32 { "
                                        "  b = boxmod.newBox<i32>(7) "
                                        "  return b.get() "
                                        "}",
                                        "boxmod", moduleSource);
    const auto& run = functionNamed(program, "run");

    const IrCall* getCall = nullptr;
    for (const auto& inst : run.body)
    {
        if (const auto* c = dynamic_cast<const IrCall*>(inst.get());
            c && c->callee.find("get") != std::string::npos)
        {
            getCall = c;
        }
    }
    EXPECT_TRUE(getCall != nullptr);
    EXPECT_EQ(getCall->callee, "Box$i32.get");
}
