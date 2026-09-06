#include "TestFramework.hpp"

#include "generics/GenericMonomorphizer.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"

namespace
{
    Program monomorphize(const std::string& source)
    {
        Lexer lexer(source);
        Parser parser(lexer.lex());
        auto program = parser.parseProgram();
        monomorphizeGenerics(program);
        return program;
    }

    const StructDecl* findStruct(const Program& program, const std::string& name)
    {
        for (const auto& item : program.items)
        {
            if (const auto* s = dynamic_cast<const StructDecl*>(item.get()); s && s->name == name)
            {
                return s;
            }
        }
        return nullptr;
    }

    std::size_t countStructs(const Program& program, const std::string& name)
    {
        std::size_t count = 0;
        for (const auto& item : program.items)
        {
            if (const auto* s = dynamic_cast<const StructDecl*>(item.get()); s && s->name == name)
            {
                ++count;
            }
        }
        return count;
    }
} // namespace

TEST("GenericMonomorphizer synthesizes a concrete StructDecl for a single explicit instantiation")
{
    auto program = monomorphize("struct Box<T> { value: T } b = Box<i32> { value: 5 }");

    const auto* boxed = findStruct(program, "Box$i32");
    EXPECT_TRUE(boxed != nullptr);
    EXPECT_EQ(boxed->fields.size(), std::size_t{1});
    EXPECT_EQ(boxed->fields[0].name, "value");
    EXPECT_EQ(boxed->fields[0].type, "i32");
}

TEST("GenericMonomorphizer rewrites a struct literal's own typeName to the mangled struct name")
{
    auto program = monomorphize("struct Box<T> { value: T } b = Box<i32> { value: 5 }");

    const AssignmentStmt* assignment = nullptr;
    for (const auto& item : program.items)
    {
        if (const auto* a = dynamic_cast<const AssignmentStmt*>(item.get()); a && a->name == "b")
        {
            assignment = a;
        }
    }
    EXPECT_TRUE(assignment != nullptr);
    const auto* literal = dynamic_cast<const StructLiteralExpr*>(assignment->value.get());
    EXPECT_TRUE(literal != nullptr);
    EXPECT_EQ(literal->typeName, "Box$i32");
}

TEST("GenericMonomorphizer substitutes a struct type parameter, not just a primitive")
{
    auto program = monomorphize(
        "struct Point { x: i32  y: i32 } "
        "struct Box<T> { value: T } "
        "p = Point { x: 1  y: 2 } "
        "b = Box<Point> { value: p }");

    const auto* boxed = findStruct(program, "Box$Point");
    EXPECT_TRUE(boxed != nullptr);
    EXPECT_EQ(boxed->fields[0].type, "Point");
}

TEST("GenericMonomorphizer reuses one synthesized instantiation across repeated uses")
{
    auto program = monomorphize(
        "struct Box<T> { value: T } "
        "a = Box<i32> { value: 1 } "
        "b = Box<i32> { value: 2 }");

    EXPECT_EQ(countStructs(program, "Box$i32"), std::size_t{1});
}

TEST("GenericMonomorphizer resolves nested generic instantiations to a fixed point")
{
    auto program = monomorphize(
        "struct Box<T> { value: T } "
        "inner = Box<i32> { value: 5 } "
        "outer = Box<Box<i32>> { value: inner }");

    const auto* innerDecl = findStruct(program, "Box$i32");
    EXPECT_TRUE(innerDecl != nullptr);
    EXPECT_EQ(innerDecl->fields[0].type, "i32");

    const auto* outerDecl = findStruct(program, "Box$Box$i32");
    EXPECT_TRUE(outerDecl != nullptr);
    EXPECT_EQ(outerDecl->fields[0].type, "Box$i32");
}

TEST("GenericMonomorphizer rejects a type-argument-count mismatch")
{
    EXPECT_THROWS(monomorphize(
        "struct Pair<A, B> { first: A  second: B } "
        "p = Pair<i32> { first: 1 }"));
}

TEST("GenericMonomorphizer rejects an unknown generic struct name")
{
    EXPECT_THROWS(monomorphize("b = Frobnicate<i32> { value: 5 }"));
}

TEST("GenericMonomorphizer leaves a built-in generic collection type completely untouched")
{
    auto program = monomorphize("numbers = List<i32>()");

    // No "List"-named StructDecl should ever be synthesized - List<T> is a compiler intrinsic,
    // never StructDecl-backed.
    EXPECT_TRUE(findStruct(program, "List$i32") == nullptr);
}
