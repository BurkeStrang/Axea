#pragma once

#include "ast/Stmt.hpp"
#include "lexer/Token.hpp"

#include <memory>
#include <vector>

class Parser
{
public:
    explicit Parser(std::vector<Token> tokens);

    std::unique_ptr<Stmt> parseStatement();

private:
    const Token& current() const;
    const Token& peek(std::size_t offset = 1) const;
    const Token& advance();
    bool match(TokenKind kind);
    const Token& expect(TokenKind kind, const char* message);

    std::unique_ptr<Expr> parseExpression(int minPrecedence = 0);
    std::unique_ptr<Expr> parsePrimary();
    int precedence(TokenKind kind) const;

    std::vector<Token> tokens_;
    std::size_t index_ {0};
};
