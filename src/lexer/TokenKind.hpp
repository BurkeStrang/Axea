#pragma once

#include <string_view>

enum class TokenKind
{
    Identifier,
    Integer,
    String,

    Plus,
    Minus,
    Star,
    Slash,

    Equal,
    EqualEqual,

    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,

    Colon,
    Arrow,

    If,
    Else,
    Struct,
    Pub,

    EndOfFile,
    Invalid
};

constexpr std::string_view tokenKindName(TokenKind kind)
{
    switch (kind)
    {
        case TokenKind::Identifier: return "Identifier";
        case TokenKind::Integer: return "Integer";
        case TokenKind::String: return "String";
        case TokenKind::Plus: return "Plus";
        case TokenKind::Minus: return "Minus";
        case TokenKind::Star: return "Star";
        case TokenKind::Slash: return "Slash";
        case TokenKind::Equal: return "Equal";
        case TokenKind::EqualEqual: return "EqualEqual";
        case TokenKind::LeftParen: return "LeftParen";
        case TokenKind::RightParen: return "RightParen";
        case TokenKind::LeftBrace: return "LeftBrace";
        case TokenKind::RightBrace: return "RightBrace";
        case TokenKind::Colon: return "Colon";
        case TokenKind::Arrow: return "Arrow";
        case TokenKind::If: return "If";
        case TokenKind::Else: return "Else";
        case TokenKind::Struct: return "Struct";
        case TokenKind::Pub: return "Pub";
        case TokenKind::EndOfFile: return "EndOfFile";
        case TokenKind::Invalid: return "Invalid";
    }
    return "Unknown";
}
