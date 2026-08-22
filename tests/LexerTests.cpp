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

TEST("Lexer tokenizes a string literal containing a nested string literal inside an "
     "interpolation span as a single String token, not truncated at the nested string's own "
     "opening quote (see docs/language/0049-printing-formatting.md's own follow-up)")
{
    Lexer lexer(R"("First two: {numbers[..2].join(",")}")");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::String);
    EXPECT_EQ(tokens[0].text, R"("First two: {numbers[..2].join(",")}")");
    EXPECT_EQ(tokens[1].kind, TokenKind::EndOfFile);
}

TEST("Lexer tokenizes a string literal with two interpolation spans, each containing its own "
     "nested string literal, as one String token")
{
    Lexer lexer(R"("{a.join(",")} and {b.join("-")}")");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::String);
    EXPECT_EQ(tokens[0].text, R"("{a.join(",")} and {b.join("-")}")");
    EXPECT_EQ(tokens[1].kind, TokenKind::EndOfFile);
}

TEST("Lexer still tokenizes doubled '{{'/'}}' literal braces correctly, unaffected by the new "
     "brace-depth tracking")
{
    Lexer lexer(R"("Set = {{1, 2, 3}}")");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::String);
    EXPECT_EQ(tokens[0].text, R"("Set = {{1, 2, 3}}")");
}

TEST("Lexer still reports Invalid for a string literal with no closing quote at all, even "
     "with the new interpolation-aware scan")
{
    Lexer lexer(R"("hello)");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::Invalid);
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

TEST("Lexer tokenizes char literals, including multi-byte UTF-8 content")
{
    Lexer lexer("'A' 'é' '🚀'");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::Char);
    EXPECT_EQ(tokens[0].text, "'A'");
    EXPECT_EQ(tokens[1].kind, TokenKind::Char);
    EXPECT_EQ(tokens[1].text, "'é'");
    EXPECT_EQ(tokens[2].kind, TokenKind::Char);
    EXPECT_EQ(tokens[2].text, "'🚀'");
    EXPECT_EQ(tokens[3].kind, TokenKind::EndOfFile);
}

TEST("Lexer marks an unterminated char literal as Invalid")
{
    Lexer lexer("'A");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::Invalid);
}

TEST("Lexer tokenizes the 'extern' keyword")
{
    Lexer lexer("extern c puts");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::Extern);
    EXPECT_EQ(tokens[1].kind, TokenKind::Identifier);
    EXPECT_EQ(tokens[1].text, "c");
    EXPECT_EQ(tokens[2].kind, TokenKind::Identifier);
}

TEST("Lexer tokenizes increment and decrement operators")
{
    Lexer lexer("++ -- + -");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::PlusPlus);
    EXPECT_EQ(tokens[1].kind, TokenKind::MinusMinus);
    EXPECT_EQ(tokens[2].kind, TokenKind::Plus);
    EXPECT_EQ(tokens[3].kind, TokenKind::Minus);
    EXPECT_EQ(tokens[4].kind, TokenKind::EndOfFile);
}

TEST("Lexer tokenizes an i64-suffixed integer literal, keeping the suffix in Token.text (see "
     "docs/language/0005-type-system.md)")
{
    Lexer lexer("100i64");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::Int64);
    EXPECT_EQ(tokens[0].text, "100i64");
    EXPECT_EQ(tokens[1].kind, TokenKind::EndOfFile);
}

TEST("Lexer tokenizes a bare decimal float literal as Float, defaulting to f64 with no suffix "
     "required")
{
    Lexer lexer("1.5");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::Float);
    EXPECT_EQ(tokens[0].text, "1.5");
}

TEST("Lexer tokenizes an f64-suffixed whole-number literal (no decimal point) as Float")
{
    Lexer lexer("100f64");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::Float);
    EXPECT_EQ(tokens[0].text, "100f64");
}

TEST("Lexer does not consume '..' (a slice range) as a float's decimal point - '5..7' stays "
     "Integer, DotDot, Integer (see docs/language/0032-slices.md)")
{
    Lexer lexer("5..7");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::Integer);
    EXPECT_EQ(tokens[0].text, "5");
    EXPECT_EQ(tokens[1].kind, TokenKind::DotDot);
    EXPECT_EQ(tokens[2].kind, TokenKind::Integer);
    EXPECT_EQ(tokens[2].text, "7");
}

TEST("Lexer does not treat 'i64' as a suffix when immediately followed by another identifier "
     "character - only a full, non-identifier-continuing match counts")
{
    Lexer lexer("100i64x");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::Integer);
    EXPECT_EQ(tokens[0].text, "100");
    EXPECT_EQ(tokens[1].kind, TokenKind::Identifier);
    EXPECT_EQ(tokens[1].text, "i64x");
}

TEST("Lexer tokenizes the 'as' keyword")
{
    Lexer lexer("x as i64");
    const auto tokens = lexer.lex();

    EXPECT_EQ(tokens[0].kind, TokenKind::Identifier);
    EXPECT_EQ(tokens[1].kind, TokenKind::As);
    EXPECT_EQ(tokens[2].kind, TokenKind::Identifier);
}
