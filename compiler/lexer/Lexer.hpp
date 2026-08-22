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
    // Scans from an opening '"' to that string literal's own real closing
    // '"', consuming '{...}' interpolation spans as real Axea code (so a
    // quote inside one doesn't end the string early) and recursively
    // skipping any string literal nested inside an active span, to
    // arbitrary depth (see docs/language/Axea_Printing_Formatting.md's
    // own "nested string literals inside interpolation expressions"
    // requirement, and docs/language/0049-printing-formatting.md's
    // follow-up that implemented it). Returns false if input runs out
    // before a real closing quote is found (lexString then reports
    // Invalid, exactly as it always has for an unterminated string).
    bool scanStringSpan();
    Token lexChar();
    Token makeToken(TokenKind kind, std::size_t start, std::size_t line, std::size_t column) const;

    std::string_view source_;
    std::size_t index_{0};
    std::size_t line_{1};
    std::size_t column_{1};
};
