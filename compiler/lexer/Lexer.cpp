#include "lexer/Lexer.hpp"

#include <cctype>
#include <unordered_map>

Lexer::Lexer(std::string_view source)
    : source_(source)
{
}

std::vector<Token> Lexer::lex()
{
    std::vector<Token> tokens;

    for (;;)
    {
        Token token = nextToken();
        tokens.push_back(token);
        if (token.kind == TokenKind::EndOfFile)
        {
            break;
        }
    }

    return tokens;
}

char Lexer::current() const
{
    return atEnd() ? '\0' : source_[index_];
}

char Lexer::peek(std::size_t offset) const
{
    const auto pos = index_ + offset;
    return pos >= source_.size() ? '\0' : source_[pos];
}

char Lexer::advance()
{
    if (atEnd())
    {
        return '\0';
    }

    const char c = source_[index_++];
    if (c == '\n')
    {
        ++line_;
        column_ = 1;
    }
    else
    {
        ++column_;
    }

    return c;
}

bool Lexer::atEnd() const
{
    return index_ >= source_.size();
}

void Lexer::skipWhitespaceAndComments()
{
    for (;;)
    {
        while (std::isspace(static_cast<unsigned char>(current())))
        {
            advance();
        }

        if (current() == '/' && peek() == '/')
        {
            while (!atEnd() && current() != '\n')
            {
                advance();
            }
            continue;
        }

        break;
    }
}

Token Lexer::nextToken()
{
    skipWhitespaceAndComments();

    const auto start = index_;
    const auto startLine = line_;
    const auto startColumn = column_;

    if (atEnd())
    {
        return Token{TokenKind::EndOfFile, "", line_, column_};
    }

    const char c = current();

    if (std::isdigit(static_cast<unsigned char>(c)))
    {
        return lexNumber();
    }

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
    {
        return lexIdentifierOrKeyword();
    }

    if (c == '"')
    {
        return lexString();
    }

    if (c == '\'')
    {
        return lexChar();
    }

    advance();

    switch (c)
    {
        case '+':
            if (current() == '+')
            {
                advance();
                return makeToken(TokenKind::PlusPlus, start, startLine, startColumn);
            }
            return makeToken(TokenKind::Plus, start, startLine, startColumn);
        case '*': return makeToken(TokenKind::Star, start, startLine, startColumn);
        case '/': return makeToken(TokenKind::Slash, start, startLine, startColumn);
        case '(': return makeToken(TokenKind::LeftParen, start, startLine, startColumn);
        case ')': return makeToken(TokenKind::RightParen, start, startLine, startColumn);
        case '{': return makeToken(TokenKind::LeftBrace, start, startLine, startColumn);
        case '}': return makeToken(TokenKind::RightBrace, start, startLine, startColumn);
        case '[': return makeToken(TokenKind::LeftBracket, start, startLine, startColumn);
        case ']': return makeToken(TokenKind::RightBracket, start, startLine, startColumn);
        case ':': return makeToken(TokenKind::Colon, start, startLine, startColumn);
        case ';': return makeToken(TokenKind::Semicolon, start, startLine, startColumn);
        case ',': return makeToken(TokenKind::Comma, start, startLine, startColumn);
        case '?': return makeToken(TokenKind::Question, start, startLine, startColumn);
        case '.':
            if (current() == '.')
            {
                advance();
                return makeToken(TokenKind::DotDot, start, startLine, startColumn);
            }
            return makeToken(TokenKind::Dot, start, startLine, startColumn);
        case '=':
            if (current() == '=')
            {
                advance();
                return makeToken(TokenKind::EqualEqual, start, startLine, startColumn);
            }
            if (current() == '>')
            {
                advance();
                return makeToken(TokenKind::FatArrow, start, startLine, startColumn);
            }
            return makeToken(TokenKind::Equal, start, startLine, startColumn);
        case '-':
            if (current() == '>')
            {
                advance();
                return makeToken(TokenKind::Arrow, start, startLine, startColumn);
            }
            if (current() == '-')
            {
                advance();
                return makeToken(TokenKind::MinusMinus, start, startLine, startColumn);
            }
            return makeToken(TokenKind::Minus, start, startLine, startColumn);
        case '<':
            if (current() == '=')
            {
                advance();
                return makeToken(TokenKind::LessEqual, start, startLine, startColumn);
            }
            return makeToken(TokenKind::Less, start, startLine, startColumn);
        case '>':
            if (current() == '=')
            {
                advance();
                return makeToken(TokenKind::GreaterEqual, start, startLine, startColumn);
            }
            return makeToken(TokenKind::Greater, start, startLine, startColumn);
        case '!':
            if (current() == '=')
            {
                advance();
                return makeToken(TokenKind::BangEqual, start, startLine, startColumn);
            }
            return makeToken(TokenKind::Invalid, start, startLine, startColumn);
        default: return makeToken(TokenKind::Invalid, start, startLine, startColumn);
    }
}

Token Lexer::lexNumber()
{
    const auto start = index_;
    const auto startLine = line_;
    const auto startColumn = column_;

    while (std::isdigit(static_cast<unsigned char>(current())))
    {
        advance();
    }

    // A '.' starts a fractional part only when followed by a digit -
    // otherwise it's DotDot (a slice range, "5..7") or a bare Dot, neither
    // of which this function should consume (see
    // docs/language/0032-slices.md). f64 is the only float type this
    // phase (see docs/language/0005-type-system.md), so a fractional
    // literal is always f64 regardless of suffix.
    bool isFloat = false;
    if (current() == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))
    {
        isFloat = true;
        advance(); // '.'
        while (std::isdigit(static_cast<unsigned char>(current())))
        {
            advance();
        }
    }

    // A numeric suffix (i64/f64 only this phase) is consumed only on a
    // full, non-identifier-continuing match, so "100i64x" isn't silently
    // misread as an i64 literal followed by a stray "x" - it's left for
    // the identifier lexer to pick up "i64x" as its own (later invalid,
    // in that position) token instead. Token.text keeps the suffix - the
    // parser strips it back off before parsing the numeric text itself
    // (see Parser::parseIntegerLiteral/parseFloatLiteral).
    const auto matchesSuffix = [this](const char* suffix)
    {
        for (std::size_t i = 0; i < 3; ++i)
        {
            if (peek(i) != suffix[i])
            {
                return false;
            }
        }
        const char after = peek(3);
        return !(std::isalnum(static_cast<unsigned char>(after)) || after == '_');
    };

    if (!isFloat && matchesSuffix("i64"))
    {
        advance();
        advance();
        advance();
        return makeToken(TokenKind::Int64, start, startLine, startColumn);
    }
    if (matchesSuffix("f64"))
    {
        advance();
        advance();
        advance();
        return makeToken(TokenKind::Float, start, startLine, startColumn);
    }

    return makeToken(
        isFloat ? TokenKind::Float : TokenKind::Integer, start, startLine, startColumn);
}

Token Lexer::lexIdentifierOrKeyword()
{
    const auto start = index_;
    const auto startLine = line_;
    const auto startColumn = column_;

    while (std::isalnum(static_cast<unsigned char>(current())) || current() == '_')
    {
        advance();
    }

    auto token = makeToken(TokenKind::Identifier, start, startLine, startColumn);

    static const std::unordered_map<std::string, TokenKind> keywords{
        {"if", TokenKind::If},
        {"else", TokenKind::Else},
        {"struct", TokenKind::Struct},
        {"pub", TokenKind::Pub},
        {"true", TokenKind::True},
        {"false", TokenKind::False},
        {"return", TokenKind::Return},
        {"read", TokenKind::Read},
        {"write", TokenKind::Write},
        {"take", TokenKind::Take},
        {"while", TokenKind::While},
        {"loop", TokenKind::Loop},
        {"break", TokenKind::Break},
        {"continue", TokenKind::Continue},
        {"for", TokenKind::For},
        {"in", TokenKind::In},
        {"extern", TokenKind::Extern},
        {"as", TokenKind::As}};

    if (const auto it = keywords.find(token.text); it != keywords.end())
    {
        token.kind = it->second;
    }

    return token;
}

bool Lexer::scanStringSpan()
{
    advance(); // opening quote

    // Depth-tracked so a quote *inside* an active interpolation span
    // (e.g. `"{x.join(",")}"`) is a nested string literal's own opening
    // quote, not this string's own closing one - only a bare '"' at
    // depth 0 ends the scan. '{{'/'}}' are only escaped-literal-brace
    // pairs at depth 0, mirroring Parser::parseStringLiteral's own
    // identical escaping rule exactly, so this scan treats the same
    // source text as an interpolation span that the later parsing pass
    // will.
    int braceDepth = 0;
    while (!atEnd())
    {
        const char c = current();
        if (c == '"' && braceDepth == 0)
        {
            advance();
            return true;
        }
        if (c == '{')
        {
            if (braceDepth == 0 && peek() == '{')
            {
                advance();
                advance();
                continue;
            }
            ++braceDepth;
            advance();
            continue;
        }
        if (c == '}')
        {
            if (braceDepth == 0 && peek() == '}')
            {
                advance();
                advance();
                continue;
            }
            if (braceDepth > 0)
            {
                --braceDepth;
            }
            advance();
            continue;
        }
        if (c == '"' && braceDepth > 0)
        {
            // A nested string literal inside an active span - recurse,
            // so it's skipped over using this exact same logic (handling
            // arbitrary further nesting inside *it*, too), rather than
            // naively scanning to the next bare '"'.
            if (!scanStringSpan())
            {
                return false;
            }
            continue;
        }
        advance();
    }

    return false; // ran out of input before a real (depth-0) closing quote
}

Token Lexer::lexString()
{
    const auto start = index_;
    const auto startLine = line_;
    const auto startColumn = column_;

    if (scanStringSpan())
    {
        return makeToken(TokenKind::String, start, startLine, startColumn);
    }

    return makeToken(TokenKind::Invalid, start, startLine, startColumn);
}

// Scans raw bytes between the opening/closing `'`, exactly like lexString
// scans between `"` - safe for a multi-byte UTF-8 sequence ('é', '🚀')
// even though this only looks at ASCII bytes, since a UTF-8 continuation
// byte (0x80-0xBF) can never equal the ASCII `'` (0x27) this loop is
// looking for. No escape sequences (matches lexString's own "no escapes
// at all" simplification) - decoding the captured bytes into a single
// Unicode scalar value happens later, in the parser (see
// docs/language/0044-char.md).
Token Lexer::lexChar()
{
    const auto start = index_;
    const auto startLine = line_;
    const auto startColumn = column_;

    advance(); // opening quote
    while (!atEnd() && current() != '\'')
    {
        advance();
    }

    if (current() == '\'')
    {
        advance();
        return makeToken(TokenKind::Char, start, startLine, startColumn);
    }

    return makeToken(TokenKind::Invalid, start, startLine, startColumn);
}

Token Lexer::makeToken(TokenKind kind,
                       std::size_t start,
                       std::size_t line,
                       std::size_t column) const
{
    return Token{kind, std::string(source_.substr(start, index_ - start)), line, column};
}
