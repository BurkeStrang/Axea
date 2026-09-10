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
    // List<T>/Stack<T>/Deque<T>/Queue<T>/PriorityQueue<T> are no longer built-in (see
    // docs/language/0006-generics.md's own port follow-up) - they're real, user-declared generic
    // structs now (std/collections.ax), so this uses LinkedList<T> instead, one of the remaining
    // intrinsic collections.
    auto program = monomorphize("numbers = LinkedList<i32>()");

    // No "LinkedList"-named StructDecl should ever be synthesized - LinkedList<T> is a
    // compiler intrinsic, never StructDecl-backed.
    EXPECT_TRUE(findStruct(program, "LinkedList$i32") == nullptr);
}

namespace
{
    const FunctionDecl* findFunction(const Program& program, const std::string& name)
    {
        for (const auto& item : program.items)
        {
            if (const auto* f = dynamic_cast<const FunctionDecl*>(item.get()); f && f->name == name)
            {
                return f;
            }
        }
        return nullptr;
    }
} // namespace

TEST("GenericMonomorphizer synthesizes a mangled method for a generic impl's own instantiation, "
     "self and return type both substituted")
{
    auto program = monomorphize("struct Box<T> { value: T } "
                                "impl<T> Box<T> { get(self) -> T { return self.value } } "
                                "b = Box<i32> { value: 5 } "
                                "x = b.get()");

    const auto* method = findFunction(program, "Box$i32.get");
    EXPECT_TRUE(method != nullptr);
    EXPECT_EQ(method->params.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(method->params[0].name, "self");
    EXPECT_EQ(method->params[0].type, "Box$i32");
    EXPECT_TRUE(method->returnType.has_value());
    EXPECT_EQ(*method->returnType, "i32");
}

TEST("GenericMonomorphizer synthesizes independent methods for two different concrete "
     "instantiations of the same generic impl")
{
    auto program = monomorphize("struct Box<T> { value: T } "
                                "impl<T> Box<T> { get(self) -> T { return self.value } } "
                                "a = Box<i32> { value: 1 } "
                                "x = a.get() "
                                "b = Box<bool> { value: true } "
                                "y = b.get()");

    const auto* intMethod = findFunction(program, "Box$i32.get");
    const auto* boolMethod = findFunction(program, "Box$bool.get");
    EXPECT_TRUE(intMethod != nullptr);
    EXPECT_TRUE(boolMethod != nullptr);
    EXPECT_EQ(*intMethod->returnType, "i32");
    EXPECT_EQ(*boolMethod->returnType, "bool");
}

TEST("GenericMonomorphizer correctly rewrites a generic method's own body when it constructs "
     "another instance of its own generic struct")
{
    auto program = monomorphize(
        "struct Box<T> { value: T } "
        "impl<T> Box<T> { wrap(self) -> Box<T> { return Box<T> { value: self.value } } } "
        "b = Box<i32> { value: 5 } "
        "w = b.wrap()");

    const auto* method = findFunction(program, "Box$i32.wrap");
    EXPECT_TRUE(method != nullptr);
    EXPECT_EQ(*method->returnType, "Box$i32");

    const auto* block = dynamic_cast<const BlockExpr*>(method->body.get());
    EXPECT_TRUE(block != nullptr);
    const auto* returnStmt = dynamic_cast<const ReturnStmt*>(block->statements.at(0).get());
    EXPECT_TRUE(returnStmt != nullptr);
    const auto* structLiteral = dynamic_cast<const StructLiteralExpr*>(returnStmt->value.get());
    EXPECT_TRUE(structLiteral != nullptr);
    EXPECT_EQ(structLiteral->typeName, "Box$i32");
}

TEST("GenericMonomorphizer synthesizes a mangled clone of a generic top-level function per "
     "explicit call-site type argument, rewriting the call site's own callee")
{
    auto program = monomorphize("identity<T>(x: T) -> T { return x } "
                                "a = identity<i32>(42)");

    const auto* clone = findFunction(program, "identity$i32");
    EXPECT_TRUE(clone != nullptr);
    EXPECT_EQ(clone->params[0].type, "i32");
    EXPECT_TRUE(clone->returnType.has_value());
    EXPECT_EQ(*clone->returnType, "i32");

    const AssignmentStmt* assignment = nullptr;
    for (const auto& item : program.items)
    {
        if (const auto* a = dynamic_cast<const AssignmentStmt*>(item.get()); a && a->name == "a")
        {
            assignment = a;
        }
    }
    EXPECT_TRUE(assignment != nullptr);
    const auto* call = dynamic_cast<const CallExpr*>(assignment->value.get());
    EXPECT_TRUE(call != nullptr);
    EXPECT_EQ(call->callee, "identity$i32");
    EXPECT_TRUE(call->typeArgument.empty());
}

TEST("GenericMonomorphizer synthesizes independent clones for two different concrete call-site "
     "type arguments to the same generic top-level function")
{
    auto program = monomorphize("identity<T>(x: T) -> T { return x } "
                                "a = identity<i32>(1) "
                                "b = identity<bool>(true)");

    EXPECT_TRUE(findFunction(program, "identity$i32") != nullptr);
    EXPECT_TRUE(findFunction(program, "identity$bool") != nullptr);
}

TEST("GenericMonomorphizer rejects a call site naming an unknown generic function")
{
    EXPECT_THROWS(monomorphize("a = ghost<i32>(1)"));
}
