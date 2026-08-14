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

    std::unique_ptr<Stmt> parseItem();
    std::unique_ptr<Stmt> parseFunctionDecl();
    std::unique_ptr<Stmt> parseStructDecl();
    std::unique_ptr<Stmt> parseAssignment();
    std::unique_ptr<Stmt> parseReturn();
    std::unique_ptr<Stmt> parseWhile();
    std::unique_ptr<Stmt> parseBreak();
    std::unique_ptr<Stmt> parseContinue();
    // `for i in a..b { body }` is pure syntactic sugar, desugared here into
    // `{ i = a  while i < b { body  i++ } }` - no dedicated ForStmt AST node,
    // matching how `=>` already desugars in parseFunctionDecl. Everything
    // downstream of the parser handles the result without any awareness
    // that `for` exists (see docs/language/0029-for-loops.md).
    std::unique_ptr<Stmt> parseFor();
    Param parseParam();
    // A type is either a plain identifier ("i32", "User") or an array type
    // "[elem;N]" (see docs/language/0031-arrays.md), canonicalized here with
    // no spaces so every downstream consumer (TypeChecker::resolveType,
    // LlvmIrEmitter::llvmType) can parse the same fixed shape. Replaces every
    // former `expect(TokenKind::Identifier, "expected ... type")` call site.
    std::string parseTypeName();

    std::unique_ptr<Expr> parseBlock();
    std::unique_ptr<Expr> parseIfExpr();
    std::unique_ptr<Expr> parseLoopExpr();
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> parseStructLiteralFields();

    std::unique_ptr<Expr> parseExpression(int minPrecedence = 0, bool allowStructLiteral = true);
    std::unique_ptr<Expr> parsePostfix(bool allowStructLiteral);
    std::unique_ptr<Expr> parsePrimary(bool allowStructLiteral);
    // Comma-separated expressions up to (not including) the closing ')' -
    // shared by a bare function call and a method call's argument list
    // (docs/language/0033-lists.md), which were previously two copies of the
    // identical loop. Caller has already consumed '(' and is responsible for
    // expecting the closing ')'.
    std::vector<std::unique_ptr<Expr>> parseArgumentList();
    int precedence(TokenKind kind) const;

    std::vector<Token> tokens_;
    std::size_t index_{0};
    // Unique per for-loop, so nested for-loops' internal counter/end names
    // (see parseFor) can never collide with each other.
    int forCounter_{0};
};
