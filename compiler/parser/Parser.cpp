#include "parser/Parser.hpp"

#include "lexer/Lexer.hpp"

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
        // A full recursive parseTypeName() call, not a single Identifier
        // token - the element type can itself be a nested generic shape
        // (e.g. `[List<i32>;3]`, `[Optional<i32>;2]`), which a single
        // token can't parse (see docs/language/0052-optional.md's own
        // follow-up, which moved every single-type-parameter kind onto a
        // genuinely recursive representation - mirrors List<elem>()'s own
        // identical "full parseTypeName(), not one token" reasoning).
        const std::string elementType = parseTypeName();
        expect(TokenKind::Semicolon, "expected ';' in array type");
        const auto& size = expect(TokenKind::Integer, "expected array size in array type");
        expect(TokenKind::RightBracket, "expected ']' after array type");

        return "[" + elementType + ";" + size.text + "]";
    }

    const auto& name = expect(TokenKind::Identifier, "expected type name");

    // "slice<elem>" (docs/language/0032-slices.md) / "List<elem>"
    // (docs/language/0033-lists.md) / "Set<elem>" (docs/language/0034-maps-and-sets.md)
    // / "Stack<elem>" (docs/language/0035-stacks.md) / "LinkedList<elem>"
    // (docs/language/0036-linked-lists.md) / "Deque<elem>"
    // (docs/language/0037-deques.md) / "Queue<elem>" (docs/language/0038-queues.md)
    // / "PriorityQueue<elem>" (docs/language/0039-priority-queues.md) /
    // "SortedSet<elem>" (docs/language/0041-sorted-sets.md)
    // - all reuse the existing Less/Greater tokens (no lexer changes needed:
    // '<'/'>' only mean this here because we're in type position, never
    // expression position).
    if ((name.text == "slice" || name.text == "List" || name.text == "Set" ||
         name.text == "Stack" || name.text == "LinkedList" || name.text == "Deque" ||
         name.text == "Queue" || name.text == "PriorityQueue" || name.text == "SortedSet" ||
         name.text == "Optional") &&
        match(TokenKind::Less))
    {
        const std::string elementType = parseTypeName();
        expect(TokenKind::Greater,
               "expected '>' after slice/List/Set/Stack/LinkedList/Deque/Queue/PriorityQueue/"
               "SortedSet/Optional element type");
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

    // "SortedMap<key,value>" - same two-type-argument shape as Map<K,V>
    // above (see docs/language/0040-sorted-maps.md).
    if (name.text == "SortedMap" && match(TokenKind::Less))
    {
        const std::string keyType = parseTypeName();
        expect(TokenKind::Comma, "expected ',' between SortedMap key and value types");
        const std::string valueType = parseTypeName();
        expect(TokenKind::Greater, "expected '>' after SortedMap value type");
        return "SortedMap<" + keyType + "," + valueType + ">";
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

    if (current().kind == TokenKind::Extern)
    {
        return parseExternDecl();
    }

    if (current().kind == TokenKind::Identifier && peek().kind == TokenKind::LeftParen)
    {
        if (looksLikeFunctionDecl())
        {
            return parseFunctionDecl();
        }
        // A bare top-level call (e.g. `print("hi")`), kept for its side
        // effect and its result discarded - mirrors parseBlock's own
        // identical "non-trailing expression" ExprStmt case. The only
        // top-level statement shape that isn't struct/extern/function-
        // decl or an assignment: `Identifier '('` can never start a valid
        // assignment target either way (`name = ...`/`name: Type = ...`
        // both require '='/':' immediately after the name, never '('),
        // so this is unambiguous once looksLikeFunctionDecl() has already
        // ruled out a declaration.
        auto expr = parseExpression();
        return std::make_unique<ExprStmt>(std::move(expr));
    }

    // `write(...)` at the top level - "write" is the `TokenKind::Write`
    // keyword (a parameter capability prefix), not `TokenKind::Identifier`,
    // so the branch above never sees it (mirrors parsePrimary's own
    // identical `TokenKind::Write` special case for expression position,
    // just one level up - a top-level `write(...)` never reaches
    // parsePrimary via parseAssignment, since parseAssignment always
    // expects an Identifier first). Always a bare call, never a
    // declaration attempt: "write" can't be a function name either
    // (it's a keyword, not an Identifier), so there's no ambiguity to
    // resolve the way looksLikeFunctionDecl() does above.
    if (current().kind == TokenKind::Write && peek().kind == TokenKind::LeftParen)
    {
        auto expr = parseExpression();
        return std::make_unique<ExprStmt>(std::move(expr));
    }

    return parseAssignment();
}

bool Parser::looksLikeFunctionDecl() const
{
    // Empty parens ('()') - ambiguous by themselves ('foo()' could be a
    // call or the start of 'foo() -> i32 { ... }'/'foo() { ... }'/
    // 'foo() => expr'), so look past the ')' at what follows the
    // signature.
    if (peek(2).kind == TokenKind::RightParen)
    {
        return peek(3).kind == TokenKind::Arrow || peek(3).kind == TokenKind::LeftBrace ||
               peek(3).kind == TokenKind::FatArrow;
    }
    // A capability keyword can only start a Param (see parseParam) -
    // never a call argument, which is an ordinary expression.
    if (peek(2).kind == TokenKind::Read || peek(2).kind == TokenKind::Write ||
        peek(2).kind == TokenKind::Take)
    {
        return true;
    }
    // 'Identifier :' is a Param's own 'name: type' shape - a call
    // argument that happens to be a bare name (`foo(x)`) is never
    // followed by ':', so this stays unambiguous.
    return peek(2).kind == TokenKind::Identifier && peek(3).kind == TokenKind::Colon;
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

Param Parser::parseExternParam()
{
    const auto& name = expect(TokenKind::Identifier, "expected extern parameter name");
    expect(TokenKind::Colon, "expected ':' after extern parameter name");
    return Param{name.text, parseTypeName(), std::nullopt};
}

std::unique_ptr<Stmt> Parser::parseExternDecl()
{
    expect(TokenKind::Extern, "expected 'extern'");
    // Only the "c" calling convention is recognized this phase (see
    // docs/language/0048-ffi.md) - a bare identifier check, not its own
    // keyword, mirroring how "String"/"Buffer" are recognized by their
    // own literal text.
    const auto& convention =
        expect(TokenKind::Identifier, "expected calling convention after 'extern'");
    if (convention.text != "c")
    {
        throw std::runtime_error("unsupported extern calling convention '" + convention.text +
                                 "' - only 'c' is supported this phase");
    }

    const auto& name =
        expect(TokenKind::Identifier, "expected function name after calling convention");
    expect(TokenKind::LeftParen, "expected '(' after extern function name");

    std::vector<Param> params;
    if (current().kind != TokenKind::RightParen)
    {
        params.push_back(parseExternParam());
        while (match(TokenKind::Comma))
        {
            if (current().kind == TokenKind::RightParen)
            {
                break;
            }
            params.push_back(parseExternParam());
        }
    }
    expect(TokenKind::RightParen, "expected ')' after extern parameters");

    std::optional<std::string> returnType;
    if (match(TokenKind::Arrow))
    {
        returnType = parseTypeName();
    }

    return std::make_unique<ExternDecl>(name.text, std::move(params), returnType);
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

            // `object.method<T>(args)` (see docs/language/0046-generic-methods.md)
            // - committed to only on the exact 4-token lookahead
            // '<' Identifier '>' '(' immediately following the method
            // name, so an ordinary comparison like `x.field < y` (where
            // '<' is the less-than operator, not a generic-call opener)
            // is never misparsed: if this exact shape isn't present,
            // nothing is consumed here and parsing falls through to the
            // existing method-call-vs-field logic below unchanged.
            std::string typeArgument;
            if (current().kind == TokenKind::Less && peek(1).kind == TokenKind::Identifier &&
                peek(2).kind == TokenKind::Greater && peek(3).kind == TokenKind::LeftParen)
            {
                advance();                     // '<'
                typeArgument = advance().text; // the type argument identifier
                advance();                     // '>'
            }

            // `object.method(args)` vs. `object.field` (docs/language/0033-lists.md)
            // - decided purely by whether '(' follows the identifier (or,
            // for a generic call, was already confirmed present above).
            const bool isCall = !typeArgument.empty() || current().kind == TokenKind::LeftParen;
            if (isCall)
            {
                expect(TokenKind::LeftParen, "expected '(' after method name");
                auto args = parseArgumentList();
                expect(TokenKind::RightParen, "expected ')' after method arguments");
                expr = std::make_unique<MethodCallExpr>(
                    std::move(expr), field.text, std::move(args), typeArgument);
                continue;
            }

            expr = std::make_unique<FieldExpr>(std::move(expr), field.text);
            continue;
        }

        if (match(TokenKind::LeftBracket))
        {
            // Either a plain index (`arr[i]`) or a range slice
            // (`str[a..b]`/`str[..b]`/`str[a..]`/`str[..]` - see
            // docs/language/0045-str-slicing.md), distinguished by
            // whether '..' follows the (optional) start expression.
            std::unique_ptr<Expr> start;
            if (current().kind != TokenKind::DotDot)
            {
                start = parseExpression();
            }

            if (match(TokenKind::DotDot))
            {
                std::unique_ptr<Expr> end;
                if (current().kind != TokenKind::RightBracket)
                {
                    end = parseExpression();
                }
                expect(TokenKind::RightBracket, "expected ']' after slice range");
                expr = std::make_unique<StrSliceExpr>(
                    std::move(expr), std::move(start), std::move(end));
                continue;
            }

            expect(TokenKind::RightBracket, "expected ']' after index");
            expr = std::make_unique<IndexExpr>(std::move(expr), std::move(start));
            continue;
        }

        // `<expr>?` (see docs/language/0052-optional.md) - inside the same
        // Dot/LeftBracket loop (not the separate `as` loop below) so it
        // chains naturally both ways: `opt?.field` unwraps then accesses a
        // field, and `foo().bar<T>()?` unwraps a method call's own result.
        if (match(TokenKind::Question))
        {
            expr = std::make_unique<TryExpr>(std::move(expr));
            continue;
        }

        break;
    }

    // `<expr> as <targetType>` (see docs/language/0005-type-system.md) -
    // deliberately outside the Dot/LeftBracket loop above (applies to the
    // whole postfix expression built so far, e.g. `foo.bar() as i64`, not
    // just a bare primary), and its own `while` to allow chaining
    // (`x as i64 as f64`).
    while (match(TokenKind::As))
    {
        std::string targetType = parseTypeName();
        expr = std::make_unique<CastExpr>(std::move(expr), std::move(targetType));
    }

    return expr;
}

std::int32_t Parser::decodeCharLiteral(const std::string& bytes)
{
    if (bytes.empty())
    {
        throw std::runtime_error("empty char literal");
    }

    const auto byteAt = [&](std::size_t i) { return static_cast<unsigned char>(bytes[i]); };
    const auto isContinuation = [&](std::size_t i)
    { return i < bytes.size() && (byteAt(i) & 0xC0) == 0x80; };

    const unsigned char lead = byteAt(0);
    std::size_t length = 0;
    std::int32_t codepoint = 0;
    std::int32_t minCodepoint = 0; // rejects overlong encodings

    if ((lead & 0x80) == 0x00)
    {
        length = 1;
        codepoint = lead;
        minCodepoint = 0;
    }
    else if ((lead & 0xE0) == 0xC0)
    {
        length = 2;
        codepoint = lead & 0x1F;
        minCodepoint = 0x80;
    }
    else if ((lead & 0xF0) == 0xE0)
    {
        length = 3;
        codepoint = lead & 0x0F;
        minCodepoint = 0x800;
    }
    else if ((lead & 0xF8) == 0xF0)
    {
        length = 4;
        codepoint = lead & 0x07;
        minCodepoint = 0x10000;
    }
    else
    {
        throw std::runtime_error("invalid char literal - malformed UTF-8");
    }

    if (bytes.size() != length)
    {
        throw std::runtime_error("char literal must contain exactly one character, found " +
                                 std::to_string(bytes.size()) + " byte(s)");
    }

    for (std::size_t i = 1; i < length; ++i)
    {
        if (!isContinuation(i))
        {
            throw std::runtime_error("invalid char literal - malformed UTF-8");
        }
        codepoint = (codepoint << 6) | (byteAt(i) & 0x3F);
    }

    if (codepoint < minCodepoint)
    {
        throw std::runtime_error("invalid char literal - overlong UTF-8 encoding");
    }
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
    {
        throw std::runtime_error("invalid char literal - not a valid Unicode scalar value");
    }

    return codepoint;
}

std::unique_ptr<Expr> Parser::parseStringLiteral(const std::string& text)
{
    std::vector<InterpolatedStringExpr::Piece> pieces;
    std::string literal;
    std::size_t i = 0;
    bool hasInterpolation = false;

    while (i < text.size())
    {
        const char c = text[i];

        if (c == '{')
        {
            if (i + 1 < text.size() && text[i + 1] == '{')
            {
                literal += '{';
                i += 2;
                continue;
            }

            hasInterpolation = true;

            // Depth-tracked so an expression segment may itself contain
            // nested braces (a struct literal, a block expression) - only
            // the top-level literal-text scan applies '{{'/'}}' escaping;
            // once inside an active expression span, every brace is a
            // real Axea token.
            std::size_t j = i + 1;
            int depth = 1;
            while (j < text.size() && depth > 0)
            {
                if (text[j] == '{')
                {
                    ++depth;
                }
                else if (text[j] == '}')
                {
                    --depth;
                    if (depth == 0)
                    {
                        break;
                    }
                }
                ++j;
            }
            if (depth != 0)
            {
                throw std::runtime_error("unterminated interpolation expression in string literal");
            }

            const std::string exprText = text.substr(i + 1, j - i - 1);
            if (exprText.empty())
            {
                throw std::runtime_error("empty interpolation expression '{}' in string literal");
            }

            if (!literal.empty())
            {
                pieces.push_back(InterpolatedStringExpr::Piece{literal, nullptr});
                literal.clear();
            }

            // A fresh, nested Lexer+Parser over just the expression
            // segment's own text - legal to call a private method
            // (parseExpression/current) on another Parser instance
            // directly, since C++ access control is per-class, not
            // per-object.
            Lexer nestedLexer(exprText);
            Parser nestedParser(nestedLexer.lex());
            auto exprAst = nestedParser.parseExpression();
            if (nestedParser.current().kind != TokenKind::EndOfFile)
            {
                throw std::runtime_error("unexpected token after interpolation expression '{" +
                                         exprText + "}'");
            }
            pieces.push_back(InterpolatedStringExpr::Piece{"", std::move(exprAst)});

            i = j + 1;
            continue;
        }

        if (c == '}')
        {
            if (i + 1 < text.size() && text[i + 1] == '}')
            {
                literal += '}';
                i += 2;
                continue;
            }
            throw std::runtime_error(
                "unmatched '}' in string literal - use '}}' for a literal '}'");
        }

        literal += c;
        ++i;
    }

    // No interpolation span found at all - every pre-existing string
    // literal in this codebase takes this path unchanged, with `literal`
    // holding only its own already-unescaped ('{{' -> '{', '}}' -> '}')
    // content (see docs/language/Axea_Printing_Formatting.md).
    if (!hasInterpolation)
    {
        return std::make_unique<StringExpr>(literal);
    }

    if (!literal.empty())
    {
        pieces.push_back(InterpolatedStringExpr::Piece{literal, nullptr});
    }

    return std::make_unique<InterpolatedStringExpr>(std::move(pieces));
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

    // "100i64" - Token.text keeps the lexer's own "i64" suffix (see
    // Lexer::lexNumber), stripped back off here before parsing the digits.
    if (current().kind == TokenKind::Int64)
    {
        const auto token = advance();
        return std::make_unique<Int64Expr>(std::stoll(token.text.substr(0, token.text.size() - 3)));
    }

    // "1.5"/"1.5f64"/"100f64" - the "f64" suffix, if present, is stripped
    // the same way Int64's own "i64" suffix is above; if absent (a bare
    // "1.5"), the text is already exactly what std::stod expects.
    if (current().kind == TokenKind::Float)
    {
        const auto token = advance();
        std::string text = token.text;
        if (text.size() >= 3 && text.substr(text.size() - 3) == "f64")
        {
            text.resize(text.size() - 3);
        }
        return std::make_unique<FloatExpr>(std::stod(text));
    }

    if (current().kind == TokenKind::String)
    {
        const auto token = advance();
        return parseStringLiteral(token.text.substr(1, token.text.size() - 2));
    }

    if (current().kind == TokenKind::Char)
    {
        const auto token = advance();
        return std::make_unique<CharExpr>(
            decodeCharLiteral(token.text.substr(1, token.text.size() - 2)));
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

    // `write(...)` (see docs/language/Axea_Printing_Formatting.md) - "write"
    // is already the `TokenKind::Write` keyword (a parameter capability
    // prefix, e.g. `write user: User`), so it never reaches the ordinary
    // Identifier-then-'(' call branch below at all; this is the one place
    // that matters, since "write" has no other legal appearance in
    // expression position. "print" needs no equivalent special-casing -
    // it was never a keyword to begin with, so it already parses as a
    // plain identifier call.
    if (current().kind == TokenKind::Write && peek().kind == TokenKind::LeftParen)
    {
        advance();
        expect(TokenKind::LeftParen, "expected '(' after 'write'");
        auto args = parseArgumentList();
        expect(TokenKind::RightParen, "expected ')' after arguments");
        return std::make_unique<CallExpr>("write", std::move(args));
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

        // `LinkedList<elem>()` construction - same trick as List<elem>()
        // above (see docs/language/0036-linked-lists.md).
        if (current().text == "LinkedList" && peek().kind == TokenKind::Less)
        {
            advance();
            expect(TokenKind::Less, "expected '<' after 'LinkedList'");
            const std::string elementType = parseTypeName();
            expect(TokenKind::Greater, "expected '>' after LinkedList element type");
            expect(TokenKind::LeftParen, "expected '(' after 'LinkedList<elem>'");
            expect(TokenKind::RightParen,
                   "expected ')' - LinkedList<elem>() takes no arguments this phase");
            return std::make_unique<LinkedListNewExpr>(elementType);
        }

        // `Deque<elem>()` construction - same trick as List<elem>() above
        // (see docs/language/0037-deques.md).
        if (current().text == "Deque" && peek().kind == TokenKind::Less)
        {
            advance();
            expect(TokenKind::Less, "expected '<' after 'Deque'");
            const std::string elementType = parseTypeName();
            expect(TokenKind::Greater, "expected '>' after Deque element type");
            expect(TokenKind::LeftParen, "expected '(' after 'Deque<elem>'");
            expect(TokenKind::RightParen,
                   "expected ')' - Deque<elem>() takes no arguments this phase");
            return std::make_unique<DequeNewExpr>(elementType);
        }

        // `Queue<elem>()` construction - same trick as List<elem>() above
        // (see docs/language/0038-queues.md).
        if (current().text == "Queue" && peek().kind == TokenKind::Less)
        {
            advance();
            expect(TokenKind::Less, "expected '<' after 'Queue'");
            const std::string elementType = parseTypeName();
            expect(TokenKind::Greater, "expected '>' after Queue element type");
            expect(TokenKind::LeftParen, "expected '(' after 'Queue<elem>'");
            expect(TokenKind::RightParen,
                   "expected ')' - Queue<elem>() takes no arguments this phase");
            return std::make_unique<QueueNewExpr>(elementType);
        }

        // `PriorityQueue<elem>()` construction - same trick as List<elem>()
        // above (see docs/language/0039-priority-queues.md). The parser
        // accepts any element type syntactically here (mirrors
        // MapNewExpr/SetNewExpr's own "parser stays general" convention) -
        // only TypeChecker enforces i32-only, with a clear error, rather
        // than the parser silently discarding what was actually written.
        if (current().text == "PriorityQueue" && peek().kind == TokenKind::Less)
        {
            advance();
            expect(TokenKind::Less, "expected '<' after 'PriorityQueue'");
            const std::string elementType = parseTypeName();
            expect(TokenKind::Greater, "expected '>' after PriorityQueue element type");
            expect(TokenKind::LeftParen, "expected '(' after 'PriorityQueue<elem>'");
            expect(TokenKind::RightParen,
                   "expected ')' - PriorityQueue<elem>() takes no arguments this phase");
            return std::make_unique<PriorityQueueNewExpr>(elementType);
        }

        // `SortedSet<elem>()` construction - same trick as Set<elem>()
        // above (see docs/language/0041-sorted-sets.md).
        if (current().text == "SortedSet" && peek().kind == TokenKind::Less)
        {
            advance();
            expect(TokenKind::Less, "expected '<' after 'SortedSet'");
            const std::string elementType = parseTypeName();
            expect(TokenKind::Greater, "expected '>' after SortedSet element type");
            expect(TokenKind::LeftParen, "expected '(' after 'SortedSet<elem>'");
            expect(TokenKind::RightParen,
                   "expected ')' - SortedSet<elem>() takes no arguments this phase");
            return std::make_unique<SortedSetNewExpr>(elementType);
        }

        // `String(text)` construction - unlike every collection above,
        // takes one runtime *value* argument, not a type parameter (see
        // docs/language/0042-string.md) - so this is a plain identifier-
        // then-'(' call shape, special-cased only to validate the
        // exactly-one-argument arity at parse time (matching every
        // collection constructor's own arity-checked-here convention)
        // rather than deferring that to TypeChecker.
        if (current().text == "String" && peek().kind == TokenKind::LeftParen)
        {
            advance();
            expect(TokenKind::LeftParen, "expected '(' after 'String'");
            auto args = parseArgumentList();
            expect(TokenKind::RightParen, "expected ')' after String(...) argument");
            if (args.size() != 1)
            {
                throw std::runtime_error("String(...) expects exactly 1 argument, got " +
                                         std::to_string(args.size()));
            }
            return std::make_unique<StringNewExpr>(std::move(args.front()));
        }

        // `Some(value)` construction (see docs/language/0052-optional.md) -
        // same one-runtime-argument shape as String(text) above, special-
        // cased only to validate the exactly-one-argument arity at parse
        // time.
        if (current().text == "Some" && peek().kind == TokenKind::LeftParen)
        {
            advance();
            expect(TokenKind::LeftParen, "expected '(' after 'Some'");
            auto args = parseArgumentList();
            expect(TokenKind::RightParen, "expected ')' after Some(...) argument");
            if (args.size() != 1)
            {
                throw std::runtime_error("Some(...) expects exactly 1 argument, got " +
                                         std::to_string(args.size()));
            }
            return std::make_unique<SomeExpr>(std::move(args.front()));
        }

        // `None` (see docs/language/0052-optional.md) - a bare identifier,
        // never followed by '(' (unlike every constructor above) since it
        // carries no value; mirrors `true`/`false`'s own bare-keyword shape
        // even though None isn't a reserved token, just a special-cased
        // identifier (consistent with String/Buffer/Some being special-cased
        // identifiers rather than reserved keywords too).
        if (current().text == "None" && peek().kind != TokenKind::LeftParen)
        {
            advance();
            return std::make_unique<NoneExpr>();
        }

        // `Buffer()` construction - always empty parens, like every
        // collection's own zero-argument constructor (see
        // docs/language/0043-buffer.md), despite Buffer not being generic
        // (no `<...>` parameter to parse, unlike List<elem>() etc).
        if (current().text == "Buffer" && peek().kind == TokenKind::LeftParen)
        {
            advance();
            expect(TokenKind::LeftParen, "expected '(' after 'Buffer'");
            expect(TokenKind::RightParen, "expected ')' - Buffer() takes no arguments");
            return std::make_unique<BufferNewExpr>();
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

        // `SortedMap<key,value>()` construction - same two-type-argument
        // shape as Map<key,value>() above (see
        // docs/language/0040-sorted-maps.md).
        if (current().text == "SortedMap" && peek().kind == TokenKind::Less)
        {
            advance();
            expect(TokenKind::Less, "expected '<' after 'SortedMap'");
            const std::string keyType = parseTypeName();
            expect(TokenKind::Comma, "expected ',' between SortedMap key and value types");
            const std::string valueType = parseTypeName();
            expect(TokenKind::Greater, "expected '>' after SortedMap value type");
            expect(TokenKind::LeftParen, "expected '(' after 'SortedMap<key,value>'");
            expect(TokenKind::RightParen,
                   "expected ')' - SortedMap<key,value>() takes no arguments this phase");
            return std::make_unique<SortedMapNewExpr>(keyType, valueType);
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
