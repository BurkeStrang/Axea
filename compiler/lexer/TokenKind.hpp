#pragma once

#include <string_view>

enum class TokenKind
{
    Identifier,
    Integer,
    Int64,
    Float,
    String,
    Char,

    Plus,
    PlusPlus,
    Minus,
    MinusMinus,
    Star,
    Slash,

    Equal,
    EqualEqual,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,

    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,

    Colon,
    Semicolon,
    Comma,
    Dot,
    DotDot,
    Arrow,
    FatArrow,

    If,
    Else,
    Struct,
    Pub,
    True,
    False,
    Return,
    Read,
    Write,
    Take,
    While,
    Loop,
    Break,
    Continue,
    For,
    In,
    Extern,
    As,
    Question,
    Trait,
    Impl,
    Enum,
    Match,
    Pipe,
    Module,
    Use,
    Fn,

    EndOfFile,
    Invalid
};

constexpr std::string_view tokenKindName(TokenKind kind)
{
    switch (kind)
    {
        case TokenKind::Identifier: return "Identifier";
        case TokenKind::Integer: return "Integer";
        case TokenKind::Int64: return "Int64";
        case TokenKind::Float: return "Float";
        case TokenKind::String: return "String";
        case TokenKind::Char: return "Char";
        case TokenKind::Plus: return "Plus";
        case TokenKind::PlusPlus: return "PlusPlus";
        case TokenKind::Minus: return "Minus";
        case TokenKind::MinusMinus: return "MinusMinus";
        case TokenKind::Star: return "Star";
        case TokenKind::Slash: return "Slash";
        case TokenKind::Equal: return "Equal";
        case TokenKind::EqualEqual: return "EqualEqual";
        case TokenKind::BangEqual: return "BangEqual";
        case TokenKind::Less: return "Less";
        case TokenKind::LessEqual: return "LessEqual";
        case TokenKind::Greater: return "Greater";
        case TokenKind::GreaterEqual: return "GreaterEqual";
        case TokenKind::LeftParen: return "LeftParen";
        case TokenKind::RightParen: return "RightParen";
        case TokenKind::LeftBrace: return "LeftBrace";
        case TokenKind::RightBrace: return "RightBrace";
        case TokenKind::LeftBracket: return "LeftBracket";
        case TokenKind::RightBracket: return "RightBracket";
        case TokenKind::Colon: return "Colon";
        case TokenKind::Semicolon: return "Semicolon";
        case TokenKind::Comma: return "Comma";
        case TokenKind::Dot: return "Dot";
        case TokenKind::DotDot: return "DotDot";
        case TokenKind::Arrow: return "Arrow";
        case TokenKind::FatArrow: return "FatArrow";
        case TokenKind::If: return "If";
        case TokenKind::Else: return "Else";
        case TokenKind::Struct: return "Struct";
        case TokenKind::Pub: return "Pub";
        case TokenKind::True: return "True";
        case TokenKind::False: return "False";
        case TokenKind::Return: return "Return";
        case TokenKind::Read: return "Read";
        case TokenKind::Write: return "Write";
        case TokenKind::Take: return "Take";
        case TokenKind::While: return "While";
        case TokenKind::Loop: return "Loop";
        case TokenKind::Break: return "Break";
        case TokenKind::Continue: return "Continue";
        case TokenKind::For: return "For";
        case TokenKind::In: return "In";
        case TokenKind::Extern: return "Extern";
        case TokenKind::As: return "As";
        case TokenKind::Question: return "Question";
        case TokenKind::Trait: return "Trait";
        case TokenKind::Impl: return "Impl";
        case TokenKind::Enum: return "Enum";
        case TokenKind::Match: return "Match";
        case TokenKind::Pipe: return "Pipe";
        case TokenKind::Module: return "Module";
        case TokenKind::Use: return "Use";
        case TokenKind::Fn: return "Fn";
        case TokenKind::EndOfFile: return "EndOfFile";
        case TokenKind::Invalid: return "Invalid";
    }
    return "Unknown";
}
