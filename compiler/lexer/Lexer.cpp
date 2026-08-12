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

    advance();

    switch (c)
    {
        case '+': return makeToken(TokenKind::Plus, start, startLine, startColumn);
        case '*': return makeToken(TokenKind::Star, start, startLine, startColumn);
        case '/': return makeToken(TokenKind::Slash, start, startLine, startColumn);
        case '(': return makeToken(TokenKind::LeftParen, start, startLine, startColumn);
        case ')': return makeToken(TokenKind::RightParen, start, startLine, startColumn);
        case '{': return makeToken(TokenKind::LeftBrace, start, startLine, startColumn);
        case '}': return makeToken(TokenKind::RightBrace, start, startLine, startColumn);
        case ':': return makeToken(TokenKind::Colon, start, startLine, startColumn);
        case ',': return makeToken(TokenKind::Comma, start, startLine, startColumn);
        case '.': return makeToken(TokenKind::Dot, start, startLine, startColumn);
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

    return makeToken(TokenKind::Integer, start, startLine, startColumn);
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

    static const std::unordered_map<std::string, TokenKind> keywords{{"if", TokenKind::If},
                                                                     {"else", TokenKind::Else},
                                                                     {"struct", TokenKind::Struct},
                                                                     {"pub", TokenKind::Pub},
                                                                     {"true", TokenKind::True},
                                                                     {"false", TokenKind::False},
                                                                     {"return", TokenKind::Return},
                                                                     {"read", TokenKind::Read},
                                                                     {"write", TokenKind::Write},
                                                                     {"take", TokenKind::Take}};

    if (const auto it = keywords.find(token.text); it != keywords.end())
    {
        token.kind = it->second;
    }

    return token;
}

Token Lexer::lexString()
{
    const auto start = index_;
    const auto startLine = line_;
    const auto startColumn = column_;

    advance(); // opening quote
    while (!atEnd() && current() != '"')
    {
        advance();
    }

    if (current() == '"')
    {
        advance();
        return makeToken(TokenKind::String, start, startLine, startColumn);
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
