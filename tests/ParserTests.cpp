#include "TestFramework.hpp"

#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"

TEST("Parser builds assignment AST with correct operator precedence")
{
    Lexer lexer("x = 1 + 2 * 3");
    Parser parser(lexer.lex());
    auto stmt = parser.parseStatement();

    auto* assignment = dynamic_cast<AssignmentStmt*>(stmt.get());
    EXPECT_TRUE(assignment != nullptr);
    EXPECT_EQ(assignment->name, "x");

    auto* add = dynamic_cast<BinaryExpr*>(assignment->value.get());
    EXPECT_TRUE(add != nullptr);
    EXPECT_EQ(add->op, TokenKind::Plus);

    auto* multiply = dynamic_cast<BinaryExpr*>(add->right.get());
    EXPECT_TRUE(multiply != nullptr);
    EXPECT_EQ(multiply->op, TokenKind::Star);
}

TEST("Parser builds if-expression AST with condition and both branches")
{
    Lexer lexer("x = if 1 < 2 { 10 } else { 20 }");
    Parser parser(lexer.lex());
    auto stmt = parser.parseStatement();

    auto* assignment = dynamic_cast<AssignmentStmt*>(stmt.get());
    EXPECT_TRUE(assignment != nullptr);

    auto* ifExpr = dynamic_cast<IfExpr*>(assignment->value.get());
    EXPECT_TRUE(ifExpr != nullptr);

    auto* condition = dynamic_cast<BinaryExpr*>(ifExpr->condition.get());
    EXPECT_TRUE(condition != nullptr);
    EXPECT_EQ(condition->op, TokenKind::Less);

    auto* thenBranch = dynamic_cast<IntegerExpr*>(ifExpr->thenBranch.get());
    EXPECT_TRUE(thenBranch != nullptr);
    EXPECT_EQ(thenBranch->value, 10);

    auto* elseBranch = dynamic_cast<IntegerExpr*>(ifExpr->elseBranch.get());
    EXPECT_TRUE(elseBranch != nullptr);
    EXPECT_EQ(elseBranch->value, 20);
}

TEST("Parser builds string and boolean literal AST nodes")
{
    Lexer lexer(R"(x = "hi")");
    Parser parser(lexer.lex());
    auto stmt = parser.parseStatement();

    auto* assignment = dynamic_cast<AssignmentStmt*>(stmt.get());
    EXPECT_TRUE(assignment != nullptr);

    auto* string = dynamic_cast<StringExpr*>(assignment->value.get());
    EXPECT_TRUE(string != nullptr);
    EXPECT_EQ(string->value, "hi");
}
