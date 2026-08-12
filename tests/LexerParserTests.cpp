#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"

#include <cassert>
#include <iostream>

int main()
{
    {
        Lexer lexer("x = 1 + 2 * 3");
        const auto tokens = lexer.lex();
        assert(tokens.size() == 8);
        assert(tokens[0].kind == TokenKind::Identifier);
        assert(tokens[1].kind == TokenKind::Equal);
        assert(tokens[2].kind == TokenKind::Integer);
        assert(tokens[3].kind == TokenKind::Plus);
        assert(tokens[4].kind == TokenKind::Integer);
        assert(tokens[5].kind == TokenKind::Star);
        assert(tokens[6].kind == TokenKind::Integer);
        assert(tokens[7].kind == TokenKind::EndOfFile);
    }

    {
        Lexer lexer("x = 1 + 2 * 3");
        Parser parser(lexer.lex());
        auto stmt = parser.parseStatement();
        auto* assignment = dynamic_cast<AssignmentStmt*>(stmt.get());
        assert(assignment != nullptr);
        assert(assignment->name == "x");

        auto* add = dynamic_cast<BinaryExpr*>(assignment->value.get());
        assert(add != nullptr);
        assert(add->op == TokenKind::Plus);

        auto* multiply = dynamic_cast<BinaryExpr*>(add->right.get());
        assert(multiply != nullptr);
        assert(multiply->op == TokenKind::Star);
    }

    std::cout << "All Axea tests passed.\n";
    return 0;
}
