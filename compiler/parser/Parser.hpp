#pragma once

#include "ast/Stmt.hpp"
#include "lexer/Token.hpp"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

class Parser
{
public:
    explicit Parser(std::vector<Token> tokens);

    Program parseProgram();

private:
    const Token& current() const;
    const Token& peek(std::size_t offset = 1) const;
    const Token& advance();
    bool match(TokenKind kind);
    const Token& expect(TokenKind kind, const char* message);

    std::unique_ptr<Stmt> parseItem();
    // Disambiguates `Identifier '('` at item level - a function
    // declaration (`foo(x: i32) -> i32 { ... }`) or a bare top-level call
    // kept for its side effect (`print("hi")`) - both start identically.
    // Only called with current() == Identifier and peek() == LeftParen
    // already confirmed; looks past that '(' at what a real Param can
    // only ever start with (see parseParam: `[read|write|take]?
    // Identifier ':'`, never anything an argument expression could also
    // start with) to decide without backtracking.
    bool looksLikeFunctionDecl() const;
    std::unique_ptr<Stmt> parseFunctionDecl();
    // `extern c name(params) [-> returnType]` (see docs/language/0048-ffi.md) -
    // no body, no capability prefixes on its own params (mirrors a plain C
    // signature - parseExternParam is a simpler, dedicated parse than
    // parseParam's own read/write/take-aware version).
    std::unique_ptr<Stmt> parseExternDecl();
    Param parseExternParam();
    std::unique_ptr<Stmt> parseStructDecl();
    // `trait Name { format(self, buf: Buffer)  ... }` (see
    // docs/language/0062-display-trait.md) - a method signature has no
    // body, just a name/params/optional return type, and its own first
    // param may be a bare `self` (parseSelfAwareParam) instead of an
    // ordinary parseParam-shaped one.
    std::unique_ptr<Stmt> parseTraitDecl();
    // `impl TraitName for TypeName { method bodies }` - each method
    // desugars into a real FunctionDecl, mangled name
    // `typeName + "." + methodName`, via parseImplMethod.
    std::unique_ptr<Stmt> parseImplDecl();
    // One parameter inside a trait method signature or an impl method's
    // own param list, where the first parameter may be a bare `self` (no
    // ':type', no capability prefix) instead of parseParam's ordinary
    // `[read|write|take]? name: Type` shape. `selfType` is what a bare
    // `self` resolves to - the impl's own concrete target type for an
    // impl method, or the literal placeholder text "Self" for a trait
    // signature (never type-checked - see TraitDecl's own comment).
    Param parseSelfAwareParam(const std::string& selfType);
    // One method body inside an `impl` block - same shape as
    // parseFunctionDecl (params/optional return type/body), except the
    // name is mangled to `typeName + "." + methodName` and the first
    // param may be a bare `self`.
    std::unique_ptr<FunctionDecl> parseImplMethod(const std::string& typeName);
    // `enum Name { Variant(T1, T2)  Other  ... }` (see docs/language/0064-enums.md) - variants
    // are whitespace-separated (no commas between them, same convention struct fields already
    // use), each optionally followed by a parenthesized, comma-separated positional payload
    // type list.
    std::unique_ptr<Stmt> parseEnumDecl();
    // `match scrutinee { Variant(a, b) => expr  _ => expr }` - arms are whitespace-separated
    // (same convention as struct fields/enum variants); each arm's `variantName` is a bare
    // identifier or the literal text "_", optionally followed by a parenthesized,
    // comma-separated binding-name list, then `=>`, then the arm's own single-expression body.
    std::unique_ptr<Expr> parseMatchExpr();
    std::unique_ptr<Stmt> parseAssignment();
    std::unique_ptr<Stmt> parseReturn();
    std::unique_ptr<Stmt> parseWhile();
    std::unique_ptr<Stmt> parseBreak();
    std::unique_ptr<Stmt> parseContinue();
    // `for i in a..b { body }` is pure syntactic sugar, desugared here into
    // `{ i = a  while i < b { body  i++ } }` - no dedicated ForStmt AST node,
    // matching how `=>` already desugars in parseFunctionDecl. Everything
    // downstream of the parser handles the result without any awareness
    // that `for` exists (see docs/language/0029-for-loops.md).
    std::unique_ptr<Stmt> parseFor();
    Param parseParam();
    // A type is either a plain identifier ("i32", "User") or an array type
    // "[elem;N]" (see docs/language/0031-arrays.md), canonicalized here with
    // no spaces so every downstream consumer (TypeChecker::resolveType,
    // LlvmIrEmitter::llvmType) can parse the same fixed shape. Replaces every
    // former `expect(TokenKind::Identifier, "expected ... type")` call site.
    std::string parseTypeName();
    // The single-alternative shape parseTypeName's old body was, before "T1 | T2 | ..."
    // union types (see docs/language/0065-unions.md) - parseTypeName itself now wraps this,
    // collecting one or more Pipe-separated atoms into a canonical, sorted-and-deduplicated
    // union string when more than one is present.
    std::string parseTypeNameAtom();

    std::unique_ptr<Expr> parseBlock();
    std::unique_ptr<Expr> parseIfExpr();
    std::unique_ptr<Expr> parseLoopExpr();
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> parseStructLiteralFields();

    std::unique_ptr<Expr> parseExpression(int minPrecedence = 0, bool allowStructLiteral = true);
    std::unique_ptr<Expr> parsePostfix(bool allowStructLiteral);
    std::unique_ptr<Expr> parsePrimary(bool allowStructLiteral);
    // Comma-separated expressions up to (not including) the closing ')' -
    // shared by a bare function call and a method call's argument list
    // (docs/language/0033-lists.md), which were previously two copies of the
    // identical loop. Caller has already consumed '(' and is responsible for
    // expecting the closing ')'.
    std::vector<std::unique_ptr<Expr>> parseArgumentList();
    int precedence(TokenKind kind) const;
    // Decodes a char literal's own raw UTF-8 bytes (already captured
    // verbatim between the opening/closing `'` by Lexer::lexChar - see
    // docs/language/0044-char.md) into a single Unicode scalar value.
    // Rejects anything that isn't exactly one well-formed, non-overlong,
    // non-surrogate codepoint - empty ('' ), multi-character ('ab'), and
    // malformed UTF-8 are all parse errors here, not silently truncated.
    static std::int32_t decodeCharLiteral(const std::string& bytes);
    // Splits a string literal's own already-quote-stripped raw content on
    // unescaped `{...}` spans (see
    // docs/language/Axea_Printing_Formatting.md) - `{{`/`}}` are literal
    // brace escapes. A literal with no interpolation span at all returns
    // a plain StringExpr, unaffected (every pre-existing string literal
    // in this codebase takes this path); one or more spans returns an
    // InterpolatedStringExpr instead. Each span's own content is parsed
    // as a standalone expression via a fresh, nested Lexer+Parser -
    // legal to call `.parseExpression()` on another Parser instance
    // directly since C++ access control is per-class, not per-object.
    std::unique_ptr<Expr> parseStringLiteral(const std::string& text);
    // Decodes a raw String token's full captured text (still carrying its
    // optional 'r' prefix and its 1- or 3-quote delimiters - see
    // docs/language/0059-raw-strings.md and
    // docs/language/0060-multiline-strings.md) into either a plain
    // StringExpr (raw: zero processing of the inner content at all, not
    // even '{{'/'}}' escaping) or a call into parseStringLiteral (non-raw:
    // identical interpolation handling for both 1- and 3-quote literals,
    // since parseStringLiteral's own char-by-char scan doesn't care
    // whether an embedded byte happens to be a real newline).
    std::unique_ptr<Expr> parseRawOrInterpolatedString(const std::string& tokenText);

    std::vector<Token> tokens_;
    std::size_t index_{0};
    // Unique per for-loop, so nested for-loops' internal counter/end names
    // (see parseFor) can never collide with each other.
    int forCounter_{0};
};
