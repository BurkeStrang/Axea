#include "TestFramework.hpp"

#include "ir/IrGenerator.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/CapabilityChecker.hpp"
#include "sema/RegionChecker.hpp"
#include "sema/TypeChecker.hpp"

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

        TypeChecker typeChecker;
        typeChecker.check(program);

        CapabilityChecker capabilityChecker;
        capabilityChecker.check(program);

        RegionChecker regionChecker;
        regionChecker.check(program, capabilityChecker.effectiveCapabilities());

        IrGenerator irGenerator;
        return irGenerator.generate(
            program, capabilityChecker.effectiveCapabilities(), regionChecker.regions());
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
} // namespace

TEST("IrGenerator lowers arithmetic and a call with correctly wired registers")
{
    auto program = generateIr("square(x: i32) -> i32 { return x * x } "
                              "add(a: i32, b: i32) -> i32 { return square(a) + b }");
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
    auto program = generateIr("pick(flag: bool) -> i32 { return if flag { 1 } else { 2 } }");
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
    auto program = generateIr("bump(n: i32) -> i32 { n++  return n }");
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
    auto program = generateIr("struct Point { x: i32 } "
                              "bump(p: Point) -> i32 { p.x++  return p.x }");
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
    auto program = generateIr("struct User { name: str } "
                              "peek(user: User) -> str { return user.name } "
                              "absorb(take user: User) -> str { return user.name }");
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
    auto program = generateIr("struct Point { x: i32 } "
                              "bump(p: Point) -> i32 { p.x++  return p.x }");
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
    auto program = generateIr("struct Point { x: i32  y: i32 } "
                              "sum_point(x: i32, y: i32) -> i32 { "
                              "  p = Point { x: x  y: y } "
                              "  return p.x + p.y "
                              "}");
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
    auto program = generateIr("struct Packet { id: i32 } "
                              "absorb(take packet: Packet) -> i32 { return packet.id }");
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

TEST("IrGenerator does not let a name mutated inside an if-branch escape past the branch")
{
    // No else: the parser desugars this to an empty (unit) else, so both
    // branches stay type-compatible. The trailing `n` is outside the if, so
    // it must still resolve to the original parameter register (0) - the
    // mutation inside the then-branch must not leak past the branch boundary
    // (see the IrScope "barrier" mechanism in IrGenerator.hpp/.cpp).
    auto program = generateIr("f(n: i32, flag: bool) -> i32 { "
                              "  if flag { n++ } "
                              "  return n "
                              "}");
    const auto& fn = functionNamed(program, "f");

    const IrReturn* returnInst = nullptr;
    for (const auto& inst : fn.body)
    {
        if (const auto* r = dynamic_cast<const IrReturn*>(inst.get()))
        {
            returnInst = r;
        }
    }
    EXPECT_TRUE(returnInst != nullptr);
    EXPECT_EQ(returnInst->value, 0); // still the original parameter register
}

TEST("IrGenerator lets a name mutated inside an if-branch persist for the rest of that same branch")
{
    // return wraps the whole if-expression (this test is specifically about
    // Branch's thenValue/elseValue, not branch-level early return).
    auto program = generateIr("f(n: i32, flag: bool) -> i32 { "
                              "  return if flag { n++  n } else { n } "
                              "}");
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
        generateIr("sign(x: i32) -> i32 { if x < 0 { return 0 - 1 } else { return 1 } }");
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
    auto program = generateIr("sumTo(limit: i32) -> i32 { "
                              "  n = 0  total = 0 "
                              "  while n < limit { n = n + 1  total = total + n } "
                              "  return total "
                              "}");
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
    auto program = generateIr("f() -> i32 { return loop { break 1 } }");
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
    auto program = generateIr("f() { "
                              "  n = 0 "
                              "  while n < 10 { "
                              "    n = n + 1 "
                              "    if n == 3 { continue } "
                              "    n = n + 100 "
                              "  } "
                              "}");
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
    auto program = generateIr("f() -> i32 { "
                              "  n = 0 "
                              "  return loop { "
                              "    n = n + 1 "
                              "    if n > 3 { break n } "
                              "  } "
                              "}");
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
    auto program = generateIr("f() -> String { numbers = [1, 2, 3] return numbers.join(\",\") }");
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

TEST("IrGenerator lowers Array/List slicing into the same IrStrSlice instruction str slicing "
     "already uses, with a -1 start when the low bound is omitted")
{
    auto program = generateIr("f() -> List<i32> { numbers = [1, 2, 3] return numbers[..2] }");
    const auto& f = functionNamed(program, "f");

    const IrStrSlice* slice = nullptr;
    for (const auto& inst : f.body)
    {
        if (const auto* s = dynamic_cast<const IrStrSlice*>(inst.get()))
        {
            slice = s;
        }
    }
    EXPECT_TRUE(slice != nullptr);
    EXPECT_EQ(slice->start, -1);
    EXPECT_TRUE(slice->end != -1);
}
