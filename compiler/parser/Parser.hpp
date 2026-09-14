#pragma once

#include "ast/Stmt.hpp"
#include "lexer/Token.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
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

    // Pushes one top-level item into `out` (two, for a struct with embedded methods - see
    // parseStructDecl's own comment) - not a return value, for the same reason.
    void parseItem(std::vector<std::unique_ptr<Stmt>>& out);
    // C-style declaration syntax (see docs/language/0068-c-style-syntax.md) - `ReturnType
    // name(params) { body }` instead of `name(params) -> ReturnType { body }`. Tried via a
    // speculative parse + rollback (see parseItem's own use, and tryParseCStyleFunctionDecl's own
    // comment) rather than a bounded lookahead scanner like looksLikeFunctionDecl/
    // looksLikeGenericCall above - a return type can itself be arbitrarily rich type grammar
    // (`*T`, `[T;N]`, `fn(T)->R`, `List<T,U>`), too rich to duplicate as a second hand-rolled
    // scanner without it silently drifting out of sync with parseTypeName's own grammar.
    // Attempts a C-style top-level function declaration at the current position; returns nullptr
    // (with `index_` restored to its original position) if the current position doesn't parse as
    // one, so the caller can fall through to ordinary statement parsing unchanged. Never partially
    // consumes tokens on failure.
    std::unique_ptr<Stmt> tryParseCStyleFunctionDecl();
    // Shared by tryParseCStyleFunctionDecl (selfType == "") and struct-embedded methods
    // (selfType == the enclosing struct's own self-type text, see buildSelfTypeText) - parses
    // everything after a C-style header's own already-parsed `ReturnType name` prefix: optional
    // `<T,U>` type params, `(params)`, then a body (`{ ... }` or `=> expr`). `mangledName` is the
    // FunctionDecl's own name ("increment" for a bare top-level function, "Counter.increment" for
    // an embedded method - see parseImplMethod's own identical mangling).
    std::unique_ptr<FunctionDecl> parseCStyleFunctionTail(std::string mangledName,
                                                          std::optional<std::string> returnType,
                                                          const std::string& selfType);
    // One parameter in C-style order (`Type name` instead of parseParam's own `name: Type`) -
    // mirrors parseParam's own capability-prefix handling exactly, just with the type/name order
    // swapped.
    Param parseParamCStyle();
    // Self-aware C-style param, mirroring parseSelfAwareParam's own relationship to parseParam -
    // recognizes a bare `self` (unambiguous here since a real C-style param named `self` would
    // need an explicit type token before it, i.e. two tokens, not one) when `selfType` is
    // non-empty, else delegates to parseParamCStyle() unconditionally (a top-level C-style
    // function has no receiver, so `self` is never special there - `selfType == ""` disables the
    // check entirely, matching how a top-level old-style function already lets `self` be an
    // ordinary, explicitly-typed parameter name).
    Param parseSelfAwareParamCStyle(const std::string& selfType);
    // `Name<T, ...>` self-type text for a possibly-generic type (see parseImplDecl's own
    // identical construction, which this factors out of) - "Name" alone if typeParams is empty,
    // else "Name<T,U,...>" with no space after each comma (see parseImplDecl's own comment on why
    // that space previously caused a real, previously-undiscovered infinite-monomorphization-loop
    // bug). Shared by parseImplDecl and struct-embedded-method parsing (parseStructDecl), both of
    // which need the exact same text for a method's own bare `self` parameter.
    static std::string buildSelfTypeText(const std::string& typeName,
                                         const std::vector<std::string>& typeParams);
    // Disambiguates `Identifier '('` at item level - a function
    // declaration (`foo(x: i32) -> i32 { ... }`) or a bare top-level call
    // kept for its side effect (`print("hi")`) - both start identically.
    // Only called with current() == Identifier and peek() == LeftParen
    // already confirmed; looks past that '(' at what a real Param can
    // only ever start with (see parseParam: `[read|write|take]?
    // Identifier ':'`, never anything an argument expression could also
    // start with) to decide without backtracking.
    bool looksLikeFunctionDecl() const;
    // `Box<i32> { ... }` (a generic struct literal, see docs/language/0006-generics.md) vs.
    // `x < y` / `x < y > z` (comparison chains) at expression position - disambiguated by a
    // bounded, non-consuming forward scan (mirrors looksLikeFunctionDecl's own "peek ahead,
    // decide, then re-enter the real grammar" idiom; never backtracking - this parser has no
    // snapshot/restore mechanism anywhere and doesn't need one here). Only called with
    // current() == Identifier and peek() == Less already confirmed.
    bool looksLikeGenericStructLiteral() const;
    // Identical scan, checking for '(' instead of '{' at the end - see
    // looksLikeGenericStructLiteral's own comment.
    bool looksLikeGenericCall() const;
    // Identical scan, checking for '.' instead of '('/'{' at the end - `Box<i32>.new(41)` (an
    // associated function called on an *explicit* generic instantiation, see
    // docs/language/0068-c-style-syntax.md's own "Associated functions on an explicit generic
    // instantiation" section) vs. `x < y > .z` (never valid - a bare '.' can't start an
    // expression - so no real comparison-chain shape is ever lost by preferring this reading
    // whenever it matches).
    bool looksLikeGenericTypeRefBeforeDot() const;
    std::unique_ptr<Stmt> parseFunctionDecl();
    // `fn(x: i32) -> i32 { x + 1 }` (see docs/language/0067-closures.md) - a closure literal,
    // same (params, optional return type, body) shape as parseFunctionDecl, as an expression.
    std::unique_ptr<Expr> parseClosureExpr();
    // `extern c name(params) [-> returnType]` (see docs/language/0048-ffi.md) -
    // no body, no capability prefixes on its own params (mirrors a plain C
    // signature - parseExternParam is a simpler, dedicated parse than
    // parseParam's own read/write/take-aware version).
    std::unique_ptr<Stmt> parseExternDecl();
    Param parseExternParam();
    // A struct body may now contain C-style embedded methods alongside its fields (see
    // docs/language/0068-c-style-syntax.md), which desugar into a synthesized ImplDecl - so a
    // single `struct { ... }` can yield *two* top-level items (the StructDecl plus that ImplDecl)
    // where it used to always yield exactly one. Pushes directly into `out` instead of returning a
    // single Stmt for that reason; a struct body with no embedded methods still pushes just the
    // one StructDecl, so old-style `struct { }` + a separate `impl { }` elsewhere is completely
    // unaffected.
    void parseStructDecl(std::vector<std::unique_ptr<Stmt>>& out);
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
    std::unique_ptr<FunctionDecl> parseImplMethod(const std::string& typeName,
                                                   const std::string& selfType);
    // `enum Name { Variant(T1, T2)  Other  ... }` (see docs/language/0064-enums.md) - variants
    // are whitespace-separated (no commas between them, same convention struct fields already
    // use), each optionally followed by a parenthesized, comma-separated positional payload
    // type list.
    std::unique_ptr<Stmt> parseEnumDecl();
    // `module math` (see docs/language/0066-modules.md) - one per file, at most (a second is a
    // parse error). Sets moduleName_, read by parseProgram to populate Program::moduleName.
    std::unique_ptr<Stmt> parseModuleDecl();
    // `use math [as m]` - records `m` (or `math` itself, with no alias) into aliases_, consulted
    // by parsePostfix to rewrite any later `m.foo(...)`/`m.field`'s own object NameExpr to the
    // real module name at the point it's parsed (see aliases_'s own comment).
    std::unique_ptr<Stmt> parseUseDecl();
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
    std::unique_ptr<Expr> parseUnsafeExpr();
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
    // "module math" (see docs/language/0066-modules.md) - nullopt until a ModuleDecl is parsed
    // in this file (at most one; parseModuleDecl throws on a second). Copied into
    // Program::moduleName once parseProgram finishes.
    std::optional<std::string> moduleName_;
    // "use math as m" (or "use math" with no alias, keyed by "math" itself) - alias/name -> real
    // module name, consulted by parsePostfix: an object NameExpr immediately followed by '.' is
    // rewritten to the real module name here, at the exact moment it's about to become a
    // MethodCallExpr/FieldExpr's own `object` - never anywhere else a plain identifier is used,
    // so an ordinary local variable that happens to share a name with an in-scope alias is
    // unaffected everywhere except that one position (the same latent shadowing risk
    // EnumName.Variant construction already accepts for a variable named after a declared enum -
    // see docs/language/0064-enums.md).
    std::unordered_map<std::string, std::string> aliases_;
};
