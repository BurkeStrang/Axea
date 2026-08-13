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
    auto program = generateIr("square(x: i32) -> i32 { x * x } "
                              "add(a: i32, b: i32) -> i32 { square(a) + b }");
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
    auto program = generateIr("pick(flag: bool) -> i32 { if flag { 1 } else { 2 } }");
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
    auto program = generateIr("bump(n: i32) -> i32 { n++  n }");
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
                              "bump(p: Point) -> i32 { p.x++  p.x }");
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
                              "peek(user: User) -> str { user.name } "
                              "absorb(take user: User) -> str { user.name }");
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
                              "bump(p: Point) -> i32 { p.x++  p.x }");
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
                              "  p.x + p.y "
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
                              "absorb(take packet: Packet) -> i32 { packet.id }");
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
                              "  n "
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
    auto program = generateIr("f(n: i32, flag: bool) -> i32 { "
                              "  if flag { n++  n } else { n } "
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
