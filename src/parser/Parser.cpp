#include "parser/Parser.hpp"

#include <stdexcept>

Parser::Parser(std::vector<Token> tokens)
    : tokens_(std::move(tokens))
{
}

std::unique_ptr<Stmt> Parser::parseStatement()
{
    const auto& name = expect(TokenKind::Identifier, "expected identifier");
    expect(TokenKind::Equal, "expected '=' after identifier");
    auto value = parseExpression();
    expect(TokenKind::EndOfFile, "expected end of file");

    return std::make_unique<AssignmentStmt>(name.text, std::move(value));
}

const Token& Parser::current() const
{
    return tokens_[index_];
}

const Token& Parser::peek(std::size_t offset) const
{
    const auto pos = index_ + offset;
    return tokens_[pos < tokens_.size() ? pos : tokens_.size() - 1];
}

const Token& Parser::advance()
{
    const auto& token = current();
    if (index_ + 1 < tokens_.size())
    {
        ++index_;
    }
    return token;
}

bool Parser::match(TokenKind kind)
{
    if (current().kind != kind)
    {
        return false;
    }
    advance();
    return true;
}

const Token& Parser::expect(TokenKind kind, const char* message)
{
    if (current().kind != kind)
    {
        throw std::runtime_error(message);
    }
    return advance();
}

std::unique_ptr<Expr> Parser::parseExpression(int minPrecedence)
{
    auto left = parsePrimary();

    while (true)
    {
        const int currentPrecedence = precedence(current().kind);
        if (currentPrecedence < minPrecedence)
        {
            break;
        }

        const auto op = advance().kind;
        auto right = parseExpression(currentPrecedence + 1);
        left = std::make_unique<BinaryExpr>(std::move(left), op, std::move(right));
    }

    return left;
}

std::unique_ptr<Expr> Parser::parsePrimary()
{
    if (match(TokenKind::LeftParen))
    {
        auto expr = parseExpression();
        expect(TokenKind::RightParen, "expected ')' after expression");
        return expr;
    }

    if (current().kind == TokenKind::Integer)
    {
        const auto token = advance();
        return std::make_unique<IntegerExpr>(std::stoll(token.text));
    }

    if (current().kind == TokenKind::Identifier)
    {
        return std::make_unique<NameExpr>(advance().text);
    }

    throw std::runtime_error("expected expression");
}

int Parser::precedence(TokenKind kind) const
{
    switch (kind)
    {
        case TokenKind::Plus:
        case TokenKind::Minus:
            return 10;
        case TokenKind::Star:
        case TokenKind::Slash:
            return 20;
        default:
            return -1;
    }
}
