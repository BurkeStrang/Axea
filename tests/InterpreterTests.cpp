#include "TestFramework.hpp"

#include "interpreter/Interpreter.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"

namespace
{
    Value run(const std::string& source)
    {
        Lexer lexer(source);
        Parser parser(lexer.lex());
        auto stmt = parser.parseStatement();

        Interpreter interpreter;
        interpreter.execute(*stmt);
        return interpreter.variables().at("x");
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
