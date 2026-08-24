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
    // Scans from an opening '"'/'"""' to that string literal's own real
    // closing quote run of the same length (`quoteLen` 1 or 3 - see
    // docs/language/0060-multiline-strings.md), consuming '{...}'
    // interpolation spans as real Axea code (so a quote inside one
    // doesn't end the string early) and recursively skipping any string
    // literal nested inside an active span (always `quoteLen == 1` for
    // the nested call - a nested literal inside `{...}` is always an
    // ordinary single-quoted string, never itself triple-quoted), to
    // arbitrary depth (see docs/language/Axea_Printing_Formatting.md's
    // own "nested string literals inside interpolation expressions"
    // requirement, and docs/language/0049-printing-formatting.md's
    // follow-up that implemented it). When `quoteLen == 3`, a lone `"`
    // (not part of a 3-in-a-row run) at brace depth 0 is ordinary literal
    // content, not a closing quote - the loop's structure falls through
    // to the plain `advance()` at the bottom for that case. Returns false
    // if input runs out before a real closing quote run is found
    // (lexString then reports Invalid, exactly as it always has for an
    // unterminated string).
    bool scanStringSpan(int quoteLen);
    // Raw-string counterpart of scanStringSpan (see
    // docs/language/0059-raw-strings.md): a raw string disables
    // interpolation entirely, so this never tracks brace depth or
    // recurses into nested literals - it just scans for the next
    // `quoteLen`-quote run, full stop.
    bool scanRawStringSpan(int quoteLen);
    Token lexChar();
    Token makeToken(TokenKind kind, std::size_t start, std::size_t line, std::size_t column) const;

    std::string_view source_;
    std::size_t index_{0};
    std::size_t line_{1};
    std::size_t column_{1};
};
