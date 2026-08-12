#pragma once

#include "Token.hpp"

#include <string_view>
#include <vector>

class Lexer
{
public:
    explicit Lexer(std::string_view source);

    std::vector<Token> lex();

private:
    char current() const;
    char peek(std::size_t offset = 1) const;
    char advance();
    bool atEnd() const;

    void skipWhitespaceAndComments();
    Token nextToken();
    Token lexNumber();
    Token lexIdentifierOrKeyword();
    Token lexString();
    Token makeToken(TokenKind kind, std::size_t start, std::size_t line, std::size_t column) const;

    std::string_view source_;
    std::size_t index_{0};
    std::size_t line_{1};
    std::size_t column_{1};
};
