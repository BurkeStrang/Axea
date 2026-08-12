#include "TestFramework.hpp"

#include "lexer/Lexer.hpp"

TEST("Lexer tokenizes assignment with arithmetic precedence")
{
    Lexer lexer("x = 1 + 2 * 3");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens.size(), static_cast<std::size_t>(8));
    EXPECT_EQ(tokens[0].kind, TokenKind::Identifier);
    EXPECT_EQ(tokens[1].kind, TokenKind::Equal);
    EXPECT_EQ(tokens[2].kind, TokenKind::Integer);
    EXPECT_EQ(tokens[3].kind, TokenKind::Plus);
    EXPECT_EQ(tokens[4].kind, TokenKind::Integer);
    EXPECT_EQ(tokens[5].kind, TokenKind::Star);
    EXPECT_EQ(tokens[6].kind, TokenKind::Integer);
    EXPECT_EQ(tokens[7].kind, TokenKind::EndOfFile);
}

TEST("Lexer tokenizes comparison operators")
{
    Lexer lexer("< <= > >= == !=");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::Less);
    EXPECT_EQ(tokens[1].kind, TokenKind::LessEqual);
    EXPECT_EQ(tokens[2].kind, TokenKind::Greater);
    EXPECT_EQ(tokens[3].kind, TokenKind::GreaterEqual);
    EXPECT_EQ(tokens[4].kind, TokenKind::EqualEqual);
    EXPECT_EQ(tokens[5].kind, TokenKind::BangEqual);
    EXPECT_EQ(tokens[6].kind, TokenKind::EndOfFile);
}

TEST("Lexer tokenizes boolean keywords and string literals")
{
    Lexer lexer(R"(true false "hi")");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::True);
    EXPECT_EQ(tokens[1].kind, TokenKind::False);
    EXPECT_EQ(tokens[2].kind, TokenKind::String);
    EXPECT_EQ(tokens[2].text, "\"hi\"");
    EXPECT_EQ(tokens[3].kind, TokenKind::EndOfFile);
}

TEST("Lexer tokenizes Phase 2 punctuation")
{
    Lexer lexer(", . -> =>");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::Comma);
    EXPECT_EQ(tokens[1].kind, TokenKind::Dot);
    EXPECT_EQ(tokens[2].kind, TokenKind::Arrow);
    EXPECT_EQ(tokens[3].kind, TokenKind::FatArrow);
    EXPECT_EQ(tokens[4].kind, TokenKind::EndOfFile);
}

TEST("Lexer tokenizes Phase 2 keywords")
{
    Lexer lexer("struct pub return read write take");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::Struct);
    EXPECT_EQ(tokens[1].kind, TokenKind::Pub);
    EXPECT_EQ(tokens[2].kind, TokenKind::Return);
    EXPECT_EQ(tokens[3].kind, TokenKind::Read);
    EXPECT_EQ(tokens[4].kind, TokenKind::Write);
    EXPECT_EQ(tokens[5].kind, TokenKind::Take);
    EXPECT_EQ(tokens[6].kind, TokenKind::EndOfFile);
}
