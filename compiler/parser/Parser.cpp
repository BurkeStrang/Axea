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

std::string Parser::parseTypeName()
{
    if (match(TokenKind::LeftBracket))
    {
        const auto& element = expect(TokenKind::Identifier, "expected element type in array type");
        expect(TokenKind::Semicolon, "expected ';' in array type");
        const auto& size = expect(TokenKind::Integer, "expected array size in array type");
        expect(TokenKind::RightBracket, "expected ']' after array type");

        return "[" + element.text + ";" + size.text + "]";
    }

    const auto& name = expect(TokenKind::Identifier, "expected type name");

    // "slice<elem>" (docs/language/0032-slices.md) / "List<elem>"
    // (docs/language/0033-lists.md) / "Set<elem>" (docs/language/0034-maps-and-sets.md)
    // / "Stack<elem>" (docs/language/0035-stacks.md) - all reuse the existing
    // Less/Greater tokens (no lexer changes needed: '<'/'>' only mean this
    // here because we're in type position, never expression position).
    if ((name.text == "slice" || name.text == "List" || name.text == "Set" ||
         name.text == "Stack") &&
        match(TokenKind::Less))
    {
        const std::string elementType = parseTypeName();
        expect(TokenKind::Greater, "expected '>' after slice/List/Set/Stack element type");
        return name.text + "<" + elementType + ">";
    }

    // "Map<key,value>" - the one two-type-argument shape (every other
    // generic-looking type here takes exactly one) - see
    // docs/language/0034-maps-and-sets.md.
    if (name.text == "Map" && match(TokenKind::Less))
    {
        const std::string keyType = parseTypeName();
        expect(TokenKind::Comma, "expected ',' between Map key and value types");
        const std::string valueType = parseTypeName();
        expect(TokenKind::Greater, "expected '>' after Map value type");
        return "Map<" + keyType + "," + valueType + ">";
    }

    return name.text;
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
    return Param{name.text, parseTypeName(), capability};
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
        returnType = parseTypeName();
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
        fields.push_back(Field{fieldName.text, parseTypeName()});
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
        declaredType = parseTypeName();
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

std::unique_ptr<Stmt> Parser::parseWhile()
{
    expect(TokenKind::While, "expected 'while'");
    auto condition = parseExpression(0, /*allowStructLiteral=*/false);
    auto body = parseBlock();
    return std::make_unique<WhileStmt>(std::move(condition), std::move(body));
}

std::unique_ptr<Stmt> Parser::parseBreak()
{
    expect(TokenKind::Break, "expected 'break'");

    std::unique_ptr<Expr> value;
    if (current().kind != TokenKind::RightBrace)
    {
        value = parseExpression();
    }

    return std::make_unique<BreakStmt>(std::move(value));
}

std::unique_ptr<Stmt> Parser::parseContinue()
{
    expect(TokenKind::Continue, "expected 'continue'");
    return std::make_unique<ContinueStmt>();
}

std::unique_ptr<Stmt> Parser::parseFor()
{
    expect(TokenKind::For, "expected 'for'");
    const auto& name = expect(TokenKind::Identifier, "expected loop variable name");
    expect(TokenKind::In, "expected 'in' after for-loop variable");
    auto first = parseExpression(0, /*allowStructLiteral=*/false);
    const bool isRange = match(TokenKind::DotDot);
    // Range form (`for i in a..b`): `first` is the range's start. Array form
    // (`for v in arr`): `first` is the array expression itself - determined
    // by whether '..' follows, since both forms parse identically up to this
    // point. See docs/language/0030-for-loops.md and 0031-arrays.md.
    std::unique_ptr<Expr> end =
        isRange ? parseExpression(0, /*allowStructLiteral=*/false) : nullptr;
    auto bodyExpr = parseBlock();

    // Desugars into (range form):
    //   { __for<N>_end = b  __for<N>_i = a - 1
    //     while true { __for<N>_i++  if __for<N>_i >= __for<N>_end { break }  i = __for<N>_i  body
    //     } }
    // or (array form):
    //   { __for<N>_arr = arr  __for<N>_i = -1
    //     while true { __for<N>_i++  if __for<N>_i >= __for<N>_arr.length { break }
    //                  v = __for<N>_arr[__for<N>_i]  body } }
    // The increment happens *before* the bound check and the user's body,
    // not after. This matters because a `continue` inside `body` jumps
    // straight back to `while true`'s (trivially-true) header - i.e.
    // straight back to the top of this same block - so it reaches the
    // increment on its very next pass. A naive `while cond { body  i++ }`
    // desugaring would let `continue` skip the increment entirely and loop
    // forever; this ordering makes `continue` and "fell off the end of the
    // body normally" reach the exact same next step, with no special-casing
    // needed. The mangled `__for<N>_*` names are never producible by user
    // source (see the declaration comment in Parser.hpp) and unique per
    // for-loop so nested for-loops can't collide with each other; the
    // user's own name gets a *fresh* definition every iteration (forceDefine
    // - see AssignmentStmt in Stmt.hpp) so it can never accidentally mutate
    // a same-named outer variable.
    const std::string counterName = "__for" + std::to_string(forCounter_) + "_i";
    const std::string boundName =
        "__for" + std::to_string(forCounter_) + (isRange ? "_end" : "_arr");
    ++forCounter_;

    auto* block = static_cast<BlockExpr*>(bodyExpr.get());

    std::vector<std::unique_ptr<Stmt>> loopBodyStatements;
    loopBodyStatements.push_back(
        std::make_unique<IncDecStmt>(std::make_unique<NameExpr>(counterName), /*increment=*/true));

    std::vector<std::unique_ptr<Stmt>> breakStatements;
    breakStatements.push_back(std::make_unique<BreakStmt>(nullptr));
    auto breakBlock = std::make_unique<BlockExpr>(std::move(breakStatements), nullptr);
    auto emptyElseBlock =
        std::make_unique<BlockExpr>(std::vector<std::unique_ptr<Stmt>>{}, nullptr);
    // Range form compares against the bound directly; array form compares
    // against the bound array's `.length` (see docs/language/0031-arrays.md).
    std::unique_ptr<Expr> boundValue;
    if (isRange)
    {
        boundValue = std::make_unique<NameExpr>(boundName);
    }
    else
    {
        boundValue = std::make_unique<FieldExpr>(std::make_unique<NameExpr>(boundName), "length");
    }
    auto boundCheck = std::make_unique<BinaryExpr>(
        std::make_unique<NameExpr>(counterName), TokenKind::GreaterEqual, std::move(boundValue));
    auto boundIf = std::make_unique<IfExpr>(
        std::move(boundCheck), std::move(breakBlock), std::move(emptyElseBlock));
    loopBodyStatements.push_back(std::make_unique<ExprStmt>(std::move(boundIf)));

    // Range form binds the loop variable to the counter directly; array form
    // indexes the bound array by the counter.
    std::unique_ptr<Expr> loopVariableValue;
    if (isRange)
    {
        loopVariableValue = std::make_unique<NameExpr>(counterName);
    }
    else
    {
        loopVariableValue = std::make_unique<IndexExpr>(std::make_unique<NameExpr>(boundName),
                                                        std::make_unique<NameExpr>(counterName));
    }
    loopBodyStatements.push_back(std::make_unique<AssignmentStmt>(
        name.text, std::nullopt, std::move(loopVariableValue), /*forceDefine=*/true));

    for (auto& stmt : block->statements)
    {
        loopBodyStatements.push_back(std::move(stmt));
    }

    auto whileBody =
        std::make_unique<BlockExpr>(std::move(loopBodyStatements), std::move(block->result));
    auto whileStmt =
        std::make_unique<WhileStmt>(std::make_unique<BoolExpr>(true), std::move(whileBody));

    // Range form's counter starts one below the range's start (pre-decrement,
    // since the loop body increments before using it); array form's counter
    // simply starts at -1 for the same reason.
    std::unique_ptr<Expr> counterInit =
        isRange ? std::make_unique<BinaryExpr>(
                      std::move(first), TokenKind::Minus, std::make_unique<IntegerExpr>(1))
                : std::unique_ptr<Expr>(std::make_unique<IntegerExpr>(-1));

    std::vector<std::unique_ptr<Stmt>> outerStatements;
    outerStatements.push_back(std::make_unique<AssignmentStmt>(
        boundName, std::nullopt, isRange ? std::move(end) : std::move(first)));
    outerStatements.push_back(
        std::make_unique<AssignmentStmt>(counterName, std::nullopt, std::move(counterInit)));
    outerStatements.push_back(std::move(whileStmt));
    auto outerBlock = std::make_unique<BlockExpr>(std::move(outerStatements), nullptr);

    return std::make_unique<ExprStmt>(std::move(outerBlock));
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

        if (current().kind == TokenKind::While)
        {
            statements.push_back(parseWhile());
            continue;
        }

        if (current().kind == TokenKind::Break)
        {
            statements.push_back(parseBreak());
            continue;
        }

        if (current().kind == TokenKind::Continue)
        {
            statements.push_back(parseContinue());
            continue;
        }

        if (current().kind == TokenKind::For)
        {
            statements.push_back(parseFor());
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
            const std::string type = parseTypeName();
            expect(TokenKind::Equal, "expected '=' after type annotation");
            auto value = parseExpression();
            statements.push_back(
                std::make_unique<AssignmentStmt>(name->name, type, std::move(value)));
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
            else if (auto* index = dynamic_cast<IndexExpr*>(expr.get()))
            {
                statements.push_back(std::make_unique<IndexAssignStmt>(
                    std::move(index->object), std::move(index->index), std::move(value)));
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

std::unique_ptr<Expr> Parser::parseLoopExpr()
{
    expect(TokenKind::Loop, "expected 'loop'");
    auto body = parseBlock();
    return std::make_unique<LoopExpr>(std::move(body));
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

std::vector<std::unique_ptr<Expr>> Parser::parseArgumentList()
{
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
    return args;
}

std::unique_ptr<Expr> Parser::parsePostfix(bool allowStructLiteral)
{
    auto expr = parsePrimary(allowStructLiteral);

    while (true)
    {
        if (match(TokenKind::Dot))
        {
            const auto& field = expect(TokenKind::Identifier, "expected field name after '.'");

            // `object.method(args)` vs. `object.field` (docs/language/0033-lists.md)
            // - decided purely by whether '(' follows the identifier.
            if (match(TokenKind::LeftParen))
            {
                auto args = parseArgumentList();
                expect(TokenKind::RightParen, "expected ')' after method arguments");
                expr =
                    std::make_unique<MethodCallExpr>(std::move(expr), field.text, std::move(args));
                continue;
            }

            expr = std::make_unique<FieldExpr>(std::move(expr), field.text);
            continue;
        }

        if (match(TokenKind::LeftBracket))
        {
            auto index = parseExpression();
            expect(TokenKind::RightBracket, "expected ']' after index");
            expr = std::make_unique<IndexExpr>(std::move(expr), std::move(index));
            continue;
        }

        break;
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

    if (match(TokenKind::LeftBracket))
    {
        std::vector<std::unique_ptr<Expr>> elements;
        if (current().kind != TokenKind::RightBracket)
        {
            elements.push_back(parseExpression());
            while (match(TokenKind::Comma))
            {
                if (current().kind == TokenKind::RightBracket)
                {
                    break;
                }
                elements.push_back(parseExpression());
            }
        }
        expect(TokenKind::RightBracket, "expected ']' after array literal elements");
        return std::make_unique<ArrayLiteralExpr>(std::move(elements));
    }

    if (current().kind == TokenKind::If)
    {
        return parseIfExpr();
    }

    if (current().kind == TokenKind::Loop)
    {
        return parseLoopExpr();
    }

    if (current().kind == TokenKind::Identifier)
    {
        // `List<elem>()` construction (docs/language/0033-lists.md) - special-
        // cased on the literal identifier "List", the same trick `slice`
        // already uses in parseTypeName to sidestep the general
        // generic-vs-comparison-operator ambiguity `<`/`>` would otherwise
        // create in expression position. Checked before the general
        // Identifier-then-'(' call branch below, since "List" followed by
        // '<' would not match that branch's `peek() == LeftParen` check
        // anyway - listed first here purely for readability.
        if (current().text == "List" && peek().kind == TokenKind::Less)
        {
            advance();
            expect(TokenKind::Less, "expected '<' after 'List'");
            // A full recursive parseTypeName() call, not a single Identifier
            // token - the element type can itself be a nested generic shape
            // (e.g. `List<List<i32>>()`, `List<Map<i32,i32>>()`), which a
            // single token can't parse (see docs/language/0034-maps-and-sets.md's
            // generic-K/V rewrite).
            const std::string elementType = parseTypeName();
            expect(TokenKind::Greater, "expected '>' after List element type");
            expect(TokenKind::LeftParen, "expected '(' after 'List<elem>'");
            expect(TokenKind::RightParen,
                   "expected ')' - List<elem>() takes no arguments this phase");
            return std::make_unique<ListNewExpr>(elementType);
        }

        // `Set<elem>()` construction - same trick as List<elem>() above (see
        // docs/language/0034-maps-and-sets.md).
        if (current().text == "Set" && peek().kind == TokenKind::Less)
        {
            advance();
            expect(TokenKind::Less, "expected '<' after 'Set'");
            const std::string elementType = parseTypeName();
            expect(TokenKind::Greater, "expected '>' after Set element type");
            expect(TokenKind::LeftParen, "expected '(' after 'Set<elem>'");
            expect(TokenKind::RightParen,
                   "expected ')' - Set<elem>() takes no arguments this phase");
            return std::make_unique<SetNewExpr>(elementType);
        }

        // `Stack<elem>()` construction - same trick as List<elem>() above
        // (see docs/language/0035-stacks.md).
        if (current().text == "Stack" && peek().kind == TokenKind::Less)
        {
            advance();
            expect(TokenKind::Less, "expected '<' after 'Stack'");
            const std::string elementType = parseTypeName();
            expect(TokenKind::Greater, "expected '>' after Stack element type");
            expect(TokenKind::LeftParen, "expected '(' after 'Stack<elem>'");
            expect(TokenKind::RightParen,
                   "expected ')' - Stack<elem>() takes no arguments this phase");
            return std::make_unique<StackNewExpr>(elementType);
        }

        // `Map<key,value>()` construction - the one two-type-argument
        // constructor here (mirrors parseTypeName's own Map<key,value> shape).
        if (current().text == "Map" && peek().kind == TokenKind::Less)
        {
            advance();
            expect(TokenKind::Less, "expected '<' after 'Map'");
            const std::string keyType = parseTypeName();
            expect(TokenKind::Comma, "expected ',' between Map key and value types");
            const std::string valueType = parseTypeName();
            expect(TokenKind::Greater, "expected '>' after Map value type");
            expect(TokenKind::LeftParen, "expected '(' after 'Map<key,value>'");
            expect(TokenKind::RightParen,
                   "expected ')' - Map<key,value>() takes no arguments this phase");
            return std::make_unique<MapNewExpr>(keyType, valueType);
        }

        if (peek().kind == TokenKind::LeftParen)
        {
            const auto& name = advance();
            expect(TokenKind::LeftParen, "expected '(' after function name");
            auto args = parseArgumentList();
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
