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
    auto program = monomorphize(R"AXEA(struct Box<T>
{
    T value
} b = Box<i32> { value: 5 })AXEA");

    const auto* boxed = findStruct(program, "Box$i32");
    EXPECT_TRUE(boxed != nullptr);
    EXPECT_EQ(boxed->fields.size(), std::size_t{1});
    EXPECT_EQ(boxed->fields[0].name, "value");
    EXPECT_EQ(boxed->fields[0].type, "i32");
}

TEST("GenericMonomorphizer rewrites a struct literal's own typeName to the mangled struct name")
{
    auto program = monomorphize(R"AXEA(struct Box<T>
{
    T value
} b = Box<i32> { value: 5 })AXEA");

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
        R"AXEA(struct Point
{
    i32 x
    i32 y
} struct Box<T>
{
    T value
} p = Point { x: 1  y: 2 } b = Box<Point> { value: p })AXEA");

    const auto* boxed = findStruct(program, "Box$Point");
    EXPECT_TRUE(boxed != nullptr);
    EXPECT_EQ(boxed->fields[0].type, "Point");
}

TEST("GenericMonomorphizer reuses one synthesized instantiation across repeated uses")
{
    auto program = monomorphize(
        R"AXEA(struct Box<T>
{
    T value
} a = Box<i32> { value: 1 } b = Box<i32> { value: 2 })AXEA");

    EXPECT_EQ(countStructs(program, "Box$i32"), std::size_t{1});
}

TEST("GenericMonomorphizer resolves nested generic instantiations to a fixed point")
{
    auto program = monomorphize(
        R"AXEA(struct Box<T>
{
    T value
} inner = Box<i32> { value: 5 } outer = Box<Box<i32>> { value: inner })AXEA");

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
        R"AXEA(struct Pair<A,B>
{
    A first
    B second
} p = Pair<i32> { first: 1 })AXEA"));
}

TEST("GenericMonomorphizer rejects an unknown generic struct name")
{
    EXPECT_THROWS(monomorphize("b = Frobnicate<i32> { value: 5 }"));
}

TEST("GenericMonomorphizer leaves a built-in generic type completely untouched")
{
    // List<T>/Stack<T>/Deque<T>/Queue<T>/PriorityQueue<T>/LinkedList<T>/Map<K,V>/Set<T>/
    // SortedMap<K,V>/SortedSet<T> are no longer built-in (see docs/language/0006-generics.md's
    // own port follow-up, docs/language/0034-maps-and-sets.md's own "2026 Update",
    // docs/language/0040-sorted-maps.md's own "2026 Update", and docs/language/0041-sorted-
    // sets.md's own "2026 Update") - they're all real, user-declared generic structs now
    // (std/collections.ax), so this uses Optional<T> instead, one of the few remaining
    // genuinely built-in generic types (see docs/language/0052-optional.md).
    auto program = monomorphize(R"AXEA(Optional<i32> f()
{ return None })AXEA");

    // No "Optional"-named StructDecl should ever be synthesized - Optional<T> is a
    // compiler intrinsic, never StructDecl-backed.
    EXPECT_TRUE(findStruct(program, "Optional$i32") == nullptr);
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
    auto program = monomorphize(R"AXEA(struct Box<T>
{
    T value

    T get(self)
    { return self.value }
} b = Box<i32> { value: 5 } x = b.get())AXEA");

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
    auto program = monomorphize(R"AXEA(struct Box<T>
{
    T value

    T get(self)
    { return self.value }
} a = Box<i32> { value: 1 } x = a.get() b = Box<bool> { value: true } y = b.get())AXEA");

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
        R"AXEA(struct Box<T>
{
    T value

    Box<T> wrap(self)
    { return Box<T> { value: self.value } }
} b = Box<i32> { value: 5 } w = b.wrap())AXEA");

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
    auto program = monomorphize(R"AXEA(T identity<T>(T x)
{ return x } a = identity<i32>(42))AXEA");

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
    auto program = monomorphize(R"AXEA(T identity<T>(T x)
{ return x } a = identity<i32>(1) b = identity<bool>(true))AXEA");

    EXPECT_TRUE(findFunction(program, "identity$i32") != nullptr);
    EXPECT_TRUE(findFunction(program, "identity$bool") != nullptr);
}

TEST("GenericMonomorphizer rejects a call site naming an unknown generic function")
{
    EXPECT_THROWS(monomorphize("a = ghost<i32>(1)"));
}
