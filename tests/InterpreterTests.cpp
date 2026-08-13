#include "TestFramework.hpp"

#include "interpreter/Interpreter.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"

namespace
{
    Program parse(const std::string& source)
    {
        Lexer lexer(source);
        Parser parser(lexer.lex());
        return parser.parseProgram();
    }

    std::unordered_map<std::string, Value> runProgram(const std::string& source)
    {
        auto program = parse(source);
        Interpreter interpreter;
        interpreter.run(program);
        return interpreter.variables();
    }

    Value run(const std::string& source)
    {
        return runProgram(source).at("x");
    }
} // namespace

TEST("Interpreter evaluates arithmetic with operator precedence")
{
    EXPECT_EQ(std::get<std::int64_t>(run("x = 1 + 2 * 3")), 7);
}

TEST("Interpreter evaluates parenthesized expressions")
{
    EXPECT_EQ(std::get<std::int64_t>(run("x = (1 + 2) * 3 - 1")), 8);
}

TEST("Interpreter throws on division by zero")
{
    EXPECT_THROWS(run("x = 1 / 0"));
}

TEST("Interpreter throws on undefined variable")
{
    EXPECT_THROWS(run("y = x + 1"));
}

TEST("Interpreter evaluates boolean literals")
{
    EXPECT_EQ(std::get<bool>(run("x = true")), true);
    EXPECT_EQ(std::get<bool>(run("x = false")), false);
}

TEST("Interpreter evaluates string literals")
{
    EXPECT_EQ(std::get<std::string>(run(R"(x = "hello")")), "hello");
}

TEST("Interpreter evaluates comparison operators")
{
    EXPECT_EQ(std::get<bool>(run("x = 1 < 2")), true);
    EXPECT_EQ(std::get<bool>(run("x = 2 <= 2")), true);
    EXPECT_EQ(std::get<bool>(run("x = 3 > 2")), true);
    EXPECT_EQ(std::get<bool>(run("x = 2 >= 3")), false);
    EXPECT_EQ(std::get<bool>(run("x = 2 == 2")), true);
    EXPECT_EQ(std::get<bool>(run("x = 2 != 2")), false);
}

TEST("Interpreter evaluates comparisons with correct precedence against arithmetic")
{
    EXPECT_EQ(std::get<bool>(run("x = 1 + 1 == 2")), true);
}

TEST("Interpreter evaluates if-expression true and false branches")
{
    EXPECT_EQ(std::get<std::int64_t>(run("x = if 1 < 2 { 10 } else { 20 }")), 10);
    EXPECT_EQ(std::get<std::int64_t>(run("x = if 1 > 2 { 10 } else { 20 }")), 20);
}

TEST("Interpreter throws when if condition is not a boolean")
{
    EXPECT_THROWS(run("x = if 1 { 10 } else { 20 }"));
}

TEST("Interpreter throws when arithmetic operand is not an integer")
{
    EXPECT_THROWS(run(R"(x = 1 + "oops")"));
}

TEST("Interpreter calls a function and evaluates its block result")
{
    EXPECT_EQ(std::get<std::int64_t>(run("square(n: i32) -> i32 { return n * n }  x = square(6)")),
              36);
}

TEST("Interpreter evaluates a fat-arrow function body")
{
    EXPECT_EQ(std::get<std::int64_t>(run("double(n: i32) -> i32 => n * 2  x = double(21)")), 42);
}

TEST("Interpreter handles recursive calls via early return and forward reference")
{
    const std::string source = "factorial(n: i32) -> i32 { "
                               "  if n <= 1 { return 1 } "
                               "  return n * factorial(n - 1) "
                               "} "
                               "x = factorial(5)";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 120);
}

TEST("Interpreter functions do not see top-level globals")
{
    EXPECT_THROWS(runProgram("g = 100  f() -> i32 { return g }  x = f()"));
}

TEST("Interpreter function parameters shadow same-named top-level variables independently")
{
    // The top-level `n` and the parameter `n` never interact.
    const std::string source = "n = 999  identity(n: i32) -> i32 { return n }  x = identity(7)";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 7);
}

TEST("Interpreter throws on wrong argument count")
{
    EXPECT_THROWS(runProgram("f(a: i32, b: i32) -> i32 { return a + b }  x = f(1)"));
}

TEST("Interpreter throws on call to an undefined function")
{
    EXPECT_THROWS(runProgram("x = missing(1)"));
}

TEST("Interpreter constructs a struct and reads its fields")
{
    const std::string source = "struct Point { x: i32  y: i32 } "
                               "p = Point { x: 3  y: 4 } "
                               "x = p.x + p.y";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 7);
}

TEST("Interpreter builds struct literal fields in declared order regardless of source order")
{
    const std::string source = "struct Point { x: i32  y: i32 } "
                               "x = Point { y: 2  x: 1 }";
    const auto instance = std::get<std::shared_ptr<StructInstance>>(run(source));
    EXPECT_EQ(instance->fields[0].first, "x");
    EXPECT_EQ(instance->fields[1].first, "y");
}

TEST("Interpreter throws on struct literal missing a field")
{
    EXPECT_THROWS(runProgram("struct Point { x: i32  y: i32 }  x = Point { x: 1 }"));
}

TEST("Interpreter throws on field access on a non-struct value")
{
    EXPECT_THROWS(run("x = (1).field"));
}

TEST("Interpreter throws on access to an undefined field")
{
    EXPECT_THROWS(runProgram("struct Point { x: i32 }  p = Point { x: 1 }  x = p.y"));
}

TEST("toString formats a struct instance deterministically by declared field order")
{
    const std::string source = "struct Point { x: i32  y: i32 } "
                               "x = Point { x: 1  y: 2 }";
    EXPECT_EQ(toString(run(source)), "Point { x: 1, y: 2 }");
}

TEST("Interpreter field assignment mutates the shared struct instance")
{
    const std::string source = "struct Point { x: i32 } "
                               "update(p: Point) -> i32 { p.x = 99  return p.x } "
                               "p = Point { x: 1 } "
                               "called = update(p) "
                               "x = p.x";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 99);
}

TEST("Interpreter field increment mutates the shared struct instance")
{
    const std::string source = "struct Point { x: i32 } "
                               "bump(p: Point) -> i32 { p.x++  return p.x } "
                               "p = Point { x: 1 } "
                               "called = bump(p) "
                               "x = p.x";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 2);
}

TEST("Interpreter increment of a plain parameter mutates it through nested blocks")
{
    const std::string source = "bump(n: i32) -> i32 { "
                               "  if n > 0 { n++ } "
                               "  return n "
                               "} "
                               "x = bump(5)";
    EXPECT_EQ(std::get<std::int64_t>(run(source)), 6);
}

TEST("Interpreter decrement works on a plain parameter")
{
    EXPECT_EQ(std::get<std::int64_t>(run("f(n: i32) -> i32 { n--  return n }  x = f(5)")), 4);
}
