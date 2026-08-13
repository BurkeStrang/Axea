#include "parser/Parser.hpp"

#include <stdexcept>

Parser::Parser(std::vector<Token> tokens)
    : tokens_(std::move(tokens))
{
}

Program Parser::parseProgram()
{
    Program program;
    while (current().kind != TokenKind::EndOfFile)
    {
        program.items.push_back(parseItem());
    }
    return program;
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

std::unique_ptr<Stmt> Parser::parseItem()
{
    match(TokenKind::Pub); // visibility is parsed and discarded; no module system yet

    if (current().kind == TokenKind::Struct)
    {
        return parseStructDecl();
    }

    if (current().kind == TokenKind::Identifier && peek().kind == TokenKind::LeftParen)
    {
        return parseFunctionDecl();
    }

    return parseAssignment();
}

Param Parser::parseParam()
{
    std::optional<Capability> capability;
    if (current().kind == TokenKind::Read)
    {
        capability = Capability::Read;
        advance();
    }
    else if (current().kind == TokenKind::Write)
    {
        capability = Capability::Write;
        advance();
    }
    else if (current().kind == TokenKind::Take)
    {
        capability = Capability::Take;
        advance();
    }

    const auto& name = expect(TokenKind::Identifier, "expected parameter name");
    expect(TokenKind::Colon, "expected ':' after parameter name");
    const auto& type = expect(TokenKind::Identifier, "expected parameter type");
    return Param{name.text, type.text, capability};
}

std::unique_ptr<Stmt> Parser::parseFunctionDecl()
{
    const auto& name = expect(TokenKind::Identifier, "expected function name");
    expect(TokenKind::LeftParen, "expected '(' after function name");

    std::vector<Param> params;
    if (current().kind != TokenKind::RightParen)
    {
        params.push_back(parseParam());
        while (match(TokenKind::Comma))
        {
            if (current().kind == TokenKind::RightParen)
            {
                break;
            }
            params.push_back(parseParam());
        }
    }
    expect(TokenKind::RightParen, "expected ')' after parameters");

    std::optional<std::string> returnType;
    if (match(TokenKind::Arrow))
    {
        const auto& type = expect(TokenKind::Identifier, "expected return type");
        returnType = type.text;
    }

    std::unique_ptr<Expr> body;
    if (match(TokenKind::FatArrow))
    {
        // `=>` is sugar for `{ return expr }`, not for "the block's result" -
        // functions require an explicit return (see
        // docs/language/0027-explicit-return.md), and desugaring to an
        // actual ReturnStmt keeps that true with no special-casing needed
        // anywhere else in the pipeline.
        auto expr = parseExpression();
        std::vector<std::unique_ptr<Stmt>> statements;
        statements.push_back(std::make_unique<ReturnStmt>(std::move(expr)));
        body = std::make_unique<BlockExpr>(std::move(statements), nullptr);
    }
    else
    {
        body = parseBlock();
    }

    return std::make_unique<FunctionDecl>(
        name.text, std::move(params), returnType, std::move(body));
}

std::unique_ptr<Stmt> Parser::parseStructDecl()
{
    expect(TokenKind::Struct, "expected 'struct'");
    const auto& name = expect(TokenKind::Identifier, "expected struct name");
    expect(TokenKind::LeftBrace, "expected '{' after struct name");

    std::vector<Field> fields;
    while (current().kind == TokenKind::Identifier)
    {
        const auto& fieldName = advance();
        expect(TokenKind::Colon, "expected ':' after field name");
        const auto& fieldType = expect(TokenKind::Identifier, "expected field type");
        fields.push_back(Field{fieldName.text, fieldType.text});
    }
    expect(TokenKind::RightBrace, "expected '}' after struct fields");

    return std::make_unique<StructDecl>(name.text, std::move(fields));
}

std::unique_ptr<Stmt> Parser::parseAssignment()
{
    const auto& name = expect(TokenKind::Identifier, "expected identifier");

    std::optional<std::string> declaredType;
    if (match(TokenKind::Colon))
    {
        const auto& type = expect(TokenKind::Identifier, "expected type name");
        declaredType = type.text;
    }

    expect(TokenKind::Equal, "expected '=' after identifier");
    auto value = parseExpression();

    return std::make_unique<AssignmentStmt>(name.text, declaredType, std::move(value));
}

std::unique_ptr<Stmt> Parser::parseReturn()
{
    expect(TokenKind::Return, "expected 'return'");

    std::unique_ptr<Expr> value;
    if (current().kind != TokenKind::RightBrace)
    {
        value = parseExpression();
    }

    return std::make_unique<ReturnStmt>(std::move(value));
}

std::unique_ptr<Expr> Parser::parseBlock()
{
    expect(TokenKind::LeftBrace, "expected '{'");

    std::vector<std::unique_ptr<Stmt>> statements;
    std::unique_ptr<Expr> result;

    while (current().kind != TokenKind::RightBrace)
    {
        if (current().kind == TokenKind::Return)
        {
            statements.push_back(parseReturn());
            continue;
        }

        // Not obviously a keyword-led statement: parse an expression (which
        // naturally stops before '=', ':', '++'/'--', since none of those are
        // infix operators) and see what follows to decide what it was.
        auto expr = parseExpression();

        if (match(TokenKind::Colon))
        {
            auto* name = dynamic_cast<NameExpr*>(expr.get());
            if (!name)
            {
                throw std::runtime_error("expected a name before ':' in a local binding");
            }
            const auto& type = expect(TokenKind::Identifier, "expected type name");
            expect(TokenKind::Equal, "expected '=' after type annotation");
            auto value = parseExpression();
            statements.push_back(
                std::make_unique<AssignmentStmt>(name->name, type.text, std::move(value)));
            continue;
        }

        if (match(TokenKind::Equal))
        {
            auto value = parseExpression();
            if (auto* name = dynamic_cast<NameExpr*>(expr.get()))
            {
                statements.push_back(
                    std::make_unique<AssignmentStmt>(name->name, std::nullopt, std::move(value)));
            }
            else if (auto* field = dynamic_cast<FieldExpr*>(expr.get()))
            {
                statements.push_back(std::make_unique<FieldAssignStmt>(
                    std::move(field->object), field->field, std::move(value)));
            }
            else
            {
                throw std::runtime_error("invalid assignment target");
            }
            continue;
        }

        if (current().kind == TokenKind::PlusPlus || current().kind == TokenKind::MinusMinus)
        {
            const bool increment = current().kind == TokenKind::PlusPlus;
            advance();
            if (!dynamic_cast<NameExpr*>(expr.get()) && !dynamic_cast<FieldExpr*>(expr.get()))
            {
                throw std::runtime_error("invalid increment/decrement target");
            }
            statements.push_back(std::make_unique<IncDecStmt>(std::move(expr), increment));
            continue;
        }

        if (current().kind == TokenKind::RightBrace)
        {
            result = std::move(expr);
            break;
        }

        // A non-trailing expression kept for its side effect (e.g. an
        // early-return guard clause); its value is discarded.
        statements.push_back(std::make_unique<ExprStmt>(std::move(expr)));
    }

    expect(TokenKind::RightBrace, "expected '}' after block");

    return std::make_unique<BlockExpr>(std::move(statements), std::move(result));
}

std::unique_ptr<Expr> Parser::parseIfExpr()
{
    expect(TokenKind::If, "expected 'if'");
    auto condition = parseExpression(0, /*allowStructLiteral=*/false);
    auto thenBranch = parseBlock();

    std::unique_ptr<Expr> elseBranch;
    if (match(TokenKind::Else))
    {
        elseBranch = current().kind == TokenKind::If ? parseIfExpr() : parseBlock();
    }
    else
    {
        // No `else`: desugar to an empty unit block so branch-type checking
        // never has to special-case a missing else.
        elseBranch = std::make_unique<BlockExpr>(std::vector<std::unique_ptr<Stmt>>{}, nullptr);
    }

    return std::make_unique<IfExpr>(
        std::move(condition), std::move(thenBranch), std::move(elseBranch));
}

std::vector<std::pair<std::string, std::unique_ptr<Expr>>> Parser::parseStructLiteralFields()
{
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> fields;

    while (current().kind == TokenKind::Identifier)
    {
        const auto& fieldName = advance();

        std::unique_ptr<Expr> value;
        if (match(TokenKind::Colon))
        {
            value = parseExpression();
        }
        else
        {
            value = std::make_unique<NameExpr>(fieldName.text); // shorthand: field: field
        }

        fields.emplace_back(fieldName.text, std::move(value));
        match(TokenKind::Comma); // optional separator
    }

    return fields;
}

std::unique_ptr<Expr> Parser::parseExpression(int minPrecedence, bool allowStructLiteral)
{
    auto left = parsePostfix(allowStructLiteral);

    while (true)
    {
        const int currentPrecedence = precedence(current().kind);
        if (currentPrecedence < minPrecedence)
        {
            break;
        }

        const auto op = advance().kind;
        auto right = parseExpression(currentPrecedence + 1, allowStructLiteral);
        left = std::make_unique<BinaryExpr>(std::move(left), op, std::move(right));
    }

    return left;
}

std::unique_ptr<Expr> Parser::parsePostfix(bool allowStructLiteral)
{
    auto expr = parsePrimary(allowStructLiteral);

    while (match(TokenKind::Dot))
    {
        const auto& field = expect(TokenKind::Identifier, "expected field name after '.'");
        expr = std::make_unique<FieldExpr>(std::move(expr), field.text);
    }

    return expr;
}

std::unique_ptr<Expr> Parser::parsePrimary(bool allowStructLiteral)
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

    if (current().kind == TokenKind::String)
    {
        const auto token = advance();
        return std::make_unique<StringExpr>(token.text.substr(1, token.text.size() - 2));
    }

    if (match(TokenKind::True))
    {
        return std::make_unique<BoolExpr>(true);
    }

    if (match(TokenKind::False))
    {
        return std::make_unique<BoolExpr>(false);
    }

    if (current().kind == TokenKind::If)
    {
        return parseIfExpr();
    }

    if (current().kind == TokenKind::Identifier)
    {
        if (peek().kind == TokenKind::LeftParen)
        {
            const auto& name = advance();
            expect(TokenKind::LeftParen, "expected '(' after function name");

            std::vector<std::unique_ptr<Expr>> args;
            if (current().kind != TokenKind::RightParen)
            {
                args.push_back(parseExpression());
                while (match(TokenKind::Comma))
                {
                    if (current().kind == TokenKind::RightParen)
                    {
                        break;
                    }
                    args.push_back(parseExpression());
                }
            }
            expect(TokenKind::RightParen, "expected ')' after arguments");

            return std::make_unique<CallExpr>(name.text, std::move(args));
        }

        if (allowStructLiteral && peek().kind == TokenKind::LeftBrace)
        {
            const auto& name = advance();
            expect(TokenKind::LeftBrace, "expected '{' after struct type name");
            auto fields = parseStructLiteralFields();
            expect(TokenKind::RightBrace, "expected '}' after struct literal fields");

            return std::make_unique<StructLiteralExpr>(name.text, std::move(fields));
        }

        return std::make_unique<NameExpr>(advance().text);
    }

    throw std::runtime_error("expected expression");
}

int Parser::precedence(TokenKind kind) const
{
    switch (kind)
    {
        case TokenKind::EqualEqual:
        case TokenKind::BangEqual: return 5;
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual: return 7;
        case TokenKind::Plus:
        case TokenKind::Minus: return 10;
        case TokenKind::Star:
        case TokenKind::Slash: return 20;
        default: return -1;
    }
}
