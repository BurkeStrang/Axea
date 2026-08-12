#pragma once

#include "ast/Stmt.hpp"
#include "lexer/Token.hpp"

#include <memory>
#include <utility>
#include <vector>

class Parser
{
public:
    explicit Parser(std::vector<Token> tokens);

    Program parseProgram();

private:
    const Token& current() const;
    const Token& peek(std::size_t offset = 1) const;
    const Token& advance();
    bool match(TokenKind kind);
    const Token& expect(TokenKind kind, const char* message);

    bool isAssignmentStart() const;

    std::unique_ptr<Stmt> parseItem();
    std::unique_ptr<Stmt> parseFunctionDecl();
    std::unique_ptr<Stmt> parseStructDecl();
    std::unique_ptr<Stmt> parseAssignment();
    std::unique_ptr<Stmt> parseReturn();
    Param parseParam();

    std::unique_ptr<Expr> parseBlock();
    std::unique_ptr<Expr> parseIfExpr();
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> parseStructLiteralFields();

    std::unique_ptr<Expr> parseExpression(int minPrecedence = 0, bool allowStructLiteral = true);
    std::unique_ptr<Expr> parsePostfix(bool allowStructLiteral);
    std::unique_ptr<Expr> parsePrimary(bool allowStructLiteral);
    int precedence(TokenKind kind) const;

    std::vector<Token> tokens_;
    std::size_t index_{0};
};
