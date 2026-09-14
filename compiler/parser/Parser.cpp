#include "parser/Parser.hpp"

#include "lexer/Lexer.hpp"

#include <algorithm>
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
    program.moduleName = moduleName_;
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
    std::string first = parseTypeNameAtom();
    if (current().kind != TokenKind::Pipe)
    {
        return first;
    }

    // "T1 | T2 | ..." anonymous union types (see docs/language/0065-unions.md)
    // - lowered, from TypeChecker down, onto the exact same machinery a
    // user-declared `enum` already has, structurally keyed the way
    // Map<K,V>/Optional<T> are rather than nominally declared like
    // struct/enum: sorted and deduplicated here so "i32 | str" and
    // "str | i32" produce the identical canonical string, and thus compare
    // equal and share one auto-registered synthetic enum (see
    // TypeChecker::resolveUnionType).
    std::vector<std::string> alternatives{std::move(first)};
    while (match(TokenKind::Pipe))
    {
        alternatives.push_back(parseTypeNameAtom());
    }
    std::sort(alternatives.begin(), alternatives.end());
    alternatives.erase(std::unique(alternatives.begin(), alternatives.end()), alternatives.end());

    std::string canonical = alternatives.front();
    for (std::size_t i = 1; i < alternatives.size(); ++i)
    {
        canonical += "|" + alternatives[i];
    }
    return canonical;
}

std::string Parser::parseTypeNameAtom()
{
    if (match(TokenKind::Star))
    {
        // `*T` (see docs/language/0019-unsafe.md) - a full recursive parseTypeNameAtom() call
        // (not parseTypeName(), so a pointer never itself absorbs a top-level '|' union - `*T | U`
        // parses as `(*T) | U`), so `**T` parses for free via plain recursion.
        return "*" + parseTypeNameAtom();
    }

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

    // "fn(T1,T2)->R" (see docs/language/0067-closures.md) - a closure type, canonicalized here
    // exactly like every other type ("no spaces" - Parser::parseTypeName's own established
    // convention), so TypeChecker::resolveType can split it back apart deterministically. `fn`
    // is a real keyword (TokenKind::Fn), not a plain Identifier, so this is checked before the
    // unconditional `expect(Identifier)` below.
    if (current().kind == TokenKind::Fn)
    {
        advance();
        expect(TokenKind::LeftParen, "expected '(' after 'fn' in a closure type");
        std::string params;
        if (current().kind != TokenKind::RightParen)
        {
            params = parseTypeName();
            while (match(TokenKind::Comma))
            {
                params += "," + parseTypeName();
            }
        }
        expect(TokenKind::RightParen, "expected ')' after closure type parameters");
        expect(TokenKind::Arrow, "expected '->' after closure type parameters");
        const std::string returnType = parseTypeName();
        return "fn(" + params + ")->" + returnType;
    }

    const auto& name = expect(TokenKind::Identifier, "expected type name");

    // "slice<elem>" (docs/language/0032-slices.md) / "Optional<elem>"/"Shared<elem>" - all reuse
    // the existing Less/Greater tokens (no lexer changes needed: '<'/'>' only mean this here
    // because we're in type position, never expression position). "List<elem>"/"Stack<elem>"/
    // "Deque<elem>"/"Queue<elem>"/"PriorityQueue<elem>"/"LinkedList<elem>"/"Map<key,value>"/
    // "Set<elem>"/"SortedMap<key,value>"/"SortedSet<elem>" are deliberately NOT here (see
    // docs/language/0006-generics.md's own port follow-up, docs/language/0034-maps-and-sets.md's
    // own "2026 Update", and docs/language/0041-sorted-sets.md's own "2026 Update") - all ten are
    // real, user-declared generic structs now (std/collections.ax), reached via the ordinary
    // generic fallback below like any other.
    if ((name.text == "slice" || name.text == "Optional" || name.text == "Shared") &&
        match(TokenKind::Less))
    {
        const std::string elementType = parseTypeName();
        expect(TokenKind::Greater, "expected '>' after slice/Optional/Shared element type");
        return name.text + "<" + elementType + ">";
    }

    // "Result<T,E>" - a two-type-argument shape (see
    // docs/language/0063-result.md).
    if (name.text == "Result" && match(TokenKind::Less))
    {
        const std::string okType = parseTypeName();
        expect(TokenKind::Comma, "expected ',' between Result Ok and Err types");
        const std::string errType = parseTypeName();
        expect(TokenKind::Greater, "expected '>' after Result Err type");
        return "Result<" + okType + "," + errType + ">";
    }

    // "SortedMap<key,value>" is deliberately NOT recognized here anymore (see
    // docs/language/0040-sorted-maps.md's own "2026 Update") - it's a real, user-declared
    // generic struct now (std/collections.ax), resolved by the ordinary struct lookup further
    // down like any other.

    // A user-defined generic struct instantiation (see docs/language/0006-generics.md) - the
    // generic fallback none of the hardcoded, fixed-arity shapes above claimed. Arbitrary arity
    // and arbitrary nesting (a full parseTypeName() call per argument, exactly like every branch
    // above), producing "Name<Arg1,Arg2,...>" - this function's own no-spaces, comma-separated
    // canonical form (mirrors the Map<key,value> branch above). GenericMonomorphizer resolves
    // this to a real, mangled struct name later; this function only ever produces the canonical
    // bracket-syntax text.
    if (match(TokenKind::Less))
    {
        std::string result = name.text + "<" + parseTypeName();
        while (match(TokenKind::Comma))
        {
            result += "," + parseTypeName();
        }
        expect(TokenKind::Greater, "expected '>' after generic struct type arguments");
        return result + ">";
    }

    return name.text;
}

std::unique_ptr<Stmt> Parser::parseItem()
{
    // `pub` (see docs/language/0066-modules.md) - only meaningful on a function/extern
    // declaration (gates whether `use`-ing code outside this file's own module can reach it);
    // harmlessly consumed-and-ignored ahead of every other item kind, exactly as before this
    // phase.
    const bool isPublic = match(TokenKind::Pub);

    if (current().kind == TokenKind::Module)
    {
        return parseModuleDecl();
    }

    if (current().kind == TokenKind::Use)
    {
        return parseUseDecl();
    }

    if (current().kind == TokenKind::Struct)
    {
        return parseStructDecl();
    }

    if (current().kind == TokenKind::Extern)
    {
        auto decl = parseExternDecl();
        static_cast<ExternDecl&>(*decl).isPublic = isPublic;
        return decl;
    }

    if (current().kind == TokenKind::Trait)
    {
        return parseTraitDecl();
    }

    if (current().kind == TokenKind::Impl)
    {
        return parseImplDecl();
    }

    if (current().kind == TokenKind::Enum)
    {
        return parseEnumDecl();
    }

    // `name<T, U>(...)` (see docs/language/0006-generics.md's own List<T> port follow-up) -
    // unambiguously a generic function declaration: no other top-level statement shape starts
    // with `Identifier '<'` in this grammar (an assignment target requires '='/':' immediately
    // after the name; a generic struct-literal reference like `Box<i32>{...}` only ever appears
    // on the *right-hand side* of an assignment, never as the first token of a top-level item).
    if (current().kind == TokenKind::Identifier && peek().kind == TokenKind::Less)
    {
        auto decl = parseFunctionDecl();
        static_cast<FunctionDecl&>(*decl).isPublic = isPublic;
        return decl;
    }

    if (current().kind == TokenKind::Identifier && peek().kind == TokenKind::LeftParen)
    {
        if (looksLikeFunctionDecl())
        {
            auto decl = parseFunctionDecl();
            static_cast<FunctionDecl&>(*decl).isPublic = isPublic;
            return decl;
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

    // `unsafe { ... }` at the top level (see docs/language/0019-unsafe.md), kept for its side
    // effect exactly like a bare top-level call just above - parseAssignment's own unconditional
    // `expect(Identifier)` fallback has no path for any keyword-led statement (bare top-level
    // `if`/`loop`/`while` have this identical, pre-existing gap too, but fixing those is out of
    // scope here - only `unsafe` is required by this milestone).
    if (current().kind == TokenKind::Unsafe)
    {
        auto expr = parseUnsafeExpr();
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

bool Parser::looksLikeGenericStructLiteral() const
{
    // Depth-counting scan starting just past the '<' at peek(1) - tracks nested angle brackets
    // (e.g. "Box<List<i32>>") exactly like type-position parsing already does; there is no
    // combined '>>' token, so a nested close is just two adjacent Greater tokens, same as today.
    // Succeeds only if, once the brackets balance back to zero, the very next token is '{' -
    // anything else (including running off the end of the file, since peek() clamps to a
    // trailing EndOfFile sentinel forever) means "not a generic struct literal", and nothing here
    // consumes a single token either way.
    std::size_t offset = 2;
    int depth = 1;
    while (depth > 0)
    {
        switch (peek(offset).kind)
        {
            case TokenKind::Less: ++depth; break;
            case TokenKind::Greater: --depth; break;
            case TokenKind::Identifier:
            case TokenKind::Comma:
            // `[T;N]`/`*T` as a type argument (see looksLikeGenericCall's own identical fix and
            // its doc comment, docs/language/0034-maps-and-sets.md's own "2026 Update") - the same
            // gap, mirrored here for consistency even though no struct-literal call site has hit
            // it yet.
            case TokenKind::LeftBracket:
            case TokenKind::RightBracket:
            case TokenKind::Semicolon:
            case TokenKind::Integer:
            case TokenKind::Star: break;
            default: return false;
        }
        ++offset;
    }
    return peek(offset).kind == TokenKind::LeftBrace;
}

bool Parser::looksLikeGenericCall() const
{
    // Identical scan to looksLikeGenericStructLiteral, checking for '(' instead of '{' once the
    // brackets balance back to zero - `name<Type>(args)` (see docs/language/0006-generics.md's
    // own List<T> port follow-up), an explicit call-site type argument to a generic top-level
    // function.
    std::size_t offset = 2;
    int depth = 1;
    while (depth > 0)
    {
        switch (peek(offset).kind)
        {
            case TokenKind::Less: ++depth; break;
            case TokenKind::Greater: --depth; break;
            case TokenKind::Identifier:
            case TokenKind::Comma:
            // `[T;N]` (a fixed array as a type argument, e.g. `newMap<i32,[i32;2]>()`) and `*T` (a
            // raw pointer as a type argument) - a real, previously-undiscovered gap found while
            // porting Map<K,V>: this scan never needed to look past bare identifiers/commas before,
            // since Map<K,V>'s own construction used to have a dedicated parsing branch bypassing
            // this lookahead entirely (see docs/language/0034-maps-and-sets.md's own "2026
            // Update") - once it went through this general mechanism instead, an array-shaped V
            // (`Map<i32,[i32;2]>`) was silently never recognized as a generic call at all.
            case TokenKind::LeftBracket:
            case TokenKind::RightBracket:
            case TokenKind::Semicolon:
            case TokenKind::Integer:
            case TokenKind::Star: break;
            default: return false;
        }
        ++offset;
    }
    return peek(offset).kind == TokenKind::LeftParen;
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

    // `name<T, U>(...)` (see docs/language/0006-generics.md's own List<T> port follow-up) -
    // identical shape to struct/impl's own type-param list. Explicit type arguments only at
    // call sites (see CallExpr::typeArgument) - no inference, matching every other generic
    // feature this session.
    std::vector<std::string> typeParams;
    if (match(TokenKind::Less))
    {
        typeParams.push_back(expect(TokenKind::Identifier, "expected type parameter name").text);
        while (match(TokenKind::Comma))
        {
            typeParams.push_back(expect(TokenKind::Identifier, "expected type parameter name").text);
        }
        expect(TokenKind::Greater, "expected '>' after function type parameters");
    }

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

    auto decl = std::make_unique<FunctionDecl>(
        name.text, std::move(params), returnType, std::move(body));
    decl->typeParams = std::move(typeParams);
    return decl;
}

std::unique_ptr<Expr> Parser::parseClosureExpr()
{
    expect(TokenKind::Fn, "expected 'fn'");
    expect(TokenKind::LeftParen, "expected '(' after 'fn'");

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
    expect(TokenKind::RightParen, "expected ')' after closure parameters");

    std::optional<std::string> returnType;
    if (match(TokenKind::Arrow))
    {
        returnType = parseTypeName();
    }

    std::unique_ptr<Expr> body;
    if (match(TokenKind::FatArrow))
    {
        // Same `=>` sugar parseFunctionDecl's own body parsing already has - see that function's
        // own comment.
        auto expr = parseExpression();
        std::vector<std::unique_ptr<Stmt>> statements;
        statements.push_back(std::make_unique<ReturnStmt>(std::move(expr)));
        body = std::make_unique<BlockExpr>(std::move(statements), nullptr);
    }
    else
    {
        body = parseBlock();
    }

    return std::make_unique<ClosureExpr>(std::move(params), returnType, std::move(body));
}

Param Parser::parseSelfAwareParam(const std::string& selfType)
{
    // `self` (see docs/language/0062-display-trait.md) - unambiguous
    // against a real parameter literally named `self` of some other type
    // (`self: Foo`, legal but exotic), since that shape has a ':' right
    // after the identifier and this only matches when it doesn't.
    if (current().kind == TokenKind::Identifier && current().text == "self" &&
        peek().kind != TokenKind::Colon)
    {
        advance();
        return Param{"self", selfType, std::nullopt};
    }
    return parseParam();
}

std::unique_ptr<FunctionDecl> Parser::parseImplMethod(const std::string& typeName,
                                                       const std::string& selfType)
{
    const auto& name = expect(TokenKind::Identifier, "expected method name");
    expect(TokenKind::LeftParen, "expected '(' after method name");

    std::vector<Param> params;
    if (current().kind != TokenKind::RightParen)
    {
        params.push_back(parseSelfAwareParam(selfType));
        while (match(TokenKind::Comma))
        {
            if (current().kind == TokenKind::RightParen)
            {
                break;
            }
            params.push_back(parseSelfAwareParam(selfType));
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
        auto expr = parseExpression();
        std::vector<std::unique_ptr<Stmt>> statements;
        statements.push_back(std::make_unique<ReturnStmt>(std::move(expr)));
        body = std::make_unique<BlockExpr>(std::move(statements), nullptr);
    }
    else
    {
        body = parseBlock();
    }

    // Mangled with '.' (see ImplDecl's own comment) - permanently
    // unreachable from ordinary call syntax, so this can never collide
    // with, or be called directly as, a real user-declared function.
    return std::make_unique<FunctionDecl>(
        typeName + "." + name.text, std::move(params), returnType, std::move(body));
}

std::unique_ptr<Stmt> Parser::parseTraitDecl()
{
    advance(); // 'trait'
    const auto& name = expect(TokenKind::Identifier, "expected trait name");
    expect(TokenKind::LeftBrace, "expected '{' after trait name");

    auto decl = std::make_unique<TraitDecl>();
    decl->name = name.text;

    while (current().kind != TokenKind::RightBrace)
    {
        const auto& methodName = expect(TokenKind::Identifier, "expected method name");
        expect(TokenKind::LeftParen, "expected '(' after method name");

        std::size_t paramCount = 0;
        if (current().kind != TokenKind::RightParen)
        {
            parseSelfAwareParam("Self");
            ++paramCount;
            while (match(TokenKind::Comma))
            {
                if (current().kind == TokenKind::RightParen)
                {
                    break;
                }
                parseSelfAwareParam("Self");
                ++paramCount;
            }
        }
        expect(TokenKind::RightParen, "expected ')' after parameters");

        // A trait method signature has no body - just optionally a
        // return type, matching the source doc's own
        // `format(self, buf: Buffer)` example (no `-> ...` at all there,
        // since format's own result is unit).
        if (match(TokenKind::Arrow))
        {
            parseTypeName();
        }

        decl->methods.push_back(TraitDecl::MethodSig{methodName.text, paramCount});
    }
    expect(TokenKind::RightBrace, "expected '}' after trait body");

    return decl;
}

std::unique_ptr<Stmt> Parser::parseImplDecl()
{
    advance(); // 'impl'

    // `impl<T, U> Name<T, U> { ... }` (see docs/language/0006-generics.md) - identical shape to
    // struct's own type-param list (parseStructDecl), parsed before the first identifier so it's
    // available regardless of which branch (inherent vs. trait) follows.
    std::vector<std::string> typeParams;
    if (match(TokenKind::Less))
    {
        typeParams.push_back(expect(TokenKind::Identifier, "expected type parameter name").text);
        while (match(TokenKind::Comma))
        {
            typeParams.push_back(expect(TokenKind::Identifier, "expected type parameter name").text);
        }
        expect(TokenKind::Greater, "expected '>' after impl type parameters");
    }

    const auto& firstName = expect(TokenKind::Identifier, "expected name after 'impl'");

    auto decl = std::make_unique<ImplDecl>();
    decl->typeParams = typeParams;

    if (match(TokenKind::For))
    {
        // Existing trait-impl path: `impl TraitName for TypeName { ... }` - the identifier
        // already parsed was the trait name.
        const auto& typeName = expect(TokenKind::Identifier, "expected type name after 'for'");
        decl->traitName = firstName.text;
        decl->typeName = typeName.text;
    }
    else
    {
        // New inherent-impl path: `impl TypeName { ... }` / `impl<T> TypeName<T> { ... }` - no
        // trait, so `traitName` stays empty (the "" sentinel, already safe everywhere it's
        // checked - see TypeChecker's own registerSignatures). The identifier already parsed
        // *is* the type name; if the impl itself is generic, the type's own re-stated
        // `<T, ...>` must follow, textually matching the impl's own list - this makes `self`'s
        // type text `Box<T>`, which GenericMonomorphizer's existing whole-token substitution
        // then rewrites to `Box$i32` per instantiation like any other type-text field.
        decl->typeName = firstName.text;
        if (!typeParams.empty())
        {
            expect(TokenKind::Less, "expected '<' restating impl type parameters after type name");
            for (std::size_t i = 0; i < typeParams.size(); ++i)
            {
                if (i > 0)
                {
                    expect(TokenKind::Comma, "expected ',' between type name's type parameters");
                }
                const auto& restated =
                    expect(TokenKind::Identifier, "expected type parameter name");
                if (restated.text != typeParams[i])
                {
                    throw std::runtime_error(
                        "type name's type parameter '" + restated.text +
                        "' does not match impl's own type parameter '" + typeParams[i] + "'");
                }
            }
            expect(TokenKind::Greater, "expected '>' after type name's type parameters");
        }
    }

    expect(TokenKind::LeftBrace, "expected '{' after impl header");

    // Inside a generic impl, `self`'s own type text is the bracket-syntax `Name<T, ...>` (not
    // the bare name) - see this function's own comment above.
    std::string selfType = decl->typeName;
    if (!typeParams.empty())
    {
        selfType += "<";
        for (std::size_t i = 0; i < typeParams.size(); ++i)
        {
            if (i > 0)
            {
                // No space after the comma (see docs/language/0034-maps-and-sets.md's own "2026
                // Update") - a real, previously-undiscovered bug found while porting Map<K,V>:
                // every other multi-argument generic type text in this codebase (parseTypeName's
                // own Map<key,value> construction, splitGenericInstantiation's own comma split)
                // joins with a bare ',', no space - this self-type text is the one place that
                // didn't, silently latent because no impl block ever had 2+ type params before
                // Map<K,V>/Set<T>. The mismatch meant `self`'s own declared type text
                // ("Pair<K, V>") never exactly matched the mangled/canonical form
                // (splitGenericInstantiation's own comma-adjacent character becoming part of the
                // *next* argument's own text, e.g. " V" with a leading space) - GenericMonomorphizer
                // then treated it as a perpetually-new, never-converging ref every fixed-point
                // iteration, growing one extra space each time forever (infinite loop, not a
                // crash).
                selfType += ",";
            }
            selfType += typeParams[i];
        }
        selfType += ">";
    }

    while (current().kind != TokenKind::RightBrace)
    {
        decl->methods.push_back(parseImplMethod(decl->typeName, selfType));
    }
    expect(TokenKind::RightBrace, "expected '}' after impl body");

    return decl;
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

    // `struct Box<T, U, ...>` (see docs/language/0006-generics.md) - no ambiguity risk: a struct
    // name is never followed by '<' for any other reason today.
    std::vector<std::string> typeParams;
    if (match(TokenKind::Less))
    {
        typeParams.push_back(expect(TokenKind::Identifier, "expected type parameter name").text);
        while (match(TokenKind::Comma))
        {
            typeParams.push_back(expect(TokenKind::Identifier, "expected type parameter name").text);
        }
        expect(TokenKind::Greater, "expected '>' after struct type parameters");
    }

    expect(TokenKind::LeftBrace, "expected '{' after struct name");

    std::vector<Field> fields;
    while (current().kind == TokenKind::Identifier)
    {
        const auto& fieldName = advance();
        expect(TokenKind::Colon, "expected ':' after field name");
        fields.push_back(Field{fieldName.text, parseTypeName()});
    }
    expect(TokenKind::RightBrace, "expected '}' after struct fields");

    return std::make_unique<StructDecl>(name.text, std::move(fields), std::move(typeParams));
}

std::unique_ptr<Stmt> Parser::parseEnumDecl()
{
    expect(TokenKind::Enum, "expected 'enum'");
    const auto& name = expect(TokenKind::Identifier, "expected enum name");
    expect(TokenKind::LeftBrace, "expected '{' after enum name");

    std::vector<EnumVariant> variants;
    while (current().kind == TokenKind::Identifier)
    {
        const auto& variantName = advance();
        std::vector<std::string> fieldTypes;
        if (match(TokenKind::LeftParen))
        {
            if (current().kind != TokenKind::RightParen)
            {
                fieldTypes.push_back(parseTypeName());
                while (match(TokenKind::Comma))
                {
                    fieldTypes.push_back(parseTypeName());
                }
            }
            expect(TokenKind::RightParen, "expected ')' after variant payload types");
        }
        variants.push_back(EnumVariant{variantName.text, std::move(fieldTypes)});
    }
    expect(TokenKind::RightBrace, "expected '}' after enum variants");

    return std::make_unique<EnumDecl>(name.text, std::move(variants));
}

std::unique_ptr<Stmt> Parser::parseModuleDecl()
{
    if (moduleName_)
    {
        throw std::runtime_error("a file may declare at most one module");
    }
    expect(TokenKind::Module, "expected 'module'");
    const auto& name = expect(TokenKind::Identifier, "expected module name");
    moduleName_ = name.text;

    auto decl = std::make_unique<ModuleDecl>();
    decl->name = name.text;
    return decl;
}

std::unique_ptr<Stmt> Parser::parseUseDecl()
{
    expect(TokenKind::Use, "expected 'use'");
    const auto& name = expect(TokenKind::Identifier, "expected module name after 'use'");

    std::optional<std::string> alias;
    if (match(TokenKind::As))
    {
        alias = expect(TokenKind::Identifier, "expected alias name after 'as'").text;
    }

    aliases_[alias.value_or(name.text)] = name.text;

    auto decl = std::make_unique<UseDecl>();
    decl->moduleName = name.text;
    decl->alias = alias;
    return decl;
}

std::unique_ptr<Expr> Parser::parseMatchExpr()
{
    expect(TokenKind::Match, "expected 'match'");
    // Struct-literal parsing is disabled for the scrutinee, the same way
    // an `if` condition already disables it - `match x { ... }`'s own
    // `{` must start the arm list, not be misread as `x`'s own struct
    // literal fields.
    auto scrutinee = parseExpression(0, /*allowStructLiteral=*/false);
    expect(TokenKind::LeftBrace, "expected '{' after match scrutinee");

    std::vector<MatchArm> arms;
    while (current().kind == TokenKind::Identifier)
    {
        const auto& variantName = advance();
        std::vector<std::string> bindingNames;
        if (match(TokenKind::LeftParen))
        {
            if (current().kind != TokenKind::RightParen)
            {
                bindingNames.push_back(expect(TokenKind::Identifier, "expected binding name").text);
                while (match(TokenKind::Comma))
                {
                    bindingNames.push_back(
                        expect(TokenKind::Identifier, "expected binding name").text);
                }
            }
            expect(TokenKind::RightParen, "expected ')' after match arm bindings");
        }
        expect(TokenKind::FatArrow, "expected '=>' after match arm pattern");
        auto body = parseExpression();
        arms.push_back(MatchArm{variantName.text, std::move(bindingNames), std::move(body)});
    }
    expect(TokenKind::RightBrace, "expected '}' after match arms");

    return std::make_unique<MatchExpr>(std::move(scrutinee), std::move(arms));
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
            else if (auto* deref = dynamic_cast<DerefExpr*>(expr.get()))
            {
                statements.push_back(std::make_unique<DerefAssignStmt>(
                    std::move(deref->operand), std::move(value)));
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

std::unique_ptr<Expr> Parser::parseUnsafeExpr()
{
    expect(TokenKind::Unsafe, "expected 'unsafe'");
    auto body = parseBlock();
    return std::make_unique<UnsafeBlockExpr>(std::move(body));
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

        // This grammar has no statement terminator, so a new statement beginning with '*' (a
        // dereference, see docs/language/0019-unsafe.md) directly under a completed statement -
        // e.g. "total = 2\n*buf = 1" - would otherwise be silently swallowed as "total = 2 * buf"
        // (an ordinary multiplication continuing onto the next line), leaving a dangling "= 1"
        // that fails to parse. '*' was the only operator token this language had ever given a
        // *prefix* meaning to until bitwise `&` joined it (see docs/language/0034-maps-and-sets.md's
        // own "2026 Update") - both need this guard: stop treating Star/Ampersand as an infix
        // operator the moment it starts on a later source line than the operand already parsed -
        // parseBlock's own statement loop then picks it up fresh as a new dereference-/address-of-
        // led statement, exactly as intended. A deliberate multiplication/bitwise-and spanning a
        // line break remains legal as long as the operator itself, not just its right operand,
        // stays on the same line as the left operand (e.g. "total = 2 *\n  buf" is unaffected).
        if ((current().kind == TokenKind::Star || current().kind == TokenKind::Ampersand) &&
            index_ > 0 && current().line != tokens_[index_ - 1].line)
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
            // `alias.foo(...)` (see docs/language/0066-modules.md) - rewritten to the *real*
            // module name right here, the exact moment `expr` (already fully parsed) is about
            // to become a MethodCallExpr/FieldExpr's own `object` - never anywhere else a bare
            // identifier appears, so an ordinary local variable that happens to share a name
            // with an in-scope alias is unaffected everywhere except this one position (see
            // aliases_'s own comment). Every later pass therefore only ever sees the real
            // module name, with no alias awareness of its own needed anywhere downstream.
            if (auto* aliasName = dynamic_cast<NameExpr*>(expr.get());
                aliasName && aliases_.contains(aliasName->name))
            {
                aliasName->name = aliases_.at(aliasName->name);
            }

            // `.write(...)` (see docs/language/0061-buffer-write.md) -
            // "write" predates this as TokenKind::Write, a parameter
            // capability-prefix keyword (docs/language/0049-printing-
            // formatting.md's own precedent for `write(...)` at call
            // position), so it never satisfies a plain `Identifier`
            // expectation. Unambiguous here for the identical reason:
            // `TokenKind::Write` only ever appears as a capability prefix
            // inside `parseParam`'s own parameter-list parsing, a wholly
            // different code path from postfix `.` access, so accepting
            // it as a field/method name right after `.` can never
            // misparse a real capability prefix as one.
            const auto& field =
                current().kind == TokenKind::Write
                    ? advance()
                    : expect(TokenKind::Identifier, "expected field name after '.'");

            // `object.method<T>(args)` / `object.method<T1,T2>(args)` (see
            // docs/language/0046-generic-methods.md, extended to more than one type argument by
            // docs/language/0034-maps-and-sets.md's own "2026 Update", e.g.
            // `collections.newMap<K,V>()`) - committed to only once a full bracket-depth-aware
            // lookahead (mirrors looksLikeGenericCall's own identical scan and its doc comment)
            // confirms the brackets balance back to zero immediately followed by '(', so an
            // ordinary comparison like `x.field < y` (where '<' is the less-than operator, not a
            // generic-call opener) is never misparsed: if that shape isn't present, nothing is
            // consumed here and parsing falls through to the existing method-call-vs-field logic
            // below unchanged. Originally a fixed 4-token check ('<' Identifier '>' '(') that
            // could only ever match a single bare-identifier type argument - a real,
            // previously-undiscovered gap found while porting Map<K,V>: even a single *nested*
            // type argument (e.g. `.newBox<List<i32>>()`) was already unsupported here, since
            // peek(2) had to be '>' immediately.
            std::string typeArgument;
            if (current().kind == TokenKind::Less)
            {
                std::size_t offset = 1;
                int depth = 1;
                bool balanced = true;
                while (depth > 0)
                {
                    switch (peek(offset).kind)
                    {
                        case TokenKind::Less: ++depth; break;
                        case TokenKind::Greater: --depth; break;
                        case TokenKind::Identifier:
                        case TokenKind::Comma:
                        case TokenKind::LeftBracket:
                        case TokenKind::RightBracket:
                        case TokenKind::Semicolon:
                        case TokenKind::Integer:
                        case TokenKind::Star: break;
                        default: balanced = false; depth = 0; break;
                    }
                    ++offset;
                }
                if (balanced && peek(offset).kind == TokenKind::LeftParen)
                {
                    advance(); // '<'
                    typeArgument = parseTypeName();
                    while (current().kind == TokenKind::Comma)
                    {
                        advance();
                        typeArgument += "," + parseTypeName();
                    }
                    expect(TokenKind::Greater, "expected '>' after method's type argument(s)");
                }
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

std::unique_ptr<Expr> Parser::parseRawOrInterpolatedString(const std::string& tokenText)
{
    const bool raw = !tokenText.empty() && tokenText[0] == 'r';
    const std::size_t prefixLen = raw ? 1 : 0;
    const bool triple = tokenText.size() >= prefixLen + 3 && tokenText[prefixLen] == '"' &&
                        tokenText[prefixLen + 1] == '"' && tokenText[prefixLen + 2] == '"';
    const std::size_t quoteLen = triple ? 3 : 1;
    const std::string content =
        tokenText.substr(prefixLen + quoteLen, tokenText.size() - prefixLen - 2 * quoteLen);

    // Raw strings (see docs/language/0059-raw-strings.md) disable
    // interpolation entirely - the lexer's own scanRawStringSpan never
    // treats '{'/'}' specially, so no unescaped '{' inside `content` was
    // ever validated as a real expression span. Building an ordinary
    // parseStringLiteral over it would therefore either misinterpret a
    // literal '{' as the start of one, or reject '{{'/'}}' pairs that
    // were never meant as escapes - so a raw literal becomes a plain
    // StringExpr directly, content untouched, byte for byte.
    if (raw)
    {
        return std::make_unique<StringExpr>(content);
    }

    return parseStringLiteral(content);
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
            // real Axea token. Also quote-aware (toggling `inString` on
            // each '"') so a '}' inside a nested string literal argument
            // (e.g. `{x.join("}")}` - see docs/language/0049-printing-
            // formatting.md's own nested-string-literal follow-up) never
            // prematurely ends the span - a real, narrow, pre-existing
            // gap found while adding the format-spec colon scan just
            // below, which needs the identical quote-awareness for its
            // own reasons anyway.
            std::size_t j = i + 1;
            int depth = 1;
            bool inString = false;
            while (j < text.size() && depth > 0)
            {
                if (text[j] == '"')
                {
                    inString = !inString;
                }
                else if (!inString && text[j] == '{')
                {
                    ++depth;
                }
                else if (!inString && text[j] == '}')
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

            const std::string span = text.substr(i + 1, j - i - 1);
            if (span.empty())
            {
                throw std::runtime_error("empty interpolation expression '{}' in string literal");
            }

            // A top-level ':' (depth-tracked for '{[('/'}])', quote-aware
            // the same way the span scan above is) splits the expression
            // from an optional trailing format spec (see
            // docs/language/0055-numeric-format-specs.md) - e.g.
            // `{value:05}`. Never ambiguous with a struct literal's own
            // `name: value` colons, which are always nested inside their
            // own '{...}' (depth > 0 by the time they're reached).
            std::size_t colonPos = std::string::npos;
            {
                int specDepth = 0;
                bool specInString = false;
                for (std::size_t k = 0; k < span.size(); ++k)
                {
                    const char sc = span[k];
                    if (sc == '"')
                    {
                        specInString = !specInString;
                    }
                    else if (!specInString && (sc == '{' || sc == '[' || sc == '('))
                    {
                        ++specDepth;
                    }
                    else if (!specInString && (sc == '}' || sc == ']' || sc == ')'))
                    {
                        if (specDepth > 0)
                        {
                            --specDepth;
                        }
                    }
                    else if (!specInString && specDepth == 0 && sc == ':')
                    {
                        colonPos = k;
                        break;
                    }
                }
            }

            std::string exprText = span;
            std::string formatSpec;
            if (colonPos != std::string::npos)
            {
                exprText = span.substr(0, colonPos);
                formatSpec = span.substr(colonPos + 1);
                if (formatSpec.empty())
                {
                    throw std::runtime_error(
                        "empty format spec after ':' in interpolation expression '{" + span + "}'");
                }
                if (exprText.empty())
                {
                    throw std::runtime_error("empty interpolation expression before ':' in '{" +
                                             span + "}'");
                }
            }

            // `{expr=}` (see docs/language/0058-debug-formatting.md) - a
            // trailing '=' right before the ':spec' split point (or the
            // closing '}' when there's no spec) marks self-documenting
            // interpolation. Never ambiguous with a comparison operator:
            // Axea has no expression-level '=' at all (assignment is
            // statement-only), so a bare trailing '=' can only be this
            // marker - guarded here purely so `==`/`!=`/`<=`/`>=` (each
            // ending in '=' too, but never legal as a *trailing* token of
            // a complete expression either, so this guard is about correct
            // error attribution, not real ambiguity) fall through to the
            // nested expression parse below and get its own clearer
            // "unexpected token"/"expected expression" error instead of
            // being silently misread as `{expr}=` for some truncated
            // `expr`.
            std::string selfDocPrefix;
            if (!exprText.empty() && exprText.back() == '=' &&
                (exprText.size() < 2 ||
                 (exprText[exprText.size() - 2] != '=' && exprText[exprText.size() - 2] != '!' &&
                  exprText[exprText.size() - 2] != '<' && exprText[exprText.size() - 2] != '>')))
            {
                exprText.pop_back();
                if (exprText.empty())
                {
                    throw std::runtime_error("empty interpolation expression before '=' in '{" +
                                             span + "}'");
                }
                selfDocPrefix = exprText;
            }

            // `{expr:?}` (see docs/language/0058-debug-formatting.md) - "?"
            // is never itself a FormatSpec-grammar string (parseFormatSpec
            // only ever accepts x/X/b/o as a type char), so it's peeled
            // off here as a wholly separate, orthogonal piece-level flag
            // rather than folded into FormatSpec.hpp's own shared grammar
            // - debug mode is "which stringifier to call", not "how to
            // format a number", and the doc never shows it combined with
            // width/precision/radix/alignment.
            bool debug = false;
            if (formatSpec == "?")
            {
                debug = true;
                formatSpec.clear();
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
            pieces.push_back(InterpolatedStringExpr::Piece{
                "", std::move(exprAst), std::move(formatSpec), std::move(selfDocPrefix), debug});

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
    // `fn(x: i32) -> i32 { x + 1 }` (see docs/language/0067-closures.md) - a closure literal,
    // same (params, optional return type, body) shape parseFunctionDecl already parses for a
    // top-level function, just as an expression instead of a named declaration.
    if (current().kind == TokenKind::Fn)
    {
        return parseClosureExpr();
    }

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
        return parseRawOrInterpolatedString(token.text);
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

    if (current().kind == TokenKind::Match)
    {
        return parseMatchExpr();
    }

    if (current().kind == TokenKind::Unsafe)
    {
        return parseUnsafeExpr();
    }

    // `*ptr` (see docs/language/0019-unsafe.md) - a prefix dereference, unambiguous here: this
    // whole function is only ever reached at a position expecting a brand-new expression to
    // start, never mid-expression (parseExpression's own binary-operator loop is the only place
    // Star is ever read as infix multiplication, and only *after* parsePostfix has already
    // returned a complete left operand - see that loop's own comment). The operand is parsed via
    // parsePostfix (not parsePrimary alone), so postfix operations bind *tighter* than this
    // prefix deref, matching C's own precedence: `*obj.field` parses as `*(obj.field)`, and
    // `*ptr + 1` parses as `(*ptr) + 1` since the enclosing parseExpression loop only reads the
    // next `+` as infix once this whole primary has already been returned. `*(ptr + 1)` needs no
    // special grammar here either - the existing parenthesized-primary branch just above already
    // handles it.
    if (current().kind == TokenKind::Star)
    {
        advance();
        return std::make_unique<DerefExpr>(parsePostfix(allowStructLiteral));
    }

    // `&name` (see docs/language/0019-unsafe.md) - unambiguous for the identical reason '*' is
    // just above, and by the identical mechanism: this prefix case only ever fires when starting a
    // fresh primary expression (from parsePostfix/parsePrimary), structurally distinct from
    // parseExpression's own infix loop (which now also recognizes Ampersand as bitwise AND, see
    // docs/language/0034-maps-and-sets.md's own "2026 Update", with its own Star-style newline-
    // boundary guard there) - by the time that loop's own `current().kind` check could see an
    // Ampersand, a complete left operand has always already been parsed, so there's no case where
    // both meanings are live at once. Operand parsed via parsePostfix, same as DerefExpr, so
    // `&x.field`/`&arr[i]` still parse (as AddressOfExpr wrapping a FieldExpr/IndexExpr) -
    // TypeChecker, not the parser, rejects anything but a bare NameExpr operand this phase.
    if (current().kind == TokenKind::Ampersand)
    {
        advance();
        return std::make_unique<AddressOfExpr>(parsePostfix(allowStructLiteral));
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
        // `List<elem>()` construction is deliberately NOT handled here anymore (see
        // docs/language/0006-generics.md's own List<T> port follow-up) - List<T> is a real,
        // user-declared generic struct now (std/collections.ax), constructed via its own
        // `newList<T>()` generic top-level function (reached through the ordinary
        // `looksLikeGenericCall()` branch above) rather than call-style `List<elem>()` sugar.

        // `Set<elem>()` construction is deliberately NOT handled here anymore (see
        // docs/language/0034-maps-and-sets.md's own "2026 Update") - Set<T> is a real,
        // user-declared generic struct now (std/collections.ax), constructed via its own
        // `newSet<T>()` generic top-level function.

        // `Stack<elem>()` construction is deliberately NOT handled here anymore (see
        // docs/language/0006-generics.md's own List<T>/Stack<T> port follow-up) - Stack<T> is a
        // real, user-declared generic struct now (std/collections.ax), constructed via its own
        // `newStack<T>()` generic top-level function.

        // `LinkedList<elem>()` construction is deliberately NOT handled here anymore (see
        // docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T>/Queue<T>/
        // PriorityQueue<T>/LinkedList<T> port follow-up) - LinkedList<T> is a real,
        // user-declared generic struct now (std/collections.ax), constructed via its own
        // `newLinkedList<T>()` generic top-level function.

        // `Deque<elem>()` construction is deliberately NOT handled here anymore (see
        // docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T> port follow-up) -
        // Deque<T> is a real, user-declared generic struct now (std/collections.ax), constructed
        // via its own `newDeque<T>()` generic top-level function.

        // `Queue<elem>()` construction is deliberately NOT handled here anymore (see
        // docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T>/Queue<T> port
        // follow-up) - Queue<T> is a real, user-declared generic struct now
        // (std/collections.ax), constructed via its own `newQueue<T>()` generic top-level
        // function.

        // `PriorityQueue<elem>()` construction is deliberately NOT handled here anymore (see
        // docs/language/0006-generics.md's own List<T>/Stack<T>/Deque<T>/Queue<T>/
        // PriorityQueue<T> port follow-up) - PriorityQueue<T> is a real, user-declared generic
        // struct now (std/collections.ax), constructed via its own `newPriorityQueue<T>()`
        // generic top-level function.

        // `SortedSet<elem>()` construction is deliberately NOT handled here anymore (see
        // docs/language/0041-sorted-sets.md's own "2026 Update") - SortedSet<T> is a real,
        // user-declared generic struct now (std/collections.ax), constructed via its own
        // `newSortedSet<T>()` generic top-level function.

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

        // `Shared(value)` - the move-semantics work's own explicit refcounting escape hatch, same
        // one-runtime-argument shape as Some(value) above.
        if (current().text == "Shared" && peek().kind == TokenKind::LeftParen)
        {
            advance();
            expect(TokenKind::LeftParen, "expected '(' after 'Shared'");
            auto args = parseArgumentList();
            expect(TokenKind::RightParen, "expected ')' after Shared(...) argument");
            if (args.size() != 1)
            {
                throw std::runtime_error("Shared(...) expects exactly 1 argument, got " +
                                         std::to_string(args.size()));
            }
            return std::make_unique<ShareExpr>(std::move(args.front()));
        }

        // `Ok(value)`/`Err(value)` (see docs/language/0063-result.md) - same
        // one-runtime-argument shape as Some(value) above.
        if (current().text == "Ok" && peek().kind == TokenKind::LeftParen)
        {
            advance();
            expect(TokenKind::LeftParen, "expected '(' after 'Ok'");
            auto args = parseArgumentList();
            expect(TokenKind::RightParen, "expected ')' after Ok(...) argument");
            if (args.size() != 1)
            {
                throw std::runtime_error("Ok(...) expects exactly 1 argument, got " +
                                         std::to_string(args.size()));
            }
            return std::make_unique<OkExpr>(std::move(args.front()));
        }

        if (current().text == "Err" && peek().kind == TokenKind::LeftParen)
        {
            advance();
            expect(TokenKind::LeftParen, "expected '(' after 'Err'");
            auto args = parseArgumentList();
            expect(TokenKind::RightParen, "expected ')' after Err(...) argument");
            if (args.size() != 1)
            {
                throw std::runtime_error("Err(...) expects exactly 1 argument, got " +
                                         std::to_string(args.size()));
            }
            return std::make_unique<ErrExpr>(std::move(args.front()));
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

        // `null` (see docs/language/0019-unsafe.md) - a special-cased bare identifier, same
        // convention as `None`/`true`/`false` just above/below - never a reserved lexer keyword.
        if (current().text == "null" && peek().kind != TokenKind::LeftParen)
        {
            advance();
            return std::make_unique<NullExpr>();
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

        // `sizeof<TypeName>()` - a builtin, not a real callable function, recognized by literal
        // text like "Buffer" above (see docs/language/0006-generics.md's own List<T> port
        // follow-up) - always exactly one type argument, empty parens.
        if (current().text == "sizeof" && peek().kind == TokenKind::Less)
        {
            advance();
            expect(TokenKind::Less, "expected '<' after 'sizeof'");
            const std::string typeName = parseTypeName();
            expect(TokenKind::Greater, "expected '>' after sizeof's type argument");
            expect(TokenKind::LeftParen, "expected '(' after 'sizeof<Type>'");
            expect(TokenKind::RightParen, "expected ')' - sizeof<Type>() takes no arguments");
            return std::make_unique<SizeOfExpr>(typeName);
        }

        // `hash<TypeName>(value)`/`keyEq<TypeName>(a, b)` - builtins, not real callable functions,
        // recognized by literal text exactly like `sizeof` above (see
        // docs/language/0034-maps-and-sets.md's own "2026 Update").
        if (current().text == "hash" && peek().kind == TokenKind::Less)
        {
            advance();
            expect(TokenKind::Less, "expected '<' after 'hash'");
            const std::string typeName = parseTypeName();
            expect(TokenKind::Greater, "expected '>' after hash's type argument");
            expect(TokenKind::LeftParen, "expected '(' after 'hash<Type>'");
            auto value = parseExpression();
            expect(TokenKind::RightParen, "expected ')' after hash<Type>'s argument");
            return std::make_unique<HashOfExpr>(typeName, std::move(value));
        }

        if (current().text == "keyEq" && peek().kind == TokenKind::Less)
        {
            advance();
            expect(TokenKind::Less, "expected '<' after 'keyEq'");
            const std::string typeName = parseTypeName();
            expect(TokenKind::Greater, "expected '>' after keyEq's type argument");
            expect(TokenKind::LeftParen, "expected '(' after 'keyEq<Type>'");
            auto left = parseExpression();
            expect(TokenKind::Comma, "expected ',' between keyEq<Type>'s two arguments");
            auto right = parseExpression();
            expect(TokenKind::RightParen, "expected ')' after keyEq<Type>'s arguments");
            return std::make_unique<KeyEqExpr>(typeName, std::move(left), std::move(right));
        }

        // `Map<key,value>()` construction is deliberately NOT handled here anymore (see
        // docs/language/0034-maps-and-sets.md's own "2026 Update") - Map<K,V> is a real,
        // user-declared generic struct now (std/collections.ax), constructed via its own
        // `newMap<K,V>()` generic top-level function.

        // `SortedMap<key,value>()` construction is deliberately NOT handled here anymore (see
        // docs/language/0040-sorted-maps.md's own "2026 Update") - SortedMap<K,V> is a real,
        // user-declared generic struct now (std/collections.ax), constructed via its own
        // `newSortedMap<K,V>()` generic top-level function.

        // `name<Type>(args)` / `name<Type1,Type2>(args)` (see docs/language/0006-generics.md's own
        // List<T> port follow-up, extended for a two-type-param generic top-level function by
        // docs/language/0034-maps-and-sets.md's own "2026 Update", e.g. `newMap<K,V>()`) - one or
        // more explicit call-site type arguments to a generic top-level function, comma-joined
        // into `CallExpr::typeArgument`'s own single string field (still storage-compatible with
        // the one-type-param case - GenericMonomorphizer is the only consumer, and its own
        // token-scanning `substituteTypeParams` already treats ',' as an ordinary separator, so no
        // AST field change is needed to carry more than one). Checked before the plain-call branch
        // just below (which only matches `Identifier '('` directly) and before the generic-struct-
        // literal branch further below (which shares the same `Identifier '<'` prefix,
        // disambiguated only by what follows the balanced '>').
        if (peek().kind == TokenKind::Less && looksLikeGenericCall())
        {
            const auto& name = advance();
            advance(); // '<'
            std::string typeArgument = parseTypeName();
            while (current().kind == TokenKind::Comma)
            {
                advance();
                typeArgument += "," + parseTypeName();
            }
            expect(TokenKind::Greater, "expected '>' after call's type argument(s)");
            expect(TokenKind::LeftParen, "expected '(' after function name");
            auto args = parseArgumentList();
            expect(TokenKind::RightParen, "expected ')' after arguments");

            return std::make_unique<CallExpr>(name.text, std::move(args), typeArgument);
        }

        if (peek().kind == TokenKind::LeftParen)
        {
            const auto& name = advance();
            expect(TokenKind::LeftParen, "expected '(' after function name");
            auto args = parseArgumentList();
            expect(TokenKind::RightParen, "expected ')' after arguments");

            return std::make_unique<CallExpr>(name.text, std::move(args));
        }

        if (allowStructLiteral &&
            (peek().kind == TokenKind::LeftBrace ||
             (peek().kind == TokenKind::Less && looksLikeGenericStructLiteral())))
        {
            const auto& name = advance();

            // "Box<i32> { ... }" (see docs/language/0006-generics.md) - explicit type arguments
            // at a struct-literal site, resolved to a real, mangled struct name later by
            // GenericMonomorphizer. looksLikeGenericStructLiteral() already confirmed this
            // matches, non-consumingly, so nothing here can fail once committed.
            std::string typeName = name.text;
            if (match(TokenKind::Less))
            {
                typeName += "<" + parseTypeName();
                while (match(TokenKind::Comma))
                {
                    typeName += "," + parseTypeName();
                }
                expect(TokenKind::Greater, "expected '>' after generic struct literal type arguments");
                typeName += ">";
            }

            expect(TokenKind::LeftBrace, "expected '{' after struct type name");
            auto fields = parseStructLiteralFields();
            expect(TokenKind::RightBrace, "expected '}' after struct literal fields");

            return std::make_unique<StructLiteralExpr>(std::move(typeName), std::move(fields));
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
        // Bitwise AND (see docs/language/0034-maps-and-sets.md's own "2026 Update") - added as a
        // real infix operator for the first time, needed for a hash table's own bucket-index
        // computation (`hash & (bucketCount - 1)`, the same AND-mask idiom the retired Map<K,V>/
        // Set<T> intrinsic's own LLVM templates already used internally). Placed looser than
        // comparisons/arithmetic (mirrors C's own relative bitwise-vs-arithmetic precedence,
        // scaled down since this language has no other bitwise operators to rank against yet) -
        // every real use in this codebase wraps the right operand in parens regardless, so the
        // exact value mostly just needs to be internally consistent.
        case TokenKind::Ampersand: return 8;
        case TokenKind::Plus:
        case TokenKind::Minus: return 10;
        case TokenKind::Star:
        case TokenKind::Slash: return 20;
        default: return -1;
    }
}
